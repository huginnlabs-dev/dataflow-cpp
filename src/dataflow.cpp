// dataflow.cpp — implementation of the Dataflow C++ SDK.
//
// Windows builds use WinHTTP + BCrypt (no third-party dependencies);
// Linux builds use cpp-httplib (vendored header) + OpenSSL. Both produce
// wire-identical REST ingest batches (POST /api/v1/ingest).

#include "dataflow.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <thread>
#include <vector>

#ifdef _WIN32
#    define WIN32_LEAN_AND_MEAN
#    define NOMINMAX
#    include <windows.h>
#    include <bcrypt.h>
#    include <winhttp.h>
#    pragma comment(lib, "bcrypt.lib")
#    pragma comment(lib, "winhttp.lib")
#else
#    include <unistd.h>
#    include <openssl/evp.h>
#    include <openssl/rand.h>
#    include "httplib.h"
#endif

namespace dataflow {
namespace {

// ---------------------------------------------------------------------------
// settings + globals

Settings g_settings;
std::once_flag g_sender_once;
std::atomic<long long> g_seq{0};

std::string env_or(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : fallback;
}

double env_double(const char* name, double fallback) {
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    try { return std::stod(v); } catch (...) { return fallback; }
}

int env_int(const char* name, int fallback) {
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    try { return std::stoi(v); } catch (...) { return fallback; }
}

std::string default_service_name() {
#ifdef _WIN32
    char buf[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD len = sizeof(buf);
    if (GetComputerNameA(buf, &len) && len > 0) return buf;
#else
    char buf[256] = {};
    if (::gethostname(buf, sizeof(buf) - 1) == 0 && buf[0]) return buf;
#endif
    return "unknown-service";
}

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string new_id() {
    static thread_local std::mt19937_64 rng(
        std::random_device{}() ^ std::chrono::steady_clock::now().time_since_epoch().count());
    static const char* hex = "0123456789abcdef";
    std::string out(16, '0');
    uint64_t v = rng();
    for (int i = 15; i >= 0; --i) {
        out[i] = hex[v & 0xF];
        v >>= 4;
    }
    return out;
}

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

std::string base64_encode(const unsigned char* data, size_t len) {
    static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        unsigned n = static_cast<unsigned>(data[i]) << 16;
        if (i + 1 < len) n |= static_cast<unsigned>(data[i + 1]) << 8;
        if (i + 2 < len) n |= static_cast<unsigned>(data[i + 2]);
        out += table[(n >> 18) & 63];
        out += table[(n >> 12) & 63];
        out += (i + 1 < len) ? table[(n >> 6) & 63] : '=';
        out += (i + 2 < len) ? table[n & 63] : '=';
    }
    return out;
}

// ---------------------------------------------------------------------------
// crypto

void random_bytes(unsigned char* out, size_t len) {
#ifdef _WIN32
    BCryptGenRandom(nullptr, out, static_cast<ULONG>(len), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
#else
    RAND_bytes(out, static_cast<int>(len));
#endif
}

bool derive_key(const std::string& secret, const unsigned char* salt, size_t salt_len,
                unsigned char out_key[32]) {
#ifdef _WIN32
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (FAILED(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) return false;
    bool ok = SUCCEEDED(BCryptDeriveKeyPBKDF2(alg,
        reinterpret_cast<PUCHAR>(const_cast<char*>(secret.data())),
        static_cast<ULONG>(secret.size()),
        reinterpret_cast<PUCHAR>(const_cast<unsigned char*>(salt)),
        static_cast<ULONG>(salt_len), 10000, out_key, 32, 0));
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
#else
    return PKCS5_PBKDF2_HMAC(secret.data(), static_cast<int>(secret.size()),
                             salt, static_cast<int>(salt_len), 10000,
                             EVP_sha256(), 32, out_key) == 1;
#endif
}

// AES-256-GCM: returns ciphertext||tag (matching Go/Python Seal output).
bool aes_gcm_encrypt(const unsigned char key[32], const unsigned char* plaintext, size_t len,
                     const unsigned char iv[12], std::string& out) {
#ifdef _WIN32
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (FAILED(BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0))) return false;
    bool ok = false;
    if (SUCCEEDED(BCryptSetProperty(alg, BCRYPT_CHAINING_MODE,
            reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
            sizeof(BCRYPT_CHAIN_MODE_GCM), 0))) {
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
        BCRYPT_INIT_AUTH_MODE_INFO(info);
        unsigned char tag[16] = {};
        info.pbNonce = const_cast<PUCHAR>(iv);
        info.cbNonce = 12;
        info.pbTag = tag;
        info.cbTag = sizeof(tag);
        ULONG out_len = 0;
        std::string ct(len, '\0');
        NTSTATUS st = BCryptEncrypt(alg,
            reinterpret_cast<PUCHAR>(const_cast<unsigned char*>(plaintext)),
            static_cast<ULONG>(len), &info, nullptr, 0,
            len ? reinterpret_cast<PUCHAR>(&ct[0]) : nullptr,
            static_cast<ULONG>(ct.size()), &out_len, 0);
        if (BCRYPT_SUCCESS(st)) {
            ct.resize(out_len);
            ct.append(reinterpret_cast<const char*>(tag), sizeof(tag));
            out = std::move(ct);
            ok = true;
        }
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
#else
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    bool ok = false;
    std::string ct(len + 16, '\0');
    do {
        if (!ctx) break;
        unsigned char iv_copy[12];
        std::memcpy(iv_copy, iv, sizeof(iv_copy));
        if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) break;
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) != 1) break;
        if (EVP_EncryptInit_ex(ctx, nullptr, nullptr, key, iv_copy) != 1) break;
        int out_len = 0;
        if (EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char*>(&ct[0]), &out_len,
                              plaintext, static_cast<int>(len)) != 1) break;
        int total = out_len;
        if (EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(&ct[0]) + total, &out_len) != 1) break;
        total += out_len;
        unsigned char tag[16];
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag) != 1) break;
        ct.resize(static_cast<size_t>(total));
        ct.append(reinterpret_cast<const char*>(tag), 16);
        out = std::move(ct);
        ok = true;
    } while (false);
    EVP_CIPHER_CTX_free(ctx);
    return ok;
