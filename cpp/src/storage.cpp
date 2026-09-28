// cpp/src/storage.cpp
#include "meradb/storage.h"
#include "meradb/errors.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;

namespace meradb {

namespace {

constexpr char kMagic[] = "MERADB01";
constexpr size_t kMagicLen = 8;
constexpr size_t kRecordHeader = 5;  // status (1) + payload length (4)
constexpr uint8_t kStatusLive = 1;
constexpr uint8_t kStatusDeleted = 0;
constexpr uint8_t kTagNull = 0;
constexpr uint8_t kTagValue = 1;
constexpr int32_t kMaxDateOrdinal = 3652059;  // date(9999, 12, 31).toordinal()

void putU32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}
uint32_t getU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
void putU64(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}
uint64_t getU64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}
void putDouble(std::vector<uint8_t>& out, double d) {
    uint64_t bits;
    std::memcpy(&bits, &d, sizeof bits);
    putU64(out, bits);
}
double getDouble(const uint8_t* p) {
    uint64_t bits = getU64(p);
    double d;
    std::memcpy(&d, &bits, sizeof d);
    return d;
}

void appendRecord(std::vector<uint8_t>& out, const std::vector<uint8_t>& payload) {
    out.push_back(kStatusLive);
    putU32(out, static_cast<uint32_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
}

[[noreturn]] void wrongValue(const std::string& type, const Value& v) {
    throw StorageError(type + " column mein " + formatValue(v) + " store nahi ho sakta");
}

std::vector<uint8_t> readWholeFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw StorageError(path + " khul nahi paayi");
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void writeBytes(std::ofstream& out, const std::vector<uint8_t>& bytes) {
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

}  // namespace

// ============================================================================
// 1. Record codec
// ============================================================================

std::vector<uint8_t> encodeRow(const std::vector<Value>& values, const std::vector<std::string>& types) {
    std::vector<uint8_t> out;
    size_t n = std::min(values.size(), types.size());  // Python zip() semantics
    for (size_t i = 0; i < n; ++i) {
        const Value& v = values[i];
        const std::string& type = types[i];
        if (v.isNull()) {
            out.push_back(kTagNull);
            continue;
        }
        out.push_back(kTagValue);
        const auto& d = v.data;
        if (type == "INT") {
            if (std::holds_alternative<int64_t>(d)) putU64(out, static_cast<uint64_t>(std::get<int64_t>(d)));
            else if (std::holds_alternative<bool>(d)) putU64(out, std::get<bool>(d) ? 1 : 0);
            else wrongValue(type, v);
        } else if (type == "FLOAT") {
            if (std::holds_alternative<double>(d)) putDouble(out, std::get<double>(d));
            else if (std::holds_alternative<int64_t>(d)) putDouble(out, static_cast<double>(std::get<int64_t>(d)));
            else if (std::holds_alternative<bool>(d)) putDouble(out, std::get<bool>(d) ? 1.0 : 0.0);
            else wrongValue(type, v);
        } else if (type == "BOOL") {
            if (std::holds_alternative<bool>(d)) out.push_back(std::get<bool>(d) ? 1 : 0);
            else if (std::holds_alternative<int64_t>(d)) out.push_back(std::get<int64_t>(d) != 0 ? 1 : 0);
            else wrongValue(type, v);
        } else if (type == "TEXT") {
            if (!std::holds_alternative<std::string>(d)) wrongValue(type, v);
            const std::string& s = std::get<std::string>(d);
            putU32(out, static_cast<uint32_t>(s.size()));
            out.insert(out.end(), s.begin(), s.end());
        } else if (type == "DATE") {
            if (!std::holds_alternative<Date>(d)) wrongValue(type, v);
            putU32(out, static_cast<uint32_t>(std::get<Date>(d).toOrdinal()));
        } else {
            throw StorageError("Unknown type " + type);
        }
    }
    return out;
}

std::vector<Value> decodeRow(const std::vector<uint8_t>& payload, const std::vector<std::string>& types) {
    std::vector<Value> values;
    values.reserve(types.size());
    size_t i = 0;
    auto need = [&](size_t bytes) {
        if (i > payload.size() || payload.size() - i < bytes)
            throw StorageError("Row decode nahi hua: payload adhoora hai");
    };
    for (const auto& type : types) {
        need(1);
        uint8_t tag = payload[i++];
        if (tag == kTagNull) {
            values.emplace_back();
        } else if (type == "INT") {
            need(8);
            values.emplace_back(static_cast<int64_t>(getU64(&payload[i])));
            i += 8;
        } else if (type == "FLOAT") {
            need(8);
            values.emplace_back(getDouble(&payload[i]));
            i += 8;
        } else if (type == "BOOL") {
            need(1);
            values.emplace_back(payload[i] == 1);
            i += 1;
        } else if (type == "TEXT") {
            need(4);
            uint32_t len = getU32(&payload[i]);
            i += 4;
            need(len);
            values.emplace_back(std::string(reinterpret_cast<const char*>(payload.data() + i), len));
            i += len;
        } else if (type == "DATE") {
            need(4);
            int32_t ordinal = static_cast<int32_t>(getU32(&payload[i]));
            // date(1, 1, 1) .. date(9999, 12, 31); Python's date.fromordinal raises outside it
            if (ordinal < 1 || ordinal > kMaxDateOrdinal)
                throw StorageError("Row decode nahi hua: DATE ordinal " + std::to_string(ordinal) +
                                   " range (1.." + std::to_string(kMaxDateOrdinal) + ") ke bahar hai");
            values.emplace_back(Date::fromOrdinal(ordinal));
            i += 4;
        } else {
            throw StorageError("Unknown type " + type);
        }
    }
    return values;
}

// ============================================================================
// 2. Heap file
// ============================================================================

HeapFile::HeapFile(std::string path) : path_(std::move(path)) {}

void HeapFile::create() {
    std::ofstream out(path_, std::ios::binary | std::ios::trunc);
    if (!out) throw StorageError(path_ + " ban nahi paayi");
    out.write(kMagic, static_cast<std::streamsize>(kMagicLen));
    if (!out) throw StorageError(path_ + " likh nahi paaye");
}

void HeapFile::destroy() {
    std::error_code ec;
    fs::remove(path_, ec);
    if (ec) throw StorageError(path_ + " hata nahi paaye: " + ec.message());
}

void HeapFile::truncate() { create(); }

int64_t HeapFile::insert(const std::vector<uint8_t>& payload) { return insertMany({payload})[0]; }

std::vector<int64_t> HeapFile::insertMany(const std::vector<std::vector<uint8_t>>& payloads) {
    // One open + one write for the whole batch (Python's insert_many does the same).
    std::error_code ec;
    auto size = fs::file_size(path_, ec);
    int64_t offset = ec ? 0 : static_cast<int64_t>(size);  // "ab" creates a missing file

    std::vector<int64_t> offsets;
    offsets.reserve(payloads.size());
    std::vector<uint8_t> buffer;
    for (const auto& payload : payloads) {
        offsets.push_back(offset + static_cast<int64_t>(buffer.size()));
        appendRecord(buffer, payload);
    }
    std::ofstream out(path_, std::ios::binary | std::ios::app);
    if (!out) throw StorageError(path_ + " khul nahi paayi");
    writeBytes(out, buffer);
    out.flush();
    if (!out) throw StorageError(path_ + " mein likh nahi paaye");
    return offsets;
}

void HeapFile::deleteOne(int64_t offset) { deleteMany({offset}); }

void HeapFile::deleteMany(const std::vector<int64_t>& offsets) {
    std::fstream f(path_, std::ios::binary | std::ios::in | std::ios::out);
    if (!f) throw StorageError(path_ + " khul nahi paayi");
    const char tombstone = static_cast<char>(kStatusDeleted);
    for (auto offset : offsets) {
        f.seekp(offset, std::ios::beg);
        f.write(&tombstone, 1);  // one byte, in place
    }
    f.flush();
    if (!f) throw StorageError(path_ + " mein likh nahi paaye");
}

std::optional<std::vector<uint8_t>> HeapFile::read(int64_t offset) const {
    std::ifstream f(path_, std::ios::binary);
    if (!f) throw StorageError(path_ + " khul nahi paayi");
    uint8_t header[kRecordHeader];
    if (offset >= 0) {
        f.seekg(offset, std::ios::beg);
        f.read(reinterpret_cast<char*>(header), kRecordHeader);
    }
    if (offset < 0 || f.gcount() < static_cast<std::streamsize>(kRecordHeader))
        throw StorageError("Row id " + std::to_string(offset) + " file ke bahar hai");
    if (header[0] != kStatusLive) return std::nullopt;
    uint32_t len = getU32(header + 1);
    // check a (possibly corrupt) length against what's left BEFORE allocating
    std::streamoff here = f.tellg();
    f.seekg(0, std::ios::end);
    std::streamoff remaining = f.tellg() - here;
    if (static_cast<std::streamoff>(len) > remaining) throw StorageError(path_ + " corrupt hai: record adhoora hai");
    f.seekg(here, std::ios::beg);
    std::vector<uint8_t> payload(len);
    f.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(len));
    if (f.gcount() < static_cast<std::streamsize>(len))
        throw StorageError(path_ + " corrupt hai: record adhoora hai");
    return payload;
}

