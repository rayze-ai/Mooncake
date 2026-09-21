#include "common/client_buffer_allocation.h"

#include "config.h"
#include "config/hugepage_config.h"
#include "ub_allocator.h"

#include <cstdlib>
#include <glog/logging.h>

#ifdef USE_CUDA
#include <cuda_runtime.h>
#endif
#ifdef USE_INTRA_NVLINK
#include "gpu_vendor/intra_nvlink.h"
#endif
#ifdef USE_VRAM_SEGMENT
#include "config/vram_fabric_config.h"
#endif
// Host DRAM exported as a fabric handle is only reachable via the final
// aligned_alloc() fallback below, which a USE_VRAM_SEGMENT build never reaches:
// there, every segment is device memory regardless of protocol. So the host
// fabric path is compiled only for CUDA builds without VRAM segments.
#if defined(USE_CUDA) && !defined(USE_VRAM_SEGMENT)
#define MOONCAKE_STORE_FABRIC_HOST 1
#include "common/host_fabric_allocator.h"
#include "config/host_fabric_config.h"
#endif
#if defined(USE_VRAM_SEGMENT) && defined(USE_MNNVL)
#include "gpu_vendor/mnnvl.h"
// mnnvl.h dispatches to a vendor backend, and only the NVLink one offers an
// alignment-aware fabric allocation (the HIP/MUSA/UBShmem backends keep the
// single-argument form, and CMake does not even build nvlink_transport for a
// USE_MNNVL+USE_HIP configuration). Gate the fabric VRAM path on the backend
// actually providing it so those combinations still compile.
#ifdef MOONCAKE_HAS_FABRIC_ALIGNED_ALLOC
#define MOONCAKE_STORE_FABRIC_VRAM 1
#endif
#endif
#if defined(USE_ASCEND_DIRECT) || defined(USE_UBSHMEM)
#include "ascend_allocator.h"
#endif
#if defined(USE_SUNRISE)
#include "sunrise_allocator.h"
#endif
#ifdef USE_NOF
#include "spdk/spdk_wrapper.h"
#endif