#endif
}

struct Envelope {
    bool valid = false;
    std::string salt_hex;
    unsigned char key[32] = {};
};

Envelope make_envelope() {
    Envelope e;
    const std::string secret = g_settings.encryption_key;
    if (secret.empty()) return e;
    unsigned char salt[16];
    random_bytes(salt, sizeof(salt));
    if (derive_key(secret, salt, sizeof(salt), e.key)) {
        static const char* hex = "0123456789abcdef";
        e.salt_hex.resize(32);
        for (int i = 0; i < 16; ++i) {
            e.salt_hex[i * 2] = hex[salt[i] >> 4];
            e.salt_hex[i * 2 + 1] = hex[salt[i] & 0xF];
        }
        e.valid = true;
    }
    return e;
}

const Envelope& envelope() {
    static Envelope env = make_envelope();
    return env;
}

// ---------------------------------------------------------------------------
// span data

thread_local Span::Impl* t_current = nullptr;

bool should_sample(double ratio) {
    if (ratio >= 1.0) return true;
    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    return dist(rng) < ratio;
}

// Client-side PII classification. Multiword keywords ("first_name") match
// by substring, single tokens ("card", "ip") by exact word match so
// "description" never lights up the "ip" category. Only category labels
// travel in metadata; values stay in the E2E-encrypted payload.
struct PiiCategory {
    const char* category;
    std::vector<const char*> keywords;
};

const std::vector<PiiCategory>& pii_categories() {
    static const std::vector<PiiCategory> cats = {
        {"password", {"password", "passwd", "pwd"}},
        {"secret", {"token", "secret", "apikey", "api_key", "credential", "session", "jwt", "auth"}},
        {"payment", {"card", "pan", "cvv", "cvc", "iban", "expiry"}},
        {"email", {"email", "e_mail", "mail"}},
        {"phone", {"phone", "mobile", "tel", "msisdn"}},
        {"government_id", {"ssn", "passport", "tax_id", "national_id"}},
        {"birth", {"birth", "dob", "age"}},
        {"name", {"first_name", "last_name", "full_name", "surname", "customer_name", "display_name"}},
        {"address", {"street", "zip", "postal", "street_address", "postal_address",
                     "home_address", "billing_address", "shipping_address", "mailing_address"}},
        {"geo", {"city", "country", "region", "location", "lat", "lon", "lng"}},
        {"ip", {"ip", "ip_address", "client_ip", "remote_addr"}},
        {"device", {"device", "user_agent", "imei", "fingerprint"}},
    };
    return cats;
}

