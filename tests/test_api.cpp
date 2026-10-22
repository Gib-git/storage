#include <sstream>
#include <thread>

#include "TempTree.hpp"
#include "check.hpp"
#include "storage/core/api.hpp"
#include "storage/core/paths.hpp"

using namespace storage;
using json::Value;
namespace fs = std::filesystem;

namespace {
std::vector<Value> mcp(Service& svc, const std::vector<std::string>& lines) {
    std::stringstream in, out;
    for (const auto& l : lines) in << l << "\n";
    runMcpServer(svc, in, out);
    std::vector<Value> replies;
    for (std::string line; std::getline(out, line);) replies.push_back(Value::parse(line));
    return replies;
}

Value toolResult(const Value& reply) {
    return Value::parse(reply.get("result").get("content")[0].get("text").asString());
}
}  // namespace

TEST_CASE("api: MCP handshake, tool list and errors") {
    TempTree db;
    Service svc(db.root() / "index.db", db.root() / "journal.db");
    auto r = mcp(svc, {
                          R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"t","version":"1"}}})",
                          R"({"jsonrpc":"2.0","method":"notifications/initialized"})",
                          R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})",
                          R"({"jsonrpc":"2.0","id":3,"method":"nope"})",
                          "this is not json",
                          R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"apply_plan","arguments":{"plan_id":"x","confirm":false}}})",
                      });
    REQUIRE(r.size() == 5);  // the notification gets no reply
    CHECK(r[0].get("result").get("serverInfo").get("name").asString() == "storage");
    CHECK(r[0].get("result").get("protocolVersion").asString() == "2025-06-18");
    CHECK(r[1].get("result").get("tools").size() == Service::tools().size());
    CHECK(r[2].get("error").get("code").asInt() == -32601);
    CHECK(r[3].get("error").get("code").asInt() == -32700);
    CHECK(r[4].get("result").get("isError").asBool());
}

TEST_CASE("api: plan, confirm, apply and undo through MCP") {
    TempTree t, db;
    t.file("a/photo.jpg", 4000, 'x');
    t.file("b/photo-copy.jpg", 4000, 'x');
    Service svc(db.root() / "index.db", db.root() / "journal.db");
    const std::string path = toUtf8(t.root());

    auto r = mcp(svc, {R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"find_duplicates","arguments":{"path":)" +
                       Value(path).dump() + R"(}}})"});
    REQUIRE(r.size() == 1);
    const Value found = toolResult(r[0]);
    CHECK(found.get("group_count").asInt() == 1);
    const std::string plan = found.get("plan_id").asString();
    REQUIRE(!plan.empty());

    // Without confirm nothing happens.
    bool refused = false;
    try {
        svc.call("apply_plan", Value::object({{"plan_id", plan}}));
    } catch (const std::invalid_argument&) {
        refused = true;
    }
    CHECK(refused);
    CHECK(fs::exists(t.root() / "b" / "photo-copy.jpg"));

    const Value applied = svc.call("apply_plan", Value::object({{"plan_id", plan}, {"confirm", true}}));
    CHECK(applied.get("done").asInt() == 1);
    CHECK(!fs::exists(t.root() / "b" / "photo-copy.jpg") || !fs::exists(t.root() / "a" / "photo.jpg"));

    // A plan runs at most once.
    bool reused = false;
    try {
        svc.call("apply_plan", Value::object({{"plan_id", plan}, {"confirm", true}}));
    } catch (const std::invalid_argument&) {
        reused = true;
    }
    CHECK(reused);

    const Value undone = svc.call("undo", Value::object({{"batch_id", applied.get("batch_id").asInt()}, {"confirm", true}}));
    CHECK(undone.get("undone").asInt() == 1);
    CHECK(fs::exists(t.root() / "a" / "photo.jpg"));
    CHECK(fs::exists(t.root() / "b" / "photo-copy.jpg"));
}

TEST_CASE("api: REST server requires the token and serves tools") {
    TempTree t, db;
    t.file("x/big.bin", 9000);
    Service svc(db.root() / "index.db", db.root() / "journal.db");
    const std::string token = "0123456789abcdef0123456789abcdef";
    http::Server server([&](const http::Request& r) { return handleRest(svc, r, token); });
    REQUIRE(!server.listen("127.0.0.1", 0));
    std::thread th([&] { server.serve(); });
    const std::string base = "http://127.0.0.1:" + std::to_string(server.port());

    http::Result res;
    REQUIRE(!http::get(base + "/v1/health", res));
    CHECK(res.status == 200);
    CHECK(Value::parse(res.body).get("ok").asBool());

    REQUIRE(!http::post(base + "/v1/tools/du", Value::object({{"path", toUtf8(t.root())}}).dump(), res));
    CHECK(res.status == 401);  // no token

    // Our client has no header option, so exercise the handler directly for the authorised paths.
    http::Request req;
    req.method = "POST";
    req.path = "/v1/tools/du";
    req.headers["authorization"] = "Bearer " + token;
    req.body = Value::object({{"path", toUtf8(t.root())}, {"depth", 1}}).dump();
    const auto ok = handleRest(svc, req, token);
    CHECK(ok.status == 200);
    CHECK(Value::parse(ok.body).get("size").asInt() == 9000);
    req.path = "/v1/tools/does_not_exist";
    CHECK(handleRest(svc, req, token).status == 400);
    req.headers["authorization"] = "Bearer wrong";
    CHECK(handleRest(svc, req, token).status == 401);

    server.stop();
    // Unblock accept() so the serving thread can exit.
    http::get(base + "/v1/health", res, 1);
    th.join();
}
