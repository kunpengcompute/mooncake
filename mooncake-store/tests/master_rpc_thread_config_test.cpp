#include <gtest/gtest.h>

#include "../src/config/master_rpc_thread_config.h"

namespace mooncake::test {

TEST(MasterRpcThreadConfigTest, OmittedOrZeroFileSettingUsesDefaultThreads) {
    EXPECT_EQ(ResolveMasterRpcThreadCount(0, 0, 16, 64), 16u);
    EXPECT_EQ(ResolveMasterRpcThreadCount(0, 0, 16, 8), 8u);
}

TEST(MasterRpcThreadConfigTest, RetainsExplicitFileSetting) {
    EXPECT_EQ(ResolveMasterRpcThreadCount(6, 0, 16, 64), 6u);
}

TEST(MasterRpcThreadConfigTest, CommandLineOverridesFileSetting) {
    EXPECT_EQ(ResolveMasterRpcThreadCount(6, 12, 16, 64), 12u);
    EXPECT_EQ(ResolveMasterRpcThreadCount(0, 12, 16, 64), 12u);
}

TEST(MasterRpcThreadConfigTest, HonorsLegacyMaxThreads) {
    EXPECT_EQ(ResolveMasterRpcThreadCount(0, 0, 4, 64), 4u);
}

TEST(MasterRpcThreadConfigTest, DefaultAlwaysProvidesAWorker) {
    EXPECT_EQ(ResolveMasterRpcThreadCount(0, 0, 16, 0), 1u);
    EXPECT_EQ(ResolveMasterRpcThreadCount(0, 0, 0, 64), 1u);
}

}  // namespace mooncake::test