std::string joined_sorted(const std::set<std::string>& parts) {
    std::string out;
    for (const auto& p : parts) {  // std::set keeps categories sorted
        if (!out.empty()) out += ",";
        out += p;
    }
    return out;
}

std::string classify_pii(const std::vector<std::string>& fields) {
    std::set<std::string> seen;
    auto norm_char = [](char c) -> char {
        if (c == '-' || c == ' ' || c == '.') return '_';
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    };
    for (const auto& field : fields) {
        std::string n;
        for (char c : field) n += norm_char(c);
        std::set<std::string> tokens;
        {
            std::string cur;
            for (char c : n) {
                if (c == '_') { if (!cur.empty()) tokens.insert(cur); cur.clear(); }
                else cur += c;
            }
            if (!cur.empty()) tokens.insert(cur);
        }
        for (const auto& cat : pii_categories()) {
            if (seen.count(cat.category)) continue;
            for (const char* kwp : cat.keywords) {
                std::string kw = kwp;
                bool hit = kw.find('_') != std::string::npos
                               ? n.find(kw) != std::string::npos
                               : tokens.count(kw) > 0;
                if (hit) { seen.insert(cat.category); break; }
            }
        }
    }
    return joined_sorted(seen);
}

void attach_payload(const std::shared_ptr<Span::Impl>& impl) {
    // Field-name lineage: key names (never values) travel as plaintext
    // metadata even when payload values are encrypted. PII categories are
    // classified client-side the same way.
    {
        std::vector<std::string> keys;
        for (const auto& kv : impl->payload) keys.push_back(kv.first);
        if (!keys.empty()) {
            std::string joined;
            for (const auto& k : keys) {
                if (!joined.empty()) joined += ",";
                joined += k;
            }
            impl->metadata.emplace_back("data.fields", joined);
            std::string pii = classify_pii(keys);
            if (!pii.empty()) impl->metadata.emplace_back("data.pii", pii);
        }
    }
    std::string json = "{";
    bool first = true;
    for (const auto& kv : impl->payload) {
        if (!first) json += ",";
        first = false;
        json += "\"" + json_escape(kv.first) + "\":" + kv.second;
    }
    json += "}";
    impl->payload.clear();

    const Envelope& env = envelope();
    if (!env.valid) {
        impl->plaintext_payload = json;
        return;
    }
    unsigned char iv[12];
    random_bytes(iv, sizeof(iv));
    std::string ct;
    if (!aes_gcm_encrypt(env.key, reinterpret_cast<const unsigned char*>(json.data()),
                         json.size(), iv, ct)) {
        impl->plaintext_payload = json;
        return;
    }
    impl->encrypted_payload = true;
    impl->payload_b64 = base64_encode(reinterpret_cast<const unsigned char*>(ct.data()), ct.size());
    impl->iv_b64 = base64_encode(iv, sizeof(iv));
    impl->key_salt = env.salt_hex;
}

// ---------------------------------------------------------------------------
// buffer + sender

std::mutex g_buf_mu;
std::deque<std::shared_ptr<Span::Impl>> g_buf;
long long g_buf_base = 1;
std::condition_variable g_wake;
std::atomic<long long> g_acked{0};

void enqueue(const std::shared_ptr<Span::Impl>& impl) {
    if (!enabled()) return;
    std::lock_guard<std::mutex> lock(g_buf_mu);
    impl->seq = ++g_seq;
    impl->duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - impl->started).count();
    g_buf.push_back(impl);
    if (static_cast<int>(g_buf.size()) > g_settings.buffer_size) {
        g_buf.pop_front();
        g_buf_base++;
    }
    g_wake.notify_one();
}

struct ParsedUrl {
    std::string scheme = "http";
    std::string host;
    int port = 80;
    std::string path = "/";
};

