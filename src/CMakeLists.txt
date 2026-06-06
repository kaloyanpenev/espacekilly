# Executable target
add_executable(${PROJECT_NAME} main.cpp)

add_subdirectory(matcher)
target_link_libraries(${PROJECT_NAME} PRIVATE matcher)

if (BUILD_TESTING)
  add_test(NAME RunMain COMMAND ${PROJECT_NAME})
endif()
