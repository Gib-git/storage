#include "storage/core/api.hpp"

#include <fstream>
#include <iostream>
#include <random>
#include <sstream>

#include "storage/core/paths.hpp"
#include "storage/core/version.hpp"

namespace storage {

using json::Value;

namespace {

Value toolList() {
    Value tools = Value::array();
    for (const auto& t : Service::tools()) {
        tools.push_back(Value::object({{"name", t.name},
                                       {"description", t.description},
                                       {"inputSchema", t.inputSchema},
                                       {"annotations", Value::object({{"readOnlyHint", !t.changesFiles},
                                                                      {"destructiveHint", t.changesFiles},
                                                                      {"openWorldHint", false}})}}));
    }
    return tools;
}

Value rpcError(const Value& id, int code, const std::string& message) {
    return Value::object({{"jsonrpc", "2.0"}, {"id", id}, {"error", Value::object({{"code", code}, {"message", message}})}});
}

bool sameToken(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0 && !a.empty();
}

}  // namespace

void runMcpServer(Service& service, std::istream& in, std::ostream& out) {
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line == "\r") continue;
        Value msg;
        try {
            msg = Value::parse(line);
        } catch (const std::exception&) {
            out << rpcError(Value(), -32700, "parse error").dump() << "\n" << std::flush;
            continue;
        }
        const bool isRequest = msg.contains("id");
        const Value id = msg.get("id");
        const std::string method = msg.get("method").asString();
        const Value& params = msg.get("params");
        if (!isRequest) continue;  // notifications (e.g. notifications/initialized) need no answer

        Value result;
        if (method == "initialize") {
            result = Value::object({
                {"protocolVersion", params.get("protocolVersion").asString("2025-06-18")},
                {"capabilities", Value::object({{"tools", Value::object({{"listChanged", false}})}})},
                {"serverInfo", Value::object({{"name", "storage"}, {"version", kVersion}})},
                {"instructions",
                 "Storage indexes disks and NAS shares, finds duplicates, organizes folders and cleans junk. Tools that change "
                 "files only plan: they return a plan_id and a summary. Show the user what will happen and call apply_plan "
                 "with confirm=true only after they agree. Every applied change can be reverted with undo."},
            });
        } else if (method == "ping") {
            result = Value::object();
        } else if (method == "tools/list") {
            result = Value::object({{"tools", toolList()}});
        } else if (method == "tools/call") {
            const std::string name = params.get("name").asString();
            Value args = params.get("arguments");
            if (args.isNull()) args = Value::object();
            try {
                const Value r = service.call(name, args);
                result = Value::object({{"content", Value::array({Value::object({{"type", "text"}, {"text", r.dump(2)}})})},
                                        {"isError", false}});
            } catch (const std::exception& e) {
                result = Value::object({{"content", Value::array({Value::object({{"type", "text"}, {"text", std::string("Error: ") + e.what()}})})},
                                        {"isError", true}});
            }
        } else {
            out << rpcError(id, -32601, "method not found: " + method).dump() << "\n" << std::flush;
            continue;
        }
        out << Value::object({{"jsonrpc", "2.0"}, {"id", id}, {"result", result}}).dump() << "\n" << std::flush;
    }
}

http::Response handleRest(Service& service, const http::Request& req, const std::string& token) {
    auto json = [](int status, const Value& v) { return http::Response{status, "application/json", v.dump(2)}; };
    if (req.path == "/v1/health") return json(200, Value::object({{"ok", true}, {"version", kVersion}}));

    const std::string auth = req.header("authorization");
    const std::string bearer = auth.rfind("Bearer ", 0) == 0 ? auth.substr(7) : std::string();
    if (!sameToken(bearer, token)) return json(401, Value::object({{"error", "missing or wrong bearer token"}}));

    if (req.path == "/v1/tools" && req.method == "GET") return json(200, toolList());
    const std::string prefix = "/v1/tools/";
    if (req.path.rfind(prefix, 0) == 0) {
        if (req.method != "POST") return json(405, Value::object({{"error", "use POST"}}));
        Value args = Value::object();
        if (!req.body.empty()) {
            try {
                args = Value::parse(req.body);
            } catch (const std::exception& e) {
                return json(400, Value::object({{"error", std::string("invalid JSON: ") + e.what()}}));
            }
        }
        try {
            return json(200, service.call(req.path.substr(prefix.size()), args));
        } catch (const std::invalid_argument& e) {
            return json(400, Value::object({{"error", e.what()}}));
        } catch (const std::exception& e) {
            return json(500, Value::object({{"error", e.what()}}));
        }
    }
    return json(404, Value::object({{"error", "not found"}, {"see", "GET /v1/tools"}}));
}

std::string apiToken() {
    const auto path = appDataDir() / "api-token";
    {
        std::ifstream f(path);
        std::string t;
        if (f && std::getline(f, t) && t.size() >= 32) return t;
    }
    std::random_device rd;
    static const char* hex = "0123456789abcdef";
    std::string t;
    for (int i = 0; i < 48; ++i) t.push_back(hex[rd() & 15]);
    std::ofstream(path, std::ios::trunc) << t << "\n";
    std::error_code ec;
    std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace, ec);
    return t;
}

}  // namespace storage
