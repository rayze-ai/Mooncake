#include "host_fabric_allocator.h"

#include <glog/logging.h>

#ifdef USE_CUDA
#include <cuda.h>
#include <cuda_runtime.h>

#include <cstdint>
#include <vector>
#endif

namespace mooncake {

#ifdef USE_CUDA
namespace {

// Every device must report fabric-handle support, mirroring the check
// NvlinkTransport makes before taking its fabric path. A single device without
// it means the handle this allocation exports cannot be imported everywhere the
// segment is expected to be reachable, so the allocation is refused outright
// rather than produce a segment that works from some peers only.
bool AllDevicesSupportFabric(int device_count) {
    for (int device_id = 0; device_id < device_count; ++device_id) {
        int supported = 0;
        const CUresult result = cuDeviceGetAttribute(
            &supported, CU_DEVICE_ATTRIBUTE_HANDLE_TYPE_FABRIC_SUPPORTED,
            device_id);
        if (result != CUDA_SUCCESS || !supported) {
            LOG(ERROR) << "HostFabricAllocator: device " << device_id
                       << " does not support CU_MEM_HANDLE_TYPE_FABRIC";
            return false;
        }
    }
    return true;
}

// The NUMA node to back the allocation with. Uses the host-NUMA node of the
// current CUDA device: a segment is served by the process that mounted it, and
// that process's device is the best available proxy for where the memory will
// be touched from locally. A driver that does not expose
// cudaDevAttrHostNumaId reports a negative value, which means HOST_NUMA
// allocation is unavailable and the caller must fail rather than guess node 0.
int HostNumaNodeForCurrentDevice() {
    int device = 0;
    const cudaError_t err = cudaGetDevice(&device);
    if (err != cudaSuccess) {
        LOG(ERROR) << "HostFabricAllocator: cudaGetDevice failed: "
                   << cudaGetErrorString(err);
        return -1;
    }
    int host_numa = -1;
    const cudaError_t attr_err =
        cudaDeviceGetAttribute(&host_numa, cudaDevAttrHostNumaId, device);
    if (attr_err != cudaSuccess || host_numa < 0) {
        LOG(ERROR) << "HostFabricAllocator: device " << device
                   << " reports no host NUMA node (cudaDevAttrHostNumaId): "
                   << cudaGetErrorString(attr_err);
        return -1;
    }
    return host_numa;
}

}  // namespace
#endif  // USE_CUDA

void *AllocateHostFabricMemory(size_t size, size_t alignment) {
#ifndef USE_CUDA
    (void)size;
    (void)alignment;
    LOG(ERROR) << "HostFabricAllocator: host fabric memory requires a CUDA "
                  "build (USE_CUDA=ON)";
    return nullptr;
#else
    int device_count = 0;
    const cudaError_t count_err = cudaGetDeviceCount(&device_count);
    if (count_err != cudaSuccess || device_count == 0) {
        LOG(ERROR) << "HostFabricAllocator: no CUDA devices: "
                   << cudaGetErrorString(count_err);
        return nullptr;
    }
    if (!AllDevicesSupportFabric(device_count)) {
        return nullptr;
    }

    const int numa_node = HostNumaNodeForCurrentDevice();
    if (numa_node < 0) {
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
        LOG(ERROR) << "HostFabricAllocator: cuMemGetAllocationGranularity "
                      "failed: "
                   << result;
        return nullptr;
    }

    // The caller may need a stricter alignment than the driver's minimum
    // granularity (cachelib requires Slab::kSize-aligned arenas). Honor
    // whichever is larger; both are powers of two in practice, so the larger
    // is a multiple of the smaller and the size rounding below still satisfies
    // cuMemCreate's granularity requirement.
    size_t effective_alignment = granularity;
    if (alignment > granularity) {
        const bool both_pow2 = (granularity & (granularity - 1)) == 0 &&
                               (alignment & (alignment - 1)) == 0;
        if (both_pow2) {
            effective_alignment = alignment;
        } else {
            LOG(WARNING) << "HostFabricAllocator: cannot honor alignment "
                         << alignment << " over granularity " << granularity
                         << " (not both powers of two), using granularity";
        }
    }

    size = (size + effective_alignment - 1) & ~(effective_alignment - 1);
    if (size == 0) size = effective_alignment;

    CUmemGenericAllocationHandle handle;
    result = cuMemCreate(&handle, size, &prop, 0);
    if (result != CUDA_SUCCESS) {
        LOG(ERROR) << "HostFabricAllocator: cuMemCreate failed: " << result;
        return nullptr;
    }

    CUdeviceptr ptr = 0;
    result = cuMemAddressReserve(&ptr, size, effective_alignment, 0, 0);
    if (result != CUDA_SUCCESS) {
        LOG(ERROR) << "HostFabricAllocator: cuMemAddressReserve failed: "
                   << result;
        cuMemRelease(handle);
        return nullptr;
    }

    result = cuMemMap(ptr, size, 0, handle, 0);
    if (result != CUDA_SUCCESS) {
        LOG(ERROR) << "HostFabricAllocator: cuMemMap failed: " << result;
        cuMemAddressFree(ptr, size);
        cuMemRelease(handle);
        return nullptr;
    }

    // Grant every device read/write access, plus the host itself: unlike a
    // VRAM segment, this region is written by the CPU (the store memcpy's into
    // it) and read by GPUs, so both location types need an access descriptor.
    std::vector<CUmemAccessDesc> access(static_cast<size_t>(device_count) + 1);
    for (int idx = 0; idx < device_count; ++idx) {
        access[idx].location.type = CU_MEM_LOCATION_TYPE_DEVICE;
        access[idx].location.id = idx;
        access[idx].flags = CU_MEM_ACCESS_FLAGS_PROT_READWRITE;
    }
    access[device_count].location.type = CU_MEM_LOCATION_TYPE_HOST_NUMA;
    access[device_count].location.id = numa_node;
    access[device_count].flags = CU_MEM_ACCESS_FLAGS_PROT_READWRITE;

    result = cuMemSetAccess(ptr, size, access.data(), access.size());
    if (result != CUDA_SUCCESS) {
        LOG(ERROR) << "HostFabricAllocator: cuMemSetAccess failed: " << result;
        cuMemUnmap(ptr, size);
        cuMemAddressFree(ptr, size);
        cuMemRelease(handle);
        return nullptr;
    }

    // The mapping holds a reference, so the local handle can be dropped here;
    // FreeHostFabricMemory() recovers one with cuMemRetainAllocationHandle().
    cuMemRelease(handle);

    LOG(INFO) << "HostFabricAllocator: allocated " << size
              << " bytes of fabric host memory on NUMA node " << numa_node
              << " at " << reinterpret_cast<void *>(ptr) << " (alignment "
              << effective_alignment << ")";
    return reinterpret_cast<void *>(ptr);
#endif  // USE_CUDA
}

void FreeHostFabricMemory(void *ptr) {
#ifndef USE_CUDA
    (void)ptr;
    LOG(ERROR) << "HostFabricAllocator: host fabric memory requires a CUDA "
                  "build (USE_CUDA=ON)";
#else
    if (ptr == nullptr) {
        return;
    }
    // Retain before unmapping: cuMemRetainAllocationHandle resolves the handle
    // through the mapping, so it must run while the mapping still exists.
    // cuMemGetAddressRange recovers the reserved size, which avoids tracking
    // sizes in a side table that AllocateHostFabricMemory's callers would have
    // to keep consistent.
    CUmemGenericAllocationHandle handle;
    CUresult result = cuMemRetainAllocationHandle(&handle, ptr);
    if (result != CUDA_SUCCESS) {
        LOG(ERROR) << "HostFabricAllocator: cuMemRetainAllocationHandle "
                      "failed: "
                   << result << "; leaking " << ptr;
        return;
    }
    size_t size = 0;
    result = cuMemGetAddressRange(nullptr, &size,
                                 reinterpret_cast<CUdeviceptr>(ptr));
    if (result == CUDA_SUCCESS && size > 0) {
        cuMemUnmap(reinterpret_cast<CUdeviceptr>(ptr), size);
        cuMemAddressFree(reinterpret_cast<CUdeviceptr>(ptr), size);
    } else {
        LOG(ERROR) << "HostFabricAllocator: cuMemGetAddressRange failed: "
                   << result << "; leaking the mapping at " << ptr;
    }
    cuMemRelease(handle);
#endif  // USE_CUDA
}

}  // namespace mooncake
