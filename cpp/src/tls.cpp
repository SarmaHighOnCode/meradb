// cpp/src/tls.cpp -- see include/meradb/tls.h
#include "meradb/tls.h"

#ifndef MERADB_HAS_TLS

namespace meradb::tls {

namespace {
[[noreturn]] void unavailable() { throw Error("Ye build TLS ke bina bana hai (cmake -DMERADB_TLS=ON se dobara banao)"); }
}  // namespace

bool available() { return false; }
struct ServerConfig::Impl {};
ServerConfig::ServerConfig(const std::string&, const std::string&) { unavailable(); }
ServerConfig::~ServerConfig() = default;
void ServerConfig::accept(net::Socket&, double, const std::atomic<bool>*) const { unavailable(); }
void connect(net::Socket&, const std::string&, const std::string&, double) { unavailable(); }
void writeDevCertificate(const std::string&, const std::string&, const std::vector<std::string>&) { unavailable(); }

}  // namespace meradb::tls

#else

#include "meradb/fs_util.h"
#include "meradb/sys_compat.h"
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/oid.h>
#include <mbedtls/pk.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/ssl.h>
#include <mbedtls/threading.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#endif

namespace meradb::tls {

namespace {

using clock = std::chrono::steady_clock;

std::string errorText(int code) {
    char text[200] = {0};
    mbedtls_strerror(code, text, sizeof text);
    return text;
}

// The server serves every connection on its own thread, so Mbed TLS must lock its shared state. It is built with
// MBEDTLS_THREADING_ALT (see cpp/mbedtls_config/) so that these portable mutexes work on every compiler.
void mutexInit(mbedtls_threading_mutex_t* m) { m->handle = new (std::nothrow) std::mutex; }
void mutexFree(mbedtls_threading_mutex_t* m) {
    delete static_cast<std::mutex*>(m->handle);
    m->handle = nullptr;
}
int mutexLock(mbedtls_threading_mutex_t* m) {
    if (m == nullptr || m->handle == nullptr) return MBEDTLS_ERR_THREADING_BAD_INPUT_DATA;
    static_cast<std::mutex*>(m->handle)->lock();
    return 0;
}
int mutexUnlock(mbedtls_threading_mutex_t* m) {
    if (m == nullptr || m->handle == nullptr) return MBEDTLS_ERR_THREADING_BAD_INPUT_DATA;
    static_cast<std::mutex*>(m->handle)->unlock();
    return 0;
}

void ensureInit() {
    static std::once_flag once;
    static psa_status_t status = PSA_ERROR_GENERIC_ERROR;
    std::call_once(once, [] {
        mbedtls_threading_set_alt(mutexInit, mutexFree, mutexLock, mutexUnlock);
        status = psa_crypto_init();
    });
    if (status != PSA_SUCCESS) throw Error("TLS start nahi hua (crypto library, code " + std::to_string(status) + ")");
}

// The operating system's generator, which needs no shared state.
int randomFromSystem(void*, unsigned char* out, std::size_t size) {
    try {
        const std::vector<std::uint8_t> bytes = sys::randomBytes(size);
        std::memcpy(out, bytes.data(), size);
        return 0;
    } catch (...) {
        return MBEDTLS_ERR_ERROR_GENERIC_ERROR;
    }
}

// Through our own file code, because Mbed TLS opens files with fopen, which on Windows cannot open a non-ASCII path.
std::string readFile(const std::string& path, const char* what) {
    std::ifstream in(pathOf(path), std::ios::binary);
    if (!in) throw Error(std::string(what) + " file khul nahi paayi: " + path);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// Length to hand to the parser: PEM text is counted with its terminating NUL, DER bytes are not.
std::size_t parseLength(const std::string& bytes) {
    return bytes.find("-----BEGIN") != std::string::npos ? bytes.size() + 1 : bytes.size();
}

void loadCertificates(mbedtls_x509_crt* chain, const std::string& path, const char* what) {
    const std::string bytes = readFile(path, what);
    const int rc = mbedtls_x509_crt_parse(chain, reinterpret_cast<const unsigned char*>(bytes.c_str()), parseLength(bytes));
    if (rc != 0) throw Error(std::string(what) + " file sahi nahi hai: " + path + " (" + errorText(rc) + ")");
}

// The trusted roots of this computer. Returns how many were loaded.
int loadSystemRoots(mbedtls_x509_crt* chain) {
    int loaded = 0;
    if (auto file = sys::getEnv("SSL_CERT_FILE"); file && !file->empty()) {
        const std::string bytes = readFile(*file, "SSL_CERT_FILE");
        const int rc = mbedtls_x509_crt_parse(chain, reinterpret_cast<const unsigned char*>(bytes.c_str()), parseLength(bytes));
        if (rc >= 0) return 1;  // a positive number: that many in the bundle could not be used, the rest are in
    }
#ifdef _WIN32
    HCERTSTORE store = CertOpenSystemStoreW(0, L"ROOT");
    if (store == nullptr) return 0;
    PCCERT_CONTEXT cert = nullptr;
    while ((cert = CertEnumCertificatesInStore(store, cert)) != nullptr)
        if (mbedtls_x509_crt_parse_der(chain, cert->pbCertEncoded, cert->cbCertEncoded) == 0) ++loaded;
    CertCloseStore(store, 0);
#else
    for (const char* path : {"/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt", "/etc/ssl/ca-bundle.pem", "/etc/ssl/cert.pem"}) {
        std::ifstream in(path, std::ios::binary);
        if (!in) continue;
        std::ostringstream text;
        text << in.rdbuf();
        const std::string bytes = text.str();
        if (mbedtls_x509_crt_parse(chain, reinterpret_cast<const unsigned char*>(bytes.c_str()), bytes.size() + 1) >= 0) {
            ++loaded;
            break;
        }
    }
#endif
    return loaded;
}

}  // namespace

// What one connection or one server needs alive for as long as its connections last.
struct ServerConfig::Impl {
    mbedtls_ssl_config config;
    mbedtls_x509_crt chain;
    mbedtls_pk_context key;

