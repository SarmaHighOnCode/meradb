// cpp/include/meradb/tls.h
//
// Encrypted connections (Mbed TLS). This header names no Mbed TLS type, and the functions exist in a build with
// -DMERADB_TLS=OFF too: there available() is false and everything else throws Error, so asking for TLS can never
// quietly turn into plain traffic.
#pragma once
#include "meradb/net_compat.h"
#include <atomic>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace meradb::tls {

class Error : public std::runtime_error {
public:
    explicit Error(const std::string& message) : std::runtime_error(message) {}
};

bool available();

// The server's certificate and key, loaded once and shared by every connection.
class ServerConfig {
public:
    // Throws Error when a file is missing, damaged, or the key does not belong to the certificate.
    ServerConfig(const std::string& certFile, const std::string& keyFile);
    ~ServerConfig();
    ServerConfig(const ServerConfig&) = delete;
    ServerConfig& operator=(const ServerConfig&) = delete;
    // The server's side of the handshake; from here on the socket carries TLS. Gives up after `seconds`, or once *stop
    // is true, with Error.
    void accept(net::Socket& socket, double seconds, const std::atomic<bool>* stop) const;

    struct Impl;

private:
    std::shared_ptr<Impl> impl_;
};

// The client's side of the handshake. The certificate must lead to `caFile` (the system's trusted roots when that is
// empty) and must name `host`. Throws Error when not: the caller has sent nothing over the socket yet.
void connect(net::Socket& socket, const std::string& host, const std::string& caFile, double seconds);

// A self-signed certificate for development: names `hosts`, valid two years, usable as its own trust anchor. The key
// file gets owner-only rights. Refuses to overwrite either file. Throws Error.
void writeDevCertificate(const std::string& certFile, const std::string& keyFile, const std::vector<std::string>& hosts);

}  // namespace meradb::tls
