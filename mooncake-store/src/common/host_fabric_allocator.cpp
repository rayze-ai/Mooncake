#include "common/host_fabric_allocator.h"

#include <glog/logging.h>

#ifdef USE_CUDA
#include <cuda.h>
#include <cuda_runtime.h>

#include <vector>
#endif

namespace mooncake {

#ifdef USE_CUDA

namespace {

const char *cu_error_name(CUresult result) {
    const char *name = nullptr;
    cuGetErrorName(result, &name);
    return name ? name : "CUDA_ERROR_UNKNOWN";
}

bool is_power_of_two(size_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

size_t round_up(size_t value, size_t multiple) {
    return (value + multiple - 1) / multiple * multiple;
}

// Which host NUMA node backs the current device. The driver exposes this as a
// device attribute; a driver that does not (attribute unsupported, or a
// negative value) is treated as a hard failure rather than guessing node 0.
// A wrong NUMA node would still "work" but place every segment on the wrong
// socket, and nothing downstream would flag it.
bool host_numa_node_for_current_device(int *numa_node) {
    int device = -1;
    cudaError_t res = cudaGetDevice(&device);
    if (res != cudaSuccess) {
        LOG(ERROR) << "Fabric host memory: cudaGetDevice failed: "
                   << cudaGetErrorString(res);
        return false;
    }
    int node = -1;
    res = cudaDeviceGetAttribute(&node, cudaDevAttrHostNumaId, device);
    if (res != cudaSuccess) {
        LOG(ERROR) << "Fabric host memory: cudaDevAttrHostNumaId query failed "
                      "on device "
                   << device << ": " << cudaGetErrorString(res);
        return false;
    }
    if (node < 0) {
        LOG(ERROR) << "Fabric host memory: device " << device
                   << " reports no host NUMA node (" << node
                   << "); refusing to guess.";
        return false;
    }
    *numa_node = node;
    return true;
}

}  // namespace

void *allocate_fabric_host_memory(size_t size, size_t alignment) {
    if (size == 0) {
        LOG(ERROR) << "Fabric host memory: refusing zero-byte allocation";
        return nullptr;
    }

    int numa_node = -1;
    if (!host_numa_node_for_current_device(&numa_node)) {
        return nullptr;
    }

    int device_count = 0;
    cudaError_t cres = cudaGetDeviceCount(&device_count);
    if (cres != cudaSuccess || device_count <= 0) {
        LOG(ERROR) << "Fabric host memory: cudaGetDeviceCount failed: "
                   << cudaGetErrorString(cres);
        return nullptr;
    }

    CUmemAllocationProp prop = {};
    prop.type = CU_MEM_ALLOCATION_TYPE_PINNED;
    prop.location.type = CU_MEM_LOCATION_TYPE_HOST_NUMA;
    prop.location.id = numa_node;
    prop.requestedHandleTypes = CU_MEM_HANDLE_TYPE_FABRIC;

    size_t granularity = 0;
    CUresult result = cuMemGetAllocationGranularity(
        &granularity, &prop, CU_MEM_ALLOC_GRANULARITY_MINIMUM);
    if (result != CUDA_SUCCESS || granularity == 0) {
        LOG(ERROR) << "Fabric host memory: cuMemGetAllocationGranularity "
                      "failed: "
                   << cu_error_name(result)
                   << ". The driver may not support fabric handles on host "
                      "NUMA memory.";
        return nullptr;
    }

    // The Store hands this base to cachelib, which needs Slab::kSize
    // alignment, and MountSegment rejects unaligned bases outright. When both
    // are powers of two the larger one satisfies both; otherwise fall back to
    // the granularity and say so, since the caller's alignment will then not
    // be honoured.
    size_t effective_alignment = granularity;
    if (alignment > granularity) {
        if (is_power_of_two(alignment) && is_power_of_two(granularity)) {
            effective_alignment = alignment;
        } else {
            LOG(WARNING) << "Fabric host memory: requested alignment "
                         << alignment << " and VMM granularity "
                         << granularity
                         << " are not both powers of two; using granularity";
        }
    }
    const size_t padded_size = round_up(size, effective_alignment);

    CUmemGenericAllocationHandle handle = 0;
    result = cuMemCreate(&handle, padded_size, &prop, 0);
    if (result != CUDA_SUCCESS) {
        LOG(ERROR) << "Fabric host memory: cuMemCreate(" << padded_size
                   << " bytes, host NUMA node " << numa_node
                   << ", FABRIC handle) failed: " << cu_error_name(result);
        return nullptr;
    }

    CUdeviceptr base = 0;
    result = cuMemAddressReserve(&base, padded_size, effective_alignment, 0, 0);
    if (result != CUDA_SUCCESS) {
        LOG(ERROR) << "Fabric host memory: cuMemAddressReserve failed: "
                   << cu_error_name(result);
        cuMemRelease(handle);
        return nullptr;
    }

    result = cuMemMap(base, padded_size, 0, handle, 0);
    if (result != CUDA_SUCCESS) {
        LOG(ERROR) << "Fabric host memory: cuMemMap failed: "
                   << cu_error_name(result);
        cuMemAddressFree(base, padded_size);
        cuMemRelease(handle);
        return nullptr;
    }

    // Unlike a VRAM segment, this region is written by the CPU (the Store
    // memcpys into it) and read by GPUs, so both location types need access:
    // one descriptor per device plus one for the host NUMA node.
    std::vector<CUmemAccessDesc> access(device_count + 1);
    for (int i = 0; i < device_count; ++i) {
        access[i].location.type = CU_MEM_LOCATION_TYPE_DEVICE;
        access[i].location.id = i;
        access[i].flags = CU_MEM_ACCESS_FLAGS_PROT_READWRITE;
    }
    access[device_count].location.type = CU_MEM_LOCATION_TYPE_HOST_NUMA;
    access[device_count].location.id = numa_node;
    access[device_count].flags = CU_MEM_ACCESS_FLAGS_PROT_READWRITE;

    result = cuMemSetAccess(base, padded_size, access.data(), access.size());
    if (result != CUDA_SUCCESS) {
        LOG(ERROR) << "Fabric host memory: cuMemSetAccess failed: "
                   << cu_error_name(result);
        cuMemUnmap(base, padded_size);
        cuMemAddressFree(base, padded_size);
        cuMemRelease(handle);
        return nullptr;
    }

    // The handle from cuMemCreate is deliberately not released here. Same
    // convention as NvlinkTransport::allocatePinnedLocalMemory(): the
    // allocation keeps one reference until free_fabric_host_memory(), which
    // recovers the handle through the mapping and releases it once.

    LOG(INFO) << "Allocated " << padded_size
              << " bytes of fabric host memory, base=0x" << std::hex << base
              << std::dec << ", alignment=" << effective_alignment
              << ", host NUMA node " << numa_node;
    return reinterpret_cast<void *>(base);
}

void free_fabric_host_memory(void *ptr) {
    if (ptr == nullptr) return;
    const CUdeviceptr base = reinterpret_cast<CUdeviceptr>(ptr);

    // Order matters: the handle is resolved *through the mapping*, so retain
    // it before unmapping. The size is recovered the same way instead of
    // being tracked in a side table. Mirrors
    // NvlinkTransport::freePinnedLocalMemory().
    CUmemGenericAllocationHandle handle = 0;
    CUresult result = cuMemRetainAllocationHandle(&handle, ptr);
    if (result != CUDA_SUCCESS) {
        LOG(ERROR) << "Fabric host memory: cuMemRetainAllocationHandle failed "
                      "on free: "
                   << cu_error_name(result) << "; leaking the region";
        return;
    }
    CUdeviceptr range_base = 0;
    size_t range_size = 0;
    result = cuMemGetAddressRange(&range_base, &range_size, base);
    if (result != CUDA_SUCCESS || range_size == 0) {
        LOG(ERROR) << "Fabric host memory: cuMemGetAddressRange failed on "
                      "free: "
                   << cu_error_name(result) << "; leaking the region";
        cuMemRelease(handle);
        return;
    }

    cuMemUnmap(range_base, range_size);
    cuMemAddressFree(range_base, range_size);
    // cuMemRetainAllocationHandle returns the same handle without adding a
    // reference of its own, so a single release balances the cuMemCreate.
    cuMemRelease(handle);
}

#else  // !USE_CUDA

void *allocate_fabric_host_memory(size_t, size_t) {
    LOG(ERROR) << "Fabric host memory requires a USE_CUDA build";
    return nullptr;
}

void free_fabric_host_memory(void *) {}

#endif  // USE_CUDA

}  // namespace mooncake