namespace mooncake {
namespace {

#ifdef USE_VRAM_SEGMENT
#ifdef MOONCAKE_STORE_FABRIC_VRAM
// Whether this buffer should be allocated as an exportable fabric allocation
// rather than with cudaMalloc. Only meaningful for the cross-node NVLink
// protocol: nvlink_intra reaches peers through CUDA IPC handles instead, and
// every other protocol reads the segment over RDMA/TCP, where a fabric handle
// buys nothing.
//
// The environment is read once so a segment and its later release agree on
// which allocator owns the memory, even if the variable changes in between.
bool use_fabric_vram(const std::string &protocol) {
    if (protocol != "nvlink") {
        return false;
    }
    static const bool enabled = VramFabricConfig::IsEnabledFromEnvironment();
    return enabled;
}
#endif

tl::expected<void *, std::string> allocate_vram_memory(
    size_t total_size, const std::string &protocol, size_t alignment) {
    // Only the fabric branch below consumes the alignment; cudaMalloc and the
    // intra-node allocator have no way to accept one.
    (void)alignment;
    cudaError_t res;
    int device;
    void *ptr = nullptr;
    res = cudaGetDevice(&device);
    if (res != cudaSuccess) {
        LOG(ERROR) << "VRAM Segment cudaGetDevice failed.";
        return tl::make_unexpected("VRAM Segment cudaGetDevice failed.");
    }
    if (protocol == "nvlink_intra") {
#ifdef USE_INTRA_NVLINK
        ptr = allocateFabricMemory_intra(total_size);
        return ptr;
#else
        LOG(ERROR) << "Protocol nvlink_intra need USE_INTRA_NVLINK=ON. Please "
                      "rebuild mooncake from source.";
        return tl::make_unexpected("Protocol not supported");
#endif
    }
#ifndef MOONCAKE_STORE_FABRIC_VRAM
    // The switch is set but this build cannot honor it, so the segment is about
    // to fall back to cudaMalloc and will register nothing over NVLink. Say so
    // loudly rather than let the operator believe the request took effect --
    // that silence is the exact failure this feature exists to remove.
    if (protocol == "nvlink" && VramFabricConfig::IsEnabledFromEnvironment()) {
        LOG(WARNING) << "MC_STORE_VRAM_FABRIC is set but this build provides no "
                        "alignment-aware fabric allocator (needs USE_MNNVL "
                        "without USE_HIP/USE_MUSA/USE_UBSHMEM). Falling back to "
                        "cudaMalloc: the segment will NOT be reachable over "
                        "NVLink from another node.";
    }
#endif
#ifdef MOONCAKE_STORE_FABRIC_VRAM
    if (use_fabric_vram(protocol)) {
        // Pass the caller's alignment through: the segment is handed to
        // cachelib, which requires a Slab::kSize-aligned base, and the CUDA
        // allocation granularity alone does not guarantee that.
        ptr = allocateFabricMemoryAligned(total_size, alignment);
        if (ptr == nullptr) {
            LOG(ERROR) << "VRAM Segment fabric allocation failed for "
                       << total_size
                       << " bytes. Check that every device reports fabric "
                          "handle support and that an IMEX domain is "
                          "configured, or unset MC_STORE_VRAM_FABRIC to fall "
                          "back to cudaMalloc.";
            return tl::make_unexpected(
                "VRAM Segment fabric allocation failed.");
        }
        LOG(INFO) << "VRAM Segment allocated " << total_size
                  << " bytes as an exportable fabric allocation, base=" << ptr
                  << ", alignment=" << alignment;
        return ptr;
    }
#endif
    res = cudaMalloc((void **)&ptr, total_size);
    if (res != cudaSuccess) {
        LOG(ERROR) << "VRAM Segment cudaMalloc failed.";
        return tl::make_unexpected("VRAM Segment cudaMalloc failed.");
    }
    return ptr;
}
#endif

#ifdef MOONCAKE_STORE_FABRIC_HOST
// Whether this host-DRAM buffer should be allocated through the CUDA VMM with
// an exportable fabric handle instead of plain aligned_alloc(). Mirrors
// use_fabric_vram(): only the cross-node NVLink protocol benefits, because
// NvlinkTransport::registerLocalMemory() needs a retainable allocation handle
// to make the segment reachable from a peer node. Every other protocol reads
// the segment over RDMA/TCP, where a fabric handle buys nothing and the extra
// VMM mapping only costs address space.
//
// Read once per process so an allocation and its later release agree on which
// allocator owns the memory, even if the variable changes in between.
bool use_fabric_host(const std::string &protocol) {
    if (protocol != "nvlink") {
        return false;
    }
    static const bool enabled = HostFabricConfig::IsEnabledFromEnvironment();
    return enabled;
}
#endif

}  // namespace

size_t get_hugepage_size_from_env(unsigned int *out_flags, bool use_memfd) {
    const HugepageConfig config = HugepageConfig::FromEnvironment();
    if (!config.enabled) {
        return 0;
    }

    const size_t size = config.page_size;
    if (out_flags == nullptr) {
        return size;
    }
    if (use_memfd) {
        *out_flags |= MFD_HUGETLB;
        *out_flags |= size == SZ_2MB     ? MFD_HUGE_2MB
                      : size == SZ_512MB ? MFD_HUGE_512MB
                                         : MFD_HUGE_1GB;
    } else {
        *out_flags |= MAP_HUGETLB;
        *out_flags |= size == SZ_2MB     ? MAP_HUGE_2MB
                      : size == SZ_512MB ? MAP_HUGE_512MB
                                         : MAP_HUGE_1GB;
    }
    LOG(INFO) << "Using hugepage size: "
              << (size == SZ_2MB     ? "2MB"
                  : size == SZ_512MB ? "512MB"
                                     : "1GB");
    return size;
}

void *allocate_buffer_allocator_memory(size_t total_size,
                                       const std::string &protocol,
                                       size_t alignment, bool use_spdk_dma) {
    const size_t default_alignment = facebook::cachelib::Slab::kSize;
    // Ensure total_size is a multiple of alignment
    if (alignment == default_alignment && total_size < alignment) {
        LOG(ERROR) << "Total size must be at least " << alignment;
        return nullptr;
    }
#if defined(USE_ASCEND_DIRECT) || defined(USE_UBSHMEM)
    if (protocol == "ascend" || protocol == "ubshmem") {
        return ascend_allocate_memory(total_size, protocol);
    }
#endif
#if defined(USE_SUNRISE)
    if (protocol == "sunrise_link") {
        return sunrise_allocate_memory(
            total_size, alignment,
            mooncake::globalConfig().sunrise_use_device_mem);
    }
#endif
#if defined(USE_UB)
    if (protocol == "ub") {
        return mooncake::ub_allocate_memory(alignment, total_size);
    }
#endif
#ifdef USE_NOF
    if (use_spdk_dma && total_size > 0) {
        return mooncake::SpdkWrapper::GetInstance().Alloc(total_size, alignment,
                                                          -1);
    }
#endif
#ifdef USE_VRAM_SEGMENT
    auto ret = allocate_vram_memory(total_size, protocol, alignment);
    if (!ret) {
        LOG(ERROR) << ret.error();
        return nullptr;
    }
    return *ret;
#endif
#ifdef MOONCAKE_STORE_FABRIC_HOST
    if (use_fabric_host(protocol)) {
        void *ptr = AllocateHostFabricMemory(total_size, alignment);
        if (ptr == nullptr) {
            // Deliberately no aligned_alloc() fallback: that pointer carries no
            // fabric handle, so registerLocalMemory() would register nothing
            // and the segment would mount but stay unreachable from peers.
            // Failing here names the real cause.
            LOG(ERROR) << "Failed to allocate " << total_size
                       << " bytes of fabric host memory for protocol "
                       << protocol
                       << "; unset MC_STORE_HOST_FABRIC to use plain host "
                          "memory instead.";
            return nullptr;
        }
        LOG(INFO) << "Allocated " << total_size
                  << " bytes of fabric host memory, base=" << ptr
                  << ", alignment=" << alignment;
        return ptr;
    }
#endif
    // Allocate aligned memory
    return aligned_alloc(alignment, total_size);
}

void free_memory(const std::string &protocol, void *ptr, bool use_spdk_dma) {
#if defined(USE_ASCEND_DIRECT) || defined(USE_UBSHMEM)
    if (protocol == "ascend" || protocol == "ubshmem") {
        return ascend_free_memory(protocol, ptr);
    }
#endif
#if defined(USE_SUNRISE)
    if (protocol == "sunrise_link") {
        return sunrise_free_memory(ptr);
    }
#endif
#if defined(USE_UB)
    if (protocol == "ub") {
        mooncake::ub_free_memory(ptr);
        return;
    }
#endif
#ifdef USE_NOF
    // Mirror allocate_buffer_allocator_memory(): a buffer taken from the SPDK
    // hugepage pool (spdk_zmalloc) must be released with spdk_free, not glibc
    // free(), which would abort with "free(): invalid pointer".
    if (use_spdk_dma) {
        mooncake::SpdkWrapper::GetInstance().Free(ptr);
        return;
    }
#endif
#ifdef USE_VRAM_SEGMENT
    // Mirror allocate_vram_memory() exactly. A fabric allocation is backed by
    // cuMemCreate, so releasing it with cudaFree would leak both the physical
    // handle and the reserved VA range; conversely freePinnedLocalMemory()
    // cannot release a cudaMalloc'd pointer. The branch must therefore key off
    // the same protocol and switch the allocation did, not just the build flags.
#ifdef MOONCAKE_STORE_FABRIC_VRAM
    if (use_fabric_vram(protocol)) {
        freeFabricMemory(ptr);
        return;
    }
#endif
#ifdef USE_INTRA_NVLINK
    if (protocol == "nvlink_intra") {
        freeFabricMemory_intra(ptr);
        return;
    }
#endif
    cudaFree(ptr);
    return;
#endif
#ifdef MOONCAKE_STORE_FABRIC_HOST
    // Mirror allocate_buffer_allocator_memory(): a fabric host buffer is a VMM
    // mapping over a cuMemCreate handle, so free() would abort on a pointer
    // glibc never handed out. The branch must key off the same protocol and
    // switch the allocation did.
    if (use_fabric_host(protocol)) {
        FreeHostFabricMemory(ptr);
        return;
    }
#endif
    free(ptr);
}

}  // namespace mooncake
