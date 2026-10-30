// cpp/tests/test_tls.cpp -- encrypted connections, end to end through a real server on 127.0.0.1.
#include "meradb/client.h"
#include "meradb/errors.h"
#include "meradb/fs_util.h"
#include "meradb/net_compat.h"
#include "meradb/protocol.h"
#include "meradb/server.h"
#include "meradb/tls.h"
#include "server_fixture.h"
#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <aclapi.h>
#endif

using namespace meradb;
using namespace meradb_test;
namespace fs = std::filesystem;

#ifdef MERADB_HAS_TLS

namespace {

struct Certs {
    TempDir dir;
    std::string cert, key;
    explicit Certs(std::vector<std::string> hosts = {"localhost", "127.0.0.1"}) {
        cert = (dir.path() / "server.crt").u8string();
        key = (dir.path() / "server.key").u8string();
        tls::writeDevCertificate(cert, key, hosts);
    }
};

ConnectOptions tlsOptions(const RunningServer& s, const std::string& ca) {
    ConnectOptions options;
    options.host = "127.0.0.1";
    options.port = s.port();
    options.tls = true;
    options.tlsCaFile = ca;
    return options;
}

bool fileIsPrivate(const std::string& path) {
#ifdef _WIN32
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    const std::wstring wide = pathOf(path).wstring();
    if (GetNamedSecurityInfoW(wide.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &sd) != ERROR_SUCCESS) return false;
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    GetSecurityDescriptorControl(sd, &control, &revision);
    const bool isPrivate = (control & SE_DACL_PROTECTED) != 0 && dacl != nullptr && dacl->AceCount == 1;
    LocalFree(sd);
    return isPrivate;
#else
    using fs::perms;
    return (fs::status(path).permissions() & (perms::group_all | perms::others_all)) == perms::none;
#endif
}

bool waitFor(const std::function<bool()>& condition, double seconds) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return condition();
}

std::function<void(ServerOptions&)> withCerts(const Certs& certs) {
    return [&certs](ServerOptions& o) {
        o.tlsCertFile = certs.cert;
        o.tlsKeyFile = certs.key;
    };
}

}  // namespace

TEST_CASE("tls a query works over an encrypted connection, large replies too", "[tls]") {
    Certs certs;
    RunningServer s("", false, protocol::kMaxMessageBytes, withCerts(certs));
    Connection c(tlsOptions(s, certs.cert));
    c.execute("BANAO TABLE t (a INT, b TEXT)");
    const std::string big(300000, 'x');
    for (int i = 0; i < 4; ++i) c.execute("DAALO MEIN t MAAN (" + std::to_string(i) + ", '" + big + "')");
    auto results = c.runScript("DIKHAO * SE t");
    REQUIRE(results[0].error == "");
    CHECK(results[0].rows.size() == 4);
    CHECK(std::get<std::string>(results[0].rows[3][1].data) == big);
    CHECK(c.status().contains("databases"));
}

TEST_CASE("tls nothing is sent in the clear", "[tls]") {
    // A relay between client and server that keeps every byte the client sends.
    Certs certs;
    RunningServer s("", false, protocol::kMaxMessageBytes, withCerts(certs));
    net::Socket listener = net::listenOn("127.0.0.1", 0);
    std::string seen;
    std::atomic<bool> done{false};
    std::thread relay([&] {
        net::Socket inbound;
        while (!inbound.valid() && !done) inbound = net::acceptWithTimeout(listener, 0.1);
        if (!inbound.valid()) return;
        net::Socket outbound = net::connectTo("127.0.0.1", s.port(), 2.0);
        char buffer[4096];
        while (!done) {
            try {
                if (inbound.waitReadable(0.01)) {
                    const std::size_t n = inbound.recvSome(buffer, sizeof buffer);
                    if (n == 0) break;
                    seen.append(buffer, n);
                    outbound.sendAll(std::string(buffer, n));
                }
                if (outbound.waitReadable(0.01)) {
                    const std::size_t n = outbound.recvSome(buffer, sizeof buffer);
                    if (n == 0) break;
                    inbound.sendAll(std::string(buffer, n));
                }
            } catch (const net::NetError&) {
                break;
            }
        }
    });
    {
        ConnectOptions options = tlsOptions(s, certs.cert);
        options.port = listener.localPort();
        options.password = "guptshabd";  // refused or not, it must not show
        try {
            Connection c(options);
        } catch (const ConnectionFailed&) {
        }
    }
    done = true;
    relay.join();
    CHECK(seen.size() > 100);
    CHECK(seen.find("guptshabd") == std::string::npos);
    CHECK(seen.find("hello") == std::string::npos);
}

