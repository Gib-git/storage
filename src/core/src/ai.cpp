#include "storage/core/ai.hpp"

#include <algorithm>
#include <map>
#include <stdexcept>

#include "storage/core/format.hpp"
#include "storage/core/http.hpp"
#include "storage/core/json.hpp"

namespace storage {

using json::Value;

namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// A JSON schema for the subset of RuleSet the model should produce.
Value ruleSetSchema() {
    Value types = Value::array();
    for (int i = static_cast<int>(Category::Image); i <= static_cast<int>(Category::Other); ++i)
        types.push_back(categoryName(static_cast<Category>(i)));
    Value strings = Value::object({{"type", "array"}, {"items", Value::object({{"type", "string"}})}});
    Value match = Value::object({{"type", "object"},
                                 {"properties", Value::object({{"types", Value::object({{"type", "array"},
                                                                                        {"items", Value::object({{"type", "string"}, {"enum", types}})}})},
                                                               {"extensions", strings},
                                                               {"nameContains", Value::object({{"type", "string"}})}})}});
    Value rule = Value::object({{"type", "object"},
                                {"properties", Value::object({{"name", Value::object({{"type", "string"}})},
                                                              {"match", match},
                                                              {"folder", Value::object({{"type", "string"}})}})},
                                {"required", Value::array({"name", "match", "folder"})}});
    return Value::object({{"type", "object"},
                          {"properties", Value::object({{"name", Value::object({{"type", "string"}})},
                                                        {"description", Value::object({{"type", "string"}})},
                                                        {"rules", Value::object({{"type", "array"}, {"items", rule}})},
                                                        {"fallbackFolder", Value::object({{"type", "string"}})}})},
                          {"required", Value::array({"name", "description", "rules", "fallbackFolder"})}});
}

const char* kSystemPrompt =
    "You design folder organization rules for a file organizer app. Reply with JSON only, matching the schema.\n"
    "Rules are checked in order and the first match wins. Each rule matches files by kind (types), by extension "
    "(lowercase, no dot) and/or by text in the name, and sends them to `folder`, a template that may use these "
    "tokens: {type} {ext} {EXT} {year} {month} {day} {monthname} {first} {parent}. Use '/' for subfolders. "
    "Photos use the date the photo was taken for {year}/{month}.\n"
    "`fallbackFolder` is where files matching no rule go; use \"\" to leave them where they are.\n"
    "Guidelines: prefer 3-10 clear top-level folders with human names; keep existing good structure; never more than "
    "3 folder levels; do not invent file types that are not in the summary; name the rule set briefly.";

}  // namespace

std::error_code ollamaModels(const OllamaSettings& s, std::vector<std::string>& out) {
    http::Result res;
    if (auto ec = http::get(s.host + "/api/tags", res, 5)) return ec;
    if (res.status != 200) return std::make_error_code(std::errc::protocol_error);
    try {
        const Value v = Value::parse(res.body);
        for (const auto& m : v.get("models").items()) out.push_back(m.get("name").asString());
    } catch (const std::exception&) {
        return std::make_error_code(std::errc::bad_message);
    }
    return {};
}

