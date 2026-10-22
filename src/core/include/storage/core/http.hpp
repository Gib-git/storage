#pragma once

// Minimal HTTP/1.1 over plain sockets: enough for a localhost JSON API and for
// talking to Ollama. No TLS (both ends are local by design).

#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <system_error>

namespace storage::http {

struct Request {
    std::string method;
    std::string path;   // without the query string
    std::string query;
    std::map<std::string, std::string> headers;  // lower-case names
    std::string body;
    std::string header(const std::string& name) const {
        auto it = headers.find(name);
        return it == headers.end() ? std::string() : it->second;
    }
};

struct Response {
    int status = 200;
    std::string contentType = "application/json";
    std::string body;
};

class Server {
public:
    using Handler = std::function<Response(const Request&)>;
    explicit Server(Handler handler);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Bind and listen. Port 0 picks a free port (see port()).
    std::error_code listen(const std::string& host, int port);
    int port() const { return port_; }
    // Accept and serve connections until stop() (one request per connection).
    void serve();
    void stop();

private:
    void handle(long long fd);
    Handler handler_;
    long long listenFd_ = -1;
    int port_ = 0;
    std::atomic<bool> stop_{false};
};

struct Result {
    int status = 0;
    std::string body;
};

// http://host[:port]/path only.
std::error_code post(const std::string& url, const std::string& body, Result& out, int timeoutSeconds = 300,
                     const std::string& contentType = "application/json");
std::error_code get(const std::string& url, Result& out, int timeoutSeconds = 10);

}  // namespace storage::http
