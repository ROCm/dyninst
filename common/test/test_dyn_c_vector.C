#include "concurrent.h"

#include <atomic>
#include <string>
#include <thread>

#include <gtest/gtest.h>

using Dyninst::dyn_c_vector;

namespace {
// embedded_segments (4) * segments of sizes 8,8,16,32 cover indices [0,64);
// the first index that forces the spill table to be allocated is 64.
constexpr std::size_t kSpillBoundary = 64;
}  // namespace

TEST(DynCVector, EmptyByDefault) {
    dyn_c_vector<int> v;
    EXPECT_TRUE(v.empty());
    EXPECT_EQ(v.size(), 0u);
}

TEST(DynCVector, SizingConstructorValueInitializes) {
    dyn_c_vector<int> v(10);
    EXPECT_EQ(v.size(), 10u);
    for(std::size_t i = 0; i < v.size(); ++i)
        EXPECT_EQ(v[i], 0);
}

TEST(DynCVector, PushBackAndIndexing) {
    dyn_c_vector<std::string> v;
    v.push_back("a");
    v.push_back("b");
    v.emplace_back("c");

    ASSERT_EQ(v.size(), 3u);
    EXPECT_EQ(v[0], "a");
    EXPECT_EQ(v[1], "b");
    EXPECT_EQ(v[2], "c");
    EXPECT_EQ(v.front(), "a");
    EXPECT_EQ(v.back(), "c");
}

TEST(DynCVector, AtThrowsOutOfRange) {
    dyn_c_vector<int> v;
    v.push_back(1);
    EXPECT_EQ(v.at(0), 1);
    EXPECT_THROW(v.at(1), std::out_of_range);
}

TEST(DynCVector, PopBackAndResize) {
    dyn_c_vector<int> v;
    for(int i = 0; i < 5; ++i)
        v.push_back(i);

    v.pop_back();
    EXPECT_EQ(v.size(), 4u);
    EXPECT_EQ(v.back(), 3);

    v.resize(2);
    EXPECT_EQ(v.size(), 2u);

    v.resize(4);
    EXPECT_EQ(v.size(), 4u);
    EXPECT_EQ(v[2], 0);  // grown elements are value-initialized
    EXPECT_EQ(v[3], 0);
}

TEST(DynCVector, ClearEmptiesAndAllowsReuse) {
    dyn_c_vector<int> v;
    v.push_back(1);
    v.clear();
    EXPECT_TRUE(v.empty());
    v.push_back(2);
    EXPECT_EQ(v[0], 2);
}

TEST(DynCVector, IterationAcrossSegmentBoundary) {
    dyn_c_vector<int> v;
    constexpr int kCount = static_cast<int>(kSpillBoundary) + 20;
    for(int i = 0; i < kCount; ++i)
        v.push_back(i);

    ASSERT_EQ(v.size(), static_cast<std::size_t>(kCount));
    int expected = 0;
    for(int x : v) {
        EXPECT_EQ(x, expected);
        ++expected;
    }
    EXPECT_EQ(expected, kCount);

    // Spot-check elements straddling the embedded/spill boundary directly.
    EXPECT_EQ(v[kSpillBoundary - 1], static_cast<int>(kSpillBoundary) - 1);
    EXPECT_EQ(v[kSpillBoundary], static_cast<int>(kSpillBoundary));
}

TEST(DynCVector, CopyConstructIsIndependent) {
    dyn_c_vector<int> v;
    v.push_back(1);
    v.push_back(2);

    dyn_c_vector<int> copy(v);
    copy.push_back(3);

    EXPECT_EQ(v.size(), 2u);
    EXPECT_EQ(copy.size(), 3u);
    EXPECT_EQ(copy[2], 3);
}

TEST(DynCVector, MoveConstructTransfersElements) {
    dyn_c_vector<int> v;
    v.push_back(1);
    v.push_back(2);

    dyn_c_vector<int> moved(std::move(v));
    EXPECT_EQ(moved.size(), 2u);
    EXPECT_EQ(moved[0], 1);
    EXPECT_EQ(moved[1], 2);
    EXPECT_TRUE(v.empty());
}

TEST(DynCVector, CopyAssignReplacesContents) {
    dyn_c_vector<int> a, b;
    a.push_back(1);
    b.push_back(2);
    b.push_back(3);

    b = a;
    EXPECT_EQ(b.size(), 1u);
    EXPECT_EQ(b[0], 1);
    EXPECT_EQ(a.size(), 1u);  // source unaffected
}

TEST(DynCVector, MoveAssignTransfersContents) {
    dyn_c_vector<int> a, b;
    a.push_back(1);
    b.push_back(2);
    b.push_back(3);

    b = std::move(a);
    EXPECT_EQ(b.size(), 1u);
    EXPECT_EQ(b[0], 1);
}

TEST(DynCVector, ConcurrentAppendWhileReading) {
    // Exercises the acquire/release publication chain: a reader observing a
    // given size() must see a fully constructed element at every index below
    // it, never a torn/partial read.
    dyn_c_vector<int> v;
    constexpr int kTotal = 5000;
    std::atomic<bool> stop{false};

    std::thread writer([&] {
        for(int i = 0; i < kTotal; ++i)
            v.push_back(i);
        stop.store(true);
    });

    std::thread reader([&] {
        while(!stop.load()) {
            std::size_t n = v.size();
            for(std::size_t i = 0; i < n; ++i)
                EXPECT_EQ(v[i], static_cast<int>(i));
        }
    });

    writer.join();
    reader.join();

    ASSERT_EQ(v.size(), static_cast<std::size_t>(kTotal));
    for(int i = 0; i < kTotal; ++i)
        EXPECT_EQ(v[i], i);
}