    explicit Impl(int endpoint) {
        mbedtls_ssl_config_init(&config);
        mbedtls_x509_crt_init(&chain);
        mbedtls_pk_init(&key);
        const int rc = mbedtls_ssl_config_defaults(&config, endpoint, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
        if (rc != 0) throw Error("TLS setting nahi ban paayi: " + errorText(rc));
        mbedtls_ssl_conf_rng(&config, randomFromSystem, nullptr);
        mbedtls_ssl_conf_min_tls_version(&config, MBEDTLS_SSL_VERSION_TLS1_2);
    }
    ~Impl() {
        mbedtls_pk_free(&key);
        mbedtls_x509_crt_free(&chain);
        mbedtls_ssl_config_free(&config);
    }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
};

namespace {

using Shared = std::shared_ptr<ServerConfig::Impl>;

// mbedtls_ssl_read / _write / _handshake take what they need through these. No exception may cross them.
int bioSend(void* context, const unsigned char* data, std::size_t size) {
    const long n = net::rawSend(*static_cast<std::intptr_t*>(context), data, size);
    if (n == net::kRawWouldBlock) return MBEDTLS_ERR_SSL_WANT_WRITE;
    if (n == net::kRawFailed) return MBEDTLS_ERR_NET_SEND_FAILED;
    return static_cast<int>(n);
}

int bioRecv(void* context, unsigned char* data, std::size_t size) {
    const long n = net::rawRecv(*static_cast<std::intptr_t*>(context), data, size);
    if (n == net::kRawWouldBlock) return MBEDTLS_ERR_SSL_WANT_READ;
    if (n == net::kRawFailed) return MBEDTLS_ERR_NET_RECV_FAILED;
    return static_cast<int>(n);  // 0: end of file, which Mbed TLS reports as MBEDTLS_ERR_SSL_CONN_EOF
}

bool wantsSocket(int rc) { return rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE; }

// TLS on one socket. The operating system's socket stays non-blocking from here on, so a half-arrived record can never
// hold a thread past its deadline; used by one thread at a time, like the socket itself.
class Channel : public net::SecureChannel {
public:
    Channel(Shared shared, std::intptr_t handle) : shared_(std::move(shared)), handle_(handle) {
        mbedtls_ssl_init(&ssl_);
        const int rc = mbedtls_ssl_setup(&ssl_, &shared_->config);
        if (rc != 0) {
            mbedtls_ssl_free(&ssl_);
            throw Error("TLS start nahi hua: " + errorText(rc));
        }
        mbedtls_ssl_set_bio(&ssl_, &handle_, bioSend, bioRecv, nullptr);
    }
    ~Channel() override { mbedtls_ssl_free(&ssl_); }

    void setHostname(const std::string& host) {
        const int rc = mbedtls_ssl_set_hostname(&ssl_, host.c_str());
        if (rc != 0) throw Error("TLS: host ka naam sahi nahi hai: " + errorText(rc));
    }

