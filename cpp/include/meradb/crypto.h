// cpp/include/meradb/crypto.h
//
// SHA-256, HMAC-SHA-256 and PBKDF2-HMAC-SHA-256, written out in full so the
// password store needs no third-party library. Byte-compatible with Python's
// hashlib (verified by known-answer tests). Educational, not hardened: no
// constant-time guarantees beyond what a plain implementation gives.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace meradb::crypto {

using Bytes = std::vector<std::uint8_t>;

class Sha256 {
public:
    Sha256() { reset(); }
    void reset();
    void update(const std::uint8_t* data, std::size_t length);
    void update(const Bytes& data) { update(data.data(), data.size()); }
    // The 32-byte digest. The object must be reset() before it is used again.
    Bytes finish();

private:
    std::uint32_t h_[8];
    std::uint8_t buffer_[64];
    std::size_t bufferLength_ = 0;
    std::uint64_t totalLength_ = 0;
    void compress(const std::uint8_t* block);
};

// HMAC with the key schedule done once: mac() copies the two saved midstates,
// so PBKDF2's 100,000 iterations cost two compression calls each.
class HmacSha256 {
public:
    explicit HmacSha256(const Bytes& key);
    Bytes mac(const std::uint8_t* message, std::size_t length) const;
    Bytes mac(const Bytes& message) const { return mac(message.data(), message.size()); }

private:
    Sha256 inner_;  // state after absorbing key XOR 0x36
    Sha256 outer_;  // state after absorbing key XOR 0x5c
};

Bytes sha256(const Bytes& data);
Bytes sha256(const std::string& data);
Bytes hmacSha256(const Bytes& key, const Bytes& message);
// Python: hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"), salt, iterations, keyLength)
Bytes pbkdf2HmacSha256(const std::string& password, const Bytes& salt, std::uint32_t iterations,
                       std::size_t keyLength);

Bytes toBytes(const std::string& text);  // the raw bytes of a std::string (UTF-8 stays UTF-8)
std::string toHex(const Bytes& bytes);   // lowercase, like bytes.hex()
Bytes fromHex(const std::string& hex);   // either case; StorageError if malformed

}  // namespace meradb::crypto
