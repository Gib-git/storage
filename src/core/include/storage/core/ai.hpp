#pragma once

#include <string>
#include <vector>

#include "storage/core/node.hpp"
#include "storage/core/organize.hpp"

namespace storage {

struct OllamaSettings {
    std::string host = "http://localhost:11434";
    std::string model = "llama3.2";
};

// Models installed in Ollama (GET /api/tags).
std::error_code ollamaModels(const OllamaSettings& s, std::vector<std::string>& out);

// What the model is told about a folder: counts by kind and extension, a sample of
// file names, date range and existing subfolders. Never file contents.
std::string describeFolderForAi(const Node& folder, size_t maxNames = 150);

// Ask the model for organize rules for `folder`, optionally steered by `goal`
// ("sort my camera uploads by trip"). The answer is validated as a RuleSet; one
// corrective retry is made if it is not. Throws std::runtime_error on failure.
RuleSet suggestRules(const OllamaSettings& s, const Node& folder, const std::string& goal = {});

}  // namespace storage
