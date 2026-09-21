#include <gtest/gtest.h>

#include <cstdlib>

#include "../src/config/host_fabric_config.h"

namespace mooncake {

class HostFabricConfigTest : public ::testing::Test {
protected:
    void SetUp() override { unsetenv("MC_STORE_HOST_FABRIC"); }
    void TearDown() override { unsetenv("MC_STORE_HOST_FABRIC"); }
};

TEST_F(HostFabricConfigTest, DefaultDisabled) {
    const auto cfg = HostFabricConfig::FromEnvironment();
    EXPECT_FALSE(cfg.enabled);
    EXPECT_FALSE(HostFabricConfig::IsEnabledFromEnvironment());
}

TEST_F(HostFabricConfigTest, Enabled1) {
    setenv("MC_STORE_HOST_FABRIC", "1", 1);
    const auto cfg = HostFabricConfig::FromEnvironment();
    EXPECT_TRUE(cfg.enabled);
    EXPECT_TRUE(HostFabricConfig::IsEnabledFromEnvironment());
}

TEST_F(HostFabricConfigTest, EnabledTrue) {
    setenv("MC_STORE_HOST_FABRIC", "true", 1);
    const auto cfg = HostFabricConfig::FromEnvironment();
    EXPECT_TRUE(cfg.enabled);
}

TEST_F(HostFabricConfigTest, Disabled0) {
    setenv("MC_STORE_HOST_FABRIC", "0", 1);
    const auto cfg = HostFabricConfig::FromEnvironment();
    EXPECT_FALSE(cfg.enabled);
    EXPECT_FALSE(HostFabricConfig::IsEnabledFromEnvironment());
}

TEST_F(HostFabricConfigTest, DisabledFalse) {
    setenv("MC_STORE_HOST_FABRIC", "false", 1);
    const auto cfg = HostFabricConfig::FromEnvironment();
    EXPECT_FALSE(cfg.enabled);
}

TEST_F(HostFabricConfigTest, InvalidValueFallsBackDisabled) {
    setenv("MC_STORE_HOST_FABRIC", "maybe", 1);
    const auto cfg = HostFabricConfig::FromEnvironment();
    EXPECT_FALSE(cfg.enabled);
}

}  // namespace mooncake
