// cpp/include/meradb/table.h
//
// A Table = its schema (from the catalog) + its heap file + its HASH INDEXES.
// Mirrors meradb/table.py.
//
// Every MUKHYA KUNJI / ANOKHA column gets an in-memory hash index
// {value -> row id}, and every composite constraint (table-level
// `ANOKHA (a, b)` / `MUKHYA KUNJI (a, b)`) gets one keyed by the tuple of its
// values. Indexes are built lazily with one full scan, kept in sync by
// insertMany/deleteMany, and thrown away by invalidateIndexes() whenever the
// whole file is rewritten (ALTER, SIKODO, SAAF, WAPAS). KHALI never takes
// part in an index. Hash indexes answer `=` only.
#pragma once
#include "meradb/catalog.h"
#include "meradb/datatypes.h"
#include "meradb/ordered_map.h"
#include "meradb/storage.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace meradb {

// Hash / equality for index keys (a tuple of Values; a single-column index
// uses a 1-element tuple). They follow Python dict-key semantics, because
// that is what table.py's indexes are: numbers compare by value across
// INT/FLOAT/BOOL (1 == 1.0 == SACH), everything else by type + value.
struct ValueVecHash {
    size_t operator()(const std::vector<Value>& key) const;
};
struct ValueVecEq {
    bool operator()(const std::vector<Value>& a, const std::vector<Value>& b) const;
};
struct VecSizeTHash {
    size_t operator()(const std::vector<size_t>& v) const;
};

using RowValues = std::vector<Value>;
using StoredRow = std::pair<int64_t, RowValues>;

// key tuple -> row id
using HashIndex = std::unordered_map<std::vector<Value>, int64_t, ValueVecHash, ValueVecEq>;
// column positions -> index. {i} for a single unique column, {i, j, ...} for
// a composite constraint. Iterates in Python's order: unique columns in
// declaration order, then composite ANOKHA groups, then the composite
// MUKHYA KUNJI -- the UNIQUE / TAKRAAV checks act on the FIRST matching
// index, so this order decides which error / which conflicting row wins.
using IndexMap = InsertionOrderedMap<HashIndex, std::vector<size_t>, VecSizeTHash>;
// A slot that can be shared by several Table objects for the same table
// (Python passes the engine-wide index_cache dict + key); empty = not built.
using IndexCache = std::shared_ptr<std::optional<IndexMap>>;

class Table {
public:
    // `cache` null -> this Table gets a private cache.
    Table(TableSchema schema, std::string path, IndexCache cache = nullptr);
    virtual ~Table() = default;

    const TableSchema& schema() const { return schema_; }
    HeapFile& heap() { return heap_; }
    virtual bool isView() const { return false; }

    // ---- reading ----
    virtual std::vector<StoredRow> rows() const;  // full scan, decoded
    virtual std::optional<RowValues> get(int64_t rowId) const;

    // ---- writing (keeps built indexes in sync) ----
    virtual void insertMany(const std::vector<RowValues>& newRows);
    // (row id, old values): the values are needed to find the index entries
    virtual void deleteMany(const std::vector<StoredRow>& oldRows);

    // ---- indexes ----
    virtual IndexMap& indexes();  // built lazily
    // The row whose indexed `column` equals `value`, or {} -- without a scan.
    // ExecutionError if `column` has no single-column index.
    virtual std::vector<StoredRow> lookup(size_t column, const Value& value);
    void invalidateIndexes();

private:
    TableSchema schema_;
    HeapFile heap_;
    IndexCache cache_;

    std::vector<std::vector<size_t>> indexedGroups() const;
    void checkWidth(const RowValues& values) const;  // ExecutionError unless one value per column
};

// A VIEW's already-computed result set, usable wherever a Table is read
// (SE / MILAO). Row id = position. No storage, so no insert/delete/indexes.
class MaterializedTable : public Table {
public:
    explicit MaterializedTable(std::vector<RowValues> data);
    MaterializedTable(TableSchema schema, std::vector<RowValues> data);

    bool isView() const override { return true; }
    std::vector<StoredRow> rows() const override;
    std::optional<RowValues> get(int64_t rowId) const override;

    void insertMany(const std::vector<RowValues>& newRows) override;
    void deleteMany(const std::vector<StoredRow>& oldRows) override;
    IndexMap& indexes() override;
    std::vector<StoredRow> lookup(size_t column, const Value& value) override;

private:
    std::vector<RowValues> data_;
};

}  // namespace meradb
