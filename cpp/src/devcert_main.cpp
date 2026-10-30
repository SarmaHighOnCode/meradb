// cpp/src/devcert_main.cpp
//
// meradb_devcert: makes a self-signed certificate and key for trying TLS out (see docs/SERVER.md).
#include "meradb/sys_compat.h"
#include "meradb/tls.h"
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    meradb::sys::Utf8Console console;
    const std::vector<std::string> args = meradb::sys::commandLineArgs(argc, argv);
    std::vector<std::string> hosts = {"localhost", "127.0.0.1"};
    std::filesystem::path folder = ".";
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--host" && i + 1 < args.size()) {
            hosts.push_back(args[++i]);
        } else if (args[i] == "--out" && i + 1 < args.size()) {
            folder = std::filesystem::u8path(args[++i]);
        } else {
            std::cerr << "usage: meradb_devcert [--out FOLDER] [--host NAME_YA_IP]...\n"
                         "  FOLDER mein meradb-dev.crt aur meradb-dev.key banata hai (sirf development ke liye).\n"
                         "  localhost aur 127.0.0.1 hamesha shaamil hain.\n";
            return 2;
        }
    }
    const std::string cert = (folder / "meradb-dev.crt").u8string();
    const std::string key = (folder / "meradb-dev.key").u8string();
    try {
        meradb::tls::writeDevCertificate(cert, key, hosts);
    } catch (const meradb::tls::Error& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
    std::cout << "certificate: " << cert << "\nkey:         " << key << " (sirf tumhare liye; kabhi commit mat karna)\n\n"
              << "server:  meradb start --tls-cert " << cert << " --tls-key " << key << "\n"
              << "client:  meradb shell --tls-ca " << cert << "\n";
    return 0;
}
