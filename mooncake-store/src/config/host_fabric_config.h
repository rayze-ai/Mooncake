#pragma once

namespace mooncake {

// Controls whether a host-memory segment is allocated with cuMemCreate +
// CU_MEM_LOCATION_TYPE_HOST_NUMA + CU_MEM_HANDLE_TYPE_FABRIC ("EGM") instead
// of aligned_alloc.
//
// The motivation mirrors VramFabricConfig: NvlinkTransport::registerLocalMemory()
// calls cuMemRetainAllocationHandle() on the region in fabric mode, and a
// glibc-allocated buffer fails that call, leaving the segment unreachable from
// other nodes over NVLink. Only a cuMemCreate allocation carrying an
// exportable fabric handle survives that path -- which for host DRAM means a
// HOST_NUMA allocation rather than a DEVICE one.
//
// Opt-in for the same reasons: fabric allocation needs driver-side support and
// an IMEX domain spanning the peers, and HOST_NUMA additionally needs a driver
// that exposes host-NUMA allocations at all. Where those are absent the
// allocation fails outright, so the default keeps the historical
// aligned_alloc behavior.
struct HostFabricConfig {
    bool enabled = false;

    static bool IsEnabledFromEnvironment();
    static HostFabricConfig FromEnvironment();
};

}  // namespace mooncake
