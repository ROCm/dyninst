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

// Thread-safe hash map with per-element locking.
//
// Replaces tbb::concurrent_hash_map while preserving the accessor/const_accessor
// interface Dyninst relies on. Keys are partitioned across a fixed number of
// shards; each shard owns a lazily allocated array of bucket heads guarded by its
// own dyn_spin_rwlock, which protects only the *structure* (the bucket array and
// the chains). In addition, every element owns its own dyn_spin_rwlock, and an
// accessor holds *that element's* lock (exclusive for `accessor`, shared for
// `const_accessor`) for its lifetime -- matching concurrent_hash_map's per-element
// locking contract.
//
// Per-element (rather than per-shard) locking is required because several call
// sites -- e.g. Parser::set_edge_parsing_status -- hold multiple accessors into
// the same map instance at once. Per-shard locking self-deadlocks as soon as two
// of those keys hash to the same shard.
//
// Elements are intrusive: each node stores its own chain pointer, its cached hash,
// its lock and the key/value pair in one allocation, and the key is stored exactly
// once. The predecessor of this implementation held nodes in a
// std::unordered_map<K, shared_ptr<node>>, which cost two allocations and two
// copies of every key per element -- around 125 MB of the ~490 MB peak heap when
// instrumenting lulesh, and the entire memory gap against the TBB-based build.
//
// Nodes are not reference counted, so the lifetime rule is structural: a node
// pointer is only ever read while its shard lock is held, and the node's own lock
// is always acquired before that shard lock is released. No thread can therefore
// hold a node pointer that a concurrent erase could free, and no revalidation
// after the fact is needed. Erase relies on the same rule from the other side: it
// unlinks under the shard lock (making the node unreachable), then takes the
// node's exclusive lock to wait for accessors already holding it to drain, and
// only then frees it.
//
// The one place that would break the rule is blocking on a contended element lock
// while holding a shard lock, which deadlocks against a thread holding that
// element and waiting for the shard (which is exactly what erase does). So an
// existing element's lock is taken with a bounded try; on failure the shard lock
// is dropped, the now-stale node pointer is discarded *without being
// dereferenced*, and the operation restarts from the lookup. This is
// concurrent_hash_map's bounded pause-and-restart.
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
    // One allocation per element. `hash` is the mixed hash, cached so that chain
    // walks reject non-matching keys on an integer compare (important when K is
    // std::string) and so that erase can locate the bucket without rehashing.
    struct node {
        node* next = nullptr;
        std::size_t hash = 0;
        mutable dyn_spin_rwlock mtx;
        std::pair<const K, V> kv;

        template<typename... Args>
        node(const K& k, std::size_t h, Args&&... args)
            : hash(h),
              kv(std::piecewise_construct, std::forward_as_tuple(k),
                 std::forward_as_tuple(std::forward<Args>(args)...)) {}
    };

    // Shard and element locks are the same type; the aliases keep which level is
    // being taken, and in which mode, legible at each acquisition site.
    using read_lock = dyncompat::shared_lock<dyn_spin_rwlock>;
    using write_lock = dyncompat::unique_lock<dyn_spin_rwlock>;

    struct shard {
        mutable dyn_spin_rwlock mtx;  // guards buckets/mask/count and the chains
        node** buckets = nullptr;     // null until this shard's first insert
        std::size_t mask = 0;         // bucket_count - 1, valid once buckets != null
        std::size_t count = 0;
    };

    // Shard count trades lock contention against per-map memory. An earlier
    // revision raised this to 256 because 64 scaled negatively past 16 threads.
    // That was the unmixed hash rather than the shard count: keys are dominated by
    // 16-byte-aligned addresses, whose low four bits are constant, so `% 64`
    // reached only 4 distinct shards. With mix() applied every shard is reachable,
    // and 64 then measures faster than 256 at every thread count from 1 to 128 on
    // a full parse while using ~120 MB less.
    static constexpr std::size_t num_shards = 64;
    static constexpr std::size_t shard_bits = 6;  // num_shards == 1 << shard_bits
    static constexpr std::size_t initial_buckets = 4;
    // Bounded attempts on a contended element lock before dropping the shard lock
    // and restarting. Element locks are held for a few instructions, so a handful
    // of pauses resolves ordinary contention without stalling shard writers.
    static constexpr int max_lock_attempts = 8;

    std::unique_ptr<shard[]> shards_{new shard[num_shards]};

    // Avalanche the hash. std::hash is the identity for pointers and integers, and
    // Dyninst's keys are dominated by heap pointers and function entry addresses,
    // which are 16-byte aligned -- so their low bits are constant. Feeding those
    // straight into a shard/bucket index would leave only every 16th slot reachable
    // and funnel the whole parallel parse through a handful of mutexes.
    // tbb_hash_compare avoided this by multiplying the key by a hash multiplier;
    // this is the same idea.
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

    static std::size_t hash_of(const K& k) {
        return mix(concurrent::hasher<K>{}(k));
    }

    // The shard takes the low bits and the bucket the bits above them, so the two
    // indices are drawn from disjoint parts of the mixed hash and a shard's keys
    // stay spread across its buckets.
    shard& shard_for(std::size_t h) { return shards_[h & (num_shards - 1)]; }
    const shard& shard_for(std::size_t h) const {
        return shards_[h & (num_shards - 1)];
    }
    static std::size_t bucket_of(const shard& s, std::size_t h) {
        return (h >> shard_bits) & s.mask;
    }

    // Caller holds the shard lock (either mode).
    static node* search(const shard& s, std::size_t h, const K& k) {
        if(!s.buckets)
            return nullptr;
        for(node* n = s.buckets[bucket_of(s, h)]; n; n = n->next)
            if(n->hash == h && n->kv.first == k)
                return n;
        return nullptr;
    }

    // Caller holds the shard lock exclusively. Doubles the bucket array (or makes
    // the first one) and relinks the chains. Nodes themselves never move, so an
    // accessor holding one is unaffected -- only bucket heads change.
    static void grow_locked(shard& s) {
        const std::size_t new_count = s.buckets ? (s.mask + 1) * 2 : initial_buckets;
        const std::size_t new_mask = new_count - 1;
        node** nb = new node*[new_count]();
        if(s.buckets) {
            for(std::size_t b = 0; b <= s.mask; ++b) {
                node* n = s.buckets[b];
                while(n) {
                    node* next = n->next;
                    node** head = &nb[(n->hash >> shard_bits) & new_mask];
                    n->next = *head;
                    *head = n;
                    n = next;
                }
            }
            delete[] s.buckets;
        }
        s.buckets = nb;
        s.mask = new_mask;
    }

    // Caller holds the shard lock exclusively, or is the destructor.
    static void destroy_locked(shard& s) {
        if(s.buckets) {
            for(std::size_t b = 0; b <= s.mask; ++b) {
                node* n = s.buckets[b];
                while(n) {
                    node* next = n->next;
                    dyn_c_annotations::rwdeinit(&n->mtx);
                    delete n;
                    n = next;
                }
            }
            delete[] s.buckets;
        }
        s.buckets = nullptr;
        s.mask = 0;
        s.count = 0;
    }

    // Caller holds the shard lock exclusively. Links an already-built node.
    static void link_locked(shard& s, node* n) {
        if(!s.buckets || s.count + 1 > s.mask + 1)
            grow_locked(s);
        node** head = &s.buckets[bucket_of(s, n->hash)];
        n->next = *head;
        *head = n;
        ++s.count;
    }

