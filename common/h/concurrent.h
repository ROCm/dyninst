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
#include <deque>
#include <iterator>
#include <memory>
#include <mutex>
#include <stddef.h>
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
}

// Thread-safe hash map backed by sharded std::unordered_map instances with
// per-element locking.
//
// Replaces tbb::concurrent_hash_map while preserving the accessor/const_accessor
// interface Dyninst relies on. Keys are partitioned across a fixed number of
// shards; each shard is an independent std::unordered_map guarded by its own
// shared_mutex that protects only the map *structure*. In addition, every stored
// element owns its own shared_mutex, and an accessor holds *that element's* lock
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
// std::shared_mutex is understood natively by Valgrind's DRD/Helgrind tools, so
// the explicit lock annotations of the old TBB-based wrapper are unnecessary.
//
// Element access via begin()/end() is not internally synchronized: callers
// populate the map during a parallel phase and iterate afterwards, matching the
// original concurrent_hash_map usage.
template<typename K, typename V>
class dyn_c_hash_map {
    struct node {
        std::pair<const K, V> kv;
        mutable dyncompat::shared_mutex mtx;

        template<typename... Args>
        explicit node(const K& k, Args&&... args)
            : kv(std::piecewise_construct, std::forward_as_tuple(k),
                 std::forward_as_tuple(std::forward<Args>(args)...)) {}
    };

    using node_ptr = std::shared_ptr<node>;
    using map_type = std::unordered_map<K, node_ptr, concurrent::hasher<K>>;

    struct shard {
        map_type map;
        mutable dyncompat::shared_mutex mtx;  // guards map structure only
    };

    // Shard count trades lock contention against per-map memory. The array below
    // is allocated eagerly, so every map instance pays 112 bytes per shard (a 56
    // byte empty unordered_map plus a 56 byte shared_mutex) whether or not it ever
    // holds an element -- and Dyninst keeps thousands of these alive at once:
    // roughly 5500 while instrumenting a 1 MB binary, so the fixed cost dominates
    // the element data on small and medium targets.
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
                dyncompat::shared_lock<dyncompat::shared_mutex> lock(other.shards_[i].mtx);
                entries.reserve(other.shards_[i].map.size());
                for(const auto& entry : other.shards_[i].map)
                    entries.emplace_back(entry.first, entry.second);
            }
            for(const auto& entry : entries) {
                dyncompat::shared_lock<dyncompat::shared_mutex> nlock(entry.second->mtx);
                shards_[i].map.emplace(
                    entry.first,
                    std::make_shared<node>(entry.first, entry.second->kv.second));
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
                dyncompat::unique_lock<dyncompat::shared_mutex> lock(shards_[i].mtx);
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
        dyncompat::shared_lock<dyncompat::shared_mutex> lock_;
        bool valid_ = false;
    public:
        const_accessor() = default;
        const_accessor(const const_accessor&) = delete;
        const_accessor& operator=(const const_accessor&) = delete;
        ~const_accessor() { release(); }

        bool empty() const { return !valid_; }
        const value_type* operator->() const { return &node_->kv; }
        const value_type& operator*() const { return node_->kv; }

        void release() {
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
        dyncompat::unique_lock<dyncompat::shared_mutex> lock_;
        bool valid_ = false;
    public:
        accessor() = default;
        accessor(const accessor&) = delete;
        accessor& operator=(const accessor&) = delete;
        ~accessor() { release(); }

        bool empty() const { return !valid_; }
        value_type* operator->() const { return &node_->kv; }
        value_type& operator*() const { return node_->kv; }

        void release() {
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
        dyncompat::shared_lock<dyncompat::shared_mutex> lock(s.mtx);
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
        dyncompat::unique_lock<dyncompat::shared_mutex> lock(s.mtx);
        auto it = s.map.find(k);
        if(it != s.map.end()) return {it->second, false};
        auto np = std::make_shared<node>(k, std::forward<Args>(args)...);
        s.map.emplace(k, np);
        // Acquire non-blocking first: the node is unreachable, so this always
        // succeeds in practice, and a try_lock is excluded from ThreadSanitizer's
        // lock-order graph (it cannot participate in a cycle), which suppresses the
        // shard->element inversions this one nesting would otherwise report.
        // try_lock may fail spuriously, so fall back to a blocking -- still
        // uncontended -- acquisition rather than return an unlocked accessor.
        out_lock = LockT(np->mtx, std::try_to_lock);
        if(!out_lock.owns_lock()) out_lock.lock();
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
        dyncompat::shared_lock<dyncompat::shared_mutex> lock(s.mtx);
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
                acc.lock_ = std::move(new_lock);
                acc.node_ = std::move(res.first);
                acc.valid_ = true;
                return true;
            }
            LockT lk(res.first->mtx);
            if(!still_current(k, res.first)) continue;
            acc.lock_ = std::move(lk);
            acc.node_ = std::move(res.first);
            acc.valid_ = true;
            return false;
        }
    }

