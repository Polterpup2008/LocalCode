# localcode-doctor: headless diagnostics. Everything it does goes through
# localcode_core, which is exactly the point - if it works here it works in the
# GUI, and it can be run on a build server with no display.
add_executable(localcode-doctor main.cpp)
target_link_libraries(localcode-doctor PRIVATE LocalCode::core LocalCode::warnings)
set_target_properties(localcode-doctor PROPERTIES OUTPUT_NAME "localcode-doctor")

# The agent loop reads the skill library from beside the executable, so a build
# of this target alone must still stage it.
add_dependencies(localcode-doctor localcode_skills)
install(TARGETS localcode-doctor RUNTIME DESTINATION bin)
