// cpp/tests/test_backend_closer.cpp -- the backend is closed on every way out of a command (Python's
// `finally: backend.close()`), including an exception that is not a MeraDBError.
#include <catch2/catch_test_macros.hpp>
#include "meradb/backend_closer.h"
#include "meradb/errors.h"
#include "repl_test_util.h"
#include <stdexcept>

using namespace meradb;

namespace {

struct CountingBackend : meradb_test::FakeBackend {
    int closes = 0;
    bool closeThrows = false;
    void close() override {
        ++closes;
        meradb_test::FakeBackend::close();
        if (closeThrows) throw MeraDBError("close failed");
    }
};

}  // namespace

TEST_CASE("BackendCloser closes the backend when a non-MeraDBError exception unwinds", "[cli]") {
    CountingBackend backend;
    const auto command = [&] {
        BackendCloser closer(backend);
        throw std::runtime_error("not a MeraDBError");  // what a failing command could throw
    };
    CHECK_THROWS_AS(command(), std::runtime_error);
    CHECK(backend.closes == 1);
    CHECK(backend.closed);
}

TEST_CASE("BackendCloser closes the backend when a MeraDBError unwinds", "[cli]") {
    CountingBackend backend;
    const auto command = [&] {
        BackendCloser closer(backend);
        throw MeraDBError("boom");
    };
    CHECK_THROWS_AS(command(), MeraDBError);
    CHECK(backend.closes == 1);
}

TEST_CASE("BackendCloser closes exactly once on a normal return", "[cli]") {
    CountingBackend backend;
    {
        BackendCloser closer(backend);
        closer.close();
        CHECK(backend.closes == 1);
    }
    CHECK(backend.closes == 1);  // the destructor does not close again
}

TEST_CASE("BackendCloser reports a failing explicit close and does not retry it", "[cli]") {
    CountingBackend backend;
    backend.closeThrows = true;
    {
        BackendCloser closer(backend);
        CHECK_THROWS_AS(closer.close(), MeraDBError);
    }
    CHECK(backend.closes == 1);
}

TEST_CASE("BackendCloser keeps the original exception when closing fails while unwinding", "[cli]") {
    CountingBackend backend;
    backend.closeThrows = true;
    const auto command = [&] {
        BackendCloser closer(backend);
        throw std::runtime_error("original");
    };
    try {
        command();
        FAIL("the exception was swallowed");
    } catch (const std::runtime_error& e) {
        CHECK(std::string(e.what()) == "original");
    }
    CHECK(backend.closes == 1);
}

TEST_CASE("BackendCloser closes in its destructor when close() was never called", "[cli]") {
    CountingBackend backend;
    { BackendCloser closer(backend); }  // leaving the scope without close(): the destructor is the safety net
    CHECK(backend.closes == 1);
}