public:
    using value_type = std::pair<const K, V>;
    using mapped_type = V;
    using key_type = K;

    dyn_c_hash_map() = default;

    ~dyn_c_hash_map() {
        for(std::size_t i = 0; i < num_shards; ++i)
            destroy_locked(shards_[i]);
    }

    // Copies without ever holding a shard lock and an element lock at the same
    // time. Keys are immutable once inserted, so they can be snapshotted under the
    // shard lock alone; values need the element lock, which is then taken through
    // the normal accessor protocol after the shard lock is gone. An element erased
    // between the two passes is simply absent from the copy.
    dyn_c_hash_map(const dyn_c_hash_map& other) {
        std::vector<K> keys;
        for(std::size_t i = 0; i < num_shards; ++i) {
            keys.clear();
            {
                const shard& s = other.shards_[i];
                read_lock lock(s.mtx);
                keys.reserve(s.count);
                if(s.buckets)
                    for(std::size_t b = 0; b <= s.mask; ++b)
                        for(node* n = s.buckets[b]; n; n = n->next)
                            keys.push_back(n->kv.first);
            }
            for(const K& k : keys) {
                const_accessor ca;
                if(other.find(ca, k))
                    insert(value_type(k, ca->second));
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
    // reference.
    dyn_c_hash_map& operator=(const dyn_c_hash_map& other) {
        if(this != &other) {
            dyn_c_hash_map tmp(other);  // snapshot without holding our locks
            for(std::size_t i = 0; i < num_shards; ++i) {
                write_lock lock(shards_[i].mtx);
                destroy_locked(shards_[i]);
                // Steal the temporary's chains rather than re-inserting: the nodes
                // are already built and are not yet visible to any other thread.
                shards_[i].buckets = tmp.shards_[i].buckets;
                shards_[i].mask = tmp.shards_[i].mask;
                shards_[i].count = tmp.shards_[i].count;
                tmp.shards_[i].buckets = nullptr;
                tmp.shards_[i].mask = 0;
                tmp.shards_[i].count = 0;
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

    // Holds a shared (read) lock on the target element while alive. The node is
    // borrowed, not owned: holding the lock is what keeps it alive, because erase
    // waits for the element's exclusive lock before freeing it.
    class const_accessor {
        friend class dyn_c_hash_map<K,V>;
    protected:
        node* node_ = nullptr;
        read_lock lock_;
        bool valid_ = false;

        // Take ownership of an already-acquired element lock and tell Valgrind
        // about it. Every acquisition goes through here so the annotation cannot
        // drift out of step with the lock it describes.
        void adopt(node* np, read_lock lk) {
            node_ = np;
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
            node_ = nullptr;
        }
    };

    // Holds an exclusive (write) lock on the target element while alive.
    class accessor {
        friend class dyn_c_hash_map<K,V>;
    protected:
        node* node_ = nullptr;
        write_lock lock_;
        bool valid_ = false;

        void adopt(node* np, write_lock lk) {
            node_ = np;
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
            node_ = nullptr;
        }
    };

private:
    // Acquire the lock on k's element and return it, or return null if k is absent.
    //
    // The element lock is taken while the shard lock is still held, so the node
    // cannot be erased between finding it and locking it -- that is the invariant
    // that makes the non-owning node pointers safe. Because blocking here would
    // deadlock against a thread that holds this element and wants the shard lock,
    // acquisition is a bounded try; on failure the shard lock is dropped and the
    // lookup restarts. The node pointer must not be touched after the shard lock
    // is released, since an erase may free it at that point.
    template<typename LockT>
    node* acquire(const K& k, LockT& out) const {
        const std::size_t h = hash_of(k);
        const shard& s = shard_for(h);
        for(;;) {
            {
                read_lock slock(s.mtx);
                node* n = search(s, h, k);
                if(!n)
                    return nullptr;
                for(int i = 0; i < max_lock_attempts; ++i) {
                    LockT attempt(n->mtx, std::try_to_lock);
                    if(attempt.owns_lock()) {
                        out = std::move(attempt);
                        return n;
                    }
                    concurrent::detail::spin_relax();
                }
            }
            std::this_thread::yield();
        }
    }

    // Shared implementation of the accessor/const_accessor insert overloads;
    // returns true if the element was created.
    //
    // A new node is locked while the shard lock is still held. That acquisition
    // cannot be contended, because the node is not yet linked anywhere another
    // thread can reach, and it provides concurrent_hash_map's atomic
    // insert-and-lock semantics: no other thread can observe the element before
    // the inserting caller has initialized it.
    template<typename Acc, typename LockT, typename... Args>
    bool do_insert(Acc& acc, const K& k, Args&&... args) {
        acc.release();
        const std::size_t h = hash_of(k);
        shard& s = shard_for(h);
        for(;;) {
            {
                write_lock slock(s.mtx);
                node* n = search(s, h, k);
                if(!n) {
                    n = new node(k, h, std::forward<Args>(args)...);
                    dyn_c_annotations::rwinit(&n->mtx);
                    link_locked(s, n);
                    acc.adopt(n, LockT(n->mtx));
                    return true;
                }
                for(int i = 0; i < max_lock_attempts; ++i) {
                    LockT attempt(n->mtx, std::try_to_lock);
                    if(attempt.owns_lock()) {
                        acc.adopt(n, std::move(attempt));
                        return false;
                    }
                    concurrent::detail::spin_relax();
                }
            }
            std::this_thread::yield();
        }
    }

public:
    bool find(const_accessor& ca, const K& k) const {
        ca.release();
        read_lock lk;
        if(node* n = acquire(k, lk)) {
            ca.adopt(n, std::move(lk));
            return true;
        }
        return false;
    }

    bool find(accessor& a, const K& k) {
        a.release();
        write_lock lk;
        if(node* n = acquire(k, lk)) {
            a.adopt(n, std::move(lk));
            return true;
        }
        return false;
    }

    // Only reports presence, so unlike find() it needs no element lock at all.
    int contains(const K& k) const {
        const std::size_t h = hash_of(k);
        const shard& s = shard_for(h);
        read_lock slock(s.mtx);
        return search(s, h, k) != nullptr;
    }

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
        const std::size_t h = hash_of(e.first);
        shard& s = shard_for(h);
        write_lock slock(s.mtx);
        if(search(s, h, e.first))
            return false;
        node* n = new node(e.first, h, e.second);
        dyn_c_annotations::rwinit(&n->mtx);
        link_locked(s, n);
        return true;
    }

    // Erase the exact element the accessor holds. The accessor owns the element's
    // exclusive lock, so no other thread is inside the element; unlinking it under
    // the shard lock makes it unreachable, after which it can be freed as soon as
    // the accessor lets go. Erasing by identity (not by key) so a key that was
    // replaced in the meantime is left alone.
    bool erase(accessor& a) {
        if(!a.valid_) return false;
        node* n = a.node_;
        shard& s = shard_for(n->hash);
        bool removed = false;
        {
            write_lock slock(s.mtx);
            if(s.buckets) {
                node** p = &s.buckets[bucket_of(s, n->hash)];
                while(*p && *p != n)
                    p = &(*p)->next;
                if(*p == n) {
                    *p = n->next;
                    --s.count;
                    removed = true;
                }
            }
        }
        a.release();  // reports the element lock release to Valgrind
        if(removed) {
            dyn_c_annotations::rwdeinit(&n->mtx);
            delete n;
        }
        return removed;
    }

    bool erase(const K& k) {
        const std::size_t h = hash_of(k);
        shard& s = shard_for(h);
        node* n = nullptr;
        {
            write_lock slock(s.mtx);
            if(!s.buckets)
                return false;
            node** p = &s.buckets[bucket_of(s, h)];
            while(*p && !((*p)->hash == h && (*p)->kv.first == k))
                p = &(*p)->next;
            if(!*p)
                return false;
            n = *p;
            *p = n->next;  // unlink first, so no new thread can reach it
            --s.count;
        }
        // Accessors taken before the unlink may still hold the element. Waiting for
        // its exclusive lock drains them, as concurrent_hash_map's erase does,
        // before the node is freed. The lock is released before rwdeinit so
        // Valgrind never sees a lock destroyed while held.
        { write_lock drain(n->mtx); }
        dyn_c_annotations::rwdeinit(&n->mtx);
        delete n;
        return true;
    }

    int size() const {
        std::size_t n = 0;
        for(std::size_t i = 0; i < num_shards; ++i) {
            read_lock lock(shards_[i].mtx);
            n += shards_[i].count;
        }
        return static_cast<int>(n);
    }

    // Pre-sizes the bucket arrays; only ever grows them, as
    // concurrent_hash_map's rehash is likewise just a capacity hint.
    void rehash(int n = 0) {
        if(n <= 0)
            return;
        const std::size_t per = static_cast<std::size_t>(n) / num_shards + 1;
        for(std::size_t i = 0; i < num_shards; ++i) {
            write_lock lock(shards_[i].mtx);
            while(!shards_[i].buckets || shards_[i].mask + 1 < per)
                grow_locked(shards_[i]);
        }
    }

    // Frees every element, so it must not run while another thread holds an
    // accessor or is looking one up -- the same restriction the previous
    // implementation had.
    void clear() {
        for(std::size_t i = 0; i < num_shards; ++i) {
            write_lock lock(shards_[i].mtx);
            destroy_locked(shards_[i]);
        }
    }

    // Forward iterator that walks every shard, bucket and chain in turn. Not
    // synchronized; use only after the concurrent insertion phase has completed.
    template<bool IsConst>
    class iter_impl {
        friend class dyn_c_hash_map<K,V>;

        using shard_ptr = std::conditional_t<IsConst, const shard*, shard*>;

    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = std::pair<const K, V>;
        using difference_type = std::ptrdiff_t;
        using reference = std::conditional_t<IsConst, const value_type&, value_type&>;
        using pointer = std::conditional_t<IsConst, const value_type*, value_type*>;

    private:
        shard_ptr shards_ = nullptr;
        std::size_t sidx_ = num_shards;
        std::size_t bidx_ = 0;
        node* cur_ = nullptr;

        // Point cur_ at the first element in the next occupied bucket at or after
        // (sidx_, bidx_), leaving it null once every shard is exhausted.
        void seek() {
            for(; sidx_ < num_shards; ++sidx_, bidx_ = 0) {
                const shard& s = shards_[sidx_];
                if(!s.buckets)
                    continue;
                for(; bidx_ <= s.mask; ++bidx_) {
                    if(s.buckets[bidx_]) {
                        cur_ = s.buckets[bidx_];
                        return;
                    }
                }
            }
            cur_ = nullptr;
        }

        iter_impl(shard_ptr s, std::size_t idx) : shards_(s), sidx_(idx) {
            if(sidx_ < num_shards)
                seek();
        }

    public:
        iter_impl() = default;

        reference operator*() const { return cur_->kv; }
        pointer operator->() const { return &cur_->kv; }

        iter_impl& operator++() {
            if(cur_ && cur_->next) {
                cur_ = cur_->next;
            } else if(cur_) {
                ++bidx_;
                cur_ = nullptr;
                seek();
            }
            return *this;
        }
        iter_impl operator++(int) {
            iter_impl tmp = *this;
            ++(*this);
            return tmp;
        }

        // Exhausted iterators compare equal because seek() leaves cur_ null and
        // sidx_ at num_shards, which is exactly how end() is built.
        bool operator==(const iter_impl& o) const {
            return cur_ == o.cur_ && sidx_ == o.sidx_;
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

    // n value-initialized elements. typeStruct::create and typeUnion::create size
    // a vector up front and then fill it by index.
    explicit dyn_c_vector(size_type n) : dyn_c_vector() {
        for(size_type i = 0; i < n; ++i)
            append_unlocked();
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
