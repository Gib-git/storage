#include "storage/core/process.hpp"

#include <cerrno>

#ifdef _WIN32
#include <windows.h>

#include "storage/core/paths.hpp"
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace storage {

std::string shellQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

#ifdef _WIN32

namespace {
std::error_code lastError() { return std::error_code(static_cast<int>(GetLastError()), std::system_category()); }

// Quote one argument per the MSVC runtime's command-line parsing rules.
std::wstring quoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') {
            ++backslashes;
        } else if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(c);
            backslashes = 0;
        } else {
            out.append(backslashes, L'\\');
            out.push_back(c);
            backslashes = 0;
        }
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}
}  // namespace

Process::~Process() { wait(true); }

std::error_code Process::start(const std::vector<std::string>& argv) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE inRead = nullptr, inWrite = nullptr, outRead = nullptr, outWrite = nullptr;
    if (!CreatePipe(&inRead, &inWrite, &sa, 0) || !CreatePipe(&outRead, &outWrite, &sa, 0)) return lastError();
    SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);

    std::wstring cmd;
    for (const auto& a : argv) {
        if (!cmd.empty()) cmd.push_back(L' ');
        cmd += quoteArg(fromUtf8(a).wstring());
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inRead;
    si.hStdOutput = outWrite;
    si.hStdError = outWrite;
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    const auto err = lastError();
    CloseHandle(inRead);
    CloseHandle(outWrite);
    if (!ok) {
        CloseHandle(inWrite);
        CloseHandle(outRead);
        return err;
    }
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    stdinWrite_ = inWrite;
    stdoutRead_ = outRead;
    return {};
}

bool Process::running() const {
    return process_ && WaitForSingleObject(static_cast<HANDLE>(process_), 0) == WAIT_TIMEOUT;
}

std::error_code Process::write(const std::string& data) {
    if (!stdinWrite_) return std::make_error_code(std::errc::broken_pipe);
    size_t off = 0;
    while (off < data.size()) {
        DWORD n = 0;
        if (!WriteFile(static_cast<HANDLE>(stdinWrite_), data.data() + off, static_cast<DWORD>(data.size() - off), &n, nullptr))
            return lastError();
        off += n;
    }
    return {};
}

size_t Process::read(char* buf, size_t len, std::error_code& ec) {
    if (!stdoutRead_) return 0;
    DWORD n = 0;
    if (!ReadFile(static_cast<HANDLE>(stdoutRead_), buf, static_cast<DWORD>(len), &n, nullptr)) {
        if (GetLastError() != ERROR_BROKEN_PIPE) ec = lastError();
        return 0;
    }
    return n;
}

void Process::closeStdin() {
    if (stdinWrite_) CloseHandle(static_cast<HANDLE>(stdinWrite_));
    stdinWrite_ = nullptr;
}

int Process::wait(bool kill) {
    if (!process_) return exitCode_;
    closeStdin();
    if (kill) TerminateProcess(static_cast<HANDLE>(process_), 1);
    WaitForSingleObject(static_cast<HANDLE>(process_), INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(static_cast<HANDLE>(process_), &code);
    exitCode_ = static_cast<int>(code);
    CloseHandle(static_cast<HANDLE>(process_));
    process_ = nullptr;
    if (stdoutRead_) CloseHandle(static_cast<HANDLE>(stdoutRead_));
    stdoutRead_ = nullptr;
    return exitCode_;
}

#else

Process::~Process() { wait(true); }

std::error_code Process::start(const std::vector<std::string>& argv) {
    int in[2], out[2];
    if (::pipe(in) != 0) return std::error_code(errno, std::system_category());
    if (::pipe(out) != 0) {
        const int e = errno;
        ::close(in[0]);
        ::close(in[1]);
        return std::error_code(e, std::system_category());
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in[0], 0);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    posix_spawn_file_actions_adddup2(&fa, out[1], 2);
    posix_spawn_file_actions_addclose(&fa, in[1]);
    posix_spawn_file_actions_addclose(&fa, out[0]);

    std::vector<char*> args;
    for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    pid_t pid = -1;
    const int rc = posix_spawnp(&pid, args[0], &fa, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    ::close(in[0]);
    ::close(out[1]);
    if (rc != 0) {
        ::close(in[1]);
        ::close(out[0]);
        return std::error_code(rc, std::system_category());
    }
    ::fcntl(in[1], F_SETFD, FD_CLOEXEC);
    ::fcntl(out[0], F_SETFD, FD_CLOEXEC);
    ::signal(SIGPIPE, SIG_IGN);  // a dead ssh must surface as an error, not kill us
    pid_ = pid;
    stdinFd_ = in[1];
    stdoutFd_ = out[0];
    return {};
}

bool Process::running() const {
    if (pid_ <= 0) return false;
    return ::waitpid(pid_, nullptr, WNOHANG) == 0;
}

std::error_code Process::write(const std::string& data) {
    if (stdinFd_ < 0) return std::make_error_code(std::errc::broken_pipe);
    size_t off = 0;
    while (off < data.size()) {
        const ssize_t n = ::write(stdinFd_, data.data() + off, data.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return std::error_code(errno, std::system_category());
        }
        off += static_cast<size_t>(n);
    }
    return {};
}

size_t Process::read(char* buf, size_t len, std::error_code& ec) {
    if (stdoutFd_ < 0) return 0;
    for (;;) {
        const ssize_t n = ::read(stdoutFd_, buf, len);
        if (n >= 0) return static_cast<size_t>(n);
        if (errno == EINTR) continue;
        ec = std::error_code(errno, std::system_category());
        return 0;
    }
}

void Process::closeStdin() {
    if (stdinFd_ >= 0) ::close(stdinFd_);
    stdinFd_ = -1;
}

int Process::wait(bool kill) {
    if (pid_ <= 0) return exitCode_;
    closeStdin();
    if (kill) ::kill(pid_, SIGTERM);
    int status = 0;
    while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
    }
    exitCode_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    pid_ = -1;
    if (stdoutFd_ >= 0) ::close(stdoutFd_);
    stdoutFd_ = -1;
    return exitCode_;
}

#endif

}  // namespace storage