ParsedUrl parse_url(const std::string& url) {
    ParsedUrl out;
    std::string rest = url;
    auto scheme_end = rest.find("://");
    if (scheme_end != std::string::npos) {
        out.scheme = rest.substr(0, scheme_end);
        rest = rest.substr(scheme_end + 3);
        if (out.scheme == "https") out.port = 443;
    }
    auto slash = rest.find('/');
    std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
    out.path = slash == std::string::npos ? "/" : rest.substr(slash);
    if (!hostport.empty()) {
        auto colon = hostport.rfind(':');
        if (colon != std::string::npos) {
            try { out.port = std::stoi(hostport.substr(colon + 1)); } catch (...) {}
            out.host = hostport.substr(0, colon);
        } else {
            out.host = hostport;
        }
    }
    return out;
}

long long extract_last_seq(const std::string& body) {
    const std::string key = "\"last_seq\"";
    auto pos = body.find(key);
    if (pos == std::string::npos) return -1;
    pos = body.find(':', pos + key.size());
    if (pos == std::string::npos) return -1;
    ++pos;
    while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos]))) ++pos;
    try { return std::stoll(body.substr(pos)); } catch (...) { return -1; }
}

#ifdef _WIN32

bool http_post_json(const ParsedUrl& ep, const std::string& path, const std::string& api_key,
                    const std::vector<std::pair<std::string, std::string>>& headers,
                    const std::string& body, long& status, std::string& response,
                    std::string& error) {
    bool secure = ep.scheme == "https";
    std::wstring whost(ep.host.begin(), ep.host.end());
    std::wstring wpath(path.begin(), path.end());
    HINTERNET session = WinHttpOpen(L"dataflow-cpp", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { error = "WinHttpOpen failed"; return false; }
    HINTERNET connect = WinHttpConnect(session, whost.c_str(),
                                       static_cast<INTERNET_PORT>(ep.port), 0);
    if (!connect) {
        error = "WinHttpConnect failed";
        WinHttpCloseHandle(session);
        return false;
    }
    HINTERNET request = WinHttpOpenRequest(connect, L"POST", wpath.c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           secure ? WINHTTP_FLAG_SECURE : 0);
    bool ok = false;
    if (request) {
        DWORD timeout = 10000;
        WinHttpSetOption(request, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));
        std::wstring wh = L"content-type: application/json\r\nx-api-key: " +
                          std::wstring(api_key.begin(), api_key.end());
        for (const auto& kv : headers) {
            std::wstring k(kv.first.begin(), kv.first.end());
            std::wstring v(kv.second.begin(), kv.second.end());
            wh += L"\r\n" + k + L": " + v;
        }
        if (WinHttpSendRequest(request, wh.c_str(), static_cast<DWORD>(wh.size()),
                               LPVOID(const_cast<char*>(body.data())),
                               static_cast<DWORD>(body.size()),
                               static_cast<DWORD>(body.size()), 0) &&
            WinHttpReceiveResponse(request, nullptr)) {
            DWORD st = 0, size = sizeof(st);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &st, &size, WINHTTP_NO_HEADER_INDEX);
            status = static_cast<long>(st);
            DWORD read = 0;
            char buf[8192];
            while (WinHttpReadData(request, buf, sizeof(buf), &read) && read > 0) {
                response.append(buf, read);
                read = 0;
            }
            ok = true;
        } else {
            error = "send/receive failed (GetLastError=" + std::to_string(GetLastError()) + ")";
        }
        WinHttpCloseHandle(request);
    } else {
        error = "WinHttpOpenRequest failed";
    }
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return ok;
}

#else

bool http_post_json(const ParsedUrl& ep, const std::string& path, const std::string& api_key,
                    const std::vector<std::pair<std::string, std::string>>& headers,
                    const std::string& body, long& status, std::string& response,
                    std::string& error) {
    httplib::Client client(ep.scheme + "://" + ep.host + ":" + std::to_string(ep.port));
    client.set_connection_timeout(5, 0);
    client.set_read_timeout(10, 0);
    httplib::Headers h = {{"x-api-key", api_key}};
    for (const auto& kv : headers) h.emplace(kv.first, kv.second);
    if (auto res = client.Post(path, h, body, "application/json")) {
        status = res->status;
        response = res->body;
        return true;
    } else if (res.error() != httplib::Error::Success) {
        error = httplib::to_string(res.error());
    } else {
        error = "unknown http client error";
    }
    return false;
}

#endif