TEST_CASE("tls a certificate nobody vouches for is refused before the password is sent", "[tls]") {
    Certs certs, other;
    RunningServer s("", false, protocol::kMaxMessageBytes, withCerts(certs));
    try {
        ConnectOptions options = tlsOptions(s, other.cert);
        options.password = "guptshabd";
        Connection c(options);
        FAIL("connected to a server whose certificate is not the trusted one");
    } catch (const ConnectionFailed& e) {
        CHECK(std::string(e.what()).find("bharosa") != std::string::npos);
    }
    CHECK_FALSE(s.logContains("login"));
}

TEST_CASE("tls the system's roots do not vouch for a self-signed certificate", "[tls]") {
    Certs certs;
    RunningServer s("", false, protocol::kMaxMessageBytes, withCerts(certs));
    CHECK_THROWS_AS(Connection(tlsOptions(s, "")), ConnectionFailed);
}

TEST_CASE("tls a certificate for another host is refused", "[tls]") {
    Certs certs(std::vector<std::string>{"other.example"});
    RunningServer s("", false, protocol::kMaxMessageBytes, withCerts(certs));
    try {
        Connection c(tlsOptions(s, certs.cert));
        FAIL("connected although the certificate names another host");
    } catch (const ConnectionFailed& e) {
        CHECK(std::string(e.what()).find("bharosa") != std::string::npos);
    }
}

TEST_CASE("tls a host name in the certificate is checked like an address", "[tls]") {
    Certs certs;
    RunningServer s("", false, protocol::kMaxMessageBytes, withCerts(certs));
    ConnectOptions options = tlsOptions(s, certs.cert);
    options.host = "localhost";
    Connection c(options);
    CHECK(c.runScript("DIKHAO DATABASES")[0].error == "");
}

TEST_CASE("tls a plain client gets a clear refusal, and nothing of its login is read", "[tls]") {
    Certs certs;
    RunningServer s("", false, protocol::kMaxMessageBytes, withCerts(certs));
    ConnectOptions options;
    options.host = "127.0.0.1";
    options.port = s.port();
    try {
        Connection c(options);
        FAIL("a plain client got in");
    } catch (const ConnectionFailed& e) {
        CHECK(std::string(e.what()).find("TLS") != std::string::npos);
    }
    CHECK(s.server().sessions() == 0);
}

TEST_CASE("tls a client asking for TLS never falls back to a plain server", "[tls]") {
    Certs certs;
    RunningServer plain;
    const auto started = std::chrono::steady_clock::now();
    ConnectOptions options = tlsOptions(plain, certs.cert);
    options.connectTimeoutSeconds = 1.0;
    options.password = "guptshabd";
    CHECK_THROWS_AS(Connection(options), ConnectionFailed);
    CHECK(std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() < 5.0);
    CHECK(plain.server().sessions() == 0);
}