    void handshake(double seconds, const std::atomic<bool>* stop, bool isClient) {
        const auto deadline = clock::now() + std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(seconds));
        for (;;) {
            const int rc = mbedtls_ssl_handshake(&ssl_);
            if (rc == 0) return;
            if (rc == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) continue;
            if (!wantsSocket(rc)) throw Error(describeFailure(rc, isClient));
            if (stop != nullptr && stop->load()) throw Error("server band ho raha hai");
            const double left = std::chrono::duration<double>(deadline - clock::now()).count();
            if (left <= 0) throw Error("TLS handshake ka time khatam -- doosri taraf TLS nahi bol rahi?");
            net::waitHandle(handle_, rc == MBEDTLS_ERR_SSL_WANT_WRITE, std::min(left, 0.1));
        }
    }

    bool waitReadable(double seconds) override {
        const auto started = clock::now();
        for (;;) {
            if (at_ < size_ || ended_ || !failure_.empty()) return true;
            const int want = fill();
            if (want == 0) continue;
            const double left = seconds < 0 ? 1.0 : seconds - std::chrono::duration<double>(clock::now() - started).count();
            if (left <= 0) return false;
            net::waitHandle(handle_, want == MBEDTLS_ERR_SSL_WANT_WRITE, std::min(left, 1.0));
        }
    }

    std::size_t read(char* buffer, std::size_t capacity) override {
        if (!waitReadable(timeout_ > 0 ? timeout_ : -1)) throw net::NetError("timed out");
        if (at_ < size_) {
            const std::size_t n = std::min(capacity, size_ - at_);
            std::memcpy(buffer, plain_.data() + at_, n);
            at_ += n;
            return n;
        }
        if (!failure_.empty()) throw net::NetError(failure_);
        return 0;
    }

    void write(const std::string& data, const std::atomic<bool>* stop, double stallSeconds) override {
        std::size_t sent = 0;
        auto lastProgress = clock::now();
        while (sent < data.size()) {
            const std::size_t chunk = std::min<std::size_t>(data.size() - sent, 16384);
            const int rc = mbedtls_ssl_write(&ssl_, reinterpret_cast<const unsigned char*>(data.data()) + sent, chunk);
            if (rc > 0) {
                sent += static_cast<std::size_t>(rc);
                lastProgress = clock::now();
                continue;
            }
            if (rc == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) continue;
            if (!wantsSocket(rc)) throw net::NetError(errorText(rc));
            if (stop != nullptr && stop->load()) throw net::NetError("server band ho raha hai");
            if (stallSeconds > 0 && std::chrono::duration<double>(clock::now() - lastProgress).count() >= stallSeconds)
                throw net::NetError("timed out");
            net::waitHandle(handle_, rc == MBEDTLS_ERR_SSL_WANT_WRITE, 0.1);
        }
    }

    void setTimeout(double seconds) override { timeout_ = seconds; }
    void closeNotify() noexcept override { mbedtls_ssl_close_notify(&ssl_); }

private:
    // One attempt at reading a record. 0: something changed (data, the end, a failure); otherwise what to wait for.
    int fill() {
        const int rc = mbedtls_ssl_read(&ssl_, reinterpret_cast<unsigned char*>(plain_.data()), plain_.size());
        if (rc > 0) {
            at_ = 0;
            size_ = static_cast<std::size_t>(rc);
            return 0;
        }
        if (rc == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) return 0;
        if (wantsSocket(rc)) return rc;
        if (rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            ended_ = true;
        } else if (rc == 0 || rc == MBEDTLS_ERR_SSL_CONN_EOF) {  // 0: the socket closed with no close_notify
            // The protocol takes an unterminated last line as a message, so a connection cut by someone in between
            // must not read as a normal end: only the other side's close_notify is one.
            failure_ = "TLS connection beech mein kat gaya";
        } else {
            failure_ = errorText(rc);
        }
        return 0;
    }

    std::string describeFailure(int rc, bool isClient) {
        if (rc == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
            char why[512] = {0};
            mbedtls_x509_crt_verify_info(why, sizeof why, "", mbedtls_ssl_get_verify_result(&ssl_));
            std::string text(why);
            std::replace(text.begin(), text.end(), '\n', ';');
            return "server ke certificate par bharosa nahi kar sakte (" + text + ") -- apna certificate --tls-ca se do";
        }
        if (isClient && (rc == MBEDTLS_ERR_SSL_INVALID_RECORD || rc == MBEDTLS_ERR_SSL_CONN_EOF || rc == MBEDTLS_ERR_SSL_BAD_PROTOCOL_VERSION))
            return "TLS handshake fail -- server TLS nahi bol raha? (" + errorText(rc) + ")";
        return "TLS handshake fail (" + errorText(rc) + ")";
    }

