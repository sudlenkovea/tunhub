# Engine: everything the privileged side does, plus the IPC both sides speak.
add_library(tunhub_engine STATIC
  paths.cpp
  log.cpp
  proc.cpp
  uapi.cpp
  netconfig.cpp
  openvpn.cpp
  supervisor.cpp
  ipc.cpp
  engine_host.cpp
)

target_include_directories(tunhub_engine PUBLIC include)
target_link_libraries(tunhub_engine PUBLIC tunhub_core iphlpapi Ws2_32 advapi32)
