#include <gtest/gtest.h>

#include <cstdlib>

#include "common/client_buffer_allocation.h"

namespace mooncake {
namespace {

// The flag is process-global, so every test restores it to false on exit.
class NvlinkFabricReadyTest : public ::testing::Test {
   protected:
    void TearDown() override { set_nvlink_fabric_ready(false); }
};

TEST_F(NvlinkFabricReadyTest, DefaultsFalse) {
    EXPECT_FALSE(nvlink_fabric_ready());
}

TEST_F(NvlinkFabricReadyTest, RoundTrips) {
    set_nvlink_fabric_ready(true);
    EXPECT_TRUE(nvlink_fabric_ready());
    set_nvlink_fabric_ready(false);
    EXPECT_FALSE(nvlink_fabric_ready());
}

// setup() may run more than once in a process (retries, tests). A stale true
// from an earlier nvlink-capable setup must not send a later, non-nvlink node
// down the fabric allocator.
TEST_F(NvlinkFabricReadyTest, LastWriteWins) {
    set_nvlink_fabric_ready(true);
    set_nvlink_fabric_ready(false);
    EXPECT_FALSE(nvlink_fabric_ready());
}

// With the flag set but the env switch off, allocation must still take the
// plain path and round-trip through free_memory() without touching the VMM
// allocator. This is the configuration every non-fabric deployment runs.
TEST_F(NvlinkFabricReadyTest, AllocationWorksWithFlagSetAndSwitchOff) {
    unsetenv("MC_STORE_HOST_FABRIC");
    set_nvlink_fabric_ready(true);
    constexpr size_t kSize = 16 * 1024 * 1024;
    void* ptr = allocate_buffer_allocator_memory(kSize, "rdma");
    ASSERT_NE(ptr, nullptr);
    free_memory("rdma", ptr);
}

}  // namespace
}  // namespace mooncake
