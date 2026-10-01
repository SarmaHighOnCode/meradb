// cpp/tests/test_net_compat.cpp
#include <catch2/catch_test_macros.hpp>
#include "meradb/net_compat.h"
#include <algorithm>
#include <string>
#include <thread>

using namespace meradb::net;

namespace {

// Reads exactly `count` bytes (recvSome may return fewer per call).
std::string readExactly(Socket& s, std::size_t count) {
    std::string out;
    char buffer[256];
    while (out.size() < count) {
        std::size_t n = s.recvSome(buffer, std::min(sizeof buffer, count - out.size()));
        if (n == 0) break;
        out.append(buffer, n);
    }
    return out;
}

}  // namespace

TEST_CASE("net_compat listens on an OS-chosen port and accepts a client", "[net]") {
    Socket listener = listenOn("127.0.0.1", 0);
    int port = listener.localPort();
    REQUIRE(port > 0);

    Socket client = connectTo("127.0.0.1", port, 2.0);
    Socket served = acceptWithTimeout(listener, 2.0);
    REQUIRE(served.valid());
    CHECK(served.peerAddress() == "127.0.0.1");
    CHECK(served.peerPort() == client.localPort());

    client.sendAll("hello\n");
    CHECK(readExactly(served, 6) == "hello\n");
    served.sendAll("world");
    CHECK(readExactly(client, 5) == "world");
}

TEST_CASE("net_compat carries a payload larger than the socket buffers", "[net]") {
    Socket listener = listenOn("127.0.0.1", 0);
    Socket client = connectTo("127.0.0.1", listener.localPort(), 2.0);
    Socket served = acceptWithTimeout(listener, 2.0);
    const std::string big(3 * 1024 * 1024, 'x');  // sendAll must loop over partial sends
    std::thread writer([&] { client.sendAll(big); });
    std::string got;
    char buffer[65536];
    while (got.size() < big.size()) {
        std::size_t n = served.recvSome(buffer, sizeof buffer);
        REQUIRE(n > 0);
        got.append(buffer, n);
    }
    writer.join();
    CHECK(got == big);
}

TEST_CASE("net_compat accept times out with an invalid socket", "[net]") {
    Socket listener = listenOn("127.0.0.1", 0);
    Socket none = acceptWithTimeout(listener, 0.05);
    CHECK_FALSE(none.valid());
}

TEST_CASE("net_compat connecting to a closed port throws", "[net]") {
    int port;
    {
        Socket listener = listenOn("127.0.0.1", 0);
        port = listener.localPort();
    }  // closed again: nothing listens here now
    CHECK_THROWS_AS(connectTo("127.0.0.1", port, 2.0), NetError);
    CHECK_FALSE(portOpen("127.0.0.1", port, 0.5));
}

TEST_CASE("net_compat portOpen sees a live listener", "[net]") {
    Socket listener = listenOn("127.0.0.1", 0);
    CHECK(portOpen("127.0.0.1", listener.localPort(), 1.0));
}

TEST_CASE("net_compat refuses a second listener on a busy port", "[net]") {
    Socket first = listenOn("127.0.0.1", 0);
    CHECK_THROWS_AS(listenOn("127.0.0.1", first.localPort()), NetError);
}

TEST_CASE("net_compat receive timeout reports 'timed out'", "[net]") {
    Socket listener = listenOn("127.0.0.1", 0);
    Socket client = connectTo("127.0.0.1", listener.localPort(), 2.0);
    Socket served = acceptWithTimeout(listener, 2.0);
    client.setReceiveTimeout(0.1);
    char buffer[8];
    try {
        client.recvSome(buffer, sizeof buffer);
        FAIL("expected a timeout");
    } catch (const NetError& e) {
        CHECK(std::string(e.what()) == "timed out");
    }
}

TEST_CASE("net_compat recvSome returns 0 after the peer closes", "[net]") {
    Socket listener = listenOn("127.0.0.1", 0);
    Socket client = connectTo("127.0.0.1", listener.localPort(), 2.0);
    Socket served = acceptWithTimeout(listener, 2.0);
    client.close();
    char buffer[8];
    CHECK(served.recvSome(buffer, sizeof buffer) == 0);
}

TEST_CASE("net_compat waitReadable times out then sees data then end-of-file", "[net]") {
    Socket listener = listenOn("127.0.0.1", 0);
    Socket client = connectTo("127.0.0.1", listener.localPort(), 2.0);
    Socket served = acceptWithTimeout(listener, 2.0);
    CHECK_FALSE(served.waitReadable(0.05));
    client.sendAll("x");
    CHECK(served.waitReadable(2.0));
    char buffer[8];
    CHECK(served.recvSome(buffer, sizeof buffer) == 1);
    client.close();
    CHECK(served.waitReadable(2.0));  // a closed peer is "readable": recvSome returns 0
    CHECK(served.recvSome(buffer, sizeof buffer) == 0);
}

TEST_CASE("net_compat shutdownBoth makes the peer see end-of-file", "[net]") {
    Socket listener = listenOn("127.0.0.1", 0);
    Socket client = connectTo("127.0.0.1", listener.localPort(), 2.0);
    Socket served = acceptWithTimeout(listener, 2.0);
    served.shutdownBoth();
    char buffer[8];
    client.setReceiveTimeout(2.0);
    CHECK(client.recvSome(buffer, sizeof buffer) == 0);
    Socket never;
    never.shutdownBoth();  // an invalid socket is ignored, never throws
}

TEST_CASE("net_compat Socket is move-only and closes exactly once", "[net]") {
    Socket a = listenOn("127.0.0.1", 0);
    int port = a.localPort();
    Socket b = std::move(a);
    CHECK_FALSE(a.valid());  // NOLINT: moved-from state is specified
    CHECK(b.valid());
    CHECK(b.localPort() == port);
    b.close();
    CHECK_FALSE(b.valid());
    b.close();  // harmless
}

TEST_CASE("net listenOn and connectTo reject ports outside 0-65535", "[net]") {
    CHECK_THROWS_AS(listenOn("127.0.0.1", 70000), NetError);
    CHECK_THROWS_AS(listenOn("127.0.0.1", -1), NetError);
    CHECK_THROWS_AS(connectTo("127.0.0.1", 65536, 0.5), NetError);
    CHECK_THROWS_AS(connectTo("127.0.0.1", -5, 0.5), NetError);
}

TEST_CASE("net accept on an idle listener returns an invalid socket without blocking", "[net]") {
    Socket listener = listenOn("127.0.0.1", 0);
    for (int i = 0; i < 3; ++i) CHECK_FALSE(acceptWithTimeout(listener, 0.05).valid());
}
