#include "config/host_fabric_config.h"

#include <glog/logging.h>

#include "bool_parser.h"
#include "environ.h"
#include "environment_variables.h"

namespace mooncake {

bool HostFabricConfig::IsEnabledFromEnvironment() {
    return FromEnvironment().enabled;
}

HostFabricConfig HostFabricConfig::FromEnvironment() {
    HostFabricConfig config;
    using Variables = HostFabricEnvironmentVariables;

    const auto raw = Environ::Read(Variables::MC_STORE_HOST_FABRIC);
    if (!raw.has_value()) {
        return config;
    }
    const auto parsed = TryParseBool(*raw);
    if (!parsed.has_value()) {
        // Stay disabled. Opting in by accident selects the one failure mode
        // that is invisible until a remote read misses: a segment that mounted
        // fine but whose memory carries no retainable allocation handle.
        LOG(WARNING) << "Invalid MC_STORE_HOST_FABRIC='" << *raw
                     << "'. Expected a boolean. Fabric host memory stays "
                        "disabled.";
        return config;
    }
    config.enabled = *parsed;
    return config;
}

}  // namespace mooncake