    Shared shared_;
    std::intptr_t handle_;
    mbedtls_ssl_context ssl_;
    std::array<char, 16384> plain_{};
    std::size_t at_ = 0;
    std::size_t size_ = 0;
    bool ended_ = false;
    std::string failure_;
    double timeout_ = 0;
};

std::string utcStamp(std::time_t when) {
    std::tm parts{};
#ifdef _WIN32
    gmtime_s(&parts, &when);
#else
    gmtime_r(&when, &parts);
#endif
    char text[16];
    std::strftime(text, sizeof text, "%Y%m%d%H%M%S", &parts);
    return text;
}

bool parseIpv4(const std::string& text, std::array<unsigned char, 4>& out) {
    std::size_t at = 0;
    for (int part = 0; part < 4; ++part) {
        if (part > 0) {
            if (at >= text.size() || text[at] != '.') return false;
            ++at;
        }
        int value = 0, digits = 0;
        while (at < text.size() && text[at] >= '0' && text[at] <= '9' && digits < 4) {
            value = value * 10 + (text[at++] - '0');
            ++digits;
        }
        if (digits == 0 || value > 255) return false;
        out[static_cast<std::size_t>(part)] = static_cast<unsigned char>(value);
    }
    return at == text.size();
}

}  // namespace

bool available() { return true; }

ServerConfig::ServerConfig(const std::string& certFile, const std::string& keyFile) {
    ensureInit();
    impl_ = std::make_shared<Impl>(MBEDTLS_SSL_IS_SERVER);
    loadCertificates(&impl_->chain, certFile, "TLS certificate");
    std::string key = readFile(keyFile, "TLS key");
    const int parsed = mbedtls_pk_parse_key(&impl_->key, reinterpret_cast<const unsigned char*>(key.c_str()), parseLength(key), nullptr, 0,
                                            randomFromSystem, nullptr);
    mbedtls_platform_zeroize(key.data(), key.size());
    if (parsed != 0) throw Error("TLS key file sahi nahi hai: " + keyFile + " (" + errorText(parsed) + ")");
    const int rc = mbedtls_ssl_conf_own_cert(&impl_->config, &impl_->chain, &impl_->key);
    if (rc != 0) throw Error("TLS certificate lag nahi paaya: " + errorText(rc));
    if (mbedtls_pk_check_pair(&impl_->chain.pk, &impl_->key, randomFromSystem, nullptr) != 0)
        throw Error("TLS key is certificate ki nahi hai: " + keyFile);
    mbedtls_ssl_conf_authmode(&impl_->config, MBEDTLS_SSL_VERIFY_NONE);  // clients prove who they are with a password
}

ServerConfig::~ServerConfig() = default;

void ServerConfig::accept(net::Socket& socket, double seconds, const std::atomic<bool>* stop) const {
    net::setHandleNonBlocking(socket.handle(), true);
    auto channel = std::make_unique<Channel>(impl_, socket.handle());
    channel->handshake(seconds, stop, false);
    socket.attachSecure(std::move(channel));
}

void connect(net::Socket& socket, const std::string& host, const std::string& caFile, double seconds) {
    ensureInit();
    auto shared = std::make_shared<ServerConfig::Impl>(MBEDTLS_SSL_IS_CLIENT);
    if (!caFile.empty()) {
        loadCertificates(&shared->chain, caFile, "TLS CA");
    } else if (loadSystemRoots(&shared->chain) == 0) {
        throw Error("Is computer ke trusted certificates nahi mile -- --tls-ca FILE se server ka certificate do");
    }
    mbedtls_ssl_conf_ca_chain(&shared->config, &shared->chain, nullptr);
    mbedtls_ssl_conf_authmode(&shared->config, MBEDTLS_SSL_VERIFY_REQUIRED);
    net::setHandleNonBlocking(socket.handle(), true);
    auto channel = std::make_unique<Channel>(shared, socket.handle());
    channel->setHostname(host);
    channel->handshake(seconds, nullptr, true);
    socket.attachSecure(std::move(channel));
}

void writeDevCertificate(const std::string& certFile, const std::string& keyFile, const std::vector<std::string>& hosts) {
    ensureInit();
    std::error_code ec;
    for (const std::string* path : {&certFile, &keyFile})
        if (std::filesystem::exists(pathOf(*path), ec)) throw Error(*path + " pehle se hai -- overwrite nahi karenge");

    struct Parts {
        mbedtls_pk_context key;
        mbedtls_x509write_cert writer;
        Parts() {
            mbedtls_pk_init(&key);
            mbedtls_x509write_crt_init(&writer);
        }
        ~Parts() {
            mbedtls_x509write_crt_free(&writer);
            mbedtls_pk_free(&key);
        }
    } parts;
    auto check = [](int rc, const char* step) {
        if (rc != 0) throw Error(std::string("Certificate nahi ban paaya (") + step + "): " + errorText(rc));
    };
    check(mbedtls_pk_setup(&parts.key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)), "key");
    check(mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(parts.key), randomFromSystem, nullptr), "key");

