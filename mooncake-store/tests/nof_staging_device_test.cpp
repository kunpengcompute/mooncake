#include "nof_staging_device.h"

#include <gtest/gtest.h>

#include <array>
#include <limits>

namespace mooncake::nof_staging {
namespace {

const void* Address(uintptr_t value) {
    return reinterpret_cast<const void*>(value);
}

TEST(NofStagingDeviceTest, StrictSwitchParsing) {
    ASSERT_EQ(ParseEnableFlag(nullptr), false);
    ASSERT_EQ(ParseEnableFlag("0"), false);
    ASSERT_EQ(ParseEnableFlag("1"), true);
    EXPECT_FALSE(ParseEnableFlag("").has_value());
    EXPECT_FALSE(ParseEnableFlag("true").has_value());
    EXPECT_FALSE(ParseEnableFlag("01").has_value());
    EXPECT_FALSE(ParseEnableFlag(" 1").has_value());
}

TEST(NofStagingDeviceTest, RegistryExactIdempotenceAndConflicts) {
    RegionRegistry registry;
    Device device{Backend::Cuda, 2};
    ASSERT_TRUE(registry.Insert(Address(4096), 1024, device));
    EXPECT_TRUE(registry.Insert(Address(4096), 1024, device));
    EXPECT_TRUE(registry.HasExact(Address(4096), 1024, device));
    EXPECT_EQ(registry.size(), 1u);
    EXPECT_FALSE(registry.Insert(Address(4096), 512, device));
    EXPECT_FALSE(registry.Insert(Address(4096), 1024, {Backend::Cuda, 3}));
    EXPECT_FALSE(registry.Insert(Address(4096), 1024, {Backend::Ascend, 2}));
    EXPECT_FALSE(registry.Insert(Address(4000), 128, device));
    EXPECT_FALSE(registry.Insert(Address(5000), 128, device));
    EXPECT_FALSE(registry.Insert(Address(3000), 3000, device));
    EXPECT_TRUE(registry.Insert(Address(5120), 256, device));
    EXPECT_TRUE(registry.Insert(Address(3072), 1024, device));
    EXPECT_EQ(registry.size(), 3u);
}

TEST(NofStagingDeviceTest, RegistryContainsWholeSliceInOneRegion) {
    RegionRegistry registry;
    Device device{Backend::Ascend, 1};
    ASSERT_TRUE(registry.Insert(Address(4096), 1024, device));
    ASSERT_TRUE(registry.Insert(Address(5120), 1024, device));
    ASSERT_TRUE(registry.Find(Address(4096), 1024));
    EXPECT_EQ(registry.Find(Address(4096), 1024)->id, 1);
    EXPECT_TRUE(registry.Find(Address(4097), 1023));
    EXPECT_TRUE(registry.Find(Address(5119), 1));
    EXPECT_FALSE(registry.Find(Address(5119), 2));
    EXPECT_FALSE(registry.Find(Address(4095), 2));
    EXPECT_FALSE(registry.Find(Address(4096), 1025));
    EXPECT_FALSE(registry.Find(Address(4096), 0));
    EXPECT_FALSE(registry.Erase(Address(4097)));
    EXPECT_TRUE(registry.Erase(Address(4096)));
    EXPECT_FALSE(registry.Find(Address(4096), 1));
    EXPECT_TRUE(registry.Find(Address(5120), 1));
    registry.Clear();
    EXPECT_EQ(registry.size(), 0u);
}

TEST(NofStagingDeviceTest, AscendIdentityIncludesApplicationContext) {
    RegionRegistry registry;
    Device original{Backend::Ascend, 1, reinterpret_cast<void*>(16), 7};
    ASSERT_TRUE(registry.Insert(Address(4096), 1024, original));
    EXPECT_TRUE(registry.Insert(Address(4096), 1024, original));
    Device other_context = original;
    other_context.context = reinterpret_cast<void*>(32);
    EXPECT_FALSE(registry.Insert(Address(4096), 1024, other_context));
    EXPECT_FALSE(registry.HasExact(Address(4096), 1024, other_context));
    Device other_location = original;
    other_location.memory_location_id = 8;
    EXPECT_FALSE(registry.Insert(Address(4096), 1024, other_location));
    ASSERT_TRUE(registry.Find(Address(4096), 1024));
    EXPECT_TRUE(*registry.Find(Address(4096), 1024) == original);
}

TEST(NofStagingDeviceTest, InvalidAndOverflowingRanges) {
    RegionRegistry registry;
    Device device{Backend::Cuda, 0};
    constexpr auto max = std::numeric_limits<uintptr_t>::max();
    EXPECT_FALSE(registry.Insert(nullptr, 1, device));
    EXPECT_FALSE(registry.Insert(Address(1024), 0, device));
    EXPECT_FALSE(registry.Insert(Address(max - 8), 9, device));
    EXPECT_FALSE(registry.Insert(Address(1024), 1, {}));
    EXPECT_FALSE(registry.Insert(Address(1024), 1, {Backend::Cuda, -1}));
    EXPECT_TRUE(registry.Insert(Address(max - 8), 8, device));
    EXPECT_TRUE(registry.Find(Address(max - 1), 1));
    EXPECT_FALSE(registry.Find(Address(max - 1), 2));
    EXPECT_FALSE(DetectDeviceRange(nullptr, 64));
    EXPECT_FALSE(DetectDeviceRange(Address(max - 8), 9));
}

TEST(NofStagingDeviceTest, HostCopiesWorkButUnsupportedDevicesFail) {
    constexpr size_t kCopySize = 64;
    std::array<unsigned char, kCopySize> source{};
    std::array<unsigned char, kCopySize> destination{};
    source.fill(42);
    CopySlice host{destination.data(), source.data(), source.size(),
                   std::nullopt};
    EXPECT_TRUE(Copy(&host, 1, true));
    EXPECT_EQ(destination, source);
    destination.fill(0);
    Device unsupported;
    EXPECT_FALSE(DetectDeviceRange(source.data(), source.size()));
    EXPECT_FALSE(CopyToHost(destination.data(), source.data(), source.size(),
                            unsupported));
    EXPECT_FALSE(CopyFromHost(destination.data(), source.data(), source.size(),
                              unsupported));
    EXPECT_EQ(destination[0], 0);
    EXPECT_FALSE(
        CopyToHost(nullptr, source.data(), source.size(), unsupported));
}

}  // namespace
}  // namespace mooncake::nof_staging