std::vector<std::pair<int64_t, std::vector<uint8_t>>> HeapFile::scan() const {
    std::vector<uint8_t> bytes = readWholeFile(path_);
    if (bytes.size() < kMagicLen || std::memcmp(bytes.data(), kMagic, kMagicLen) != 0)
        throw StorageError(path_ + " MeraDB ki file nahi hai (magic header galat)");

    std::vector<std::pair<int64_t, std::vector<uint8_t>>> result;
    size_t pos = kMagicLen;
    while (pos < bytes.size()) {
        if (bytes.size() - pos < kRecordHeader)
            throw StorageError(path_ + " corrupt hai: record header adhoora hai");
        uint8_t status = bytes[pos];
        uint32_t len = getU32(&bytes[pos + 1]);
        size_t start = pos + kRecordHeader;
        if (bytes.size() - start < len) throw StorageError(path_ + " corrupt hai: record adhoora hai");
        if (status == kStatusLive)
            result.emplace_back(static_cast<int64_t>(pos),
                                std::vector<uint8_t>(bytes.begin() + static_cast<std::ptrdiff_t>(start),
                                                     bytes.begin() + static_cast<std::ptrdiff_t>(start + len)));
        pos = start + len;
    }
    return result;
}

void HeapFile::rewrite(const std::vector<std::vector<uint8_t>>& payloads) {
    // Write a complete new file next to the old one, then swap it in
    // (Python: os.replace). A crash half-way leaves the old file untouched.
    std::vector<uint8_t> buffer(kMagic, kMagic + kMagicLen);
    for (const auto& payload : payloads) appendRecord(buffer, payload);

    std::string tmp = path_ + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) throw StorageError(tmp + " ban nahi paayi");
        writeBytes(out, buffer);
        out.flush();
        if (!out) throw StorageError(tmp + " mein likh nahi paaye");
    }
    std::error_code ec;
    fs::rename(tmp, path_, ec);
    if (ec) throw StorageError(path_ + " rewrite nahi hua: " + ec.message());
}

void HeapFile::compact() {
    std::vector<std::vector<uint8_t>> live;
    for (auto& record : scan()) live.push_back(std::move(record.second));
    rewrite(live);
}

}  // namespace meradb