std::string buffer_to_json() {
    std::vector<std::shared_ptr<Span::Impl>> batch;
    {
        std::lock_guard<std::mutex> lock(g_buf_mu);
        long long acked = g_acked.load();
        size_t offset = 0;
        if (acked + 1 > g_buf_base) offset = static_cast<size_t>(acked + 1 - g_buf_base);
        if (offset >= g_buf.size()) return "";
        size_t count = std::min<size_t>(g_buf.size() - offset, 200);
        for (size_t i = 0; i < count; ++i) batch.push_back(g_buf[offset + i]);
    }
    if (batch.empty()) return "";

    std::string out = "{\"events\":[";
    bool first_ev = true;
    for (const auto& e : batch) {
        if (!first_ev) out += ",";
        first_ev = false;
        std::vector<std::pair<std::string, std::string>> metadata;
        std::string payload_json = "null";
        std::string error_message;
        int status_code = 0;
        {
            std::lock_guard<std::mutex> lock(e->mu);
            metadata = e->metadata;
            error_message = e->error_message;
            status_code = e->status_code;
            if (e->encrypted_payload) {
                payload_json = "{\"encrypted\":true,\"data_b64\":\"" + e->payload_b64 +
                               "\",\"iv_b64\":\"" + e->iv_b64 + "\",\"key_salt\":\"" +
                               json_escape(e->key_salt) + "\"}";
            } else if (!e->plaintext_payload.empty()) {
                payload_json = "{\"encrypted\":false,\"data\":\"" +
                               json_escape(e->plaintext_payload) + "\"}";
            }
        }
        out += "{";
        out += "\"event_id\":\"" + json_escape(e->event_id) + "\",";
        out += "\"seq\":" + std::to_string(e->seq) + ",";
        out += "\"trace_id\":\"" + json_escape(e->trace_id) + "\",";
        out += "\"span_id\":\"" + json_escape(e->span_id) + "\",";
        out += "\"parent_span_id\":\"" + json_escape(e->parent_span_id) + "\",";
        out += "\"type\":\"" + json_escape(e->type) + "\",";
        out += "\"service_name\":\"" + json_escape(e->service_name) + "\",";
        out += "\"name\":\"" + json_escape(e->name) + "\",";
        out += "\"caller_package\":\"" + json_escape(e->caller_package) + "\",";
        out += "\"callee_package\":\"" + json_escape(e->callee_package) + "\",";
        out += "\"function_name\":\"" + json_escape(e->name) + "\",";
        out += "\"timestamp\":" + std::to_string(e->timestamp_ms) + ",";
        out += "\"duration_ms\":" + std::to_string(e->duration_ms) + ",";
        out += "\"status_code\":" + std::to_string(status_code) + ",";
        out += "\"error_message\":\"" + json_escape(error_message) + "\",";
        out += "\"payload\":" + payload_json + ",";
        out += "\"metadata\":{";
        bool first = true;
        for (const auto& kv : metadata) {
            if (!first) out += ",";
            first = false;
            out += "\"" + json_escape(kv.first) + "\":\"" + json_escape(kv.second) + "\"";
        }
        out += "}}";
    }
    out += "]}";
    return out;
}

void trim_acked(long long acked) {
    std::lock_guard<std::mutex> lock(g_buf_mu);
    long long drop = std::max(acked + 1 - g_buf_base, 0LL);
    drop = std::min<long long>(drop, static_cast<long long>(g_buf.size()));
    for (long long i = 0; i < drop; ++i) g_buf.pop_front();
    g_buf_base += drop;
}

