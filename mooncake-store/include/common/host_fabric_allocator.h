#pragma once

#include <cstddef>

namespace mooncake {

// Host (DRAM) memory allocated through CUDA VMM with a fabric handle -- what
// NVIDIA calls EGM (Extended GPU Memory). A GPU on another node in the same
// NVLink domain can import the handle and read this memory directly over
// NVLink, which is what lets a Store segment living in one node's DRAM serve
// Get() requests from every other node without going through RDMA.
//
// Why this is needed at all: NvlinkTransport::registerLocalMemory() calls
// cuMemRetainAllocationHandle() on the segment base. That call only succeeds
// on VMM allocations; on an aligned_alloc() or cudaMalloc() pointer it fails,
// and (on main) registration then returns success having registered nothing.
// The segment mounts, shows up in the master's index, gets selected, and
// only fails at the first remote read. So the segment has to be VMM-backed
// from the start, and this allocator refuses to hand back anything else.
//
// Both functions return / accept plain pointers so the call sites in
// client_buffer_allocation.cpp stay one-line branches.

// Allocates `size` bytes of fabric-exportable host memory, aligned to at
// least `alignment` (rounded up to the VMM granularity if larger). Returns
// nullptr on any failure and logs why. Never falls back to plain memory: a
// non-fabric pointer would only defer the failure to mount/read time with a
// far less obvious cause.
void *allocate_fabric_host_memory(size_t size, size_t alignment);

// Releases memory from allocate_fabric_host_memory(). Safe to call with
// nullptr. The size is recovered from the mapping itself, so callers do not
// need to track it.
void free_fabric_host_memory(void *ptr);

}  // namespace mooncake
