#include "../src/nof/namespace_refresh.h"

#include <gtest/gtest.h>

#include <cerrno>
#include <chrono>

namespace mooncake::test {
namespace {

using namespace std::chrono_literals;

TEST(NofNamespaceRefreshTest, ActiveNamespaceNeedsNoAdminPoll) {
    int polls = 0;
    const auto result = RefreshNofNamespace(
        [] { return true; }, [&] { return ++polls; }, 1000ms, 10ms);
    EXPECT_TRUE(result.active);
    EXPECT_EQ(polls, 0);
    EXPECT_EQ(result.poll_count, 0u);
}

TEST(NofNamespaceRefreshTest, ZeroTimeoutStillRefreshesAnInactiveNamespace) {
    bool active = false;
    const auto result = RefreshNofNamespace([&] { return active; },
                                            [&] {
                                                active = true;
                                                return 1;
                                            },
                                            0ms, 10ms);
    EXPECT_TRUE(result.active);
    EXPECT_EQ(result.poll_count, 1u);
    EXPECT_EQ(result.admin_error, 0);
}

TEST(NofNamespaceRefreshTest, RechecksNamespaceAfterMultipleAdminPolls) {
    int polls = 0;
    const auto result = RefreshNofNamespace(
        [&] { return polls == 3; }, [&] { return ++polls; }, 1000ms, 0ms);
    EXPECT_TRUE(result.active);
    EXPECT_EQ(result.poll_count, 3u);
    EXPECT_EQ(result.admin_error, 0);
}

TEST(NofNamespaceRefreshTest, InactiveNamespaceTimesOutWithoutControllerError) {
    const auto result =
        RefreshNofNamespace([] { return false; }, [] { return 0; }, 0ms, 10ms);
    EXPECT_FALSE(result.active);
    EXPECT_EQ(result.admin_error, 0);
    EXPECT_EQ(result.poll_count, 1u);
}

TEST(NofNamespaceRefreshTest, AdminFailureStopsRefreshAndPreservesError) {
    const auto result = RefreshNofNamespace(
        [] { return false; }, [] { return -ENXIO; }, 1000ms, 10ms);
    EXPECT_FALSE(result.active);
    EXPECT_EQ(result.admin_error, -ENXIO);
    EXPECT_EQ(result.poll_count, 1u);
}

}  // namespace
}  // namespace mooncake::test
