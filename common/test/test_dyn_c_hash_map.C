#include "concurrent.h"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using Dyninst::dyn_c_hash_map;

namespace {
using map_type = dyn_c_hash_map<int, std::string>;
}

TEST(DynCHashMap, InsertFindContains) {
    map_type m;
    EXPECT_TRUE(m.insert(map_type::value_type(1, "one")));
    EXPECT_FALSE(m.insert(map_type::value_type(1, "again")));  // already present

    map_type::const_accessor ca;
    EXPECT_TRUE(m.find(ca, 1));
    EXPECT_EQ(ca->second, "one");
    ca.release();

    EXPECT_FALSE(m.find(ca, 2));
    EXPECT_TRUE(m.contains(1));
    EXPECT_FALSE(m.contains(2));
}

TEST(DynCHashMap, AccessorInsertMutatesInPlace) {
    map_type m;
    map_type::accessor a;
    EXPECT_TRUE(m.insert(a, 1));
    a->second = "one";
    a.release();

    map_type::const_accessor ca;
    ASSERT_TRUE(m.find(ca, 1));
    EXPECT_EQ(ca->second, "one");
}

TEST(DynCHashMap, EraseByKeyAndByAccessor) {
    map_type m;
    m.insert(map_type::value_type(1, "one"));
    m.insert(map_type::value_type(2, "two"));

    EXPECT_TRUE(m.erase(1));
    EXPECT_FALSE(m.contains(1));
    EXPECT_FALSE(m.erase(1));  // already gone

    map_type::accessor a;
    ASSERT_TRUE(m.find(a, 2));
    EXPECT_TRUE(m.erase(a));
    EXPECT_FALSE(m.contains(2));
}

TEST(DynCHashMap, SizeClearRehash) {
    map_type m;
    m.rehash(128);
    for(int i = 0; i < 100; ++i)
        m.insert(map_type::value_type(i, std::to_string(i)));
    EXPECT_EQ(m.size(), 100);

    m.clear();
    EXPECT_EQ(m.size(), 0);
    EXPECT_FALSE(m.contains(0));
}

TEST(DynCHashMap, IterateVisitsEveryElement) {
    map_type m;
    constexpr int kCount = 50;
    for(int i = 0; i < kCount; ++i)
        m.insert(map_type::value_type(i, std::to_string(i)));

    std::vector<bool> seen(kCount, false);
    int visited = 0;
    for(auto& kv : m) {
        ASSERT_GE(kv.first, 0);
        ASSERT_LT(kv.first, kCount);
        seen[kv.first] = true;
        ++visited;
    }
    EXPECT_EQ(visited, kCount);
    for(bool s : seen)
        EXPECT_TRUE(s);
}

TEST(DynCHashMap, CopyConstructIsIndependent) {
    map_type m;
    m.insert(map_type::value_type(1, "one"));

    map_type copy(m);
    EXPECT_TRUE(copy.contains(1));

    copy.insert(map_type::value_type(2, "two"));
    EXPECT_FALSE(m.contains(2));  // original unaffected by mutation of the copy
}

TEST(DynCHashMap, MoveConstructLeavesSourceUsable) {
    map_type m;
    m.insert(map_type::value_type(1, "one"));

    map_type moved(std::move(m));
    EXPECT_TRUE(moved.contains(1));

    // The moved-from map must remain a valid, usable object: insertable,
    // queryable, and safely destructible.
    EXPECT_FALSE(m.contains(1));
    EXPECT_TRUE(m.insert(map_type::value_type(2, "two")));
    EXPECT_TRUE(m.contains(2));
}

TEST(DynCHashMap, CopyAssignReplacesContents) {
    map_type a, b;
    a.insert(map_type::value_type(1, "one"));
    b.insert(map_type::value_type(2, "two"));

    b = a;
    EXPECT_TRUE(b.contains(1));
    EXPECT_FALSE(b.contains(2));
    EXPECT_TRUE(a.contains(1));  // source unaffected
}

