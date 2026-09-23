#pragma once

namespace mooncake {

// MC_STORE_HOST_FABRIC=1 makes the Store allocate its host (DRAM) segment
// through CUDA VMM and export a fabric handle, so a remote GPU in the same
// NVLink domain can read it directly (EGM). Requires USE_CUDA and an nvlink
// transport running in fabric mode; see use_fabric_host() in
// client_buffer_allocation.cpp for the full gate.
struct HostFabricConfig {
    bool enabled = false;

    static bool IsEnabledFromEnvironment();
    static HostFabricConfig FromEnvironment();
};

}  // namespace mooncake
