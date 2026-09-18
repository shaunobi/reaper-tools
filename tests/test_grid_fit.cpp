#include "srt/grid_fit.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using Catch::Matchers::WithinAbs;

TEST_CASE("planGridFit moves the item so bar 1 lands on its bar line and stretches to the last bar") {
    srt::GridFitInput in;
    in.srcStart = 1.0; // item shows source from 1.0 s
    in.srcVisibleLength = 30.0; // to 31.0 s
    in.playrate = 1.0;
    in.firstDownbeatSrc = 1.5; // 0.5 s after the item start
    in.lastDownbeatSrc = 21.5; // 20 s later in the source
    in.barCount = 10;
    in.firstBarTime = 4.0; // bar 1 at 4.0 s in the project (120 BPM, 2 s bars)
    in.lastBarTime = 24.0; // bar 11 at 24.0 s
    srt::GridFitPlan p = srt::planGridFit(in);
    REQUIRE_THAT(p.itemPosition, WithinAbs(3.5, 1e-9));
    REQUIRE_THAT(p.first.itemPos, WithinAbs(0.5, 1e-9));
    REQUIRE_THAT(p.first.srcPos, WithinAbs(1.5, 1e-9));
    REQUIRE_THAT(p.last.itemPos, WithinAbs(20.5, 1e-9));
    REQUIRE_THAT(p.last.srcPos, WithinAbs(21.5, 1e-9));
    REQUIRE_THAT(p.stretchRatio, WithinAbs(1.0, 1e-9));
    REQUIRE_THAT(p.itemLength, WithinAbs(30.0, 1e-9));
}

TEST_CASE("planGridFit stretches a 121 BPM recording onto a 120 BPM grid and keeps the tail visible") {
    srt::GridFitInput in;
    in.srcStart = 0.0;
    in.srcVisibleLength = 10.0;
    in.firstDownbeatSrc = 0.35;
    const double barAt121 = 4.0 * 60.0 / 121.0;
    in.lastDownbeatSrc = 0.35 + 4 * barAt121; // 5 downbeats, 4 bars
    in.barCount = 4;
    in.firstBarTime = 10.0;
    in.lastBarTime = 10.0 + 4 * 2.0; // 120 BPM bars are 2 s
    srt::GridFitPlan p = srt::planGridFit(in);
    REQUIRE_THAT(p.itemPosition, WithinAbs(9.65, 1e-9));
    REQUIRE_THAT(p.stretchRatio, WithinAbs(121.0 / 120.0, 1e-9));
    REQUIRE_THAT(p.last.itemPos, WithinAbs(8.35, 1e-9));
    const double tailSrc = 10.0 - in.lastDownbeatSrc;
    REQUIRE_THAT(p.itemLength, WithinAbs(8.35 + tailSrc * 121.0 / 120.0, 1e-9));
}

TEST_CASE("planGridFit honours the take playrate") {
    srt::GridFitInput in;
    in.srcStart = 2.0;
    in.srcVisibleLength = 20.0;
    in.playrate = 2.0; // item plays at double speed: 1 item second = 2 source seconds
    in.firstDownbeatSrc = 4.0; // 1 item second in
    in.lastDownbeatSrc = 20.0; // 9 item seconds in
    in.barCount = 4;
    in.firstBarTime = 100.0;
    in.lastBarTime = 108.0;
    srt::GridFitPlan p = srt::planGridFit(in);
    REQUIRE_THAT(p.itemPosition, WithinAbs(99.0, 1e-9));
    REQUIRE_THAT(p.first.itemPos, WithinAbs(1.0, 1e-9));
    REQUIRE_THAT(p.last.itemPos, WithinAbs(9.0, 1e-9));
    REQUIRE_THAT(p.stretchRatio, WithinAbs(1.0, 1e-9));
}

TEST_CASE("planGridFit rejects impossible input") {
    srt::GridFitInput in;
    in.srcVisibleLength = 10.0;
    in.firstDownbeatSrc = 5.0;
    in.lastDownbeatSrc = 4.0;
    in.barCount = 2;
    in.firstBarTime = 0.0;
    in.lastBarTime = 4.0;
    REQUIRE_THROWS(srt::planGridFit(in));
    in.lastDownbeatSrc = 9.0;
    in.barCount = 0;
    REQUIRE_THROWS(srt::planGridFit(in));
}

TEST_CASE("barCountFromTempo rounds to whole bars") {
    REQUIRE(srt::barCountFromTempo(0.35, 0.35 + 64 * 4 * 60.0 / 121.3, 121.3) == 64);
    REQUIRE(srt::barCountFromTempo(0.0, 7.9, 120.0) == 4); // 3.95 bars -> 4
    REQUIRE(srt::barCountFromTempo(0.0, 6.9, 120.0) == 3); // 3.45 bars -> 3
}

TEST_CASE("planConform pairs interior downbeats with their bar lines") {
    auto plan = srt::planConform(10.0, {1.0, 2.9, 5.1}, {12.0, 14.0, 16.0});
    REQUIRE(plan.size() == 3);
    REQUIRE_THAT(plan[0].itemPos, WithinAbs(2.0, 1e-9));
    REQUIRE_THAT(plan[0].srcPos, WithinAbs(1.0, 1e-9));
    REQUIRE_THAT(plan[2].itemPos, WithinAbs(6.0, 1e-9));
    REQUIRE_THROWS(srt::planConform(0.0, {1.0}, {}));
}

TEST_CASE("assignNearestBars skips bars with no candidate and never goes backwards") {
    std::vector<double> bars{0.0, 2.0, 4.0, 6.0, 8.0};
    // Downbeats: one near bar 0, none near bar 1, one near bar 2, two fighting over bar 3, one far off.
    std::vector<double> downbeats{0.1, 3.9, 5.8, 6.3, 9.5};
    auto a = srt::assignNearestBars(downbeats, bars);
    REQUIRE(a.size() == 3);
    REQUIRE(a[0] == std::make_pair(0, 0));
    REQUIRE(a[1] == std::make_pair(1, 2));
    REQUIRE(a[2] == std::make_pair(2, 3));
}
