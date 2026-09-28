// cpp/tests/test_storage.cpp
#include <catch2/catch_test_macros.hpp>
#include "meradb/errors.h"
#include "meradb/storage.h"
#include "test_util.h"
#include <filesystem>
#include <fstream>
#include <iterator>

using namespace meradb;
using meradb_test::TempDir;

namespace {
std::vector<uint8_t> readBytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
void writeBytes(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}
}  // namespace

TEST_CASE("encodeRow/decodeRow round-trips every type including NULL", "[storage]") {
    std::vector<std::string> types = {"INT", "FLOAT", "TEXT", "BOOL", "DATE", "DATE"};
    std::vector<Value> values = {Value(int64_t{42}), Value(3.5), Value(std::string("hi")), Value(true),
                                 Value(parseDate("2024-01-31")), Value()};
    auto decoded = decodeRow(encodeRow(values, types), types);
    REQUIRE(decoded.size() == 6);
    REQUIRE(std::get<int64_t>(decoded[0].data) == 42);
    REQUIRE(std::get<double>(decoded[1].data) == 3.5);
    REQUIRE(std::get<std::string>(decoded[2].data) == "hi");
    REQUIRE(std::get<bool>(decoded[3].data) == true);
    REQUIRE(std::get<Date>(decoded[4].data).isoFormat() == "2024-01-31");
    REQUIRE(decoded[5].isNull());
}

TEST_CASE("encodeRow produces exactly the bytes Python's encode_row does", "[storage]") {
    // meradb.storage.encode_row([-2, 3.5, 'hi' + chr(0xe9), True, date(2004, 5, 6), None],
    //                           ['INT', 'FLOAT', 'TEXT', 'BOOL', 'DATE', 'INT'])
    const std::vector<uint8_t> expected = {
        0x01, 0xfe, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,  // INT -2
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x40,  // FLOAT 3.5
        0x01, 0x04, 0x00, 0x00, 0x00, 0x68, 0x69, 0xc3, 0xa9,  // TEXT 'hi' + e-acute (4 UTF-8 bytes)
        0x01, 0x01,                                            // BOOL SACH
        0x01, 0x3b, 0x2a, 0x0b, 0x00,                          // DATE ordinal 731707
        0x00};                                                 // KHALI
    std::vector<std::string> types = {"INT", "FLOAT", "TEXT", "BOOL", "DATE", "INT"};
    std::vector<Value> values = {Value(int64_t{-2}), Value(3.5), Value(std::string("hi\xC3\xA9")), Value(true),
                                 Value(parseDate("2004-05-06")), Value()};
    REQUIRE(encodeRow(values, types) == expected);
    auto back = decodeRow(expected, types);
    REQUIRE(std::get<int64_t>(back[0].data) == -2);
    REQUIRE(std::get<Date>(back[4].data).toOrdinal() == 731707);
}

TEST_CASE("encodeRow widens INT to FLOAT and rejects wrong value kinds", "[storage]") {
    auto bytes = encodeRow({Value(int64_t{2})}, {"FLOAT"});
    REQUIRE(std::get<double>(decodeRow(bytes, {"FLOAT"})[0].data) == 2.0);
    REQUIRE_THROWS_AS(encodeRow({Value(std::string("x"))}, {"INT"}), StorageError);
    REQUIRE_THROWS_AS(encodeRow({Value(int64_t{1})}, {"BLOB"}), StorageError);
}

TEST_CASE("decodeRow reports a truncated payload instead of reading past it", "[storage]") {
    REQUIRE_THROWS_AS(decodeRow({0x01, 0x01, 0x02}, {"INT"}), StorageError);
    REQUIRE_THROWS_AS(decodeRow({}, {"INT"}), StorageError);
}

TEST_CASE("HeapFile inserts and reads rows back by row id", "[storage]") {
    TempDir dir;
    HeapFile hf(dir.file("t.tbl"));
    hf.create();
    auto id1 = hf.insert({1, 2, 3});
    auto id2 = hf.insert({4, 5});
    REQUIRE(id1 == 8);  // right after the 8-byte magic header
    REQUIRE(id2 == 16);
    REQUIRE(hf.read(id1).value() == std::vector<uint8_t>{1, 2, 3});
    REQUIRE(hf.read(id2).value() == std::vector<uint8_t>{4, 5});
    REQUIRE_THROWS_AS(hf.read(1000), StorageError);
}

TEST_CASE("HeapFile writes the same bytes as Python's HeapFile", "[storage]") {
    TempDir dir;
    HeapFile hf(dir.file("t.tbl"));
    hf.create();
    auto ids = hf.insertMany({{1, 2, 3}, {4, 5}});
    hf.deleteOne(ids[0]);
    // Python: create(); insert(b'\x01\x02\x03'); insert(b'\x04\x05'); delete(8)
    const std::vector<uint8_t> expected = {'M', 'E', 'R', 'A', 'D', 'B', '0', '1', 0x00, 0x03, 0x00, 0x00,
                                           0x00, 0x01, 0x02, 0x03, 0x01, 0x02, 0x00, 0x00, 0x00, 0x04, 0x05};
    REQUIRE(readBytes(hf.path()) == expected);
}