void sender_loop() {
    long long backoff_ms = 1000;
    for (;;) {
        std::string body;
        {
            std::unique_lock<std::mutex> lock(g_buf_mu);
            long long acked = g_acked.load();
            size_t offset = 0;
            if (acked + 1 > g_buf_base) offset = static_cast<size_t>(acked + 1 - g_buf_base);
            if (offset >= g_buf.size()) {
                g_wake.wait_for(lock, std::chrono::milliseconds(500));
                continue;
            }
        }
        body = buffer_to_json();
        if (body.empty()) continue;

        ParsedUrl ep = parse_url(g_settings.endpoint);
        long status = 0;
        std::string response, error;
        if (http_post_json(ep, "/api/v1/ingest", g_settings.api_key, {}, body, status, response, error) &&
            status == 200) {
            long long last_seq = extract_last_seq(response);
            if (last_seq > 0) {
                trim_acked(last_seq);
            } else {
                std::fprintf(stderr, "dataflow: debug: cannot parse ack: %.120s\n", response.c_str());
            }
            backoff_ms = 1000;
        } else {
            if (!error.empty()) {
                std::fprintf(stderr, "dataflow: ingest error: %s; retrying in %lldms\n",
                             error.c_str(), backoff_ms);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
            backoff_ms = std::min(backoff_ms * 2, 30000LL);
        }
    }
}

void ensure_sender_started() {
    std::call_once(g_sender_once, [] {
        if (!enabled()) {
            if (!g_settings.disabled) {
                std::fprintf(stderr,
                             "dataflow: DATAFLOW_API_KEY/DATAFLOW_ENDPOINT not set; SDK stays passive\n");
            }
            return;
        }
        std::thread(sender_loop).detach();
    });
}

// Host/process descriptor: built once per process, stamped onto root
// HTTP_SERVER spans so the dashboard can show OS/runtime/SDK versions.
const std::vector<std::pair<std::string, std::string>>& agent_attrs() {
    static const std::vector<std::pair<std::string, std::string>> attrs = [] {
        std::vector<std::pair<std::string, std::string>> a;
        std::string os_name, arch, compiler;
#if defined(_WIN32)
        os_name = "windows";
#  if defined(_M_ARM64)
        arch = "arm64";
#  else
        arch = "amd64";
#  endif
        compiler = "msvc-" + std::to_string(_MSC_VER);
#else
        os_name = "linux";
#  if defined(__aarch64__)
        arch = "arm64";
#  else
        arch = "amd64";
#  endif
        compiler = "gcc-" + std::to_string(__GNUC__) + "." + std::to_string(__GNUC_MINOR__);
#endif
        a.push_back({"agent.os", os_name + "/" + arch});
        a.push_back({"agent.runtime", "c++17 " + compiler});
        a.push_back({"agent.sdk", std::string("cpp-sdk/") + kVersion});
        a.push_back({"agent.cpu", std::to_string(std::thread::hardware_concurrency())});
#ifdef _WIN32
        a.push_back({"agent.pid", std::to_string(GetCurrentProcessId())});
#else
        a.push_back({"agent.pid", std::to_string(::getpid())});
#endif
        a.push_back({"agent.started", std::to_string(now_ms())});
        if (const char* env = std::getenv("DATAFLOW_ENV"); env && *env)
            a.push_back({"agent.env", env});
        if (const char* ver = std::getenv("DATAFLOW_APP_VERSION"); ver && *ver)
            a.push_back({"agent.app_version", ver});
        return a;
    }();
    return attrs;
}

// Traced request used by http_get/http_post.
HttpResponse traced_request(const char* method, const std::string& url, const std::string& body,
                            const std::string& content_type, int timeout_ms) {
    HttpResponse out;
    Span span = start_span("HTTP " + url, kHttpClient);
    ParsedUrl ep = parse_url(url);
    if (span) span.set_callee(ep.host);
    if (span) span.set_attr("http.url", url);
    const std::string trace_header = span ? span.trace_id() : "";

#ifdef _WIN32
    std::wstring whost(ep.host.begin(), ep.host.end());
    std::wstring wpath(ep.path.begin(), ep.path.end());
    std::wstring wmethod(method, method + std::strlen(method));
    HINTERNET session = WinHttpOpen(L"dataflow-cpp", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connect = session ? WinHttpConnect(session, whost.c_str(),
                                                 static_cast<INTERNET_PORT>(ep.port), 0) : nullptr;
    HINTERNET request = connect ? WinHttpOpenRequest(connect, wmethod.c_str(), wpath.c_str(),
                                                     nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                     ep.scheme == "https" ? WINHTTP_FLAG_SECURE : 0)
                                : nullptr;
    if (request) {
        DWORD timeout = static_cast<DWORD>(timeout_ms);
        WinHttpSetOption(request, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));
        std::wstring wh = L"x-dataflow-trace-id: " + std::wstring(trace_header.begin(), trace_header.end());
        if (!content_type.empty()) {
            std::wstring ct(content_type.begin(), content_type.end());
            wh += L"\r\ncontent-type: " + ct;
        }
        const char* body_ptr = body.empty() ? nullptr : body.data();
        BOOL sent = WinHttpSendRequest(request, wh.c_str(), static_cast<DWORD>(wh.size()),
                                       LPVOID(const_cast<char*>(body_ptr)),
                                       static_cast<DWORD>(body.size()),
                                       static_cast<DWORD>(body.size()), 0);
        if (sent && WinHttpReceiveResponse(request, nullptr)) {
            DWORD st = 0, size = sizeof(st);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &st, &size, WINHTTP_NO_HEADER_INDEX);
            out.status = static_cast<long>(st);
            DWORD read = 0;
            char buf[8192];
            while (WinHttpReadData(request, buf, sizeof(buf), &read) && read > 0) {
                out.body.append(buf, read);
                read = 0;
            }
        } else {
            out.error = "request failed (GetLastError=" + std::to_string(GetLastError()) + ")";
        }
        WinHttpCloseHandle(request);
    } else {
        out.error = "request setup failed";
    }
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
#else
    httplib::Client client(ep.scheme + "://" + ep.host + ":" + std::to_string(ep.port));
    client.set_connection_timeout(5, 0);
    client.set_read_timeout(timeout_ms / 1000, (timeout_ms % 1000) * 1000000);
    httplib::Headers headers = {{"x-dataflow-trace-id", trace_header}};
    httplib::Result res;
    if (std::strcmp(method, "GET") == 0) {
        res = client.Get(ep.path, headers);
    } else {
        res = client.Post(ep.path, headers, body,
                          content_type.empty() ? "application/json" : content_type);
    }
    if (res) {
        out.status = res->status;
        out.body = res->body;
    } else if (res.error() != httplib::Error::Success) {
        out.error = httplib::to_string(res.error());
    } else {
        out.error = "unknown http client error";
    }
#endif

    if (!out.error.empty()) {
        span.record_error(out.error);
    } else {
        span.set_status(static_cast<int>(out.status));
        if (out.status >= 500) span.record_error("http " + std::to_string(out.status));
    }
    span.end();
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// public API

void configure() {
    Settings s;
    s.api_key = env_or("DATAFLOW_API_KEY", "");
    s.endpoint = env_or("DATAFLOW_ENDPOINT", "http://localhost:25080");
    s.service_name = env_or("DATAFLOW_SERVICE_NAME", default_service_name());
    s.encryption_key = env_or("DATAFLOW_ENCRYPTION_KEY", "");
    s.sample_ratio = env_double("DATAFLOW_SAMPLE_RATIO", 1.0);
    s.buffer_size = env_int("DATAFLOW_BUFFER_SIZE", 10000);
    const char* dis = std::getenv("DATAFLOW_DISABLED");
    s.disabled = dis && *dis && std::strcmp(dis, "0") != 0 && std::strcmp(dis, "false") != 0;
    configure(s);
}

void configure(const Settings& cfg) {
    g_settings = cfg;
    if (g_settings.sample_ratio <= 0) g_settings.sample_ratio = 1.0;
    if (g_settings.sample_ratio > 1) g_settings.sample_ratio = 1;
    if (g_settings.service_name.empty()) g_settings.service_name = default_service_name();
    if (!g_settings.endpoint.empty() && g_settings.endpoint.rfind("http", 0) != 0) {
        g_settings.endpoint = "http://" + g_settings.endpoint;
    }
    ensure_sender_started();
    if (enabled() && g_settings.encryption_key.empty()) {
        std::fprintf(stderr, "dataflow: warning: no encryption key set; payloads are sent as plaintext\n");
    }
}

const Settings& settings() { return g_settings; }

bool enabled() {
    return !g_settings.disabled && !g_settings.api_key.empty() && !g_settings.endpoint.empty();
}

std::string version() { return kVersion; }

// --- Span -------------------------------------------------------------------

Span::Span(Span&& other) noexcept
    : impl_(std::move(other.impl_)), trace_id_(std::move(other.trace_id_)),
      span_id_(std::move(other.span_id_)), sampled_(other.sampled_), ended_(other.ended_) {
    other.ended_ = true; // moved-from shell must not end the real span
}

Span& Span::operator=(Span&& other) noexcept {
    if (this != &other) {
        impl_ = std::move(other.impl_);
        trace_id_ = std::move(other.trace_id_);
        span_id_ = std::move(other.span_id_);
        sampled_ = other.sampled_;
        ended_ = other.ended_;
        other.ended_ = true;
    }
    return *this;
}

Span::~Span() { end(); }

Span& Span::set_attr(const std::string& key, const std::string& value) {
    if (impl_) {
        std::lock_guard<std::mutex> lock(impl_->mu);
        impl_->metadata.emplace_back(key, value);
    }
    return *this;
}

Span& Span::set_data(const std::string& key, const std::string& value) {
    return set_data_json(key, "\"" + json_escape(value) + "\"");
}

Span& Span::set_data_json(const std::string& key, const std::string& json_value) {
    if (impl_) {
        std::lock_guard<std::mutex> lock(impl_->mu);
        impl_->payload.emplace_back(key, json_value);
    }
    return *this;
}

Span& Span::set_callee(const std::string& pkg) {
    if (impl_) impl_->callee_package = pkg;
    return *this;
}

Span& Span::record_error(const std::string& message) {
    if (impl_) {
        std::lock_guard<std::mutex> lock(impl_->mu);
        impl_->error_message = message;
    }
    return *this;
}

Span& Span::set_status(int code) {
    if (impl_) {
        std::lock_guard<std::mutex> lock(impl_->mu);
        impl_->status_code = code;
    }
    return *this;
}

void Span::end() {
    if (ended_ || !impl_ || !sampled_) {
        ended_ = true;
        return;
    }
    ended_ = true;
    attach_payload(impl_);
    enqueue(impl_);
}

Span start_span(const std::string& name, const char* type) {
    const Settings& s = settings();
    Span span;
    if (!enabled() || !should_sample(s.sample_ratio)) {
        span.ended_ = true;
        span.sampled_ = false;
        return span;
    }
    span.impl_ = std::make_shared<Span::Impl>();
    span.sampled_ = true;
    Span::Impl& impl = *span.impl_;
    impl.event_id = new_id();
    impl.span_id = new_id();
    impl.type = type;
    impl.service_name = s.service_name;
    impl.name = name;
    impl.started = std::chrono::steady_clock::now();
    impl.timestamp_ms = now_ms();

    Span::Impl* parent = t_current;
    if (parent) {
        impl.trace_id = parent->trace_id;
        impl.parent_span_id = parent->span_id;
    } else {
        impl.trace_id = new_id();
    }
    // Package attribution: "pkg.Func" pins the callee; the caller is the
    // enclosing span's callee (route labels have no package).
    auto dot = name.rfind('.');
    bool labeled = dot != std::string::npos && name.find(' ') == std::string::npos &&
                   name.find('/') == std::string::npos;
    if (labeled) {
        impl.callee_package = name.substr(0, dot);
        // Caller stays empty on roots: caller==callee self-edges would
        // corrupt the flow graph layering.
        impl.caller_package =
            (parent && !parent->callee_package.empty()) ? parent->callee_package : "";
    } else if (parent) {
        impl.callee_package = parent->callee_package;
        impl.caller_package = parent->caller_package;
    }
    // Root entry-point spans carry the host descriptor.
    if (impl.type == kHttpServer && !parent) {
        for (const auto& kv : agent_attrs()) impl.metadata.emplace_back(kv.first, kv.second);
    }
    span.trace_id_ = impl.trace_id;
    span.span_id_ = impl.span_id;
    return span;
}

// --- Trace + current span -----------------------------------------------------

Trace::Trace(const std::string& name, const char* type) : span_(start_span(name, type)) {
    if (span_.impl_) {
        previous_ = t_current;
        t_current = span_.impl_.get();
    }
}

Trace::~Trace() {
    if (span_.impl_) t_current = static_cast<Span::Impl*>(previous_);
}

Span current_span() {
    Span s;
    if (t_current) {
        s.trace_id_ = t_current->trace_id;
        s.span_id_ = t_current->span_id;
    }
    return s;
}

// --- traced HTTP client ---------------------------------------------------------

HttpResponse http_get(const std::string& url, int timeout_ms) {
    return traced_request("GET", url, "", "", timeout_ms);
}

HttpResponse http_post(const std::string& url, const std::string& body,
                       const std::string& content_type, int timeout_ms) {
    return traced_request("POST", url, body, content_type, timeout_ms);
}

} // namespace dataflow
