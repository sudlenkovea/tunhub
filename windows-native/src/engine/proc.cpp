#include "tunhub/proc.h"

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <atomic>
#include <thread>

#include "tunhub/str.h"

#pragma comment(lib, "psapi.lib")

namespace tunhub {
namespace {

/// Quote an argument per the CommandLineToArgvW rules so paths with spaces survive.
std::string quoteArg(const std::string& arg) {
    if (!arg.empty() && arg.find_first_of(" \t\"") == std::string::npos) return arg;
    std::string out = "\"";
    size_t backslashes = 0;
    for (char c : arg) {
        if (c == '\\') { ++backslashes; continue; }
        if (c == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out += '"';
        } else {
            out.append(backslashes, '\\');
            out += c;
        }
        backslashes = 0;
    }
    out.append(backslashes * 2, '\\');
    out += '"';
    return out;
}

std::string buildCommandLine(const std::string& exe, const std::vector<std::string>& args) {
    std::string cmd = quoteArg(exe);
    for (const auto& a : args) cmd += " " + quoteArg(a);
    return cmd;
}

/// Build a UTF-16 environment block: current environment plus the requested overrides.
std::wstring buildEnvironment(const std::map<std::string, std::string>& extra) {
    std::map<std::wstring, std::wstring> merged;
    if (LPWCH env = GetEnvironmentStringsW()) {
        for (LPWCH p = env; *p;) {
            std::wstring entry(p);
            p += entry.size() + 1;
            const auto eq = entry.find(L'=', 1);   // a leading '=' marks drive vars
            if (eq != std::wstring::npos) merged[entry.substr(0, eq)] = entry.substr(eq + 1);
        }
        FreeEnvironmentStringsW(env);
    }
    for (const auto& [k, v] : extra) merged[str::widen(k)] = str::widen(v);

    std::wstring block;
    for (const auto& [k, v] : merged) {
        block += k;
        block += L'=';
        block += v;
        block += L'\0';
    }
    block += L'\0';
    return block;
}

}  // namespace

CommandResult runCommand(const std::string& exe, const std::vector<std::string>& args,
                         int timeoutMs) {
    CommandResult result;

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) return result;
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = writeEnd;
    si.hStdError = writeEnd;
    si.hStdInput = nullptr;

    PROCESS_INFORMATION pi{};
    auto cmd = str::widen(buildCommandLine(exe, args));
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');

    const BOOL ok = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(writeEnd);
    if (!ok) {
        CloseHandle(readEnd);
        result.output = "failed to launch " + exe;
        return result;
    }

    char buf[4096];
    DWORD read = 0;
    while (ReadFile(readEnd, buf, sizeof(buf), &read, nullptr) && read > 0)
        result.output.append(buf, read);
    CloseHandle(readEnd);

    if (WaitForSingleObject(pi.hProcess, static_cast<DWORD>(timeoutMs)) == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        result.output += "\n(timed out)";
    }
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    result.exitCode = static_cast<int>(code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return result;
}

// ── ChildProcess ─────────────────────────────────────────────────────────────

/// Owns the reader thread. Shared with ChildProcess so a terminating process can't pull the
/// buffer out from under the thread.
class OutputPump {
public:
    OutputPump(HANDLE readEnd, ChildProcess::OutputHandler handler)
        : read_(readEnd), handler_(std::move(handler)) {
        thread_ = std::thread([this] { run(); });
    }

    ~OutputPump() {
        stop_ = true;
        if (read_ && read_ != INVALID_HANDLE_VALUE) {
            CancelIoEx(read_, nullptr);
            CloseHandle(read_);
            read_ = nullptr;
        }
        if (thread_.joinable()) thread_.join();
    }

private:
    void run() {
        std::string pending;
        char buf[4096];
        DWORD read = 0;
        while (!stop_ && read_ && ReadFile(read_, buf, sizeof(buf), &read, nullptr) && read > 0) {
            pending.append(buf, read);
            size_t nl;
            while ((nl = pending.find('\n')) != std::string::npos) {
                auto line = pending.substr(0, nl);
                pending.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (!line.empty() && handler_) handler_(line);
            }
        }
        if (!pending.empty() && handler_) handler_(pending);
    }

    HANDLE read_ = nullptr;
    ChildProcess::OutputHandler handler_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

ChildProcess::~ChildProcess() {
    terminate(0);
}

bool ChildProcess::start(const std::string& exe, const std::vector<std::string>& args,
                         const std::map<std::string, std::string>& env,
                         OutputHandler onOutput, std::string* error) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) {
        if (error) *error = "CreatePipe failed";
        return false;
    }
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = writeEnd;
    si.hStdError = writeEnd;

    PROCESS_INFORMATION pi{};
    auto cmd = str::widen(buildCommandLine(exe, args));
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');
    auto envBlock = buildEnvironment(env);

    const BOOL ok = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                                   envBlock.data(), nullptr, &si, &pi);
    CloseHandle(writeEnd);
    if (!ok) {
        CloseHandle(readEnd);
        if (error) *error = "failed to launch " + exe + " (error " +
                            std::to_string(GetLastError()) + ")";
        return false;
    }
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    pid_ = pi.dwProcessId;
    stdoutRead_ = readEnd;
    pump_ = std::make_shared<OutputPump>(readEnd, std::move(onOutput));
    return true;
}

bool ChildProcess::running() const {
    if (!process_) return false;
    return WaitForSingleObject(static_cast<HANDLE>(process_), 0) == WAIT_TIMEOUT;
}

void ChildProcess::terminate(int graceMs) {
    if (process_) {
        if (graceMs > 0 && running()) {
            // These cores have no window and ignore console signals when detached, so a
            // graceful path isn't available — wait briefly in case they're already exiting.
            WaitForSingleObject(static_cast<HANDLE>(process_), static_cast<DWORD>(graceMs));
        }
        if (running()) TerminateProcess(static_cast<HANDLE>(process_), 1);
        CloseHandle(static_cast<HANDLE>(process_));
        process_ = nullptr;
    }
    pump_.reset();            // joins the reader thread and closes the pipe
    stdoutRead_ = nullptr;
    pid_ = 0;
}

// ── process discovery ────────────────────────────────────────────────────────

std::vector<unsigned long> findProcessesInDirectory(const std::string& dirPrefix,
                                                    const std::vector<std::string>& names) {
    std::vector<unsigned long> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;

    const auto prefix = str::lower(dirPrefix) + "\\";
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snap, &entry)) {
        do {
            const auto exeName = str::lower(str::narrow(entry.szExeFile));
            bool nameMatches = false;
            for (const auto& n : names) if (exeName == str::lower(n)) { nameMatches = true; break; }
            if (!nameMatches) continue;

            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (!h) continue;
            wchar_t path[MAX_PATH]{};
            DWORD size = MAX_PATH;
            if (QueryFullProcessImageNameW(h, 0, path, &size)) {
                // Only ours: the binary must live in our install directory.
                if (str::startsWith(str::lower(str::narrow(path)), prefix))
                    out.push_back(entry.th32ProcessID);
            }
            CloseHandle(h);
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return out;
}

bool killProcess(unsigned long pid, int graceMs) {
    HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
    if (!h) return false;
    TerminateProcess(h, 1);
    WaitForSingleObject(h, static_cast<DWORD>(graceMs));
    CloseHandle(h);
    return true;
}

}  // namespace tunhub
