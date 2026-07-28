add_executable(tunhub-helper main.cpp)
target_link_libraries(tunhub-helper PRIVATE tunhub_engine)
set_target_properties(tunhub-helper PROPERTIES OUTPUT_NAME "tunhub-helper")
