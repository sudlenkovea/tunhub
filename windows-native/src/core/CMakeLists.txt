# Core: models, parsers, JSON, keys. No UI and no service dependencies, so it can be
# unit-tested on its own.
add_library(tunhub_core STATIC
  str.cpp
  json.cpp
  ipaddr.cpp
  wgkey.cpp
  models.cpp
  wgquick_parser.cpp
  ovpn_parser.cpp
  conflict_checker.cpp
)

target_include_directories(tunhub_core PUBLIC include)
target_link_libraries(tunhub_core PUBLIC Ws2_32 bcrypt)