TEST_CASE("tls a connection that never finishes its handshake is closed in time", "[tls]") {
    Certs certs;
    RunningServer s("", false, protocol::kMaxMessageBytes, [&certs](ServerOptions& o) {
        o.tlsCertFile = certs.cert;
        o.tlsKeyFile = certs.key;
        o.handshakeTimeoutSeconds = 0.3;
    });
    net::Socket quiet = net::connectTo("127.0.0.1", s.port(), 2.0);
    net::Socket halfway = net::connectTo("127.0.0.1", s.port(), 2.0);
    halfway.sendAll(std::string("\x16\x03\x01\x00\x10", 5));  // the start of a ClientHello, and then nothing
    for (net::Socket* socket : {&quiet, &halfway}) {
        socket->setReceiveTimeout(3.0);
        char byte;
        CHECK(socket->recvSome(&byte, 1) == 0);
    }
}

TEST_CASE("tls rubbish instead of a handshake closes the connection and the server goes on", "[tls]") {
    Certs certs;
    RunningServer s("", false, protocol::kMaxMessageBytes, withCerts(certs));
    {
        net::Socket junk = net::connectTo("127.0.0.1", s.port(), 2.0);
        junk.sendAll("GET / HTTP/1.0\r\n\r\n");
        junk.setReceiveTimeout(3.0);
        char buffer[256];
        try {
            while (junk.recvSome(buffer, sizeof buffer) > 0) {
            }
        } catch (const net::NetError& e) {
            CHECK(std::string(e.what()) != "timed out");
        }
    }
    Connection c(tlsOptions(s, certs.cert));
    CHECK(c.runScript("DIKHAO DATABASES")[0].error == "");
}

TEST_CASE("tls the server stops promptly with idle and half-open connections", "[tls]") {
    Certs certs;
    auto s = std::make_unique<RunningServer>("", false, protocol::kMaxMessageBytes, withCerts(certs));
    Connection idle(tlsOptions(*s, certs.cert));
    net::Socket halfOpen = net::connectTo("127.0.0.1", s->port(), 2.0);
    halfOpen.sendAll(std::string("\x16\x03\x01\x00\x10", 5));
    const auto started = std::chrono::steady_clock::now();
    s.reset();
    CHECK(std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() < 3.0);
}

TEST_CASE("tls an open transaction rolls back when a TLS client disappears", "[tls]") {
    Certs certs;
    RunningServer s("", false, protocol::kMaxMessageBytes, withCerts(certs));
    Connection admin(tlsOptions(s, certs.cert));
    admin.execute("BANAO TABLE t (a INT)");
    {
        Connection c(tlsOptions(s, certs.cert));
        c.execute("SHURU");
        c.execute("DAALO MEIN t MAAN (1)");
    }
    CHECK(waitFor([&] { return s.server().sessions() == 1; }, 5.0));
    CHECK(admin.runScript("DIKHAO * SE t")[0].rows.empty());
}

TEST_CASE("tls a cut connection is an error, not an end of messages", "[tls]") {
    Certs certs;
    net::Socket listener = net::listenOn("127.0.0.1", 0);
    tls::ServerConfig config(certs.cert, certs.key);
    std::thread server([&] {
        net::Socket accepted;
        while (!accepted.valid()) accepted = net::acceptWithTimeout(listener, 0.1);
        config.accept(accepted, 3.0, nullptr);
        accepted.sendAll("half a mess");
        accepted.shutdownBoth();  // no close_notify: this is what someone in between can do
    });
    net::Socket client = net::connectTo("127.0.0.1", listener.localPort(), 2.0);
    tls::connect(client, "127.0.0.1", certs.cert, 3.0);
    client.setReceiveTimeout(3.0);
    std::string received;
    char buffer[64];
    CHECK_THROWS_AS([&] {
        for (;;) {
            const std::size_t n = client.recvSome(buffer, sizeof buffer);
            if (n == 0) break;
            received.append(buffer, n);
        }
    }(), net::NetError);
    CHECK(received == "half a mess");
    server.join();
}

