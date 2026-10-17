// Tests des places dans les pages de géométrie partagées (RangeAllocator) : réservées sans chevauchement, rendues et
// réunies à leurs voisines.
#include "Check.hpp"

#include "Renderer/RangeAllocator.hpp"

using namespace Flux;

namespace {
    bool TestAllocate() {
        RangeAllocator ranges(100);
        CHECK(ranges.Allocate(40) == 0u);
        CHECK(ranges.Allocate(50) == 40u);
        CHECK(!ranges.Allocate(20).has_value()); // 10 restent
        CHECK(ranges.Allocate(10) == 90u);
        CHECK(ranges.GetLargestFree() == 0);
        CHECK(ranges.Allocate(0) == 0u); // rien à réserver
        return true;
    }

    bool TestFreeMerges() {
        RangeAllocator ranges(100);
        const auto a = ranges.Allocate(30);
        const auto b = ranges.Allocate(30);
        const auto c = ranges.Allocate(30);
        CHECK(a && b && c);
        ranges.Free(*a, 30);
        ranges.Free(*c, 30); // réunie aux 10 de la fin
        CHECK(ranges.GetLargestFree() == 40);
        CHECK(!ranges.Allocate(50).has_value());
        ranges.Free(*b, 30); // réunit tout
        CHECK(ranges.GetLargestFree() == 100);
        CHECK(ranges.Allocate(100) == 0u);
        return true;
    }

    bool TestFirstFit() {
        RangeAllocator ranges(100);
        const auto a = ranges.Allocate(20);
        ranges.Allocate(20);
        CHECK(a);
        ranges.Free(*a, 20);
        CHECK(ranges.Allocate(10) == 0u);  // dans le trou du début
        CHECK(ranges.Allocate(15) == 40u); // le trou restant (10) est trop petit
        CHECK(ranges.Allocate(10) == 10u);
        return true;
    }
}

bool RunGeometryTests() {
    return TestAllocate() && TestFreeMerges() && TestFirstFit();
}
