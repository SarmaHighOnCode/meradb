#include "meradb/engine.h"
#include "test_util.h"
#include <pthread.h>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace meradb;

static std::string parens(int depth, const std::string& core) {
    return std::string(depth, '(') + core + std::string(depth, ')');
}
static std::string repeated(const std::string& unit, int times, const std::string& sep) {
    std::string out;
    for (int i = 0; i < times; ++i) out += (i ? sep : std::string()) + unit;
    return out;
}

static void* work(void*) {
    meradb_test::TempDir dir;
    Engine e(dir.str());
    e.execute("BANAO TABLE t (x INT); DAALO MEIN t MAAN (1), (2)");
    int bad = 0;
    auto run = [&](const std::string& q) { auto r = e.runScript(q)[0]; if (!r.error.empty()) { ++bad; std::printf("ERR %s\n", r.error.substr(0, 100).c_str()); } };
    run("DIKHAO * SE t JAHAN " + parens(390, "x = 1"));
    run("DIKHAO * SE t JAHAN x = " + repeated("1", 390, " * "));
    run("DIKHAO * SE t JAHAN " + repeated("x > 0", 390, " AUR "));
    run("SAMJHAO DIKHAO * SE t JAHAN " + parens(390, "x = 1"));
    run("DIKHAO * SE t JAHAN x MEIN (DIKHAO x SE t JAHAN " + parens(390, "x = 2") + ")");
    run("DIKHAO * SE t JAHAN x MEIN (" + repeated("1", 390, ", ") + ")");
    run("DIKHAO * SE t JAHAN AGAR x = 1 TAB 1 WARNA 0 KHATAM = 1");
    std::printf("done bad=%d\n", bad);
    return nullptr;
}

int main(int argc, char** argv) {
    size_t kb = argc > 1 ? std::atoi(argv[1]) : 1024;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kb * 1024);
    pthread_t t;
    pthread_create(&t, &attr, work, nullptr);
    pthread_join(t, nullptr);
    return 0;
}
