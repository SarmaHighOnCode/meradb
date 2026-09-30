// cpp/tests/test_users.cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include "meradb/crypto.h"
#include "meradb/errors.h"
#include "meradb/users.h"
#include "test_util.h"
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

using namespace meradb;
using meradb_test::TempDir;
using meradb_test::readText;
namespace fs = std::filesystem;

namespace {
void writeFile(const fs::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary);
    out << text;
}
}  // namespace

TEST_CASE("users create stores salt and PBKDF2 hash, never the password", "[users]") {
    TempDir dir;
    UserStore users(dir.str());
    users.create("ravi", "hunter2");
    REQUIRE(users.exists("ravi"));
    REQUIRE_FALSE(users.exists("asha"));

    std::string text = readText(dir.file("users.json"));
    CHECK(text.find("hunter2") == std::string::npos);
    auto j = nlohmann::ordered_json::parse(text);
    REQUIRE(j.contains("ravi"));
    std::string saltHex = j["ravi"]["salt"];
    std::string hashHex = j["ravi"]["hash"];
    CHECK(saltHex.size() == 32);   // 16 random bytes
    CHECK(hashHex.size() == 64);   // 32-byte digest
    CHECK(hashHex == crypto::toHex(crypto::pbkdf2HmacSha256("hunter2", crypto::fromHex(saltHex), 100000, 32)));
    CHECK(j["ravi"]["grants"].is_object());
    CHECK(j["ravi"]["grants"].empty());
}

TEST_CASE("users verify accepts the right password only", "[users]") {
    TempDir dir;
    UserStore users(dir.str());
    users.create("ravi", "hunter2");
    CHECK(users.verify("ravi", "hunter2"));
    CHECK_FALSE(users.verify("ravi", "hunter3"));
    CHECK_FALSE(users.verify("ravi", ""));
    CHECK_FALSE(users.verify("nobody", "hunter2"));
}

TEST_CASE("users duplicate and missing names use Python's wording", "[users]") {
    TempDir dir;
    UserStore users(dir.str());
    users.create("ravi", "x");
    REQUIRE_THROWS_WITH(users.create("ravi", "y"), "[Execution Galti] User 'ravi' pehle se hai");
    REQUIRE_THROWS_WITH(users.drop("ghost"), "[Execution Galti] User 'ghost' exist nahi karta");
    REQUIRE_THROWS_WITH(users.grant("ghost", "main", "t", {"DIKHAO"}), "[Execution Galti] User 'ghost' exist nahi karta");
    REQUIRE_THROWS_WITH(users.revoke("ghost", "main", "t", {"DIKHAO"}), "[Execution Galti] User 'ghost' exist nahi karta");
    users.drop("ravi");
    CHECK_FALSE(users.exists("ravi"));
}

TEST_CASE("users grants are kept in DIKHAO/DAALO/BADLO/MITAO order and revoked cleanly", "[users]") {
    TempDir dir;
    UserStore users(dir.str());
    users.create("ravi", "x");
    users.grant("ravi", "main", "students", {"MITAO", "DIKHAO"});
    users.grant("ravi", "main", "students", {"BADLO", "DIKHAO"});
    CHECK(users.hasPrivilege("ravi", "main", "students", "DIKHAO"));
    CHECK(users.hasPrivilege("ravi", "main", "students", "MITAO"));
    CHECK_FALSE(users.hasPrivilege("ravi", "main", "students", "DAALO"));
    CHECK_FALSE(users.hasPrivilege("ravi", "other", "students", "DIKHAO"));   // keyed by database too
    CHECK_FALSE(users.hasPrivilege("nobody", "main", "students", "DIKHAO"));

    auto j = nlohmann::ordered_json::parse(readText(dir.file("users.json")));
    CHECK(j["ravi"]["grants"]["main.students"] == nlohmann::ordered_json::parse(R"(["DIKHAO","BADLO","MITAO"])"));

    users.revoke("ravi", "main", "students", {"DIKHAO", "BADLO"});
    users.revoke("ravi", "main", "students", {"MITAO"});
    j = nlohmann::ordered_json::parse(readText(dir.file("users.json")));
    CHECK(j["ravi"]["grants"].empty());  // the key disappears once nothing is left
    users.revoke("ravi", "main", "students", {"DAALO"});  // revoking what was never granted is fine
}