TEST_CASE("tls a clean close by the other side is a normal end", "[tls]") {
    Certs certs;
    net::Socket listener = net::listenOn("127.0.0.1", 0);
    tls::ServerConfig config(certs.cert, certs.key);
    std::thread server([&] {
        net::Socket accepted;
        while (!accepted.valid()) accepted = net::acceptWithTimeout(listener, 0.1);
        config.accept(accepted, 3.0, nullptr);
        accepted.sendAll("bye");
    });
    net::Socket client = net::connectTo("127.0.0.1", listener.localPort(), 2.0);
    tls::connect(client, "127.0.0.1", certs.cert, 3.0);
    client.setReceiveTimeout(3.0);
    server.join();
    std::string received;
    char buffer[64];
    for (std::size_t n; (n = client.recvSome(buffer, sizeof buffer)) > 0;) received.append(buffer, n);
    CHECK(received == "bye");
}

TEST_CASE("tls a bad certificate or key stops the server from starting", "[tls]") {
    Certs certs, other;
    TempDir data;
    auto starting = [&](const std::string& cert, const std::string& key) {
        ServerOptions o;
        o.dataDir = data.str();
        o.host = "127.0.0.1";
        o.port = 0;
        o.tlsCertFile = cert;
        o.tlsKeyFile = key;
        Server server(o);
    };
    CHECK_THROWS(starting(certs.cert, ""));
    CHECK_THROWS(starting("", certs.key));
    CHECK_THROWS(starting((data.path() / "nahi.crt").u8string(), certs.key));
    CHECK_THROWS(starting(certs.cert, (data.path() / "nahi.key").u8string()));
    CHECK_THROWS(starting(certs.cert, other.key));  // not the key of this certificate
    CHECK_THROWS(starting(certs.key, certs.cert));
}

TEST_CASE("tls the dev certificate tool will not overwrite, and its key is private", "[tls]") {
    Certs certs;
    CHECK_THROWS_AS(tls::writeDevCertificate(certs.cert, certs.key + ".2", {"localhost"}), tls::Error);
    CHECK_THROWS_AS(tls::writeDevCertificate(certs.cert + ".2", certs.key, {"localhost"}), tls::Error);
    CHECK_FALSE(fs::exists(certs.key + ".2"));
    CHECK(fileIsPrivate(certs.key));
    std::ifstream in(certs.key, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(text.find("PRIVATE KEY") != std::string::npos);
}

TEST_CASE("tls plain traffic from elsewhere is warned about, TLS and local traffic are not", "[tls]") {
    {
        RunningServer s("", false, protocol::kMaxMessageBytes, [](ServerOptions& o) { o.testPeerIsRemote = true; });
        RawClient c(s.port());
        REQUIRE(c.hello().value("ok", false));
        CHECK(s.logContains("WARNING"));
        CHECK(s.logContains("TLS ke bina"));
    }
    {
        RunningServer s;
        RawClient c(s.port());
        REQUIRE(c.hello().value("ok", false));
        CHECK_FALSE(s.logContains("TLS ke bina"));
    }
    {
        Certs certs;
        RunningServer s("", false, protocol::kMaxMessageBytes, [&certs](ServerOptions& o) {
            o.tlsCertFile = certs.cert;
            o.tlsKeyFile = certs.key;
            o.testPeerIsRemote = true;
        });
        Connection c(tlsOptions(s, certs.cert));
        CHECK_FALSE(s.logContains("TLS ke bina"));
    }
}

#else

TEST_CASE("tls a build without TLS refuses to use it, it does not go plain", "[tls]") {
    CHECK_FALSE(tls::available());
    RunningServer plain;
    ConnectOptions options;
    options.host = "127.0.0.1";
    options.port = plain.port();
    options.tls = true;
    CHECK_THROWS_AS(Connection(options), ConnectionFailed);
    CHECK(plain.server().sessions() == 0);
    TempDir data;
    ServerOptions o;
    o.dataDir = data.str();
    o.host = "127.0.0.1";
    o.port = 0;
    o.tlsCertFile = "a.crt";
    o.tlsKeyFile = "a.key";
    CHECK_THROWS(Server(o));
}

#endif
