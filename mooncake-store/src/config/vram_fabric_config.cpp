#include "vram_fabric_config.h"

#include <glog/logging.h>

#include "environ.h"
#include "environment_variables.h"

namespace mooncake {

bool VramFabricConfig::IsEnabledFromEnvironment() {
    return FromEnvironment().enabled;
}

VramFabricConfig VramFabricConfig::FromEnvironment() {
    using Variables = VramFabricEnvironmentVariables;
    VramFabricConfig config;

    const auto raw = Environ::Read(Variables::MC_STORE_VRAM_FABRIC);
    if (!raw.has_value()) {
        return config;
    }
    // Unlike MC_STORE_USE_HUGEPAGE, enablement is value-based rather than
    // presence-based: an unparseable or explicitly false value leaves the
    // feature off. A silent opt-in from `MC_STORE_VRAM_FABRIC=0` would be
    // especially bad here, because the failure mode it selects (a segment that
    // registers nothing) is invisible until a remote read misses.
    const auto parsed = TryParseBool(*raw);
    if (!parsed.has_value()) {
        LOG(WARNING) << "Invalid MC_STORE_VRAM_FABRIC='" << *raw
                     << "'. Expected a boolean. Fabric VRAM stays disabled.";
        return config;
    }

    config.enabled = *parsed;
    return config;
}

}  // namespace mooncake
