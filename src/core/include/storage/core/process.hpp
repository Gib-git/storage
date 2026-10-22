#pragma once

#include <cstddef>
#include <string>
#include <system_error>
#include <vector>

namespace storage {

// A child process with pipes to its stdin and stdout (stderr goes to stdout).
// Used to drive `ssh` without linking an SSH library.
class Process {
public:
    Process() = default;
    ~Process();
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    // argv[0] is looked up on PATH.
    std::error_code start(const std::vector<std::string>& argv);
    bool running() const;

    std::error_code write(const std::string& data);
    // Blocks until at least one byte arrives; returns 0 at EOF.
    size_t read(char* buf, size_t len, std::error_code& ec);

    void closeStdin();
    // Wait for exit (killing it first if `kill`); returns the exit code or -1.
    int wait(bool kill = false);

private:
#ifdef _WIN32
    void* process_ = nullptr;
    void* stdinWrite_ = nullptr;
    void* stdoutRead_ = nullptr;
#else
    int pid_ = -1;
    int stdinFd_ = -1;
    int stdoutFd_ = -1;
#endif
    int exitCode_ = -1;
};

// Quote a string for a POSIX shell: 'it'\''s' style.
std::string shellQuote(const std::string& s);

}  // namespace storage
