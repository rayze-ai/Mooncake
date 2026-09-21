#pragma once

#include <cstddef>

namespace mooncake {

// Host DRAM allocated through the CUDA VMM API with
// CU_MEM_LOCATION_TYPE_HOST_NUMA and an exportable
// CU_MEM_HANDLE_TYPE_FABRIC handle -- "EGM" (Extended GPU Memory).
//
// Such a region can be imported by GPUs on other nodes of a multi-node NVLink
// domain, so a CPU-side segment is readable over NVLink instead of over
// RDMA/TCP. The plain aligned_alloc() buffer the store otherwise uses carries
// no fabric handle, so NvlinkTransport::registerLocalMemory() cannot retain an
// allocation handle for it and the segment ends up unreachable from peers.
//
// Both entry points are no-ops returning failure unless the build enables CUDA
// (USE_CUDA); see host_fabric_allocator.cpp. Callers gate on
// HostFabricConfig::IsEnabledFromEnvironment() -- reading it once per process
// so an allocation and its later release agree on which allocator owns the
// memory -- and must pair AllocateHostFabricMemory() with
// FreeHostFabricMemory(), never with free().
//
// `alignment` is honored when it exceeds the driver's minimum granularity and
// both values are powers of two; the store needs Slab::kSize-aligned bases
// because MountSegment rejects anything else. Returns nullptr on any failure,
// including a driver without host-NUMA or fabric support: there is no silent
// fallback, because a non-fabric pointer would fail later at registration or
// mount time with a far less obvious cause.
void *AllocateHostFabricMemory(size_t size, size_t alignment);

void FreeHostFabricMemory(void *ptr);

}  // namespace mooncake
