#include <gtest/gtest.h>

#include "vrm/cgroup.hpp"

TEST(CgroupHelpers, CpuMax) {
    EXPECT_EQ(vrm::format_cpu_max(0.5), "50000 100000");
    EXPECT_EQ(vrm::format_cpu_max(2.0), "200000 100000");
    EXPECT_EQ(vrm::format_cpu_max(0.25, 20000), "5000 20000");
    EXPECT_EQ(vrm::format_cpu_max(0.001), "1000 100000");  // Kernel minimum quota.
}

TEST(CgroupHelpers, CpuListFormatting) {
    EXPECT_EQ(vrm::format_cpu_list({0}), "0");
    EXPECT_EQ(vrm::format_cpu_list({2, 0, 1, 5}), "0-2,5");
    EXPECT_EQ(vrm::format_cpu_list({3, 3, 4}), "3-4");
    EXPECT_EQ(vrm::format_cpu_list({}), "");
}

TEST(CgroupHelpers, CpuListParsing) {
    EXPECT_EQ(vrm::parse_cpu_list("0-3\n"), (std::vector<int>{0, 1, 2, 3}));
    EXPECT_EQ(vrm::parse_cpu_list("0,2-3,7"), (std::vector<int>{0, 2, 3, 7}));
    EXPECT_TRUE(vrm::parse_cpu_list("").empty());
}

TEST(CgroupHelpers, FlatKeyedFiles) {
    const auto stat = vrm::parse_flat_keyed("usage_usec 1500\nuser_usec 1000\nnr_throttled 3\n");
    EXPECT_EQ(stat.at("usage_usec"), 1500u);
    EXPECT_EQ(stat.at("nr_throttled"), 3u);
    EXPECT_EQ(stat.count("missing"), 0u);
}

TEST(CgroupHelpers, FormatBytes) {
    EXPECT_EQ(vrm::format_bytes(512), "512B");
    EXPECT_EQ(vrm::format_bytes(64ull * 1024 * 1024), "64.0Mi");
    EXPECT_EQ(vrm::format_bytes(1536ull * 1024 * 1024), "1.5Gi");
}
