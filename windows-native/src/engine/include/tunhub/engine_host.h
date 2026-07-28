#pragma once
// The privileged process's brain: owns the supervisor and answers IPC requests.

#include <memory>

#include "tunhub/ipc.h"
#include "tunhub/log.h"
#include "tunhub/supervisor.h"

namespace tunhub {

class EngineHost {
public:
    explicit EngineHost(FileLog& log);
    ~EngineHost();

    bool run(std::string* error);
    void shutdown();

private:
    ipc::Response handle(const ipc::Request& request);

    FileLog& log_;
    TunnelSupervisor supervisor_;
    std::unique_ptr<ipc::Server> server_;
};

}  // namespace tunhub
