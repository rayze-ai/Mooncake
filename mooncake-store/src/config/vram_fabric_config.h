#pragma once

namespace mooncake {

// Controls whether a VRAM segment is allocated with cuMemCreate +
// CU_MEM_HANDLE_TYPE_FABRIC instead of cudaMalloc.
//
// This matters because NvlinkTransport::registerLocalMemory() calls
// cuMemRetainAllocationHandle() on the region in fabric mode. A cudaMalloc'd
// buffer fails that call, and the transport then logs a warning and returns 0
// -- registration "succeeds" while registering nothing, leaving the segment
// unreachable from other nodes over NVLink. Only a cuMemCreate allocation
// with an exportable fabric handle survives that path.
//
// Opt-in because fabric allocation requires driver-side support (all devices
// must report CU_DEVICE_ATTRIBUTE_HANDLE_TYPE_FABRIC_SUPPORTED) and an IMEX
// domain spanning the peers that should reach the segment. Where those are
// absent the allocation fails outright, so the default keeps the historical
// cudaMalloc behavior.
struct VramFabricConfig {
    bool enabled = false;

    static bool IsEnabledFromEnvironment();
    static VramFabricConfig FromEnvironment();
};

}  // namespace mooncake
