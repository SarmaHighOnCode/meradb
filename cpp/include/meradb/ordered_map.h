// cpp/include/meradb/ordered_map.h
//
// A hash map that iterates in INSERTION order, like a Python dict. Used
// wherever the Python engine relies on dict order: catalog.json lists
// tables/views in creation order, engine checks walk every table in that
// order, and Table::indexes() is walked "first matching index wins" by the
// UNIQUE / TAKRAAV checks.
#pragma once
#include <cstddef>
#include <functional>
#include <iterator>
#include <list>
#include <string>
#include <unordered_map>
#include <utility>

namespace meradb {

// Element references stay valid until that element is erased (backed by
// std::list). Copies are deep and independent; moves keep references valid.
template <typename V, typename K = std::string, typename Hash = std::hash<K>>
class InsertionOrderedMap {
public:
    using key_type = K;
    using mapped_type = V;
    using value_type = std::pair<const K, V>;
    using iterator = typename std::list<value_type>::iterator;
    using const_iterator = typename std::list<value_type>::const_iterator;

    InsertionOrderedMap() = default;
    InsertionOrderedMap(const InsertionOrderedMap& other) : items_(other.items_) { rebuildIndex(); }
    InsertionOrderedMap(InsertionOrderedMap&&) = default;
    InsertionOrderedMap& operator=(const InsertionOrderedMap& other) {
        if (this != &other) {
            InsertionOrderedMap copy(other);
            swap(copy);
        }
        return *this;
    }
    InsertionOrderedMap& operator=(InsertionOrderedMap&&) = default;
    ~InsertionOrderedMap() = default;

    void swap(InsertionOrderedMap& other) noexcept {
        items_.swap(other.items_);  // list iterators follow their nodes
        index_.swap(other.index_);
    }

    iterator begin() { return items_.begin(); }
    iterator end() { return items_.end(); }
    const_iterator begin() const { return items_.begin(); }
    const_iterator end() const { return items_.end(); }

    size_t size() const { return items_.size(); }
    bool empty() const { return items_.empty(); }
    size_t count(const K& key) const { return index_.count(key); }

    iterator find(const K& key) {
        auto it = index_.find(key);
        return it == index_.end() ? items_.end() : it->second;
    }
    const_iterator find(const K& key) const {
        auto it = index_.find(key);
        return it == index_.end() ? items_.cend() : const_iterator(it->second);
    }

    V& at(const K& key) { return index_.at(key)->second; }
    const V& at(const K& key) const { return index_.at(key)->second; }

    // Existing keys keep their position (Python dict semantics); new keys go last.
    V& operator[](const K& key) {
        auto it = index_.find(key);
        if (it != index_.end()) return it->second->second;
        items_.emplace_back(key, V{});
        auto last = std::prev(items_.end());
        index_.emplace(key, last);
        return last->second;
    }

    size_t erase(const K& key) {
        auto it = index_.find(key);
        if (it == index_.end()) return 0;
        items_.erase(it->second);
        index_.erase(it);
        return 1;
    }

    void clear() {
        items_.clear();
        index_.clear();
    }

private:
    std::list<value_type> items_;
    std::unordered_map<K, iterator, Hash> index_;

    void rebuildIndex() {
        index_.clear();
        for (auto it = items_.begin(); it != items_.end(); ++it) index_.emplace(it->first, it);
    }
};

}  // namespace meradb