TEST_CASE("users file has exactly Python's json.dump(indent=2) shape", "[users]") {
    TempDir dir;
    writeFile(dir.path() / "users.json",
              "{\n"
              "  \"ravi\": {\n"
              "    \"salt\": \"000102030405060708090a0b0c0d0e0f\",\n"
              "    \"hash\": \"fbe79903d759b826e1d54bd9e4f8beb2de5a87ac86037c76328ad485fd894671\",\n"
              "    \"grants\": {\n"
              "      \"main.students\": [\n"
              "        \"DIKHAO\",\n"
              "        \"DAALO\"\n"
              "      ]\n"
              "    }\n"
              "  }\n"
              "}");
    // that file was written by the Python engine for password "pw": we must accept it ...
    UserStore users(dir.str());
    CHECK(users.verify("ravi", "pw"));
    CHECK_FALSE(users.verify("ravi", "PW"));
    CHECK(users.hasPrivilege("ravi", "main", "students", "DAALO"));

    // ... and write it back byte for byte (no trailing newline, 2-space indent, insertion order)
    users.grant("ravi", "main", "students", {"DIKHAO"});  // no change in content
    CHECK(readText(dir.file("users.json")) ==
          "{\n"
          "  \"ravi\": {\n"
          "    \"salt\": \"000102030405060708090a0b0c0d0e0f\",\n"
          "    \"hash\": \"fbe79903d759b826e1d54bd9e4f8beb2de5a87ac86037c76328ad485fd894671\",\n"
          "    \"grants\": {\n"
          "      \"main.students\": [\n"
          "        \"DIKHAO\",\n"
          "        \"DAALO\"\n"
          "      ]\n"
          "    }\n"
          "  }\n"
          "}");
}

TEST_CASE("users persist across instances and non-ASCII passwords work", "[users]") {
    TempDir dir;
    {
        UserStore users(dir.str());
        users.create("asha", "p\xC3\xA4ssw\xC3\xB6rd");
    }
    UserStore again(dir.str());
    CHECK(again.exists("asha"));
    CHECK(again.verify("asha", "p\xC3\xA4ssw\xC3\xB6rd"));
    CHECK_FALSE(again.verify("asha", "passwoerd"));
    CHECK(again.names() == std::vector<std::string>{"asha"});
    // no leftover temp file
    CHECK_FALSE(fs::exists(dir.path() / "users.json.tmp"));
}

TEST_CASE("users corrupt file is a StorageError, not a crash", "[users]") {
    TempDir dir;
    writeFile(dir.path() / "users.json", "{ not json");
    REQUIRE_THROWS_AS(UserStore(dir.str()), StorageError);
}

namespace {
// Makes every save fail: users.json becomes a non-empty directory, so renaming the temp file
// onto it is refused.
void blockSaves(const fs::path& dir) {
    fs::path target = dir / "users.json";
    fs::remove(target);
    fs::create_directories(target / "blocker");
}
}  // namespace

TEST_CASE("users failed save removes the leftover temp file", "[users]") {
    TempDir dir;
    UserStore users(dir.str());
    blockSaves(dir.path());
    CHECK_THROWS_AS(users.create("ravi", "pw"), StorageError);
    CHECK_FALSE(fs::exists(dir.path() / "users.json.tmp"));
}

TEST_CASE("users create, drop, grant and revoke roll back when the save fails", "[users]") {
    TempDir dir;
    UserStore users(dir.str());
    users.create("ravi", "pw");
    users.grant("ravi", "main", "students", {"DIKHAO", "DAALO"});
    blockSaves(dir.path());

    CHECK_THROWS_AS(users.create("asha", "pw"), StorageError);
    CHECK_FALSE(users.exists("asha"));

    CHECK_THROWS_AS(users.drop("ravi"), StorageError);
    CHECK(users.exists("ravi"));
    CHECK(users.verify("ravi", "pw"));

    CHECK_THROWS_AS(users.grant("ravi", "main", "students", {"BADLO"}), StorageError);
    CHECK_FALSE(users.hasPrivilege("ravi", "main", "students", "BADLO"));
    CHECK(users.hasPrivilege("ravi", "main", "students", "DIKHAO"));

    CHECK_THROWS_AS(users.revoke("ravi", "main", "students", {"DIKHAO", "DAALO"}), StorageError);
    CHECK(users.hasPrivilege("ravi", "main", "students", "DIKHAO"));
    CHECK(users.hasPrivilege("ravi", "main", "students", "DAALO"));
}

TEST_CASE("users grant/revoke on a hand-edited malformed entry give a clean error", "[users]") {
    for (const char* bad : {R"({"ravi": "oops"})", R"({"ravi": [1, 2]})",
                            R"({"ravi": {"salt": "00", "hash": "00", "grants": "oops"}})",
                            R"({"ravi": {"salt": "00", "hash": "00", "grants": [1]}})"}) {
        TempDir dir;
        writeFile(dir.path() / "users.json", bad);
        UserStore users(dir.str());
        CAPTURE(bad);
        CHECK_THROWS_AS(users.grant("ravi", "main", "t", {"DIKHAO"}), StorageError);
        CHECK_THROWS_AS(users.revoke("ravi", "main", "t", {"DIKHAO"}), StorageError);
        CHECK(users.exists("ravi"));  // untouched
        CHECK_FALSE(users.verify("ravi", "pw"));
        CHECK_FALSE(fs::exists(dir.path() / "users.json.tmp"));
    }
}

TEST_CASE("users verify rejects near-miss hashes and passwords", "[users]") {
    TempDir dir;
    UserStore users(dir.str());
    users.create("ravi", "hunter2");
    CHECK(users.verify("ravi", "hunter2"));
    CHECK_FALSE(users.verify("ravi", "hunter3"));
    CHECK_FALSE(users.verify("ravi", ""));
    CHECK_FALSE(users.verify("nobody", "hunter2"));
}
