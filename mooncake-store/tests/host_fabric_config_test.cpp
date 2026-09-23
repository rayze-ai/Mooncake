#include <gtest/gtest.h>

#include <cstdlib>
#include <optional>
#include <string>

#include "config/host_fabric_config.h"

namespace mooncake {
namespace {

class ScopedEnvVar {
   public:
    explicit ScopedEnvVar(const char* name) : name_(name) {
        if (const char* value = std::getenv(name)) {
            original_ = value;
        }
        unsetenv(name);
    }

    ~ScopedEnvVar() {
        if (original_.has_value()) {
            setenv(name_.c_str(), original_->c_str(), 1);
        } else {
            unsetenv(name_.c_str());
        }
    }

    ScopedEnvVar(const ScopedEnvVar&) = delete;
    ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

    void Set(const char* value) { setenv(name_.c_str(), value, 1); }

   private:
    std::string name_;
    std::optional<std::string> original_;
};

TEST(HostFabricConfigTest, DisabledWhenUnset) {
    ScopedEnvVar env("MC_STORE_HOST_FABRIC");
    EXPECT_FALSE(HostFabricConfig::IsEnabledFromEnvironment());
    EXPECT_FALSE(HostFabricConfig::FromEnvironment().enabled);
}

TEST(HostFabricConfigTest, EnabledByOne) {
    ScopedEnvVar env("MC_STORE_HOST_FABRIC");
    env.Set("1");
    EXPECT_TRUE(HostFabricConfig::IsEnabledFromEnvironment());
}

TEST(HostFabricConfigTest, EnabledByTrueCaseInsensitive) {
    ScopedEnvVar env("MC_STORE_HOST_FABRIC");
    env.Set("TRUE");
    EXPECT_TRUE(HostFabricConfig::IsEnabledFromEnvironment());
}

// The switch is value-based, unlike MC_STORE_USE_HUGEPAGE. A deployment that
// writes "=0" to turn it off must actually get "off".
TEST(HostFabricConfigTest, ZeroIsDisabled) {
    ScopedEnvVar env("MC_STORE_HOST_FABRIC");
    env.Set("0");
    EXPECT_FALSE(HostFabricConfig::IsEnabledFromEnvironment());
}

TEST(HostFabricConfigTest, FalseIsDisabled) {
    ScopedEnvVar env("MC_STORE_HOST_FABRIC");
    env.Set("false");
    EXPECT_FALSE(HostFabricConfig::IsEnabledFromEnvironment());
}

// An unparsable value must not silently opt in: the failure mode it would
// select (a segment that mounts but is unreachable) is invisible until the
// first remote read.
TEST(HostFabricConfigTest, InvalidValueFallsBackDisabled) {
    ScopedEnvVar env("MC_STORE_HOST_FABRIC");
    env.Set("maybe");
    EXPECT_FALSE(HostFabricConfig::IsEnabledFromEnvironment());
}

}  // namespace
}  // namespace mooncake
