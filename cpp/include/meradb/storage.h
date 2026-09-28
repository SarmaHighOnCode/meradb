// cpp/include/meradb/storage.h
//
// STORAGE: how rows live on disk. Byte-for-byte the same format as
// meradb/storage.py, so a .tbl file written by one engine is readable by
// the other.
//
// Record codec, per column value (types come from the catalog, never the row):
//   0x00                  -> KHALI (NULL), nothing follows
//   0x01 + 8 bytes        -> INT   signed little-endian            ('<q')
//   0x01 + 8 bytes        -> FLOAT IEEE-754 double little-endian  ('<d')
//   0x01 + 1 byte         -> BOOL  0 or 1
//   0x01 + u32 len + N    -> TEXT  N UTF-8 bytes                   ('<I')
//   0x01 + 4 bytes        -> DATE  signed proleptic ordinal        ('<i')
//
// Heap file (<table>.tbl): "MERADB01" magic, then records of
//   status (1 byte: 1 live, 0 tombstone) + u32 LE payload length + payload.
// A row id is the byte offset of its record's status byte.
#pragma once
#include "meradb/datatypes.h"
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace meradb {

std::vector<uint8_t> encodeRow(const std::vector<Value>& values, const std::vector<std::string>& types);
std::vector<Value> decodeRow(const std::vector<uint8_t>& payload, const std::vector<std::string>& types);

class HeapFile {
public:
    explicit HeapFile(std::string path);

    const std::string& path() const { return path_; }

    // ---- lifecycle ----
    void create();    // new file holding only the magic header (overwrites)
    void destroy();   // remove the file if it exists
    void truncate();  // drop every row, keep the (empty) file

    // ---- row operations ----
    int64_t insert(const std::vector<uint8_t>& payload);  // returns the row id
    std::vector<int64_t> insertMany(const std::vector<std::vector<uint8_t>>& payloads);
    void deleteOne(int64_t offset);  // flips the status byte to a tombstone
    void deleteMany(const std::vector<int64_t>& offsets);
    // nullopt for a deleted row; StorageError if the offset is past the end
    std::optional<std::vector<uint8_t>> read(int64_t offset) const;
    // (row id, payload) of every LIVE record, in file order
    std::vector<std::pair<int64_t, std::vector<uint8_t>>> scan() const;
    // atomic: writes <path>.tmp, then renames it over the file
    void rewrite(const std::vector<std::vector<uint8_t>>& payloads);
    void compact();  // rewrite without the tombstones

private:
    std::string path_;
};

}  // namespace meradb
