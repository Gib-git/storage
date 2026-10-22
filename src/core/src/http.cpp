#include "storage/core/http.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
#define STORAGE_CLOSESOCK closesocket
#define STORAGE_BADSOCK INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
using socket_t = int;
#define STORAGE_CLOSESOCK ::close
#define STORAGE_BADSOCK (-1)
#endif

namespace storage::http {

namespace {

constexpr size_t kMaxBody = 32 * 1024 * 1024;

void netInit() {
#ifdef _WIN32
    static const bool once = [] {
        WSADATA d;
        return WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    (void)once;
#endif
}

std::error_code sockErr() {
#ifdef _WIN32
    return std::error_code(WSAGetLastError(), std::system_category());
#else
    return std::error_code(errno, std::system_category());
#endif
}

void setTimeout(socket_t s, int seconds) {
#ifdef _WIN32
    DWORD ms = static_cast<DWORD>(seconds) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof ms);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ms), sizeof ms);
#else
    timeval tv{seconds, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
}

bool sendAll(socket_t s, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
#ifdef _WIN32
        const int n = ::send(s, data.data() + off, static_cast<int>(data.size() - off), 0);
#else
        const ssize_t n = ::send(s, data.data() + off, data.size() - off, MSG_NOSIGNAL);
#endif
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

long recvSome(socket_t s, char* buf, size_t len) {
#ifdef _WIN32
    return ::recv(s, buf, static_cast<int>(len), 0);
#else
    return static_cast<long>(::recv(s, buf, len, 0));
#endif
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

// Parse "Name: value" lines after the first line of a header block.
std::map<std::string, std::string> parseHeaders(const std::string& block) {
    std::map<std::string, std::string> h;
    size_t pos = block.find("\r\n");
    while (pos != std::string::npos && pos + 2 < block.size()) {
        const size_t end = block.find("\r\n", pos + 2);
        const std::string line = block.substr(pos + 2, (end == std::string::npos ? block.size() : end) - pos - 2);
        const size_t colon = line.find(':');
        if (colon != std::string::npos) h[lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
        pos = end;
    }
    return h;
}

std::string decodeChunked(const std::string& in) {
    std::string out;
    size_t pos = 0;
    while (pos < in.size()) {
        const size_t eol = in.find("\r\n", pos);
        if (eol == std::string::npos) break;
        const size_t len = std::strtoul(in.substr(pos, eol - pos).c_str(), nullptr, 16);
        if (len == 0) break;
        out.append(in, eol + 2, len);
        pos = eol + 2 + len + 2;
    }
    return out;
}

const char* reason(int status) {
    switch (status) {
        case 200: return "OK";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        default: return status >= 500 ? "Internal Server Error" : "OK";
    }
}

}  // namespace

// --- Server ----------------------------------------------------------------------

Server::Server(Handler handler) : handler_(std::move(handler)) { netInit(); }

Server::~Server() { stop(); }

std::error_code Server::listen(const std::string& host, int port) {
    const socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == STORAGE_BADSOCK) return sockErr();
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof yes);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        STORAGE_CLOSESOCK(s);
        return std::make_error_code(std::errc::invalid_argument);
    }
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || ::listen(s, 16) != 0) {
        const auto ec = sockErr();
        STORAGE_CLOSESOCK(s);
        return ec;
    }
    socklen_t len = sizeof addr;
    getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len);
    port_ = ntohs(addr.sin_port);
    listenFd_ = static_cast<long long>(s);
    return {};
}

void Server::serve() {
    while (!stop_) {
        const socket_t c = ::accept(static_cast<socket_t>(listenFd_), nullptr, nullptr);
        if (c == STORAGE_BADSOCK) {
            if (stop_) break;
            continue;
        }
        std::thread([this, c] { handle(static_cast<long long>(c)); }).detach();
    }
}

void Server::stop() {
    if (stop_.exchange(true)) return;
    if (listenFd_ != -1) {
#ifndef _WIN32
        ::shutdown(static_cast<socket_t>(listenFd_), SHUT_RDWR);
#endif
        STORAGE_CLOSESOCK(static_cast<socket_t>(listenFd_));
        listenFd_ = -1;
    }
}

