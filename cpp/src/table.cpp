// cpp/src/table.cpp
#include "meradb/table.h"
#include "meradb/errors.h"
#include <cmath>
#include <functional>

namespace meradb {

namespace {

// Python dict keys: 1, 1.0 and True are the SAME key. Fold every number to
// an int64 when it is integral (bool, int64, or a double with no fraction
// that fits in int64); otherwise it stays a double.
struct NumKey {
    bool isInt;
    int64_t i;
    double d;
};

bool numericKey(const Value& v, NumKey& out) {
    const auto& data = v.data;
    if (std::holds_alternative<bool>(data)) {
        out = {true, std::get<bool>(data) ? 1 : 0, 0.0};
        return true;
    }
    if (std::holds_alternative<int64_t>(data)) {
        out = {true, std::get<int64_t>(data), 0.0};
        return true;
    }
    if (std::holds_alternative<double>(data)) {
        double d = std::get<double>(data);
        // [-2^63, 2^63) are exactly the doubles that convert to int64 safely
        if (std::isfinite(d) && d == std::floor(d) && d >= -9223372036854775808.0 && d < 9223372036854775808.0)
            out = {true, static_cast<int64_t>(d), 0.0};
        else
            out = {false, 0, d};
        return true;
    }
    return false;
}

size_t hashOne(const Value& v) {
    NumKey n;
    if (numericKey(v, n)) return n.isInt ? std::hash<int64_t>{}(n.i) : std::hash<double>{}(n.d);
    const auto& data = v.data;
    if (std::holds_alternative<std::string>(data)) return std::hash<std::string>{}(std::get<std::string>(data));
    if (std::holds_alternative<Date>(data)) return std::hash<int32_t>{}(std::get<Date>(data).toOrdinal()) ^ 0x5bd1e995u;
    return 0x9e3779b9u;  // KHALI (never stored, but hashable)
}

bool equalOne(const Value& a, const Value& b) {
    NumKey x, y;
    bool xn = numericKey(a, x), yn = numericKey(b, y);
    if (xn || yn) {
        if (!(xn && yn)) return false;
        if (x.isInt && y.isInt) return x.i == y.i;
        if (!x.isInt && !y.isInt) return x.d == y.d;
        return false;  // an integral number never equals a non-integral one
    }
    return a.data == b.data;  // string/Date/NULL: same alternative and value
}

void combine(size_t& h, size_t part) { h ^= part + 0x9e3779b9 + (h << 6) + (h >> 2); }

// The index key for `positions`, or nullopt when any part is KHALI (KHALI
// never participates in an index / uniqueness violation).
std::optional<std::vector<Value>> keyFor(const std::vector<size_t>& positions, const RowValues& values) {
    std::vector<Value> key;
    key.reserve(positions.size());
    for (size_t p : positions) {
        if (p >= values.size())
            throw ExecutionError("Row mein sirf " + std::to_string(values.size()) + " values hain, column #" +
                                 std::to_string(p) + " nahi mila");
        if (values[p].isNull()) return std::nullopt;
        key.push_back(values[p]);
    }
    return key;
}

}  // namespace

size_t ValueVecHash::operator()(const std::vector<Value>& key) const {
    size_t h = key.size();
    for (const auto& v : key) combine(h, hashOne(v));
    return h;
}

bool ValueVecEq::operator()(const std::vector<Value>& a, const std::vector<Value>& b) const {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (!equalOne(a[i], b[i])) return false;
    return true;
}

size_t VecSizeTHash::operator()(const std::vector<size_t>& v) const {
    size_t h = v.size();
    for (auto x : v) combine(h, std::hash<size_t>{}(x));
    return h;
}

// ============================================================================
// Table
// ============================================================================

Table::Table(TableSchema schema, std::string path, IndexCache cache)
    : schema_(std::move(schema)),
      heap_(std::move(path)),
      cache_(cache ? std::move(cache) : std::make_shared<std::optional<IndexMap>>()) {}

std::vector<StoredRow> Table::rows() const {
    std::vector<StoredRow> result;
    auto types = schema_.types();
    for (auto& [id, payload] : heap_.scan()) result.emplace_back(id, decodeRow(payload, types));
    return result;
}

std::optional<RowValues> Table::get(int64_t rowId) const {
    auto payload = heap_.read(rowId);
    if (!payload.has_value()) return std::nullopt;
    return decodeRow(*payload, schema_.types());
}

void Table::checkWidth(const RowValues& values) const {
    if (values.size() != schema_.columns.size())
        throw ExecutionError("Table '" + schema_.name + "' mein " + std::to_string(schema_.columns.size()) +
                             " columns hain, par row mein " + std::to_string(values.size()) + " values");
}

void Table::insertMany(const std::vector<RowValues>& newRows) {
    // A short row would be silently truncated by encodeRow (and break the
    // index keys); refuse it before anything reaches the file.
    for (const auto& values : newRows) checkWidth(values);
    auto types = schema_.types();
    std::vector<std::vector<uint8_t>> payloads;
    payloads.reserve(newRows.size());
    for (const auto& values : newRows) payloads.push_back(encodeRow(values, types));
    auto ids = heap_.insertMany(payloads);
    if (!cache_->has_value()) return;  // not built yet: it will be built from the file later
    for (size_t i = 0; i < newRows.size(); ++i) {
        for (auto& [positions, index] : **cache_) {
            if (auto key = keyFor(positions, newRows[i])) index[*key] = ids[i];
        }
    }
}

void Table::deleteMany(const std::vector<StoredRow>& oldRows) {
    for (const auto& row : oldRows) checkWidth(row.second);
    std::vector<int64_t> ids;
    ids.reserve(oldRows.size());
    for (const auto& row : oldRows) ids.push_back(row.first);
    heap_.deleteMany(ids);
    if (!cache_->has_value()) return;
    for (const auto& [rowId, values] : oldRows) {
        for (auto& [positions, index] : **cache_) {
            auto key = keyFor(positions, values);
            if (!key) continue;
            // only drop the entry if it still points at THIS row (an UPDATE
            // inserts the new version before deleting the old one)
            auto it = index.find(*key);
            if (it != index.end() && it->second == rowId) index.erase(it);
        }
    }
}

std::vector<std::vector<size_t>> Table::indexedGroups() const {
    std::vector<std::vector<size_t>> groups;
    for (size_t i = 0; i < schema_.columns.size(); ++i)
        if (schema_.columns[i].isUnique()) groups.push_back({i});
    auto addComposite = [&](const std::vector<std::string>& names) {
        std::vector<size_t> positions;
        for (const auto& n : names) positions.push_back(schema_.indexOf(n));
        groups.push_back(std::move(positions));
    };
    for (const auto& g : schema_.compositeUnique) addComposite(g);
    if (schema_.compositePk.has_value() && !schema_.compositePk->empty()) addComposite(*schema_.compositePk);
    return groups;
}

IndexMap& Table::indexes() {
    if (!cache_->has_value()) {
        IndexMap built;
        auto groups = indexedGroups();
        for (const auto& g : groups) built[g];
        if (!groups.empty()) {
            for (const auto& [rowId, values] : rows()) {
                for (const auto& g : groups) {
                    if (auto key = keyFor(g, values)) built[g][*key] = rowId;
                }
            }
        }
        *cache_ = std::move(built);
    }
    return **cache_;
}

std::vector<StoredRow> Table::lookup(size_t column, const Value& value) {
    IndexMap& idx = indexes();
    auto it = idx.find(std::vector<size_t>{column});
    if (it == idx.end())
        throw ExecutionError("Table '" + schema_.name + "' ke column #" + std::to_string(column) +
                             " par koi index nahi hai");
    auto hit = it->second.find(std::vector<Value>{value});
    if (hit == it->second.end()) return {};
    auto values = get(hit->second);
    if (!values.has_value()) return {};
    return {StoredRow{hit->second, std::move(*values)}};
}

void Table::invalidateIndexes() { cache_->reset(); }

// ============================================================================
// MaterializedTable
// ============================================================================

MaterializedTable::MaterializedTable(std::vector<RowValues> data)
    : MaterializedTable(TableSchema{}, std::move(data)) {}

MaterializedTable::MaterializedTable(TableSchema schema, std::vector<RowValues> data)
    : Table(std::move(schema), ""), data_(std::move(data)) {}

std::vector<StoredRow> MaterializedTable::rows() const {
    std::vector<StoredRow> result;
    result.reserve(data_.size());
    for (size_t i = 0; i < data_.size(); ++i) result.emplace_back(static_cast<int64_t>(i), data_[i]);
    return result;
}

std::optional<RowValues> MaterializedTable::get(int64_t rowId) const {
    if (rowId < 0 || static_cast<size_t>(rowId) >= data_.size()) return std::nullopt;
    return data_[static_cast<size_t>(rowId)];
}

namespace {
[[noreturn]] void viewIsReadOnly(const std::string& name) {
    throw ExecutionError("'" + name + "' ek VIEW hai, table nahi -- isme DAALO/BADLO/MITAO nahi kar sakte");
}
}  // namespace

void MaterializedTable::insertMany(const std::vector<RowValues>&) { viewIsReadOnly(schema().name); }
void MaterializedTable::deleteMany(const std::vector<StoredRow>&) { viewIsReadOnly(schema().name); }
IndexMap& MaterializedTable::indexes() {
    throw ExecutionError("VIEW '" + schema().name + "' par index nahi hota");
}
std::vector<StoredRow> MaterializedTable::lookup(size_t, const Value&) {
    throw ExecutionError("VIEW '" + schema().name + "' par index nahi hota");
}

}  // namespace meradb
