#include "storage/core/node.hpp"

#include <algorithm>
#include <string_view>
#include <utility>

namespace storage {

std::string Node::path(char sep) const {
    std::vector<const Node*> chain;
    for (const Node* n = this; n; n = n->parent) chain.push_back(n);

    std::string out = chain.back()->name;
    for (auto it = chain.rbegin() + 1; it != chain.rend(); ++it) {
        if (out.empty() || (out.back() != '/' && out.back() != '\\')) out.push_back(sep);
        out += (*it)->name;
    }
    return out;
}

void ScanTree::finalize() {
    if (!root) return;

    // Iterative post-order traversal: trees can be far deeper than the call stack.
    std::vector<std::pair<Node*, bool>> stack;
    stack.emplace_back(root.get(), false);
    while (!stack.empty()) {
        auto [node, visited] = stack.back();
        stack.pop_back();

        if (!node->isDir()) {
            node->files = node->kind == NodeKind::File ? 1 : 0;
            node->dirs = 0;
            node->errors = 0;
            continue;
        }
        if (!visited) {
            stack.emplace_back(node, true);
            for (auto& c : node->children) stack.emplace_back(c.get(), false);
            continue;
        }

        uint64_t size = 0, files = 0, dirs = 0;
        uint64_t errors = (node->flags & NodeFlag::Unreadable) ? 1 : 0;
        for (auto& c : node->children) {
            if (!(c->flags & NodeFlag::HardlinkRepeat)) size += c->size;
            files += c->files;
            dirs += c->dirs + (c->isDir() ? 1 : 0);
            errors += c->errors;
        }
        node->size = size;
        node->files = files;
        node->dirs = dirs;
        node->errors = errors;
        std::sort(node->children.begin(), node->children.end(), [](const auto& a, const auto& b) {
            if (a->size != b->size) return a->size > b->size;
            return a->name < b->name;
        });
    }
}

Node* ScanTree::find(const std::string& path) const {
    if (!root) return nullptr;
    const std::string& rootPath = root->name;
    if (path == rootPath) return root.get();
    if (path.size() <= rootPath.size() || path.compare(0, rootPath.size(), rootPath) != 0) return nullptr;

    std::string_view rest(path);
    rest.remove_prefix(rootPath.size());
    Node* cur = root.get();
    while (!rest.empty()) {
        while (!rest.empty() && (rest.front() == '/' || rest.front() == '\\')) rest.remove_prefix(1);
        if (rest.empty()) break;
        const size_t end = rest.find_first_of("/\\");
        const std::string_view part = rest.substr(0, end);
        Node* next = nullptr;
        for (auto& c : cur->children) {
            if (c->name == part) {
                next = c.get();
                break;
            }
        }
        if (!next) return nullptr;
        cur = next;
        rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end);
    }
    return cur;
}

}  // namespace storage
