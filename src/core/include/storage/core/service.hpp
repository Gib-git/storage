#pragma once

#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "storage/core/journal.hpp"
#include "storage/core/json.hpp"

namespace storage {

// The operations exposed to scripts and AI agents (REST API and MCP server).
// Anything that changes files is two-step: a planning tool returns a plan_id and a
// summary; apply_plan with {"confirm": true} then runs it through the journal.
class Service {
public:
    struct Tool {
        std::string name;
        std::string description;
        json::Value inputSchema;  // JSON Schema for the arguments
        bool changesFiles = false;
    };

    explicit Service(std::filesystem::path indexPath = {}, std::filesystem::path journalPath = {});

    static const std::vector<Tool>& tools();
    // Throws std::invalid_argument for bad arguments and std::runtime_error for failures.
    json::Value call(const std::string& tool, const json::Value& args);

private:
    struct Plan {
        std::string kind, title, volume;
        std::vector<JournalOp> ops;
        uint64_t bytes = 0;
    };
    std::string remember(Plan plan);

    json::Value scan(const json::Value& a);
    json::Value du(const json::Value& a);
    json::Value listScans(const json::Value& a);
    json::Value findDuplicates(const json::Value& a);
    json::Value planOrganize(const json::Value& a);
    json::Value planCleanup(const json::Value& a);
    json::Value insights(const json::Value& a);
    json::Value applyPlan(const json::Value& a);
    json::Value history(const json::Value& a);
    json::Value undo(const json::Value& a);
    json::Value presets(const json::Value& a);
    json::Value nasList(const json::Value& a);

    std::filesystem::path indexPath_, journalPath_;
    std::mutex mu_;
    std::map<std::string, Plan> plans_;
    int nextPlan_ = 1;
};

}  // namespace storage
