#include "host_fabric_config.h"

#include <glog/logging.h>

#include "environ.h"
#include "environment_variables.h"

namespace mooncake {

bool HostFabricConfig::IsEnabledFromEnvironment() {
    return FromEnvironment().enabled;
}

HostFabricConfig HostFabricConfig::FromEnvironment() {
    using Variables = VramFabricEnvironmentVariables;
    HostFabricConfig config;

    const auto raw = Environ::Read(Variables::MC_STORE_HOST_FABRIC);
    if (!raw.has_value()) {
        return config;
    }
    // Value-based rather than presence-based, matching MC_STORE_VRAM_FABRIC:
    // an unparseable or explicitly false value leaves the feature off, so
    // `MC_STORE_HOST_FABRIC=0` cannot silently opt a node in.
    const auto parsed = TryParseBool(*raw);
    if (!parsed.has_value()) {
        LOG(WARNING) << "Invalid MC_STORE_HOST_FABRIC='" << *raw
                     << "'. Expected a boolean. Fabric host memory stays "
                        "disabled.";
        return config;
    }

    config.enabled = *parsed;
    return config;
}

}  // namespace mooncake
