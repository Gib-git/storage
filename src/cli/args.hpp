#pragma once

// Tiny declarative command-line parser: subcommands, --long/-s options with
// values, boolean flags and positionals. Avoids pulling in a third-party library.

#include <cstdio>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace cli {

struct UsageError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct OptionSpec {
    std::string name;   // long name without dashes
    char shortName = 0;
    bool takesValue = false;
    std::string help;
    std::string valueName = "VALUE";
};

struct Command {
    std::string name;
    std::string summary;
    std::vector<std::string> positionals;  // names, all required
    std::vector<OptionSpec> options;

    Command& positional(std::string n) {
        positionals.push_back(std::move(n));
        return *this;
    }
    Command& option(std::string n, char s, std::string valueName, std::string help) {
        options.push_back({std::move(n), s, true, std::move(help), std::move(valueName)});
        return *this;
    }
    Command& flag(std::string n, char s, std::string help) {
        options.push_back({std::move(n), s, false, std::move(help), {}});
        return *this;
    }
};

struct Parsed {
    std::string command;
    std::vector<std::string> positional;
    std::map<std::string, std::string> values;
    std::set<std::string> flags;

    bool has(const std::string& f) const { return flags.count(f) > 0; }
    std::string value(const std::string& k, const std::string& def = {}) const {
        auto it = values.find(k);
        return it == values.end() ? def : it->second;
    }
    long long number(const std::string& k, long long def) const {
        auto it = values.find(k);
        if (it == values.end()) return def;
        try {
            size_t used = 0;
            const long long v = std::stoll(it->second, &used);
            if (used != it->second.size()) throw std::invalid_argument(k);
            return v;
        } catch (const std::exception&) {
            throw UsageError("--" + k + " expects a number, got '" + it->second + "'");
        }
    }
};

class Parser {
public:
    Parser(std::string program, std::string about) : program_(std::move(program)), about_(std::move(about)) {}

    Command& global() { return global_; }
    Command& add(std::string name, std::string summary) {
        commands_.push_back({std::move(name), std::move(summary), {}, {}});
        return commands_.back();
    }

    Parsed parse(int argc, char** argv) const {
        Parsed p;
        std::vector<std::string> args(argv + 1, argv + argc);
        const Command* cmd = nullptr;
        for (size_t i = 0; i < args.size(); ++i) {
            const std::string& a = args[i];
            if (a.size() > 1 && a[0] == '-' && a != "-") {
                std::string name, inlineValue;
                bool hasInline = false;
                const OptionSpec* spec = nullptr;
                if (a.rfind("--", 0) == 0) {
                    name = a.substr(2);
                    if (auto eq = name.find('='); eq != std::string::npos) {
                        inlineValue = name.substr(eq + 1);
                        name = name.substr(0, eq);
                        hasInline = true;
                    }
                    spec = find(cmd, name, 0);
                } else if (a.size() == 2) {
                    spec = find(cmd, {}, a[1]);
                }
                if (!spec) throw UsageError("unknown option '" + a + "'");
                if (spec->takesValue) {
                    if (hasInline) {
                        p.values[spec->name] = inlineValue;
                    } else {
                        if (i + 1 >= args.size()) throw UsageError("option '" + a + "' needs a value");
                        p.values[spec->name] = args[++i];
                    }
                } else {
                    p.flags.insert(spec->name);
                }
            } else if (!cmd) {
                for (const auto& c : commands_)
                    if (c.name == a) cmd = &c;
                if (!cmd) throw UsageError("unknown command '" + a + "'");
                p.command = a;
            } else {
                p.positional.push_back(a);
            }
        }
        if (p.has("help") || p.has("version")) return p;
        if (!cmd) throw UsageError("missing command");
        if (p.positional.size() < cmd->positionals.size())
            throw UsageError("missing <" + cmd->positionals[p.positional.size()] + ">");
        if (p.positional.size() > cmd->positionals.size())
            throw UsageError("unexpected argument '" + p.positional[cmd->positionals.size()] + "'");
        return p;
    }

    void printHelp(const std::string& command = {}) const {
        const Command* cmd = nullptr;
        for (const auto& c : commands_)
            if (c.name == command) cmd = &c;
        if (!cmd) {
            std::printf("%s\n\nUsage: %s [global options] <command> [options]\n\nCommands:\n", about_.c_str(),
                        program_.c_str());
            for (const auto& c : commands_) std::printf("  %-10s %s\n", c.name.c_str(), c.summary.c_str());
            std::printf("\nGlobal options:\n");
            printOptions(global_);
            std::printf("\nRun '%s <command> --help' for command options.\n", program_.c_str());
            return;
        }
        std::printf("%s\n\nUsage: %s %s [options]", cmd->summary.c_str(), program_.c_str(), cmd->name.c_str());
        for (const auto& pos : cmd->positionals) std::printf(" <%s>", pos.c_str());
        std::printf("\n\nOptions:\n");
        printOptions(*cmd);
        printOptions(global_);
    }

private:
    const OptionSpec* find(const Command* cmd, const std::string& name, char s) const {
        auto match = [&](const Command& c) -> const OptionSpec* {
            for (const auto& o : c.options)
                if ((!name.empty() && o.name == name) || (s && o.shortName == s)) return &o;
            return nullptr;
        };
        if (cmd)
            if (auto* o = match(*cmd)) return o;
        return match(global_);
    }

    static void printOptions(const Command& c) {
        for (const auto& o : c.options) {
            std::string left = o.shortName ? std::string("-") + o.shortName + ", " : "    ";
            left += "--" + o.name;
            if (o.takesValue) left += " " + o.valueName;
            std::printf("  %-28s %s\n", left.c_str(), o.help.c_str());
        }
    }

    std::string program_, about_;
    Command global_;
    std::vector<Command> commands_;
};

}  // namespace cli