std::string describeFolderForAi(const Node& folder, size_t maxNames) {
    std::map<std::string, std::pair<uint64_t, uint64_t>> byType, byExt;  // count, bytes
    std::vector<const Node*> files;
    int64_t oldest = 0, newest = 0;
    std::vector<const Node*> stack{&folder};
    while (!stack.empty()) {
        const Node* n = stack.back();
        stack.pop_back();
        if (n->isDir()) {
            if (n != &folder && isPackageName(n->name)) continue;
            for (const auto& c : n->children) stack.push_back(c.get());
            continue;
        }
        if (n->kind != NodeKind::File || isJunkFile(n->name)) continue;
        files.push_back(n);
        auto& t = byType[categoryName(categorize(n->name))];
        ++t.first;
        t.second += n->size;
        const size_t dot = n->name.rfind('.');
        auto& e = byExt[dot == std::string::npos || dot == 0 ? "(none)" : lower(n->name.substr(dot + 1))];
        ++e.first;
        e.second += n->size;
        if (n->mtime && (!oldest || n->mtime < oldest)) oldest = n->mtime;
        newest = std::max(newest, n->mtime);
    }

    std::string s = "Folder: " + folder.name + "\nFiles: " + std::to_string(files.size()) + ", total " + humanSize(folder.size) + "\n";
    if (oldest) s += "Modified between " + formatTime(oldest).substr(0, 10) + " and " + formatTime(newest).substr(0, 10) + "\n";
    s += "\nBy kind:\n";
    for (const auto& [k, v] : byType) s += "- " + k + ": " + std::to_string(v.first) + " files, " + humanSize(v.second) + "\n";
    std::vector<std::pair<std::string, std::pair<uint64_t, uint64_t>>> exts(byExt.begin(), byExt.end());
    std::sort(exts.begin(), exts.end(), [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
    s += "\nMost common extensions:\n";
    for (size_t i = 0; i < exts.size() && i < 25; ++i)
        s += "- " + exts[i].first + ": " + std::to_string(exts[i].second.first) + "\n";
    s += "\nExisting subfolders:\n";
    size_t shown = 0;
    for (const auto& c : folder.children)
        if (c->isDir() && shown++ < 40) s += "- " + c->name + " (" + std::to_string(c->files) + " files)\n";
    s += "\nSample file names:\n";
    // Spread the sample across the folder instead of taking only the first files.
    const size_t step = std::max<size_t>(1, files.size() / std::max<size_t>(1, maxNames));
    for (size_t i = 0, n = 0; i < files.size() && n < maxNames; i += step, ++n) s += "- " + files[i]->name + "\n";
    return s;
}

RuleSet suggestRules(const OllamaSettings& s, const Node& folder, const std::string& goal) {
    Value messages = Value::array(
        {Value::object({{"role", "system"}, {"content", kSystemPrompt}}),
         Value::object({{"role", "user"},
                        {"content", describeFolderForAi(folder) + "\nGoal: " +
                                        (goal.empty() ? std::string("organize this folder sensibly") : goal)}})});

    std::string lastError;
    for (int attempt = 0; attempt < 2; ++attempt) {
        const Value request = Value::object({{"model", s.model},
                                             {"messages", messages},
                                             {"stream", false},
                                             {"format", ruleSetSchema()},
                                             {"options", Value::object({{"temperature", 0.2}})}});
        http::Result res;
        if (auto ec = http::post(s.host + "/api/chat", request.dump(), res))
            throw std::runtime_error("cannot reach Ollama at " + s.host + " (" + ec.message() + "). Is it running?");
        if (res.status == 404) throw std::runtime_error("Ollama does not have the model \"" + s.model + "\" (try: ollama pull " + s.model + ")");
        if (res.status != 200) throw std::runtime_error("Ollama answered HTTP " + std::to_string(res.status) + ": " + res.body.substr(0, 300));

        std::string content;
        try {
            content = Value::parse(res.body).get("message").get("content").asString();
            RuleSet rules = RuleSet::fromJson(Value::parse(content));
            if (rules.rules.empty() && rules.fallbackFolder.empty()) throw std::invalid_argument("no rules and no fallback folder");
            if (rules.name.empty()) rules.name = "AI suggestion";
            rules.name = "AI: " + rules.name;
            return rules;
        } catch (const std::exception& e) {
            lastError = e.what();
            messages.push_back(Value::object({{"role", "assistant"}, {"content", content}}));
            messages.push_back(Value::object({{"role", "user"},
                                              {"content", std::string("That was not valid: ") + e.what() +
                                                              ". Reply again with JSON matching the schema exactly."}}));
        }
    }
    throw std::runtime_error("the model's rules were not usable: " + lastError);
}

}  // namespace storage
