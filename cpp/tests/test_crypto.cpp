// cpp/tests/test_crypto.cpp
#include <catch2/catch_test_macros.hpp>
#include "meradb/crypto.h"
#include "meradb/errors.h"

using namespace meradb;
using namespace meradb::crypto;

TEST_CASE("crypto sha256 known answers", "[crypto]") {
    CHECK(toHex(sha256(std::string(""))) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(toHex(sha256(std::string("abc"))) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(toHex(sha256(std::string("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))) ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST_CASE("crypto sha256 streaming equals one-shot at every split", "[crypto]") {
    std::string data(200, 'a');
    const std::string expected = "c2a908d98f5df987ade41b5fce213067efbcc21ef2240212a41e54b5e7c28ae5";
    CHECK(toHex(sha256(data)) == expected);
    for (size_t split : {1u, 55u, 56u, 63u, 64u, 65u, 128u, 199u}) {
        Sha256 h;
        h.update(reinterpret_cast<const uint8_t*>(data.data()), split);
        h.update(reinterpret_cast<const uint8_t*>(data.data()) + split, data.size() - split);
        CHECK(toHex(h.finish()) == expected);
    }
}

TEST_CASE("crypto hmac-sha256 RFC 4231 vectors", "[crypto]") {
    Bytes key1(20, 0x0b);
    CHECK(toHex(hmacSha256(key1, toBytes("Hi There"))) ==
          "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    CHECK(toHex(hmacSha256(toBytes("Jefe"), toBytes("what do ya want for nothing?"))) ==
          "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    // a key longer than the 64-byte block is hashed first
    Bytes longKey(131, 0xaa);
    Bytes shortKey = sha256(longKey);
    CHECK(hmacSha256(longKey, toBytes("x")) == hmacSha256(shortKey, toBytes("x")));
    // RFC 4231 test case 6 (131-byte key)
    CHECK(toHex(hmacSha256(longKey, toBytes("Test Using Larger Than Block-Size Key - Hash Key First"))) ==
          "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
}

TEST_CASE("crypto pbkdf2-hmac-sha256 known answers", "[crypto]") {
    Bytes salt = toBytes("salt");
    CHECK(toHex(pbkdf2HmacSha256("password", salt, 1, 32)) ==
          "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
    CHECK(toHex(pbkdf2HmacSha256("password", salt, 2, 32)) ==
          "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43");
    CHECK(toHex(pbkdf2HmacSha256("password", salt, 4096, 32)) ==
          "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a");
    // two output blocks (dkLen = 64), RFC 7914 section 11
    CHECK(toHex(pbkdf2HmacSha256("passwd", salt, 1, 64)) ==
          "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc"
          "49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783");
}

TEST_CASE("crypto pbkdf2 matches Python at MeraDB's 100000 iterations", "[crypto]") {
    Bytes salt;
    for (uint8_t i = 0; i < 16; ++i) salt.push_back(i);
    CHECK(toHex(pbkdf2HmacSha256("pw", salt, 100000, 32)) ==
          "fbe79903d759b826e1d54bd9e4f8beb2de5a87ac86037c76328ad485fd894671");
    // UTF-8 bytes of the password are hashed, like password.encode("utf-8")
    CHECK(toHex(pbkdf2HmacSha256("p\xC3\xA4ssw\xC3\xB6rd", salt, 100000, 32)) ==
          "b6b9a7d7879a26a7ad99c592249c145952ca50ec9fefae618844337989ef6a53");
}

TEST_CASE("crypto hex helpers round-trip and validate", "[crypto]") {
    Bytes b = {0x00, 0x0f, 0xa5, 0xff};
    CHECK(toHex(b) == "000fa5ff");
    CHECK(fromHex("000fa5ff") == b);
    CHECK(fromHex("000FA5FF") == b);
    CHECK_THROWS_AS(fromHex("abc"), StorageError);   // odd length
    CHECK_THROWS_AS(fromHex("zz"), StorageError);    // not hex
}
