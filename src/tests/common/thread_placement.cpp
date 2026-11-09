// SPDX-License-Identifier: GPL-3.0-or-later

#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "common/core_pinning.h"
#include "common/thread.h"

TEST_CASE("CPU lists parse ranges and singles", "[common][thread]") {
    CHECK(Common::ParseCpuList("0-11\n") ==
          std::vector<u32>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11});
    CHECK(Common::ParseCpuList("12-19,24") ==
          std::vector<u32>{12, 13, 14, 15, 16, 17, 18, 19, 24});
    CHECK(Common::ParseCpuList("3") == std::vector<u32>{3});
    CHECK(Common::ParseCpuList("0,2-3,7") == std::vector<u32>{0, 2, 3, 7});
}

TEST_CASE("Malformed CPU lists parse as empty", "[common][thread]") {
    CHECK(Common::ParseCpuList("").empty());
    CHECK(Common::ParseCpuList("\n").empty());
    CHECK(Common::ParseCpuList("5-2").empty());
    CHECK(Common::ParseCpuList("0-").empty());
    CHECK(Common::ParseCpuList("0,").empty());
    CHECK(Common::ParseCpuList("a-b").empty());
    CHECK(Common::ParseCpuList("1;2").empty());
}