public:
    bool find(const_accessor& ca, const K& k) const {
        ca.release();
        for(;;) {
            node_ptr np = find_node(k);
            if(!np) return false;
            dyncompat::shared_lock<dyncompat::shared_mutex> lk(np->mtx);
            if(!still_current(k, np)) continue;  // erased/replaced after lookup; retry
            ca.lock_ = std::move(lk);
            ca.node_ = std::move(np);
            ca.valid_ = true;
            return true;
        }
    }

    bool find(accessor& a, const K& k) {
        a.release();
        for(;;) {
            node_ptr np = find_node(k);
            if(!np) return false;
            dyncompat::unique_lock<dyncompat::shared_mutex> lk(np->mtx);
            if(!still_current(k, np)) continue;  // erased/replaced after lookup; retry
            a.lock_ = std::move(lk);
            a.node_ = std::move(np);
            a.valid_ = true;
            return true;
        }
    }

    int contains(const K& k) const { return find_node(k) != nullptr; }

    bool insert(accessor& a, const K& k) {
        return do_insert<accessor, dyncompat::unique_lock<dyncompat::shared_mutex>>(a, k);
    }

    bool insert(accessor& a, const value_type& e) {
        return do_insert<accessor, dyncompat::unique_lock<dyncompat::shared_mutex>>(
            a, e.first, e.second);
    }

    bool insert(const_accessor& ca, const K& k) {
        return do_insert<const_accessor, dyncompat::shared_lock<dyncompat::shared_mutex>>(ca, k);
    }

    bool insert(const_accessor& ca, const value_type& e) {
        return do_insert<const_accessor, dyncompat::shared_lock<dyncompat::shared_mutex>>(
            ca, e.first, e.second);
    }

    bool insert(const value_type& e) {
        shard& s = shard_for(e.first);
        dyncompat::unique_lock<dyncompat::shared_mutex> lock(s.mtx);
        auto it = s.map.find(e.first);
        if(it != s.map.end()) return false;
        s.map.emplace(e.first, std::make_shared<node>(e.first, e.second));
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
        dyncompat::unique_lock<dyncompat::shared_mutex> slock(s.mtx);
        bool removed = false;
        auto it = s.map.find(k);
        if(it != s.map.end() && it->second == np) {  // erase by identity, not by key
            s.map.erase(it);
            removed = true;
        }
        a.release();
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
            dyncompat::unique_lock<dyncompat::shared_mutex> elock(np->mtx);
            shard& s = shard_for(k);
            dyncompat::unique_lock<dyncompat::shared_mutex> slock(s.mtx);
            auto it = s.map.find(k);
            if(it == s.map.end()) return false;
            if(it->second != np) continue;  // replaced after lookup; retry
            s.map.erase(it);
            return true;
        }
    }

    int size() const {
        std::size_t n = 0;
        for(std::size_t i = 0; i < num_shards; ++i) {
            dyncompat::shared_lock<dyncompat::shared_mutex> lock(shards_[i].mtx);
            n += shards_[i].map.size();
        }
        return static_cast<int>(n);
    }

    void rehash(int n = 0) {
        const std::size_t per =
            (n > 0) ? static_cast<std::size_t>(n) / num_shards + 1 : 0;
        for(std::size_t i = 0; i < num_shards; ++i) {
            dyncompat::unique_lock<dyncompat::shared_mutex> lock(shards_[i].mtx);
            shards_[i].map.rehash(per);
        }
    }

    void clear() {
        for(std::size_t i = 0; i < num_shards; ++i) {
            dyncompat::unique_lock<dyncompat::shared_mutex> lock(shards_[i].mtx);
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

// Thread-safe, append-during-parallel-phase sequence container backed by
// std::deque.
//
// Replaces tbb::concurrent_vector, preserving the two properties Dyninst relies
// on: (1) push_back/emplace_back may be called concurrently (serialized here by
// an internal mutex), and (2) pointers and references to existing elements stay
// valid as the container grows (std::deque never relocates its elements).
//
// CONCURRENCY CONTRACT: appends (push_back/emplace_back) and indexed reads
// (operator[], at, front, back, size, empty) are synchronized, so a reader that
// addresses elements by index may run alongside an appender, as it could with
// tbb::concurrent_vector. Iteration (begin/end/rbegin/rend) and the non-append
// mutators (clear, insert, erase, resize, ...) are NOT synchronized and must not
// run concurrently with an append, because push_back invalidates every deque
// iterator. Dyninst satisfies that restriction by appending during the parallel
// phase and iterating afterwards.
//
// std::deque is inherited privately so a dyn_c_vector cannot be sliced to, or
// bound as, a std::deque& -- which would silently bypass the append lock. The
// subset of the std::deque API that Dyninst uses is re-exported below.
template<typename T>
class dyn_c_vector : private std::deque<T> {
    using base = std::deque<T>;
    mutable dyncompat::mutex _mutex;

public:
    using typename base::value_type;
    using typename base::size_type;
    using typename base::difference_type;
    using typename base::reference;
    using typename base::const_reference;
    using typename base::pointer;
    using typename base::const_pointer;
    using typename base::iterator;
    using typename base::const_iterator;
    using typename base::reverse_iterator;
    using typename base::const_reverse_iterator;

    using base::base;

    dyn_c_vector() = default;

    dyn_c_vector(const dyn_c_vector& other) : base() {
        dyncompat::lock_guard<dyncompat::mutex> lock(other._mutex);
        base::operator=(static_cast<const base&>(other));
    }

    dyn_c_vector(dyn_c_vector&& other) : base() {
        dyncompat::lock_guard<dyncompat::mutex> lock(other._mutex);
        base::operator=(std::move(static_cast<base&>(other)));
    }

    dyn_c_vector& operator=(const dyn_c_vector& other) {
        if(this != &other) {
            std::scoped_lock locks(_mutex, other._mutex);
            base::operator=(static_cast<const base&>(other));
        }
        return *this;
    }

    dyn_c_vector& operator=(dyn_c_vector&& other) {
        if(this != &other) {
            std::scoped_lock locks(_mutex, other._mutex);
            base::operator=(std::move(static_cast<base&>(other)));
        }
        return *this;
    }

    void push_back(const T& value) {
        dyncompat::lock_guard<dyncompat::mutex> lock(_mutex);
        base::push_back(value);
    }

    void push_back(T&& value) {
        dyncompat::lock_guard<dyncompat::mutex> lock(_mutex);
        base::push_back(std::move(value));
    }

    template<typename... Args>
    typename base::reference emplace_back(Args&&... args) {
        dyncompat::lock_guard<dyncompat::mutex> lock(_mutex);
        return base::emplace_back(std::forward<Args>(args)...);
    }

    // Synchronized element access. tbb::concurrent_vector let one thread read
    // while another appended; std::deque does not, because push_back can
    // reallocate the internal map array out from under a reader that is walking
    // it to locate an element. Dyninst depends on that guarantee in
    // fieldListType::operator==, which compares a type's fields while another
    // OpenMP worker may still be adding fields to it (a type is published into
    // typesByID before its members are parsed).
    //
    // Releasing the lock before the caller uses the returned reference is safe:
    // std::deque never relocates existing elements, so only the traversal that
    // locates the element needs protecting, not the element itself.
    reference operator[](size_type n) {
        dyncompat::lock_guard<dyncompat::mutex> lock(_mutex);
        return base::operator[](n);
    }
    const_reference operator[](size_type n) const {
        dyncompat::lock_guard<dyncompat::mutex> lock(_mutex);
        return base::operator[](n);
    }
    reference at(size_type n) {
        dyncompat::lock_guard<dyncompat::mutex> lock(_mutex);
        return base::at(n);
    }
    const_reference at(size_type n) const {
        dyncompat::lock_guard<dyncompat::mutex> lock(_mutex);
        return base::at(n);
    }
    reference front() {
        dyncompat::lock_guard<dyncompat::mutex> lock(_mutex);
        return base::front();
    }
    const_reference front() const {
        dyncompat::lock_guard<dyncompat::mutex> lock(_mutex);
        return base::front();
    }
    reference back() {
        dyncompat::lock_guard<dyncompat::mutex> lock(_mutex);
        return base::back();
    }
    const_reference back() const {
        dyncompat::lock_guard<dyncompat::mutex> lock(_mutex);
        return base::back();
    }
    size_type size() const {
        dyncompat::lock_guard<dyncompat::mutex> lock(_mutex);
        return base::size();
    }
    bool empty() const {
        dyncompat::lock_guard<dyncompat::mutex> lock(_mutex);
        return base::empty();
    }

    // Unsynchronized iteration and non-append mutation. Per the concurrency
    // contract above, these must not run concurrently with an append to the same
    // instance: push_back invalidates all deque iterators.
    using base::begin;
    using base::end;
    using base::cbegin;
    using base::cend;
    using base::rbegin;
    using base::rend;
    using base::max_size;
    using base::clear;
    using base::resize;
    using base::assign;
    using base::insert;
    using base::erase;
    using base::pop_back;
    using base::swap;
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
