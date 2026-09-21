#include <gflags/gflags.h>
#include <glog/logging.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <optional>
#include <string>

#include "../src/config/vram_fabric_config.h"

namespace mooncake {
namespace {

class ScopedEnvVar {
   public:
    explicit ScopedEnvVar(const char* name) : name_(name) {
        if (const char* value = std::getenv(name)) {
            original_ = value;
        }
        EXPECT_EQ(unsetenv(name), 0);
    }

    ~ScopedEnvVar() {
        if (original_.has_value()) {
            EXPECT_EQ(setenv(name_.c_str(), original_->c_str(), 1), 0);
        } else {
            EXPECT_EQ(unsetenv(name_.c_str()), 0);
        }
    }

    ScopedEnvVar(const ScopedEnvVar&) = delete;
    ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

    void Set(const char* value) {
        ASSERT_EQ(setenv(name_.c_str(), value, 1), 0);
    }

    void Unset() { ASSERT_EQ(unsetenv(name_.c_str()), 0); }

   private:
    std::string name_;
    std::optional<std::string> original_;
};

class VramFabricConfigTest : public ::testing::Test {
   protected:
    void SetUp() override {
        FLAGS_logtostderr = 1;
        FLAGS_minloglevel = google::WARNING;
    }

    ScopedEnvVar vram_fabric{"MC_STORE_VRAM_FABRIC"};
};

TEST_F(VramFabricConfigTest, UnsetKeepsFabricDisabled) {
    const VramFabricConfig config = VramFabricConfig::FromEnvironment();

    EXPECT_FALSE(config.enabled);
    EXPECT_FALSE(VramFabricConfig::IsEnabledFromEnvironment());
}

TEST_F(VramFabricConfigTest, TruthyValuesEnableFabric) {
    for (const char* value :
         {"1", "true", "TRUE", "True", "yes", "on", "enable", " 1 "}) {
        SCOPED_TRACE(value);
        vram_fabric.Set(value);

        EXPECT_TRUE(VramFabricConfig::FromEnvironment().enabled);
    }
}

TEST_F(VramFabricConfigTest, FalsyValuesKeepFabricDisabled) {
    for (const char* value :
         {"0", "false", "FALSE", "no", "off", "disable"}) {
        SCOPED_TRACE(value);
        vram_fabric.Set(value);

        EXPECT_FALSE(VramFabricConfig::FromEnvironment().enabled);
    }
}

// Enablement is value-based rather than presence-based, unlike
// MC_STORE_USE_HUGEPAGE. Setting the variable to a false value must not turn
// the feature on: the failure mode it selects on unsupported hardware is a
// segment that registers nothing, which stays invisible until a remote read
// misses.
TEST_F(VramFabricConfigTest, ExplicitFalseIsNotTreatedAsOptIn) {
    vram_fabric.Set("0");

    EXPECT_FALSE(VramFabricConfig::IsEnabledFromEnvironment());
}

TEST_F(VramFabricConfigTest, InvalidValueWarnsAndKeepsFabricDisabled) {
    vram_fabric.Set("maybe");
    ::testing::internal::CaptureStderr();

    const VramFabricConfig config = VramFabricConfig::FromEnvironment();

    EXPECT_FALSE(config.enabled);
    const std::string logs = ::testing::internal::GetCapturedStderr();
    EXPECT_NE(logs.find("Invalid MC_STORE_VRAM_FABRIC"), std::string::npos);
}

TEST_F(VramFabricConfigTest, EmptyValueKeepsFabricDisabled) {
    vram_fabric.Set("");

    EXPECT_FALSE(VramFabricConfig::FromEnvironment().enabled);
}

}  // namespace
}  // namespace mooncake