TEST_CASE("HeapFile reads a file written by Python", "[storage]") {
    TempDir dir;
    std::string path = dir.file("py.tbl");
    writeBytes(path, {'M', 'E', 'R', 'A', 'D', 'B', '0', '1', 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x02,
                      0x03, 0x01, 0x02, 0x00, 0x00, 0x00, 0x04, 0x05});
    HeapFile hf(path);
    auto rows = hf.scan();
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0].first == 16);
    REQUIRE(rows[0].second == std::vector<uint8_t>{4, 5});
    REQUIRE_FALSE(hf.read(8).has_value());
}

TEST_CASE("HeapFile.scan yields only live records", "[storage]") {
    TempDir dir;
    HeapFile hf(dir.file("t.tbl"));
    hf.create();
    auto id1 = hf.insert({1});
    auto id2 = hf.insert({2});
    hf.deleteOne(id1);
    auto rows = hf.scan();
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0].first == id2);
}

TEST_CASE("HeapFile.scan rejects foreign and truncated files", "[storage]") {
    TempDir dir;
    std::string path = dir.file("bad.tbl");
    writeBytes(path, {'N', 'O', 'T', 'M', 'E', 'R', 'A', 'D'});
    REQUIRE_THROWS_AS(HeapFile(path).scan(), StorageError);
    writeBytes(path, {'M', 'E', 'R', 'A', 'D', 'B', '0', '1', 0x01, 0x05, 0x00});
    REQUIRE_THROWS_AS(HeapFile(path).scan(), StorageError);
    writeBytes(path, {'M', 'E', 'R', 'A', 'D', 'B', '0', '1', 0x01, 0x05, 0x00, 0x00, 0x00, 0x09});
    REQUIRE_THROWS_AS(HeapFile(path).scan(), StorageError);
}

TEST_CASE("HeapFile.read returns nullopt for a deleted row", "[storage]") {
    TempDir dir;
    HeapFile hf(dir.file("t.tbl"));
    hf.create();
    auto id = hf.insert({9});
    hf.deleteOne(id);
    REQUIRE_FALSE(hf.read(id).has_value());
}

TEST_CASE("HeapFile.compact drops tombstones and keeps live rows readable", "[storage]") {
    TempDir dir;
    HeapFile hf(dir.file("t.tbl"));
    hf.create();
    auto id1 = hf.insert({1});
    hf.insert({2});
    hf.deleteOne(id1);
    hf.compact();
    auto rows = hf.scan();
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0].first == 8);
    REQUIRE(rows[0].second == std::vector<uint8_t>{2});
    REQUIRE(std::filesystem::file_size(hf.path()) == 8 + 5 + 1);
}

TEST_CASE("HeapFile.rewrite is atomic: no .tmp left behind after success", "[storage]") {
    TempDir dir;
    HeapFile hf(dir.file("t.tbl"));
    hf.create();
    hf.insert({1});
    hf.rewrite({{7, 7, 7}});
    REQUIRE_FALSE(std::filesystem::exists(hf.path() + ".tmp"));
    auto rows = hf.scan();
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0].second == std::vector<uint8_t>{7, 7, 7});
}

TEST_CASE("HeapFile.truncate empties the file and destroy removes it", "[storage]") {
    TempDir dir;
    HeapFile hf(dir.file("t.tbl"));
    hf.create();
    hf.insertMany({{1}, {2}});
    hf.truncate();
    REQUIRE(hf.scan().empty());
    REQUIRE(std::filesystem::file_size(hf.path()) == 8);
    hf.destroy();
    REQUIRE_FALSE(std::filesystem::exists(hf.path()));
    hf.destroy();  // already gone: no error, like Python
}

TEST_CASE("decodeRow rejects a DATE ordinal Python's date.fromordinal would reject", "[storage]") {
    REQUIRE_THROWS_AS(decodeRow({0x01, 0x00, 0x00, 0x00, 0x00}, {"DATE"}), StorageError);  // ordinal 0
    REQUIRE_THROWS_AS(decodeRow({0x01, 0xff, 0xff, 0xff, 0xff}, {"DATE"}), StorageError);  // -1
    REQUIRE_THROWS_AS(decodeRow({0x01, 0xdc, 0xb9, 0x37, 0x00}, {"DATE"}), StorageError);  // 3652060
    // the bounds themselves are fine: 0001-01-01 and 9999-12-31
    REQUIRE(std::get<Date>(decodeRow({0x01, 0x01, 0x00, 0x00, 0x00}, {"DATE"})[0].data).isoFormat() == "0001-01-01");
    REQUIRE(std::get<Date>(decodeRow({0x01, 0xdb, 0xb9, 0x37, 0x00}, {"DATE"})[0].data).isoFormat() == "9999-12-31");
}

TEST_CASE("HeapFile.read checks a corrupt record length before allocating", "[storage]") {
    TempDir dir;
    std::string path = dir.file("t.tbl");
    // one live record claiming a 0xFFFFFFFF-byte payload, with 2 bytes present
    writeBytes(path, {'M', 'E', 'R', 'A', 'D', 'B', '0', '1', 0x01, 0xff, 0xff, 0xff, 0xff, 0x01, 0x02});
    HeapFile hf(path);
    REQUIRE_THROWS_AS(hf.read(8), StorageError);
    REQUIRE_THROWS_AS(hf.scan(), StorageError);
}
