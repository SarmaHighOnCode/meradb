// cpp/tests/test_tokenizer_unicode.cpp -- columns and error characters for non-ASCII source text.
// Found by the interop matrix (Task 23): Python counts characters, Phase 1 counted UTF-8 bytes.
#include <catch2/catch_test_macros.hpp>
#include "meradb/errors.h"
#include "meradb/tokenizer.h"
#include <string>

using namespace meradb;

namespace {
std::string errorOf(const std::string& source) {
    try {
        tokenize(source);
    } catch (const TokenizerError& e) {
        return e.message();
    }
    return "(no error)";
}
}  // namespace

TEST_CASE("tokenizer_unicode columns after a multi-byte string count characters", "[tokenizer][unicode]") {
    // 'caf<e-acute>' @  -> quote, c, a, f, e-acute, quote, space, @ : the @ is character 8 (bytes would say 9)
    CHECK(errorOf("'caf\xC3\xA9' @") == "Ye character samajh nahi aaya: '@' (line 1, col 8)");
    // two 4-byte characters (emoji) are two columns, not eight
    CHECK(errorOf("'\xF0\x9F\x98\x80\xF0\x9F\x98\x80' @") == "Ye character samajh nahi aaya: '@' (line 1, col 6)");
    // a line break resets the column as before
    CHECK(errorOf("'\xC3\xA9'\n  @") == "Ye character samajh nahi aaya: '@' (line 2, col 3)");
}

TEST_CASE("tokenizer_unicode an unexpected non-ASCII character is shown whole", "[tokenizer][unicode]") {
    CHECK(errorOf("DIKHAO \xC2\xA3") == "Ye character samajh nahi aaya: '\xC2\xA3' (line 1, col 8)");        // pound sign
    CHECK(errorOf("x \xE2\x82\xAC") == "Ye character samajh nahi aaya: '\xE2\x82\xAC' (line 1, col 3)");     // euro sign
    CHECK(errorOf("\xF0\x9F\x98\x80") == "Ye character samajh nahi aaya: '\xF0\x9F\x98\x80' (line 1, col 1)");  // emoji
}

TEST_CASE("tokenizer_unicode non-ASCII text inside strings is kept byte for byte", "[tokenizer][unicode]") {
    auto tokens = tokenize("'caf\xC3\xA9 \xE4\xB8\x96\xE7\x95\x8C'");
    REQUIRE(tokens.size() == 2);
    CHECK(tokens[0].textValue == "caf\xC3\xA9 \xE4\xB8\x96\xE7\x95\x8C");
    CHECK(tokens[0].col == 1);
}
