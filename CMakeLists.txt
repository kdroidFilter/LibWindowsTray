cmake_minimum_required(VERSION 3.13 FATAL_ERROR) # target_link_directories

project(tray LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

# Generate 'compile_commands.json' for clang_complete
set(CMAKE_COLOR_MAKEFILE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)


# Sources for libtray

list(APPEND SRCS ${CMAKE_CURRENT_SOURCE_DIR}/tray_windows.c)


add_library(tray SHARED ${SRCS})
set_property(TARGET tray PROPERTY C_STANDARD 99)
target_compile_definitions(tray PRIVATE TRAY_EXPORTS)
set_target_properties(tray PROPERTIES C_VISIBILITY_PRESET hidden)

target_compile_definitions(tray PRIVATE TRAY_WINAPI=1 WIN32_LEAN_AND_MEAN NOMINMAX)


INSTALL(TARGETS tray DESTINATION lib)

IF (NOT WIN32)
    INSTALL(FILES tray.h DESTINATION include)
ENDIF ()


# Tray Example
add_executable(tray_example ${CMAKE_CURRENT_SOURCE_DIR}/example.c)
target_link_libraries(tray_example PRIVATE tray)
add_dependencies(tray_example tray) # Ensure that tray is built before tray_example

configure_file(${CMAKE_CURRENT_LIST_DIR}/icon.ico ${CMAKE_BINARY_DIR}/icon.ico COPYONLY)
configure_file(${CMAKE_CURRENT_LIST_DIR}/icon2.ico ${CMAKE_BINARY_DIR}/icon2.ico COPYONLY)