TEST(DynCHashMap, MoveAssignTransfersContents) {
    map_type a, b;
    a.insert(map_type::value_type(1, "one"));
    b.insert(map_type::value_type(2, "two"));

    b = std::move(a);
    EXPECT_TRUE(b.contains(1));
    EXPECT_FALSE(b.contains(2));
}

TEST(DynCHashMap, ConcurrentInsertFindEraseStress) {
    // Run under Helgrind/DRD locally to check the element-lock annotations
    // (dyn_c_annotations) match the actual acquire/release pattern; no
    // sanitizer job exists in CI yet to do this automatically.
    map_type m;
    constexpr int kThreads = 8;
    constexpr int kKeys = 256;
    std::atomic<bool> start{false};

    std::vector<std::thread> threads;
    for(int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            while(!start.load())
                std::this_thread::yield();
            for(int round = 0; round < 200; ++round) {
                int key = (t * 37 + round) % kKeys;
                m.insert(map_type::value_type(key, std::to_string(key)));
                map_type::const_accessor ca;
                if(m.find(ca, key))
                    EXPECT_EQ(ca->second, std::to_string(key));
                ca.release();
                if(round % 5 == 0)
                    m.erase(key);
            }
        });
    }
    start.store(true);
    for(auto& th : threads)
        th.join();

    // No crash, no deadlock, and the map is left in a consistent state.
    EXPECT_LE(m.size(), kKeys);
}

// Regression test for a genuine circular-wait deadlock: two threads each hold
// an accessor on one key and then try to acquire an accessor on the *other*
// thread's key (the lock-order-crossing pattern the bounded try-lock-and-
// restart design exists to avoid). Unlike ordinary contention, this pattern
// cannot resolve itself -- each side holds a lock the other needs for as long
// as its find() call is in flight, so no amount of restarting frees it. The
// map's contract is therefore to fail fast (throw) rather than spin forever;
// this test asserts every crossing find() completes, one way or another,
// well within the CTest TIMEOUT rather than hanging CI silently.
TEST(DynCHashMap, NoDeadlockOnCrossingAccessors) {
    map_type m;
    m.insert(map_type::value_type(1, "one"));
    m.insert(map_type::value_type(2, "two"));

    std::atomic<bool> start{false};
    std::atomic<int> caught{0};
    // A pair of 2-thread barriers forces both threads to hold their first
    // accessor and *then* both attempt the crossing second acquire at the
    // same time every round. Without this, the two independently-scheduled
    // loops rarely land in the crossing window and the test would pass
    // whether or not the lock-order-violation path is even reachable.
    std::atomic<int> at_barrier1{0};
    std::atomic<int> at_barrier2{0};
    // Small on purpose: each round that genuinely deadlocks costs up to
    // acquire_timeout (500ms) before the map gives up and throws, so this is
    // sized to stay well under the CTest TIMEOUT on this executable rather
    // than to maximize stress.
    constexpr int kRounds = 5;

    auto barrier = [](std::atomic<int>& counter, int round) {
        int target = (round + 1) * 2;
        counter.fetch_add(1);
        while(counter.load() < target)
            std::this_thread::yield();
    };

    auto run = [&](int key_a, int key_b) {
        while(!start.load())
            std::this_thread::yield();
        for(int i = 0; i < kRounds; ++i) {
            map_type::accessor first;
            m.find(first, key_a);
            barrier(at_barrier1, i);
            try {
                map_type::accessor second;
                m.find(second, key_b);
            } catch(const std::runtime_error&) {
                // Expected outcome of a genuine lock-order violation: the map
                // gave up and threw instead of spinning forever.
                caught.fetch_add(1);
            }
            barrier(at_barrier2, i);
        }
    };

    std::thread t1([&] { run(1, 2); });
    std::thread t2([&] { run(2, 1); });

    start.store(true);
    t1.join();
    t2.join();

    // Confirms this test actually exercises the lock-order-violation path
    // (and isn't accidentally a no-op that never crosses accessors).
    EXPECT_GT(caught.load(), 0);
}
