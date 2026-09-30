// cpp/src/crypto.cpp
#include "meradb/crypto.h"
#include "meradb/errors.h"
#include <algorithm>
#include <cstring>

namespace meradb::crypto {

namespace {

const std::uint32_t kRoundConstants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline std::uint32_t rotr(std::uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }

}  // namespace

void Sha256::reset() {
    static const std::uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::copy(init, init + 8, h_);
    bufferLength_ = 0;
    totalLength_ = 0;
}

void Sha256::compress(const std::uint8_t* block) {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<std::uint32_t>(block[4 * i]) << 24) | (static_cast<std::uint32_t>(block[4 * i + 1]) << 16) |
               (static_cast<std::uint32_t>(block[4 * i + 2]) << 8) | static_cast<std::uint32_t>(block[4 * i + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; ++i) {
        std::uint32_t bigS1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        std::uint32_t ch = (e & f) ^ (~e & g);
        std::uint32_t t1 = h + bigS1 + ch + kRoundConstants[i] + w[i];
        std::uint32_t bigS0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        std::uint32_t t2 = bigS0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
    h_[5] += f;
    h_[6] += g;
    h_[7] += h;
}

void Sha256::update(const std::uint8_t* data, std::size_t length) {
    totalLength_ += length;
    if (bufferLength_ > 0) {
        std::size_t take = std::min(length, sizeof buffer_ - bufferLength_);
        std::memcpy(buffer_ + bufferLength_, data, take);
        bufferLength_ += take;
        data += take;
        length -= take;
        if (bufferLength_ < sizeof buffer_) return;
        compress(buffer_);
        bufferLength_ = 0;
    }
    while (length >= 64) {
        compress(data);
        data += 64;
        length -= 64;
    }
    if (length > 0) {
        std::memcpy(buffer_, data, length);
        bufferLength_ = length;
    }
}

Bytes Sha256::finish() {
    const std::uint64_t bits = totalLength_ * 8;
    std::uint8_t padding[72] = {0x80};
    std::size_t paddingLength = bufferLength_ < 56 ? 56 - bufferLength_ : 120 - bufferLength_;
    update(padding, paddingLength);
    std::uint8_t lengthBytes[8];
    for (int i = 0; i < 8; ++i) lengthBytes[i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
    update(lengthBytes, 8);
    Bytes out(32);
    for (int i = 0; i < 8; ++i) {
        out[4 * i] = static_cast<std::uint8_t>(h_[i] >> 24);
        out[4 * i + 1] = static_cast<std::uint8_t>(h_[i] >> 16);
        out[4 * i + 2] = static_cast<std::uint8_t>(h_[i] >> 8);
        out[4 * i + 3] = static_cast<std::uint8_t>(h_[i]);
    }
    return out;
}

HmacSha256::HmacSha256(const Bytes& key) {
    Bytes block = key.size() > 64 ? sha256(key) : key;
    block.resize(64, 0);
    Bytes ipad(64), opad(64);
    for (std::size_t i = 0; i < 64; ++i) {
        ipad[i] = static_cast<std::uint8_t>(block[i] ^ 0x36);
        opad[i] = static_cast<std::uint8_t>(block[i] ^ 0x5c);
    }
    inner_.update(ipad);
    outer_.update(opad);
}

Bytes HmacSha256::mac(const std::uint8_t* message, std::size_t length) const {
    Sha256 inner = inner_;  // copy = start from the midstate
    inner.update(message, length);
    Bytes innerDigest = inner.finish();
    Sha256 outer = outer_;
    outer.update(innerDigest);
    return outer.finish();
}

Bytes sha256(const Bytes& data) {
    Sha256 h;
    h.update(data);
    return h.finish();
}

Bytes sha256(const std::string& data) { return sha256(toBytes(data)); }

Bytes hmacSha256(const Bytes& key, const Bytes& message) { return HmacSha256(key).mac(message); }

Bytes pbkdf2HmacSha256(const std::string& password, const Bytes& salt, std::uint32_t iterations,
                       std::size_t keyLength) {
    if (iterations == 0) throw StorageError("PBKDF2: iterations kam se kam 1 chahiye");
    HmacSha256 prf(toBytes(password));
    Bytes out;
    out.reserve(keyLength);
    for (std::uint32_t block = 1; out.size() < keyLength; ++block) {
        Bytes message = salt;
        message.push_back(static_cast<std::uint8_t>(block >> 24));
        message.push_back(static_cast<std::uint8_t>(block >> 16));
        message.push_back(static_cast<std::uint8_t>(block >> 8));
        message.push_back(static_cast<std::uint8_t>(block));
        Bytes u = prf.mac(message);
        Bytes t = u;
        for (std::uint32_t i = 1; i < iterations; ++i) {
            u = prf.mac(u);
            for (std::size_t j = 0; j < t.size(); ++j) t[j] ^= u[j];
        }
        std::size_t take = std::min<std::size_t>(t.size(), keyLength - out.size());
        out.insert(out.end(), t.begin(), t.begin() + static_cast<std::ptrdiff_t>(take));
    }
    return out;
}

Bytes toBytes(const std::string& text) { return Bytes(text.begin(), text.end()); }

std::string toHex(const Bytes& bytes) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (std::uint8_t b : bytes) {
        out += digits[b >> 4];
        out += digits[b & 0x0f];
    }
    return out;
}

Bytes fromHex(const std::string& hex) {
    if (hex.size() % 2 != 0) throw StorageError("hex string ki length odd hai");
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        throw StorageError("hex string mein galat character hai");
    };
    Bytes out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        int high = nibble(hex[i]);
        int low = nibble(hex[i + 1]);
        out.push_back(static_cast<std::uint8_t>((high << 4) | low));
    }
    return out;
}

}  // namespace meradb::crypto
