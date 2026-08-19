/*
 * See the dyninst/COPYRIGHT file for copyright information.
 * 
 * We provide the Paradyn Tools (below described as "Paradyn")
 * on an AS IS basis, and do not warrant its validity or performance.
 * We reserve the right to update, modify, or discontinue this
 * software at any time.  We shall have no obligation to supply such
 * updates or modifications or any other form of support to you.
 * 
 * By your use of Paradyn, you understand and agree that we (or any
 * other person or entity with proprietary rights in Paradyn) are
 * under no obligation to provide either maintenance services,
 * update services, notices of latent defects, or correction of
 * defects for Paradyn.
 * 
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef _CONCURRENT_H_
#define _CONCURRENT_H_

#include "util.h"
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <mutex>
#include <new>
#include <stddef.h>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>
#include <dyncompat/atomic.hpp>
#include <dyncompat/thread/mutex.hpp>
#include <dyncompat/thread/condition_variable.hpp>
#include <dyncompat/thread/locks.hpp>
#include <dyncompat/thread/shared_mutex.hpp>
#include <dyncompat/functional/hash.hpp>

namespace Dyninst {

namespace dyn_c_annotations {
    void COMMON_EXPORT rwinit(void*);
    void COMMON_EXPORT rwdeinit(void*);
    void COMMON_EXPORT wlock(void*);
    void COMMON_EXPORT wunlock(void*);
    void COMMON_EXPORT rlock(void*);
    void COMMON_EXPORT runlock(void*);
}

namespace concurrent {
  template <typename K>
  struct hasher {
    size_t operator()(K const& k) const {
      return dyncompat::hash<K>{}(k);
    }
  };

  namespace detail {
    // Tell the CPU this thread is spinning, so the core stops speculating through
    // iterations it will have to discard and (on SMT) yields its issue slots to
    // the sibling thread that most likely holds the lock.
    inline void spin_relax() {
#if defined(__i386__) || defined(__x86_64__)
      __asm__ __volatile__("pause" ::: "memory");
#elif defined(__aarch64__) || defined(__arm__)
      __asm__ __volatile__("yield" ::: "memory");
#elif defined(__powerpc__) || defined(__powerpc64__)
      __asm__ __volatile__("or 27,27,27" ::: "memory");  // lower SMT priority
#else
      // No architectural hint on this target; the caller still escalates to yield.
#endif
    }

    // Spin for a geometrically growing number of iterations, then hand the core
    // back to the scheduler. Container locks are held for a few instructions, so
    // contention almost always resolves during the spin phase; the yield exists
    // for the case where the holder has itself been preempted.
    class spin_backoff {
      static constexpr int max_spins = 16;
      int spins_ = 1;

    public:
      void pause() {
        if(spins_ <= max_spins) {
          for(int i = 0; i < spins_; ++i)
            spin_relax();
          spins_ *= 2;
        } else {
          std::this_thread::yield();
        }
      }

      // Use after losing a race for an otherwise-free lock: the next attempt is
      // likely to win, so it should not inherit an already-escalated spin count.
      void reset() { spins_ = 1; }
    };

    // floor(log2(x)), for x >= 1. Used to map an element index onto its segment.
    inline unsigned log2_floor(std::size_t x) {
#if defined(__GNUC__) || defined(__clang__)
      return static_cast<unsigned>(
          8 * sizeof(unsigned long long) - 1 -
          static_cast<unsigned>(__builtin_clzll(static_cast<unsigned long long>(x))));
#else
      unsigned r = 0;
      while(x >>= 1)
        ++r;
      return r;
#endif
    }
  }
}

// Reader/writer spin lock.
//
// Dyninst acquires these locks tens of millions of times over a parallel parse,
// nearly always uncontended, and dyn_c_hash_map embeds one in every element.
// std::shared_mutex occupies 56 bytes and defers to glibc's pthread rwlock, whose
// reader path is itself a write to shared state; at Dyninst's element counts that
// dominates both the container's memory footprint and its uncontended fast path.
//
// The whole lock is one 4-byte word: bit 0 is the active writer, bit 1 marks a
// waiting writer, and the remaining bits count active readers. Uncontended
// acquisition is a single compare-exchange for a writer, a single fetch-add for a
// reader.
//
// A writer that cannot enter immediately publishes WRITER_PENDING, which stops new
// readers from joining, so a sustained stream of readers cannot starve it. Beyond
// that the lock is unfair and grants no FIFO order. It is not recursive, and since
// waiters spin instead of sleeping it must not be held across a blocking call.
//
// Note that this lock is invisible to Valgrind's DRD/Helgrind and to
// ThreadSanitizer, all of which derive happens-before edges from pthread calls.
// dyn_c_hash_map therefore reports its element lock transitions explicitly through
// dyn_c_annotations, as the TBB-based implementation did for TBB's spin locks.
class dyn_spin_rwlock {
    using state_type = std::uint32_t;

    static constexpr state_type WRITER = 1;
    static constexpr state_type WRITER_PENDING = 2;
    static constexpr state_type ONE_READER = 4;
    static constexpr state_type READERS = ~(WRITER | WRITER_PENDING);
    // Everything that has to drain before a writer may enter.
    static constexpr state_type BUSY = WRITER | READERS;

    dyncompat::atomic<state_type> state_{0};

public:
    dyn_spin_rwlock() = default;
    dyn_spin_rwlock(dyn_spin_rwlock const&) = delete;
    dyn_spin_rwlock& operator=(dyn_spin_rwlock const&) = delete;

    void lock() {
        for(concurrent::detail::spin_backoff backoff;;) {
            state_type s = state_.load(dyncompat::memory_order_relaxed);
            if(!(s & BUSY)) {
                // Storing WRITER also clears WRITER_PENDING, which this thread may
                // have set on an earlier iteration.
                if(state_.compare_exchange_weak(s, WRITER,
                                                dyncompat::memory_order_acquire,
                                                dyncompat::memory_order_relaxed))
                    return;
                backoff.reset();
            } else if(!(s & WRITER_PENDING)) {
                state_.fetch_or(WRITER_PENDING, dyncompat::memory_order_relaxed);
            }
            backoff.pause();
        }
    }

    bool try_lock() {
        state_type s = state_.load(dyncompat::memory_order_relaxed);
        return !(s & BUSY) &&
               state_.compare_exchange_strong(s, WRITER,
                                              dyncompat::memory_order_acquire,
                                              dyncompat::memory_order_relaxed);
    }

    void unlock() {
        // Leaves WRITER_PENDING alone so a writer already queued behind this one
        // continues to hold readers off instead of re-announcing itself.
        state_.fetch_and(static_cast<state_type>(~WRITER),
                         dyncompat::memory_order_release);
    }

    void lock_shared() {
        for(concurrent::detail::spin_backoff backoff;;) {
            state_type s = state_.load(dyncompat::memory_order_relaxed);
            if(!(s & (WRITER | WRITER_PENDING))) {
                state_type prev =
                    state_.fetch_add(ONE_READER, dyncompat::memory_order_acquire);
                if(!(prev & WRITER))
                    return;
                // A writer claimed the lock between the load and the increment.
                state_.fetch_sub(ONE_READER, dyncompat::memory_order_relaxed);
            }
            backoff.pause();
        }
    }

    bool try_lock_shared() {
        state_type s = state_.load(dyncompat::memory_order_relaxed);
        if(s & (WRITER | WRITER_PENDING))
            return false;
        state_type prev =
            state_.fetch_add(ONE_READER, dyncompat::memory_order_acquire);
        if(!(prev & WRITER))
            return true;
        state_.fetch_sub(ONE_READER, dyncompat::memory_order_relaxed);
        return false;
    }

    void unlock_shared() {
        state_.fetch_sub(ONE_READER, dyncompat::memory_order_release);
    }
};

// Thread-safe hash map backed by sharded std::unordered_map instances with
// per-element locking.
//
// Replaces tbb::concurrent_hash_map while preserving the accessor/const_accessor
// interface Dyninst relies on. Keys are partitioned across a fixed number of
// shards; each shard is an independent std::unordered_map guarded by its own
// dyn_spin_rwlock that protects only the map *structure*. In addition, every stored
// element owns its own dyn_spin_rwlock, and an accessor holds *that element's* lock
// (exclusive for `accessor`, shared for `const_accessor`) for its lifetime --
// matching tbb::concurrent_hash_map's per-element locking contract.
//
// Per-element (rather than per-shard) locking is required because several call
// sites -- e.g. Parser::set_edge_parsing_status -- hold multiple accessors into
// the same map instance at once. Per-shard locking self-deadlocks as soon as two
// of those keys hash to the same shard.
//
// Elements are held through shared_ptr so a concurrent erase cannot destroy a
// node (and its mutex) out from under a thread that is acquiring or holding it.
// The shard lock is always released before an *existing* element's lock is taken,
// so the two lock levels cannot form a cycle. The sole exception is a newly
// created node, which is locked while the shard lock is still held but is not yet
// reachable by any other thread, so it can never be contended (see
// emplace_locked).
//
// Element locks are reported to Valgrind's DRD/Helgrind through dyn_c_annotations,
// because dyn_spin_rwlock is built from plain atomics that those tools cannot
// recognize as a lock on their own.
//
// Element access via begin()/end() is not internally synchronized: callers
// populate the map during a parallel phase and iterate afterwards, matching the
// original concurrent_hash_map usage.
template<typename K, typename V>
class dyn_c_hash_map {
    struct node {
        std::pair<const K, V> kv;
        mutable dyn_spin_rwlock mtx;

        template<typename... Args>
        explicit node(const K& k, Args&&... args)
            : kv(std::piecewise_construct, std::forward_as_tuple(k),
                 std::forward_as_tuple(std::forward<Args>(args)...)) {}
    };

    using node_ptr = std::shared_ptr<node>;
    using map_type = std::unordered_map<K, node_ptr, concurrent::hasher<K>>;

    // Shard and element locks are the same type; the aliases keep which level is
    // being taken, and in which mode, legible at each acquisition site.
    using read_lock = dyncompat::shared_lock<dyn_spin_rwlock>;
    using write_lock = dyncompat::unique_lock<dyn_spin_rwlock>;

    struct shard {
        map_type map;
        mutable dyn_spin_rwlock mtx;  // guards map structure only
    };

    // Shard count trades lock contention against per-map memory. The array below
    // is allocated eagerly, so every map instance pays 64 bytes per shard (a 56
    // byte empty unordered_map plus the 4 byte lock, padded) whether or not it ever
    // holds an element -- and Dyninst keeps thousands of these alive at once:
    // roughly 5500 while instrumenting a 1 MB binary, so the fixed cost dominates
    // the element data on small and medium targets. Making the buckets lazy, as
    // tbb::concurrent_hash_map does, is the remaining win here.
    //
    // An earlier revision raised this to 256 because 64 scaled negatively past 16
    // threads. That was the unmixed hash rather than the shard count: keys are
    // dominated by 16-byte-aligned addresses, whose low four bits are constant, so
    // `% 64` reached only 4 distinct shards. With mix() applied (see shard_of)
    // every shard is reachable, and 64 then measures faster than 256 at every
    // thread count from 1 to 128 on a full parse while using ~120 MB less.
    static constexpr std::size_t num_shards = 64;
    std::unique_ptr<shard[]> shards_{new shard[num_shards]};

    // Avalanche the hash before selecting a shard. std::hash is the identity for
    // pointers and integers, and Dyninst's keys are dominated by heap pointers and
    // function entry addresses, which are 16-byte aligned -- so their low bits are
    // constant. Feeding those straight into `% num_shards` would leave only every
    // 16th shard reachable (4 of 64, 16 of 256) and funnel the whole parallel
    // parse through a handful of mutexes. tbb_hash_compare avoided this by
    // multiplying the key by a hash multiplier; this is the same idea.
    static std::size_t mix(std::size_t h) {
        if constexpr(sizeof(std::size_t) == 8) {
            h ^= h >> 33;
            h *= 0xff51afd7ed558ccdULL;  // MurmurHash3 64-bit finalizer
            h ^= h >> 33;
        } else {
            h ^= h >> 16;
            h *= 0x85ebca6bUL;
            h ^= h >> 13;
        }
        return h;
    }

    static std::size_t shard_of(const K& k) {
        return mix(concurrent::hasher<K>{}(k)) % num_shards;
    }
    shard& shard_for(const K& k) { return shards_[shard_of(k)]; }
    const shard& shard_for(const K& k) const { return shards_[shard_of(k)]; }

public:
    using value_type = std::pair<const K, V>;
    using mapped_type = V;
    using key_type = K;

    dyn_c_hash_map() = default;
    ~dyn_c_hash_map() = default;

    // Copies element values without ever holding a shard lock and an element lock
    // at the same time: snapshot the (key, node) pairs under the shard lock, drop
    // it, then lock each element in turn. The snapshot holds shared_ptrs, so the
    // nodes stay alive even if the source erases them in the meantime.
    dyn_c_hash_map(const dyn_c_hash_map& other) {
        std::vector<std::pair<K, node_ptr>> entries;
        for(std::size_t i = 0; i < num_shards; ++i) {
            entries.clear();
            {
                read_lock lock(other.shards_[i].mtx);
                entries.reserve(other.shards_[i].map.size());
                for(const auto& entry : other.shards_[i].map)
                    entries.emplace_back(entry.first, entry.second);
            }
            for(const auto& entry : entries) {
                read_lock nlock(entry.second->mtx);
                auto np =
                    std::make_shared<node>(entry.first, entry.second->kv.second);
                dyn_c_annotations::rwinit(&np->mtx);
                shards_[i].map.emplace(entry.first, std::move(np));
            }
        }
    }

    // Deliberately not noexcept: the moved-from map is left with a fresh (empty)
    // shard array so it remains usable, and that allocation can throw.
    dyn_c_hash_map(dyn_c_hash_map&& other) : shards_(std::move(other.shards_)) {
        other.shards_.reset(new shard[num_shards]);
    }

    // Keep this map's shard array in place and assign per shard under its own
    // lock. Replacing the array wholesale would free it while a concurrent reader
    // may still hold a `shard&` obtained from shard_for(), leaving a dangling
    // reference -- the shared_ptr nodes do not protect the shard array itself.
    dyn_c_hash_map& operator=(const dyn_c_hash_map& other) {
        if(this != &other) {
            dyn_c_hash_map tmp(other);  // snapshot without holding our locks
            for(std::size_t i = 0; i < num_shards; ++i) {
                write_lock lock(shards_[i].mtx);
                shards_[i].map = std::move(tmp.shards_[i].map);
            }
        }
        return *this;
    }

    // Swap rather than reallocate: both objects already own a shard array, so this
    // needs no allocation and is genuinely nothrow.
    dyn_c_hash_map& operator=(dyn_c_hash_map&& other) noexcept {
        shards_.swap(other.shards_);
        return *this;
    }

    // Holds a shared (read) lock on the target element while alive.
    class const_accessor {
        friend class dyn_c_hash_map<K,V>;
    protected:
        node_ptr node_;
        read_lock lock_;
        bool valid_ = false;

        // Take ownership of an already-acquired element lock and tell Valgrind
        // about it. Every acquisition goes through here so the annotation cannot
        // drift out of step with the lock it describes.
        void adopt(node_ptr np, read_lock lk) {
            node_ = std::move(np);
            lock_ = std::move(lk);
            valid_ = true;
            dyn_c_annotations::rlock(&node_->mtx);
        }
    public:
        const_accessor() = default;
        const_accessor(const const_accessor&) = delete;
        const_accessor& operator=(const const_accessor&) = delete;
        ~const_accessor() { release(); }

        bool empty() const { return !valid_; }
        const value_type* operator->() const { return &node_->kv; }
        const value_type& operator*() const { return node_->kv; }

        void release() {
            if(valid_) dyn_c_annotations::runlock(&node_->mtx);
            valid_ = false;
            if(lock_.owns_lock()) lock_.unlock();
            lock_ = {};
            node_.reset();
        }
    };

    // Holds an exclusive (write) lock on the target element while alive.
    class accessor {
        friend class dyn_c_hash_map<K,V>;
    protected:
        node_ptr node_;
        write_lock lock_;
        bool valid_ = false;

        void adopt(node_ptr np, write_lock lk) {
            node_ = std::move(np);
            lock_ = std::move(lk);
            valid_ = true;
            dyn_c_annotations::wlock(&node_->mtx);
        }
    public:
        accessor() = default;
        accessor(const accessor&) = delete;
        accessor& operator=(const accessor&) = delete;
        ~accessor() { release(); }

        bool empty() const { return !valid_; }
        value_type* operator->() const { return &node_->kv; }
        value_type& operator*() const { return node_->kv; }

        void release() {
            if(valid_) dyn_c_annotations::wunlock(&node_->mtx);
            valid_ = false;
            if(lock_.owns_lock()) lock_.unlock();
            lock_ = {};
            node_.reset();
        }
    };

private:
    // Look up k under the shard's shared lock and return its node (or null). The
    // shard lock is released on return, before the caller takes the element lock.
    node_ptr find_node(const K& k) const {
        const shard& s = shard_for(k);
        read_lock lock(s.mtx);
        auto it = s.map.find(k);
        return (it == s.map.end()) ? node_ptr{} : it->second;
    }

    // Find-or-create the node for k under the shard's exclusive lock. Returns the
    // node and whether it was newly inserted.
    //
    // When a node is newly created it is locked (into out_lock) *before* the shard
    // lock is dropped. The node is not yet reachable by any other thread, so this
    // is uncontended (cannot deadlock) and it guarantees that no other thread can
    // observe the element before the inserting caller has initialized it -- this
    // matches tbb::concurrent_hash_map's atomic insert-and-lock semantics.
    //
    // Existing nodes are returned unlocked; the caller takes their lock only after
    // the shard lock is released, so shard and element locks never nest.
    template<typename LockT, typename... Args>
    std::pair<node_ptr, bool> emplace_locked(LockT& out_lock, const K& k, Args&&... args) {
        shard& s = shard_for(k);
        write_lock lock(s.mtx);
        auto it = s.map.find(k);
        if(it != s.map.end()) return {it->second, false};
        auto np = std::make_shared<node>(k, std::forward<Args>(args)...);
        dyn_c_annotations::rwinit(&np->mtx);
        s.map.emplace(k, np);
        // Uncontended by construction: no other thread can reach the node yet, so
        // this resolves in a single atomic operation and cannot deadlock despite
        // being the one place a shard lock is held across an element acquisition.
        // The nesting is invisible to ThreadSanitizer, which cannot see
        // dyn_spin_rwlock, so it is not reported as a lock-order inversion either.
        out_lock = LockT(np->mtx);
        return {np, true};
    }

    // True iff k still maps to exactly this node. Confirms that a node obtained
    // after the shard lock was released was not erased or replaced before its
    // element lock was taken -- restoring the atomic find/insert-and-lock
    // guarantee of tbb::concurrent_hash_map. Takes only the shard lock (shared),
    // while the caller holds the element lock, so it never holds a shard lock
    // while waiting for a contended element lock.
    bool still_current(const K& k, const node_ptr& np) const {
        const shard& s = shard_for(k);
        read_lock lock(s.mtx);
        auto it = s.map.find(k);
        return it != s.map.end() && it->second == np;
    }

    // Shared implementation of the accessor/const_accessor insert overloads.
    // A freshly created node is already locked under the shard lock (no gap). An
    // existing node is locked after the shard lock is dropped, then validated
    // with still_current(); if it was erased/replaced in between, retry.
    template<typename Acc, typename LockT, typename... Args>
    bool do_insert(Acc& acc, const K& k, Args&&... args) {
        acc.release();
        for(;;) {
            LockT new_lock;
            auto res = emplace_locked<LockT>(new_lock, k, std::forward<Args>(args)...);
            if(res.second) {
                acc.adopt(std::move(res.first), std::move(new_lock));
                return true;
            }
            LockT lk(res.first->mtx);
            if(!still_current(k, res.first)) continue;
            acc.adopt(std::move(res.first), std::move(lk));
            return false;
        }
    }

public:
    bool find(const_accessor& ca, const K& k) const {
        ca.release();
        for(;;) {
            node_ptr np = find_node(k);
            if(!np) return false;
            read_lock lk(np->mtx);
            if(!still_current(k, np)) continue;  // erased/replaced after lookup; retry
            ca.adopt(std::move(np), std::move(lk));
            return true;
        }
    }

    bool find(accessor& a, const K& k) {
        a.release();
        for(;;) {
            node_ptr np = find_node(k);
            if(!np) return false;
            write_lock lk(np->mtx);
            if(!still_current(k, np)) continue;  // erased/replaced after lookup; retry
            a.adopt(std::move(np), std::move(lk));
            return true;
        }
    }

    int contains(const K& k) const { return find_node(k) != nullptr; }

    bool insert(accessor& a, const K& k) {
        return do_insert<accessor, write_lock>(a, k);
    }

    bool insert(accessor& a, const value_type& e) {
        return do_insert<accessor, write_lock>(
            a, e.first, e.second);
    }

    bool insert(const_accessor& ca, const K& k) {
        return do_insert<const_accessor, read_lock>(ca, k);
    }

    bool insert(const_accessor& ca, const value_type& e) {
        return do_insert<const_accessor, read_lock>(
            ca, e.first, e.second);
    }

    bool insert(const value_type& e) {
        shard& s = shard_for(e.first);
        write_lock lock(s.mtx);
        auto it = s.map.find(e.first);
        if(it != s.map.end()) return false;
        auto np = std::make_shared<node>(e.first, e.second);
        dyn_c_annotations::rwinit(&np->mtx);
        s.map.emplace(e.first, std::move(np));
        return true;
    }

    // Erase the exact element the accessor holds. The accessor already owns the
    // element lock, so taking the shard lock here is node -> shard ordering and
    // never nests a shard lock while waiting for a contended element lock.
    bool erase(accessor& a) {
        if(!a.valid_) return false;
        const K k = a.node_->kv.first;
        node_ptr np = a.node_;
        shard& s = shard_for(k);
        write_lock slock(s.mtx);
        bool removed = false;
        auto it = s.map.find(k);
        if(it != s.map.end() && it->second == np) {  // erase by identity, not by key
            s.map.erase(it);
            removed = true;
        }
        a.release();  // reports the element lock release to Valgrind
        if(removed) dyn_c_annotations::rwdeinit(&np->mtx);
        return removed;
    }

    bool erase(const K& k) {
        for(;;) {
            node_ptr np = find_node(k);
            if(!np) return false;
            // Acquire the element lock first, so erase waits for outstanding
            // accessors (as tbb::concurrent_hash_map does), then remove under the
            // shard lock. node -> shard ordering; no shard lock is held while
            // waiting for the element lock.
            {
                write_lock elock(np->mtx);
                shard& s = shard_for(k);
                write_lock slock(s.mtx);
                auto it = s.map.find(k);
                if(it == s.map.end()) return false;
                if(it->second != np) continue;  // replaced after lookup; retry
                s.map.erase(it);
            }
            // Only once the element lock is dropped, so Valgrind never sees a lock
            // destroyed while it is still held.
            dyn_c_annotations::rwdeinit(&np->mtx);
            return true;
        }
    }

    int size() const {
        std::size_t n = 0;
        for(std::size_t i = 0; i < num_shards; ++i) {
            read_lock lock(shards_[i].mtx);
            n += shards_[i].map.size();
        }
        return static_cast<int>(n);
    }

    void rehash(int n = 0) {
        const std::size_t per =
            (n > 0) ? static_cast<std::size_t>(n) / num_shards + 1 : 0;
        for(std::size_t i = 0; i < num_shards; ++i) {
            write_lock lock(shards_[i].mtx);
            shards_[i].map.rehash(per);
        }
    }

    void clear() {
        for(std::size_t i = 0; i < num_shards; ++i) {
            write_lock lock(shards_[i].mtx);
            shards_[i].map.clear();
        }
    }

    // Forward iterator that walks every shard in turn. Not synchronized; use
    // only after the concurrent insertion phase has completed.
    template<bool IsConst>
    class iter_impl {
        friend class dyn_c_hash_map<K,V>;

        using shard_ptr = std::conditional_t<IsConst, const shard*, shard*>;
        using inner = std::conditional_t<IsConst, typename map_type::const_iterator,
                                         typename map_type::iterator>;

    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = std::pair<const K, V>;
        using difference_type = std::ptrdiff_t;
        using reference = std::conditional_t<IsConst, const value_type&, value_type&>;
        using pointer = std::conditional_t<IsConst, const value_type*, value_type*>;

    private:
        shard_ptr shards_ = nullptr;
        std::size_t idx_ = num_shards;
        inner cur_{};

        void advance_to_valid() {
            while(idx_ < num_shards && cur_ == shards_[idx_].map.end()) {
                if(++idx_ < num_shards) cur_ = shards_[idx_].map.begin();
            }
        }

        iter_impl(shard_ptr s, std::size_t idx) : shards_(s), idx_(idx) {
            if(idx_ < num_shards) {
                cur_ = shards_[idx_].map.begin();
                advance_to_valid();
            }
        }

    public:
        iter_impl() = default;

        reference operator*() const { return cur_->second->kv; }
        pointer operator->() const { return &cur_->second->kv; }

        iter_impl& operator++() {
            ++cur_;
            advance_to_valid();
            return *this;
        }
        iter_impl operator++(int) {
            iter_impl tmp = *this;
            ++(*this);
            return tmp;
        }

        bool operator==(const iter_impl& o) const {
            if(idx_ != o.idx_) return false;
            if(idx_ == num_shards) return true;
            return cur_ == o.cur_;
        }
        bool operator!=(const iter_impl& o) const { return !(*this == o); }
    };

    using iterator = iter_impl<false>;
    using const_iterator = iter_impl<true>;

    iterator begin() { return iterator(shards_.get(), 0); }
    iterator end() { return iterator(shards_.get(), num_shards); }
    const_iterator begin() const { return const_iterator(shards_.get(), 0); }
    const_iterator end() const { return const_iterator(shards_.get(), num_shards); }
};

// Thread-safe, append-during-parallel-phase sequence container.
//
// Replaces tbb::concurrent_vector, preserving the two properties Dyninst relies
// on: (1) push_back/emplace_back may be called concurrently, and (2) pointers and
// references to existing elements stay valid as the container grows.
//
// Storage is a table of geometrically growing segments, the same structure
// tbb::concurrent_vector uses. Segment 0 holds min_segment_size elements and each
// subsequent segment doubles, so an index maps to (segment, offset) with a single
// bit scan, growth only ever appends a segment pointer, and existing elements are
// never relocated. The pointers for the first embedded_segments segments live
// inside the object; a vector that outgrows them allocates one fixed-size spill
// table that is likewise never reallocated.
//
// Because no reachable pointer ever moves, reads need no lock at all: locating an
// element is two acquire loads and some index arithmetic. That is the whole point
// of the design. Backing this with std::deque instead forces every indexed read
// to take the append lock, because push_back can reallocate the deque's internal
// map array out from under a reader walking it -- and Dyninst reads these vectors
// far more often than it appends to them.
//
// CONCURRENCY CONTRACT: appends (push_back/emplace_back) and reads (operator[],
// at, front, back, size, empty) may run concurrently, as they could with
// tbb::concurrent_vector. size() only ever reports fully constructed elements, so
// any index below it is safe to read; this is why appends are serialized against
// each other rather than reserving indices in parallel. Iteration and the
// non-append mutators (clear, resize, pop_back, swap, assignment) are NOT
// synchronized and must not run concurrently with anything else on the same
// instance. Dyninst satisfies that by appending during the parallel phase and
// iterating afterwards.
template<typename T>
class dyn_c_vector {
    using allocator = std::allocator<T>;
    using cell = dyncompat::atomic<T*>;

    // Segment 0 holds min_segment_size elements; segment k > 0 holds
    // min_segment_size << (k-1), so segment k begins at index
    // min_segment_size << (k-1) and the two halves of the table line up.
    static constexpr std::size_t min_segment_size = 8;
    // Segments whose pointers live in the object rather than the spill table.
    // Four covers the first 64 elements, which is more than nearly every
    // fieldList or localVars instance ever holds, so most vectors never allocate
    // a spill table at all.
    static constexpr std::size_t embedded_segments = 4;
    // Caps capacity at min_segment_size << (max_segments-1) == 2^42 elements,
    // which no Dyninst target can approach, and keeps the spill table at 288
    // bytes so it can be allocated once and never grown.
    static constexpr std::size_t max_segments = 40;

    cell embedded_[embedded_segments];
    dyncompat::atomic<cell*> spill_{nullptr};
    // Published with release once an element is fully constructed, so an acquire
    // load bounds the range of indices that are safe to read.
    dyncompat::atomic<std::size_t> size_{0};
    // Serializes appends against each other. Readers never take it.
    mutable dyn_spin_rwlock append_mutex_;

    static std::size_t segment_of(std::size_t n) {
        if(n < min_segment_size)
            return 0;
        return concurrent::detail::log2_floor(n / min_segment_size) + 1;
    }
    static std::size_t segment_base(std::size_t seg) {
        return (seg == 0) ? 0 : (min_segment_size << (seg - 1));
    }
    static std::size_t segment_size(std::size_t seg) {
        return (seg == 0) ? min_segment_size : (min_segment_size << (seg - 1));
    }

    // Table slot holding segment `seg`, or null if it cannot exist yet because no
    // spill table has been allocated.
    cell* cell_of(std::size_t seg) const {
        if(seg < embedded_segments)
            return const_cast<cell*>(&embedded_[seg]);
        cell* spill = spill_.load(dyncompat::memory_order_acquire);
        return spill ? (spill + (seg - embedded_segments)) : nullptr;
    }

    // Address of element n, which the caller must already know exists. Both loads
    // are acquire, so the element read is ordered after the append that published
    // it even when the caller did not obtain n from size() on this thread.
    T* slot(std::size_t n) const {
        const std::size_t seg = segment_of(n);
        return cell_of(seg)->load(dyncompat::memory_order_acquire) +
               (n - segment_base(seg));
    }

    // Make element n's storage exist and return its address. Only ever called
    // under append_mutex_, so the table can be mutated without further care.
    T* reserve_slot(std::size_t n) {
        const std::size_t seg = segment_of(n);
        if(seg >= max_segments)
            throw std::length_error("dyn_c_vector: capacity exceeded");

        cell* c;
        if(seg < embedded_segments) {
            c = &embedded_[seg];
        } else {
            cell* spill = spill_.load(dyncompat::memory_order_relaxed);
            if(!spill) {
                spill = new cell[max_segments - embedded_segments];
                for(std::size_t i = 0; i < max_segments - embedded_segments; ++i)
                    spill[i].store(nullptr, dyncompat::memory_order_relaxed);
                spill_.store(spill, dyncompat::memory_order_release);
            }
            c = spill + (seg - embedded_segments);
        }

        T* base = c->load(dyncompat::memory_order_relaxed);
        if(!base) {
            base = allocator{}.allocate(segment_size(seg));
            c->store(base, dyncompat::memory_order_release);
        }
        return base + (n - segment_base(seg));
    }

    // Append without taking append_mutex_, for callers that already hold it or
    // that hold the only reference to this object.
    template<typename... Args>
    T& append_unlocked(Args&&... args) {
        const std::size_t n = size_.load(dyncompat::memory_order_relaxed);
        T* p = reserve_slot(n);
        new(p) T(std::forward<Args>(args)...);
        // Release: publishes the element and any table or segment pointer stored
        // by reserve_slot above.
        size_.store(n + 1, dyncompat::memory_order_release);
        return *p;
    }

    // Destroy every element and release all storage, leaving an empty vector.
    void reset_unlocked() {
        const std::size_t n = size_.load(dyncompat::memory_order_relaxed);
        for(std::size_t i = n; i > 0; --i)
            slot(i - 1)->~T();
        size_.store(0, dyncompat::memory_order_relaxed);

        for(std::size_t seg = 0; seg < max_segments; ++seg) {
            cell* c = cell_of(seg);
            if(!c)
                break;
            T* base = c->load(dyncompat::memory_order_relaxed);
            if(!base)
                break;  // segments are filled in order, so nothing follows
            allocator{}.deallocate(base, segment_size(seg));
            c->store(nullptr, dyncompat::memory_order_relaxed);
        }

        if(cell* spill = spill_.load(dyncompat::memory_order_relaxed)) {
            delete[] spill;
            spill_.store(nullptr, dyncompat::memory_order_relaxed);
        }
    }

    // Move `other`'s storage into this (empty) vector. Element addresses are
    // preserved, so pointers into the source stay valid.
    void adopt_unlocked(dyn_c_vector& other) {
        for(std::size_t seg = 0; seg < embedded_segments; ++seg) {
            embedded_[seg].store(other.embedded_[seg].load(dyncompat::memory_order_relaxed),
                                 dyncompat::memory_order_relaxed);
            other.embedded_[seg].store(nullptr, dyncompat::memory_order_relaxed);
        }
        spill_.store(other.spill_.load(dyncompat::memory_order_relaxed),
                     dyncompat::memory_order_relaxed);
        other.spill_.store(nullptr, dyncompat::memory_order_relaxed);
        size_.store(other.size_.load(dyncompat::memory_order_relaxed),
                    dyncompat::memory_order_relaxed);
        other.size_.store(0, dyncompat::memory_order_relaxed);
    }

    using append_guard = std::lock_guard<dyn_spin_rwlock>;

public:
    using value_type = T;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using reference = T&;
    using const_reference = const T&;
    using pointer = T*;
    using const_pointer = const T*;

    dyn_c_vector() {
        for(std::size_t seg = 0; seg < embedded_segments; ++seg)
            embedded_[seg].store(nullptr, dyncompat::memory_order_relaxed);
    }

    ~dyn_c_vector() { reset_unlocked(); }

    dyn_c_vector(const dyn_c_vector& other) : dyn_c_vector() {
        append_guard guard(other.append_mutex_);
        const std::size_t n = other.size_.load(dyncompat::memory_order_relaxed);
        for(std::size_t i = 0; i < n; ++i)
            append_unlocked(*other.slot(i));
    }

    dyn_c_vector(dyn_c_vector&& other) : dyn_c_vector() {
        append_guard guard(other.append_mutex_);
        adopt_unlocked(other);
    }

    dyn_c_vector& operator=(const dyn_c_vector& other) {
        if(this != &other) {
            dyn_c_vector tmp(other);  // copy without holding our own lock
            append_guard guard(append_mutex_);
            reset_unlocked();
            adopt_unlocked(tmp);
        }
        return *this;
    }

    dyn_c_vector& operator=(dyn_c_vector&& other) {
        if(this != &other) {
            std::scoped_lock locks(append_mutex_, other.append_mutex_);
            reset_unlocked();
            adopt_unlocked(other);
        }
        return *this;
    }

    void push_back(const T& value) {
        append_guard guard(append_mutex_);
        append_unlocked(value);
    }

    void push_back(T&& value) {
        append_guard guard(append_mutex_);
        append_unlocked(std::move(value));
    }

    template<typename... Args>
    reference emplace_back(Args&&... args) {
        append_guard guard(append_mutex_);
        return append_unlocked(std::forward<Args>(args)...);
    }

    // Lock-free element access, safe to call while another thread appends.
    // Dyninst depends on that in fieldListType::operator==, which compares a
    // type's fields while another OpenMP worker may still be adding fields to it
    // (a type is published into typesByID before its members are parsed).
    reference operator[](size_type n) { return *slot(n); }
    const_reference operator[](size_type n) const { return *slot(n); }

    reference at(size_type n) {
        if(n >= size())
            throw std::out_of_range("dyn_c_vector::at");
        return *slot(n);
    }
    const_reference at(size_type n) const {
        if(n >= size())
            throw std::out_of_range("dyn_c_vector::at");
        return *slot(n);
    }

    reference front() { return *slot(0); }
    const_reference front() const { return *slot(0); }
    reference back() { return *slot(size() - 1); }
    const_reference back() const { return *slot(size() - 1); }

    size_type size() const { return size_.load(dyncompat::memory_order_acquire); }
    bool empty() const { return size() == 0; }
    static constexpr size_type max_size() { return segment_base(max_segments); }

    // Per the concurrency contract, the mutators below must not run concurrently
    // with anything else on the same instance. They take the append lock only so
    // that a stray concurrent append cannot corrupt the segment table outright.
    void clear() {
        append_guard guard(append_mutex_);
        reset_unlocked();
    }

    void pop_back() {
        append_guard guard(append_mutex_);
        const std::size_t n = size_.load(dyncompat::memory_order_relaxed);
        if(n == 0)
            return;
        size_.store(n - 1, dyncompat::memory_order_release);
        slot(n - 1)->~T();
    }

    void resize(size_type n) {
        append_guard guard(append_mutex_);
        std::size_t cur = size_.load(dyncompat::memory_order_relaxed);
        while(cur > n) {
            size_.store(--cur, dyncompat::memory_order_release);
            slot(cur)->~T();
        }
        while(cur++ < n)
            append_unlocked();
    }

    void swap(dyn_c_vector& other) {
        if(this == &other)
            return;
        std::scoped_lock locks(append_mutex_, other.append_mutex_);
        dyn_c_vector tmp;
        tmp.adopt_unlocked(other);
        other.adopt_unlocked(*this);
        adopt_unlocked(tmp);
    }

    // Random-access iterator over indices. symtabAPI/src/Type.C needs
    // `begin() + n`, so this cannot be a forward iterator.
    template<bool IsConst>
    class iter_impl {
        friend class dyn_c_vector<T>;
        template<bool> friend class iter_impl;

        using container = std::conditional_t<IsConst, const dyn_c_vector, dyn_c_vector>;

        container* vec_ = nullptr;
        std::size_t idx_ = 0;

        iter_impl(container* v, std::size_t i) : vec_(v), idx_(i) {}

    public:
        using iterator_category = std::random_access_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = std::conditional_t<IsConst, const T*, T*>;
        using reference = std::conditional_t<IsConst, const T&, T&>;

        iter_impl() = default;

        // iterator converts to const_iterator, but not the other way around.
        template<bool WasConst = IsConst, typename = std::enable_if_t<WasConst>>
        iter_impl(const iter_impl<false>& o) : vec_(o.vec_), idx_(o.idx_) {}

        reference operator*() const { return (*vec_)[idx_]; }
        pointer operator->() const { return &(*vec_)[idx_]; }
        reference operator[](difference_type n) const {
            return (*vec_)[idx_ + static_cast<std::size_t>(n)];
        }

        iter_impl& operator++() { ++idx_; return *this; }
        iter_impl& operator--() { --idx_; return *this; }
        iter_impl operator++(int) { iter_impl t = *this; ++idx_; return t; }
        iter_impl operator--(int) { iter_impl t = *this; --idx_; return t; }

        iter_impl& operator+=(difference_type n) {
            idx_ += static_cast<std::size_t>(n);
            return *this;
        }
        iter_impl& operator-=(difference_type n) {
            idx_ -= static_cast<std::size_t>(n);
            return *this;
        }

        friend iter_impl operator+(iter_impl it, difference_type n) { return it += n; }
        friend iter_impl operator+(difference_type n, iter_impl it) { return it += n; }
        friend iter_impl operator-(iter_impl it, difference_type n) { return it -= n; }
        friend difference_type operator-(const iter_impl& a, const iter_impl& b) {
            return static_cast<difference_type>(a.idx_) -
                   static_cast<difference_type>(b.idx_);
        }

        friend bool operator==(const iter_impl& a, const iter_impl& b) {
            return a.idx_ == b.idx_;
        }
        friend bool operator!=(const iter_impl& a, const iter_impl& b) {
            return a.idx_ != b.idx_;
        }
        friend bool operator<(const iter_impl& a, const iter_impl& b) {
            return a.idx_ < b.idx_;
        }
        friend bool operator>(const iter_impl& a, const iter_impl& b) {
            return a.idx_ > b.idx_;
        }
        friend bool operator<=(const iter_impl& a, const iter_impl& b) {
            return a.idx_ <= b.idx_;
        }
        friend bool operator>=(const iter_impl& a, const iter_impl& b) {
            return a.idx_ >= b.idx_;
        }
    };

    using iterator = iter_impl<false>;
    using const_iterator = iter_impl<true>;
    using reverse_iterator = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    iterator begin() { return iterator(this, 0); }
    iterator end() { return iterator(this, size()); }
    const_iterator begin() const { return const_iterator(this, 0); }
    const_iterator end() const { return const_iterator(this, size()); }
    const_iterator cbegin() const { return begin(); }
    const_iterator cend() const { return end(); }

    reverse_iterator rbegin() { return reverse_iterator(end()); }
    reverse_iterator rend() { return reverse_iterator(begin()); }
    const_reverse_iterator rbegin() const { return const_reverse_iterator(end()); }
    const_reverse_iterator rend() const { return const_reverse_iterator(begin()); }
};

class dyn_mutex : public dyncompat::mutex {
public:
    using unique_lock = dyncompat::unique_lock<dyn_mutex>;
};

class COMMON_EXPORT dyn_rwlock {
    // Reader management members
    dyncompat::atomic<unsigned int> rin;
    dyncompat::atomic<unsigned int> rout;
    unsigned int last;
    dyn_mutex inlock;
    dyncompat::condition_variable rcond;
    bool rwakeup[2];

    // Writer management members
    dyn_mutex wlock;
    dyn_mutex outlock;
    dyncompat::condition_variable wcond;
    bool wwakeup;
public:
    dyn_rwlock();
    ~dyn_rwlock();

    void lock_shared();
    void unlock_shared();
    void lock();
    void unlock();

    using unique_lock = dyncompat::unique_lock<dyn_rwlock>;
    using shared_lock = dyncompat::shared_lock<dyn_rwlock>;
};

class COMMON_EXPORT dyn_thread {
public:
    dyn_thread();
    unsigned int getId();
    static unsigned int threads();

    operator unsigned int() { return getId(); }

    static thread_local dyn_thread me;
};

template<typename T>
class dyn_threadlocal {
    std::vector<T> cache;
    T base;
    dyn_rwlock lock;
public:
    dyn_threadlocal() : cache(), base() {}
    dyn_threadlocal(const T& d) : cache(), base(d) {}
    ~dyn_threadlocal() {}

    T get() {
        {
            dyn_rwlock::shared_lock l(lock);
            if(cache.size() > dyn_thread::me)
                return cache[dyn_thread::me];
        }
        {
            dyn_rwlock::unique_lock l(lock);
            if(cache.size() <= dyn_thread::me)
                cache.insert(cache.end(), dyn_thread::threads() - cache.size(), base);
        }
        return base;
    }

    void set(const T& val) {
        {
            dyn_rwlock::shared_lock l(lock);
            if(cache.size() > dyn_thread::me) {
                cache[dyn_thread::me] = val;
                return;
            }
        }
        {
            dyn_rwlock::unique_lock l(lock);
            if(cache.size() <= dyn_thread::me)
                cache.insert(cache.end(), dyn_thread::threads() - cache.size(), base);
            cache[dyn_thread::me] = val;
        }
    }
};

} // namespace Dyninst

#endif
