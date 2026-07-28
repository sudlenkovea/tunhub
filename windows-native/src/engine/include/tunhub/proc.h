#pragma once
// Process helpers: run a command and capture output, or spawn a long-lived core process
// whose stderr is streamed into the log.

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace tunhub {

struct CommandResult {
    int exitCode = -1;
    std::string output;      // stdout + stderr, merged
    bool ok() const { return exitCode == 0; }
};

/// Run `exe args…` to completion, capturing output. No shell, no window.
CommandResult runCommand(const std::string& exe, const std::vector<std::string>& args,
                         int timeoutMs = 20000);

/// A spawned tunnel core. Its output is delivered line by line to `onOutput` on a reader
/// thread until the process exits.
class ChildProcess {
public:
    using OutputHandler = std::function<void(const std::string& line)>;

    ChildProcess() = default;
    ~ChildProcess();

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    bool start(const std::string& exe, const std::vector<std::string>& args,
               const std::map<std::string, std::string>& env, OutputHandler onOutput,
               std::string* error);

    bool running() const;
    unsigned long pid() const { return pid_; }

    /// Ask politely (console ctrl / WM_CLOSE isn't available for these), then kill.
    void terminate(int graceMs = 3000);

private:
    void* process_ = nullptr;   // HANDLE
    void* stdoutRead_ = nullptr;
    unsigned long pid_ = 0;
    std::shared_ptr<class OutputPump> pump_;
};

/// PIDs of running processes whose executable path starts with `dirPrefix` and whose file
/// name matches one of `names`. Used to reap cores left behind by a previous run.
std::vector<unsigned long> findProcessesInDirectory(const std::string& dirPrefix,
                                                    const std::vector<std::string>& names);

bool killProcess(unsigned long pid, int graceMs = 2000);

}  // namespace tunhub
