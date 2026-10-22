#include <thread>

#include "TempTree.hpp"
#include "check.hpp"
#include "storage/core/ai.hpp"
#include "storage/core/http.hpp"
#include "storage/core/paths.hpp"
#include "storage/core/scanner.hpp"

using namespace storage;
using json::Value;

namespace {
// A stand-in for Ollama that records the request and replies with `answers` in turn.
struct FakeOllama {
    std::vector<std::string> answers;
    std::vector<Value> requests;
    size_t next = 0;
    http::Server server{[this](const http::Request& r) {
        if (r.path == "/api/tags") return http::Response{200, "application/json", R"({"models":[{"name":"llama3.2"},{"name":"qwen2.5"}]})"};
        requests.push_back(Value::parse(r.body));
        const std::string content = answers[std::min(next++, answers.size() - 1)];
        return http::Response{200, "application/json",
                              Value::object({{"message", Value::object({{"role", "assistant"}, {"content", content}})}}).dump()};
    }};
    std::thread th;
    std::string url;
    FakeOllama() {
        server.listen("127.0.0.1", 0);
        url = "http://127.0.0.1:" + std::to_string(server.port());
        th = std::thread([this] { server.serve(); });
    }
    ~FakeOllama() {
        server.stop();
        http::Result r;
        http::get(url + "/api/tags", r, 1);
        th.join();
    }
};
}  // namespace

TEST_CASE("ai: folder summary has names and counts but never contents") {
    TempTree t;
    t.file("IMG_0001.jpg", 100, 'S');
    t.file("invoice-2024.pdf", 200, 'S');
    t.file("Thing.app/Contents/secret.txt", 10, 'S');
    LocalFileSystem lfs;
    auto tree = Scanner(lfs, ScanOptions::defaultsFor(toUtf8(t.root()))).run();
    const std::string d = describeFolderForAi(*tree->root);
    CHECK(d.find("IMG_0001.jpg") != std::string::npos);
    CHECK(d.find("invoice-2024.pdf") != std::string::npos);
    CHECK(d.find("Images: 1 files") != std::string::npos);
    CHECK(d.find("secret.txt") == std::string::npos);  // inside an app bundle
    CHECK(d.find("SSSS") == std::string::npos);        // file contents are never read
}

TEST_CASE("ai: suggested rules are validated, with one corrective retry") {
    TempTree t;
    t.file("a.jpg", 10);
    t.file("b.pdf", 10);
    LocalFileSystem lfs;
    auto tree = Scanner(lfs, ScanOptions::defaultsFor(toUtf8(t.root()))).run();

    FakeOllama ollama;
    ollama.answers = {
        R"({"name":"Bad","description":"x","rules":[{"name":"r","match":{"types":["Pictures"]},"folder":"P"}],"fallbackFolder":""})",
        R"({"name":"By kind","description":"Photos and papers","rules":[{"name":"Photos","match":{"types":["Images"]},"folder":"Photos/{year}"}],"fallbackFolder":"Other"})",
    };
    OllamaSettings s;
    s.host = ollama.url;
    s.model = "llama3.2";
    const RuleSet rules = suggestRules(s, *tree->root, "keep it simple");
    CHECK(rules.name == "AI: By kind");
    REQUIRE(rules.rules.size() == 1);
    CHECK(rules.rules[0].folder == "Photos/{year}");
    CHECK(rules.fallbackFolder == "Other");
    REQUIRE(ollama.requests.size() == 2);  // first answer had an unknown type -> retried
    CHECK(ollama.requests[0].get("model").asString() == "llama3.2");
    CHECK(ollama.requests[0].get("format").isObject());
    CHECK(ollama.requests[1].get("messages").size() == 4);  // system, user, bad answer, correction

    std::vector<std::string> models;
    REQUIRE(!ollamaModels(s, models));
    CHECK(models == std::vector<std::string>{"llama3.2", "qwen2.5"});
}

TEST_CASE("ai: unreachable Ollama gives a clear error") {
    TempTree t;
    t.file("a.jpg", 10);
    LocalFileSystem lfs;
    auto tree = Scanner(lfs, ScanOptions::defaultsFor(toUtf8(t.root()))).run();
    OllamaSettings s;
    s.host = "http://127.0.0.1:9";  // discard port: nothing listens
    bool threw = false;
    try {
        suggestRules(s, *tree->root);
    } catch (const std::runtime_error& e) {
        threw = std::string(e.what()).find("cannot reach Ollama") != std::string::npos;
    }
    CHECK(threw);
}
