# The GUI. WIN32 so no console window appears.
add_executable(TunHub WIN32
  main.cpp
  main_window.cpp
  dialogs.cpp
  store.cpp
  loc.cpp
  app.rc
)

target_link_libraries(TunHub PRIVATE tunhub_engine comctl32 comdlg32 shell32 crypt32)
set_target_properties(TunHub PROPERTIES OUTPUT_NAME "TunHub")

# We embed our own manifest through app.rc. MSBuild otherwise generates one as well and the
# linker refuses two RT_MANIFEST resources with the same id (CVT1100).
target_link_options(TunHub PRIVATE "/MANIFEST:NO")

# The .rc references app.manifest and TunHub.ico by bare name; the resource compiler resolves
# those against its include path, not against the .rc file's directory.
set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} /I\"${CMAKE_CURRENT_SOURCE_DIR}\"" PARENT_SCOPE)
set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} /I\"${CMAKE_CURRENT_SOURCE_DIR}\"")