void Server::handle(long long fdValue) {
    const socket_t fd = static_cast<socket_t>(fdValue);
    setTimeout(fd, 30);
    std::string data;
    char buf[16384];
    size_t headerEnd = std::string::npos;
    while (headerEnd == std::string::npos) {
        const long n = recvSome(fd, buf, sizeof buf);
        if (n <= 0 || data.size() > 64 * 1024) {
            STORAGE_CLOSESOCK(fd);
            return;
        }
        data.append(buf, static_cast<size_t>(n));
        headerEnd = data.find("\r\n\r\n");
    }
    Request req;
    const std::string head = data.substr(0, headerEnd);
    const size_t sp1 = head.find(' '), sp2 = head.find(' ', sp1 + 1);
    Response res;
    if (sp1 == std::string::npos || sp2 == std::string::npos) {
        res = {400, "application/json", R"({"error":"bad request"})"};
    } else {
        req.method = head.substr(0, sp1);
        std::string target = head.substr(sp1 + 1, sp2 - sp1 - 1);
        if (const size_t qm = target.find('?'); qm != std::string::npos) {
            req.query = target.substr(qm + 1);
            target.resize(qm);
        }
        req.path = target;
        req.headers = parseHeaders(head);
        const size_t length = std::strtoull(req.header("content-length").c_str(), nullptr, 10);
        if (length > kMaxBody) {
            res = {413, "application/json", R"({"error":"request too large"})"};
        } else {
            req.body = data.substr(headerEnd + 4);
            while (req.body.size() < length) {
                const long n = recvSome(fd, buf, sizeof buf);
                if (n <= 0) break;
                req.body.append(buf, static_cast<size_t>(n));
            }
            req.body.resize(std::min(req.body.size(), length));
            try {
                res = handler_(req);
            } catch (const std::exception& e) {
                res = {500, "application/json", std::string(R"({"error":"internal error"})")};
                (void)e;
            }
        }
    }
    std::string out = "HTTP/1.1 " + std::to_string(res.status) + " " + reason(res.status) + "\r\n";
    out += "Content-Type: " + res.contentType + "\r\n";
    out += "Content-Length: " + std::to_string(res.body.size()) + "\r\n";
    out += "Connection: close\r\nCache-Control: no-store\r\n\r\n";
    out += res.body;
    sendAll(fd, out);
    STORAGE_CLOSESOCK(fd);
}

// --- Client ----------------------------------------------------------------------

namespace {

std::error_code request(const std::string& method, const std::string& url, const std::string& body,
                        const std::string& contentType, int timeout, Result& out) {
    netInit();
    if (url.rfind("http://", 0) != 0) return std::make_error_code(std::errc::protocol_not_supported);
    std::string rest = url.substr(7);
    const size_t slash = rest.find('/');
    const std::string hostPort = rest.substr(0, slash);
    const std::string path = slash == std::string::npos ? "/" : rest.substr(slash);
    std::string host = hostPort, port = "80";
    if (const size_t colon = hostPort.rfind(':'); colon != std::string::npos) {
        host = hostPort.substr(0, colon);
        port = hostPort.substr(colon + 1);
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res)
        return std::make_error_code(std::errc::host_unreachable);
    socket_t s = STORAGE_BADSOCK;
    for (addrinfo* a = res; a; a = a->ai_next) {
        s = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == STORAGE_BADSOCK) continue;
        setTimeout(s, timeout);
        if (::connect(s, a->ai_addr, static_cast<int>(a->ai_addrlen)) == 0) break;
        STORAGE_CLOSESOCK(s);
        s = STORAGE_BADSOCK;
    }
    freeaddrinfo(res);
    if (s == STORAGE_BADSOCK) return std::make_error_code(std::errc::connection_refused);

    std::string req = method + " " + path + " HTTP/1.1\r\nHost: " + hostPort + "\r\nConnection: close\r\nAccept: application/json\r\n";
    if (method == "POST") req += "Content-Type: " + contentType + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    req += "\r\n" + body;
    if (!sendAll(s, req)) {
        STORAGE_CLOSESOCK(s);
        return std::make_error_code(std::errc::connection_reset);
    }
    std::string data;
    char buf[16384];
    for (;;) {
        const long n = recvSome(s, buf, sizeof buf);
        if (n < 0) {
            STORAGE_CLOSESOCK(s);
            return std::make_error_code(std::errc::timed_out);
        }
        if (n == 0) break;
        data.append(buf, static_cast<size_t>(n));
    }
    STORAGE_CLOSESOCK(s);
    const size_t headerEnd = data.find("\r\n\r\n");
    if (headerEnd == std::string::npos || data.rfind("HTTP/", 0) != 0) return std::make_error_code(std::errc::bad_message);
    const std::string head = data.substr(0, headerEnd);
    out.status = std::atoi(head.c_str() + head.find(' ') + 1);
    const auto headers = parseHeaders(head);
    out.body = data.substr(headerEnd + 4);
    if (auto it = headers.find("transfer-encoding"); it != headers.end() && lower(it->second).find("chunked") != std::string::npos)
        out.body = decodeChunked(out.body);
    return {};
}

}  // namespace

std::error_code post(const std::string& url, const std::string& body, Result& out, int timeoutSeconds,
                     const std::string& contentType) {
    return request("POST", url, body, contentType, timeoutSeconds, out);
}

std::error_code get(const std::string& url, Result& out, int timeoutSeconds) {
    return request("GET", url, {}, {}, timeoutSeconds, out);
}

}  // namespace storage::http
