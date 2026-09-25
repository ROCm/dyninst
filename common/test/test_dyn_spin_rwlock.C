#include "concurrent.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using Dyninst::dyn_spin_rwlock;

namespace {

// Bounds how long a liveness assertion will spin before declaring a hang, so a
// regression shows up as a test failure instead of blocking CI forever.
constexpr auto kTimeout = std::chrono::seconds(10);

bool wait_until(std::function<bool()> pred) {
    const auto deadline = std::chrono::steady_clock::now() + kTimeout;
    while(std::chrono::steady_clock::now() < deadline) {
        if(pred())
            return true;
        std::this_thread::yield();
    }
    return pred();
}

}  // namespace

TEST(DynSpinRwlock, WriterExcludesSecondWriter) {
    dyn_spin_rwlock lock;
    lock.lock();
    EXPECT_FALSE(lock.try_lock());
    lock.unlock();
    EXPECT_TRUE(lock.try_lock());
    lock.unlock();
}

TEST(DynSpinRwlock, WriterExcludesReader) {
    dyn_spin_rwlock lock;
    lock.lock();
    EXPECT_FALSE(lock.try_lock_shared());
    lock.unlock();
    EXPECT_TRUE(lock.try_lock_shared());
    lock.unlock_shared();
}

TEST(DynSpinRwlock, ReaderExcludesWriter) {
    dyn_spin_rwlock lock;
    lock.lock_shared();
    EXPECT_FALSE(lock.try_lock());
    lock.unlock_shared();
    EXPECT_TRUE(lock.try_lock());
    lock.unlock();
}

TEST(DynSpinRwlock, MultipleReadersConcurrent) {
    constexpr int kReaders = 8;
    dyn_spin_rwlock lock;
    std::atomic<int> active{0};
    std::atomic<int> max_observed{0};
    std::atomic<bool> start{false};

    std::vector<std::thread> threads;
    for(int i = 0; i < kReaders; ++i) {
        threads.emplace_back([&] {
            while(!start.load())
                std::this_thread::yield();
            lock.lock_shared();
            int cur = active.fetch_add(1) + 1;
            int prev = max_observed.load();
            while(cur > prev && !max_observed.compare_exchange_weak(prev, cur))
                ;
            // Give other readers a chance to enter concurrently before
            // releasing, without holding a blocking call under the lock.
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            active.fetch_sub(1);
            lock.unlock_shared();
        });
    }
    start.store(true);
    for(auto& t : threads)
        t.join();

    EXPECT_GT(max_observed.load(), 1) << "readers never overlapped";
}

TEST(DynSpinRwlock, WriterAcquiresAfterReadersRelease) {
    dyn_spin_rwlock lock;
    std::atomic<bool> writer_done{false};

    lock.lock_shared();
    std::thread writer([&] {
        lock.lock();
        writer_done.store(true);
        lock.unlock();
    });

    // Writer must not be able to proceed while the reader is still active.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(writer_done.load());

    lock.unlock_shared();
    EXPECT_TRUE(wait_until([&] { return writer_done.load(); }));
    writer.join();
}