    mbedtls_x509write_cert& writer = parts.writer;
    const char* const name = "CN=MeraDB dev certificate,O=MeraDB";
    mbedtls_x509write_crt_set_md_alg(&writer, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&writer, &parts.key);
    mbedtls_x509write_crt_set_issuer_key(&writer, &parts.key);
    check(mbedtls_x509write_crt_set_subject_name(&writer, name), "name");
    check(mbedtls_x509write_crt_set_issuer_name(&writer, name), "name");
    std::vector<std::uint8_t> serial = sys::randomBytes(16);
    serial[0] = static_cast<std::uint8_t>((serial[0] & 0x7f) | 0x40);  // positive, and the full length
    check(mbedtls_x509write_crt_set_serial_raw(&writer, serial.data(), serial.size()), "serial");
    const std::time_t now = std::time(nullptr);
    const std::string from = utcStamp(now - 24 * 3600), until = utcStamp(now + 730 * 24 * 3600);
    check(mbedtls_x509write_crt_set_validity(&writer, from.c_str(), until.c_str()), "validity");
    check(mbedtls_x509write_crt_set_basic_constraints(&writer, 1, -1), "constraints");
    check(mbedtls_x509write_crt_set_subject_key_identifier(&writer), "key id");
    check(mbedtls_x509write_crt_set_authority_key_identifier(&writer), "key id");
    check(mbedtls_x509write_crt_set_key_usage(&writer, MBEDTLS_X509_KU_DIGITAL_SIGNATURE | MBEDTLS_X509_KU_KEY_CERT_SIGN), "usage");

    std::vector<mbedtls_x509_san_list> names(hosts.size());
    std::vector<std::array<unsigned char, 4>> addresses(hosts.size());
    for (std::size_t i = 0; i < hosts.size(); ++i) {
        mbedtls_x509_san_list& entry = names[i];
        std::memset(&entry, 0, sizeof entry);
        if (parseIpv4(hosts[i], addresses[i])) {
            entry.node.type = MBEDTLS_X509_SAN_IP_ADDRESS;
            entry.node.san.unstructured_name.p = addresses[i].data();
            entry.node.san.unstructured_name.len = addresses[i].size();
        } else {
            entry.node.type = MBEDTLS_X509_SAN_DNS_NAME;
            entry.node.san.unstructured_name.p = reinterpret_cast<unsigned char*>(const_cast<char*>(hosts[i].c_str()));
            entry.node.san.unstructured_name.len = hosts[i].size();
        }
        entry.next = i + 1 < hosts.size() ? &names[i + 1] : nullptr;
    }
    if (!names.empty()) check(mbedtls_x509write_crt_set_subject_alternative_name(&writer, names.data()), "hosts");

    std::vector<unsigned char> pem(16384);
    check(mbedtls_x509write_crt_pem(&writer, pem.data(), pem.size(), randomFromSystem, nullptr), "sign");
    const std::string certificate(reinterpret_cast<const char*>(pem.data()));
    check(mbedtls_pk_write_key_pem(&parts.key, pem.data(), pem.size()), "key");
    const std::string key(reinterpret_cast<const char*>(pem.data()));
    mbedtls_platform_zeroize(pem.data(), pem.size());

    if (!writeNewPrivateFile(keyFile, key)) throw Error(keyFile + " likh nahi paaye");
    std::ofstream out(pathOf(certFile), std::ios::binary);
    out << certificate;
    out.flush();
    if (!out) throw Error(certFile + " likh nahi paaye");
}

}  // namespace meradb::tls

#endif  // MERADB_HAS_TLS
