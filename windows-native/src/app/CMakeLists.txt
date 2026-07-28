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

# The .rc references the icon relative to its own directory.
set_source_files_properties(app.rc PROPERTIES
  VS_SETTINGS "AdditionalIncludeDirectories=${CMAKE_CURRENT_SOURCE_DIR}")
