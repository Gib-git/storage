#include "storage/core/service.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

#include "storage/core/cleaner.hpp"
#include "storage/core/duplicates.hpp"
#include "storage/core/executor.hpp"
#include "storage/core/format.hpp"
#include "storage/core/index.hpp"
#include "storage/core/organize.hpp"
#include "storage/core/paths.hpp"
#include "storage/core/scanner.hpp"
#include "storage/core/ssh_fs.hpp"
#include "storage/core/volumes.hpp"

namespace storage {

using json::Value;

namespace {

Value schema(std::initializer_list<std::pair<std::string, Value>> props, std::vector<std::string> required = {}) {
    Value p = Value::object();
    for (const auto& [k, v] : props) p[k] = v;
    Value req = Value::array();
    for (const auto& r : required) req.push_back(r);
    return Value::object({{"type", "object"}, {"properties", p}, {"required", req}, {"additionalProperties", false}});
}
Value str(const std::string& d) { return Value::object({{"type", "string"}, {"description", d}}); }
Value num(const std::string& d) { return Value::object({{"type", "integer"}, {"description", d}}); }
Value boolean(const std::string& d) { return Value::object({{"type", "boolean"}, {"description", d}}); }

const Value kNas = str("Name of a saved NAS (see nas_list). Omit for this computer.");
const Value kPath = str("Folder to work on. Absolute, or relative to the NAS's default folder.");

std::string needString(const Value& a, const char* key) {
    const std::string v = a.get(key).asString();
    if (v.empty()) throw std::invalid_argument(std::string("missing \"") + key + "\"");
    return v;
}

struct Target {
    FileSystem* fs;
    std::string path;
};

Target resolve(const Value& a) {
    const std::string nas = a.get("nas").asString();
    std::string path = a.get("path").asString();
    if (nas.empty()) {
        if (path.empty()) throw std::invalid_argument("missing \"path\"");
        return {filesystemFor("local"), normalizeLocalPath(path)};
    }
    for (const auto& cfg : loadNasConfigs()) {
        if (cfg.name != nas) continue;
        FileSystem* fs = filesystemFor(cfg.volume());
        if (path.empty() || path == ".") path = cfg.root;
        else if (path[0] != '/') path = (cfg.root.back() == '/' ? cfg.root : cfg.root + "/") + path;
        return {fs, path};
    }
    throw std::invalid_argument("no saved NAS named \"" + nas + "\"");
}

std::unique_ptr<ScanTree> scanTree(const Target& t) {
    auto tree = Scanner(*t.fs, ScanOptions::defaultsFor(t.path)).run();
    if (tree->root->flags & NodeFlag::Unreadable) throw std::runtime_error("cannot read " + t.path);
    return tree;
}

Value nodeJson(const Node& n, int depth, size_t top) {
    Value j = Value::object({{"name", n.name}, {"size", n.size}, {"files", n.files}, {"kind", n.isDir() ? "dir" : "file"},
                             {"modified", n.mtime}});
    if (n.isDir() && depth > 0) {
        Value kids = Value::array();
        for (size_t i = 0; i < n.children.size() && i < top; ++i) kids.push_back(nodeJson(*n.children[i], depth - 1, top));
        if (n.children.size() > top) j["more_children"] = n.children.size() - top;
        j["children"] = kids;
    }
    return j;
}

Value findingList(const std::vector<Finding>& v, size_t limit) {
    Value a = Value::array();
    for (size_t i = 0; i < v.size() && i < limit; ++i)
        a.push_back(Value::object({{"path", v[i].path}, {"size", v[i].size}, {"modified", v[i].mtime}, {"reason", v[i].reason}}));
    return a;
}

Value batchJson(const JournalBatch& b) {
    return Value::object({{"batch_id", b.id}, {"kind", b.kind}, {"title", b.title}, {"state", b.state},
                          {"volume", b.volume}, {"done", b.done}, {"skipped", b.skipped}, {"failed", b.failed},
                          {"undone", b.undone}, {"bytes", b.bytes}, {"created", b.created}});
}

}  // namespace

Service::Service(std::filesystem::path indexPath, std::filesystem::path journalPath)
    : indexPath_(indexPath.empty() ? defaultIndexPath() : std::move(indexPath)),
      journalPath_(journalPath.empty() ? Journal::defaultPath() : std::move(journalPath)) {}

const std::vector<Service::Tool>& Service::tools() {
    static const std::vector<Tool> t = {
        {"scan", "Scan a folder (on this computer or a saved NAS) and save it to the index. Returns totals.",
         schema({{"path", kPath}, {"nas", kNas}}, {"path"})},
        {"du", "Show the largest folders and files under a path, as a size tree.",
         schema({{"path", kPath}, {"nas", kNas}, {"depth", num("Levels to include (default 1)")},
                 {"top", num("Children per folder (default 20)")}},
                {"path"})},
        {"list_scans", "List scans saved in the index.", schema({})},
        {"find_duplicates",
         "Find files with identical contents (SHA-256). Returns duplicate groups and a plan_id that removes every copy "
         "except one per group (chosen by `keep`). Nothing changes until apply_plan.",
         schema({{"path", kPath}, {"nas", kNas}, {"min_size", num("Ignore files smaller than this many bytes (default 1)")},
                 {"keep", str("Which copy to keep: oldest (default), newest, shortest, longest, alpha")},
                 {"action", str("What to do with the extra copies: trash (default), hardlink, clone, delete")},
                 {"skip_hidden", boolean("Ignore hidden files (default true)")}},
                {"path"})},
        {"plan_organize",
         "Plan reorganizing a folder with a preset (see presets) or custom rules (RuleSet JSON). Returns the moves and a "
         "plan_id. Nothing moves until apply_plan.",
         schema({{"path", kPath}, {"nas", kNas}, {"preset", str("Preset name, e.g. \"By file type\"")},
                 {"rules", Value::object({{"type", "object"}, {"description", "Custom RuleSet (see docs/RULES.md)"}})},
                 {"destination", str("Put organized files here instead of in place")}},
                {"path"})},
        {"plan_cleanup",
         "Measure caches, logs and other junk on this computer and plan cleaning them. Returns sizes per location and a "
         "plan_id. Only caches marked Safe are deleted permanently (when permanent=true); everything else goes to the Trash.",
         schema({{"targets", Value::object({{"type", "array"}, {"items", Value::object({{"type", "string"}})},
                                            {"description", "Target ids; default = the recommended ones"}})},
                 {"permanent", boolean("Delete Safe caches permanently instead of trashing (default true)")}})},
        {"insights", "Find large old files, idle project folders (node_modules, target, .venv...) and empty folders.",
         schema({{"path", kPath}, {"nas", kNas}, {"older_than_days", num("Default 365")},
                 {"min_size", num("Minimum size in bytes for large files (default 100 MB)")}},
                {"path"})},
        {"apply_plan",
         "Carry out a plan from find_duplicates, plan_organize or plan_cleanup. Every change is journaled and can be "
         "undone. Requires confirm=true — only call this after the user has approved the plan.",
         schema({{"plan_id", str("From a planning tool")}, {"confirm", boolean("Must be true")}}, {"plan_id", "confirm"}),
         true},
        {"history", "List recent batches of changes Storage made.", schema({{"limit", num("Default 20")}})},
        {"undo", "Revert a batch of changes (see history). Requires confirm=true.",
         schema({{"batch_id", num("Batch to undo")}, {"confirm", boolean("Must be true")}}, {"batch_id", "confirm"}), true},
        {"presets", "List the built-in organize presets with their rules.", schema({})},
        {"nas_list", "List saved NAS connections.", schema({})},
    };
    return t;
}

Value Service::call(const std::string& tool, const Value& args) {
    const Value& a = args;
    if (tool == "scan") return scan(a);
    if (tool == "du") return du(a);
    if (tool == "list_scans") return listScans(a);
    if (tool == "find_duplicates") return findDuplicates(a);
    if (tool == "plan_organize") return planOrganize(a);
    if (tool == "plan_cleanup") return planCleanup(a);
    if (tool == "insights") return insights(a);
    if (tool == "apply_plan") return applyPlan(a);
    if (tool == "history") return history(a);
    if (tool == "undo") return undo(a);
    if (tool == "presets") return presets(a);
    if (tool == "nas_list") return nasList(a);
    throw std::invalid_argument("unknown tool \"" + tool + "\"");
}

std::string Service::remember(Plan plan) {
    std::lock_guard lk(mu_);
    const std::string id = "plan-" + std::to_string(nextPlan_++);
    plans_[id] = std::move(plan);
    return id;
}

Value Service::scan(const Value& a) {
    const Target t = resolve(a);
    auto tree = scanTree(t);
    Index(indexPath_).save(*tree);
    const Node& r = *tree->root;
    return Value::object({{"scan_id", tree->id}, {"root", r.name}, {"volume", tree->volume}, {"bytes", r.size},
                          {"size", humanSize(r.size)}, {"files", r.files}, {"folders", r.dirs}, {"unreadable", r.errors}});
}

Value Service::du(const Value& a) {
    const Target t = resolve(a);
    auto tree = scanTree(t);
    return nodeJson(*tree->root, static_cast<int>(a.get("depth").asInt(1)), static_cast<size_t>(a.get("top").asInt(20)));
}

Value Service::listScans(const Value&) {
    Value arr = Value::array();
    for (const auto& s : Index(indexPath_).scans())
        arr.push_back(Value::object({{"scan_id", s.id}, {"root", s.root}, {"volume", s.volume}, {"bytes", s.bytes},
                                     {"files", s.files}, {"finished", s.finished}, {"complete", s.complete}}));
    return arr;
}

Value Service::findDuplicates(const Value& a) {
    const Target t = resolve(a);
    const std::string action = a.get("action").asString(Action::Trash);
    if (action != Action::Trash && action != Action::Hardlink && action != Action::Clone && action != Action::Delete)
        throw std::invalid_argument("action must be trash, hardlink, clone or delete");
    DupOptions opt;
    opt.minSize = static_cast<uint64_t>(std::max<int64_t>(1, a.get("min_size").asInt(1)));
    opt.skipHidden = a.get("skip_hidden").asBool(true);
    opt.excludeNames = {".git", ".svn", ".hg", "node_modules"};
    const KeepRule rule = parseKeepRule(a.get("keep").asString("oldest"));

    auto tree = scanTree(t);
    Index cache(indexPath_);
    auto groups = DuplicateFinder(*t.fs, opt, &cache).run(*tree->root);
    std::vector<std::vector<bool>> remove(groups.size());
    uint64_t total = 0;
    Value out = Value::array();
    for (size_t g = 0; g < groups.size(); ++g) {
        const size_t keep = chooseKeeper(groups[g], rule);
        remove[g].assign(groups[g].files.size(), true);
        remove[g][keep] = false;
        total += groups[g].reclaimable();
        if (g < 50) {
            Value files = Value::array();
            for (size_t i = 0; i < groups[g].files.size(); ++i)
                files.push_back(Value::object({{"path", groups[g].files[i].path}, {"keep", i == keep}}));
            out.push_back(Value::object({{"size", groups[g].size}, {"copies", groups[g].files.size()},
                                         {"reclaimable", groups[g].reclaimable()}, {"files", files}}));
        }
    }
    Value result = Value::object({{"groups", out}, {"group_count", groups.size()}, {"reclaimable_bytes", total},
                                  {"reclaimable", humanSize(total)}, {"keep_rule", keepRuleName(rule)}, {"action", action}});
    if (!groups.empty()) {
        Plan p{"dedupe", "Duplicates in " + t.path + " (" + action + ")", t.fs->volumeName(), planDedupe(groups, remove, action), total};
        result["plan_id"] = remember(std::move(p));
        result["note"] = "Show the user what will happen, then call apply_plan with this plan_id and confirm=true.";
    }
    return result;
}

Value Service::planOrganize(const Value& a) {
    const Target t = resolve(a);
    RuleSet rules;
    if (a.get("rules").isObject()) {
        rules = RuleSet::fromJson(a.get("rules"));
    } else {
        const std::string want = a.get("preset").asString("By file type");
        bool found = false;
        for (const auto& p : builtinPresets()) {
            std::string n = p.name, w = want;
            for (auto& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            for (auto& c : w) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (n == w) {
                rules = p;
                found = true;
            }
        }
        if (!found) throw std::invalid_argument("unknown preset \"" + want + "\" (see presets)");
    }
    std::string dest = a.get("destination").asString();
    if (!dest.empty() && isLocalVolume(t.fs->volumeName())) dest = normalizeLocalPath(dest);

    auto tree = scanTree(t);
    const OrganizePlan plan = Organizer(*t.fs, rules).plan(*tree->root, dest);
    Value moves = Value::array();
    for (size_t i = 0; i < plan.moves.size() && i < 200; ++i)
        moves.push_back(Value::object({{"from", plan.moves[i].src}, {"to", plan.moves[i].dst}, {"rule", plan.moves[i].rule}}));
    Value result = Value::object({{"rules", rules.name}, {"moves", moves}, {"move_count", plan.moves.size()},
                                  {"bytes", plan.bytes}, {"folders_created", plan.createDirs.size()},
                                  {"emptied_folders_removed", plan.removeDirs.size()}, {"already_in_place", plan.alreadyInPlace},
                                  {"unmatched", plan.unmatched}, {"skipped_name_taken", plan.conflictsSkipped},
                                  {"inside_apps_or_libraries", plan.protectedSkipped}});
    if (!plan.moves.empty()) {
        Plan p{"organize", "Organize " + t.path + " (" + rules.name + ")", t.fs->volumeName(), plan.toOps(), plan.bytes};
        result["plan_id"] = remember(std::move(p));
        result["note"] = "Show the user the moves, then call apply_plan with this plan_id and confirm=true.";
    }
    return result;
}

Value Service::planCleanup(const Value& a) {
    std::vector<std::string> ids;
    for (const auto& v : a.get("targets").items()) ids.push_back(v.asString());
    std::vector<CleanTarget> targets;
    for (const auto& t : cleanCatalogue())
        if (ids.empty() ? t.defaultOn : std::find(ids.begin(), ids.end(), t.id) != ids.end()) targets.push_back(t);
    const auto items = analyzeCleanTargets(*filesystemFor("local"), targets);
    uint64_t total = 0;
    Value list = Value::array();
    for (const auto& i : items) {
        total += i.bytes;
        list.push_back(Value::object({{"id", i.target.id}, {"name", i.target.name}, {"safety", safetyName(i.target.safety)},
                                      {"bytes", i.bytes}, {"size", humanSize(i.bytes)}}));
    }
    Value other = Value::array();
    for (const auto& t : cleanCatalogue())
        if (std::find_if(targets.begin(), targets.end(), [&](const CleanTarget& x) { return x.id == t.id; }) == targets.end())
            other.push_back(Value::object({{"id", t.id}, {"name", t.name}, {"safety", safetyName(t.safety)}}));
    Value result = Value::object({{"targets", list}, {"bytes", total}, {"size", humanSize(total)},
                                  {"other_available_targets", other}});
    if (total) {
        Plan p{"clean", "Clean up " + humanSize(total), "local", planClean(items, a.get("permanent").asBool(true)), total};
        result["plan_id"] = remember(std::move(p));
        result["note"] = "Show the user the sizes, then call apply_plan with this plan_id and confirm=true.";
    }
    return result;
}

Value Service::insights(const Value& a) {
    const Target t = resolve(a);
    auto tree = scanTree(t);
    const int days = static_cast<int>(a.get("older_than_days").asInt(365));
    const uint64_t minSize = static_cast<uint64_t>(a.get("min_size").asInt(100LL * 1024 * 1024));
    const char sep = t.fs->separator();
    return Value::object({{"large_old_files", findingList(findLargeOldFiles(*tree->root, sep, minSize, days, 100), 100)},
                          {"project_junk", findingList(findProjectJunk(*tree->root, sep, std::min(days, 180)), 100)},
                          {"empty_folders", findingList(findEmptyFolders(*tree->root, sep), 200)}});
}

Value Service::applyPlan(const Value& a) {
    if (!a.get("confirm").asBool(false)) throw std::invalid_argument("confirm must be true (ask the user first)");
    const std::string id = needString(a, "plan_id");
    Plan plan;
    {
        std::lock_guard lk(mu_);
        auto it = plans_.find(id);
        if (it == plans_.end()) throw std::invalid_argument("unknown or already used plan_id \"" + id + "\"");
        plan = std::move(it->second);
        plans_.erase(it);  // a plan can run once
    }
    FileSystem* fs = filesystemFor(plan.volume);
    if (!fs) throw std::runtime_error("the NAS for this plan is no longer saved");
    Journal journal(journalPath_);
    Executor::recover(*fs, journal);
    const int64_t batch = journal.createBatch(plan.kind, plan.title, plan.volume, plan.ops);
    const ExecResult r = Executor(*fs, journal).run(batch);
    Value out = batchJson(journal.batch(batch));
    out["freed_or_moved"] = humanSize(r.bytes);
    out["note"] = "Undo with the undo tool and this batch_id.";
    return out;
}

Value Service::history(const Value& a) {
    Value arr = Value::array();
    for (const auto& b : Journal(journalPath_).batches(static_cast<int>(a.get("limit").asInt(20)))) arr.push_back(batchJson(b));
    return arr;
}

Value Service::undo(const Value& a) {
    if (!a.get("confirm").asBool(false)) throw std::invalid_argument("confirm must be true (ask the user first)");
    const int64_t id = a.get("batch_id").asInt(0);
    Journal journal(journalPath_);
    const JournalBatch b = journal.batch(id);
    FileSystem* fs = filesystemFor(b.volume);
    if (!fs) throw std::runtime_error("the NAS for this batch is no longer saved");
    Executor::recover(*fs, journal);
    Executor(*fs, journal).undo(id);
    return batchJson(journal.batch(id));
}

Value Service::presets(const Value&) {
    Value arr = Value::array();
    for (const auto& p : builtinPresets()) arr.push_back(p.toJson());
    return arr;
}

Value Service::nasList(const Value&) {
    Value arr = Value::array();
    for (const auto& c : loadNasConfigs())
        arr.push_back(Value::object({{"name", c.name}, {"volume", c.volume()}, {"root", c.root}}));
    return arr;
}

}  // namespace storage
