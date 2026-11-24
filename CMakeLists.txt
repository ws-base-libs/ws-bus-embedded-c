cmake_minimum_required(VERSION 3.21)

# ws-bus-embedded-c — static-allocation pub/sub message bus for embedded C.
project(ws-bus-embedded-c
    VERSION 0.1.0
    DESCRIPTION "Static-allocation pub/sub message bus for embedded C firmware (ws-bus-embedded-c)"
    LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set(WS_BUS_WARNING_FLAGS -Wall -Wextra -Werror)
else()
    set(WS_BUS_WARNING_FLAGS "")
endif()

# Resolve ws-error-embedded-c without reaching into a parent project tree.
if(NOT TARGET ws-error-embedded-c)
    set(WS_ERROR_DIR "" CACHE PATH "Path to ws-error-embedded-c checkout (or resolved from $WS_ROOT/ws-libs)")
    set(_ws_err_src "${WS_ERROR_DIR}")
    if(NOT _ws_err_src AND DEFINED ENV{WS_ROOT} AND EXISTS "$ENV{WS_ROOT}/ws-libs/ws-error-embedded-c/CMakeLists.txt")
        set(_ws_err_src "$ENV{WS_ROOT}/ws-libs/ws-error-embedded-c")
    endif()
    if(_ws_err_src AND EXISTS "${_ws_err_src}/CMakeLists.txt")
        add_subdirectory("${_ws_err_src}" "${CMAKE_CURRENT_BINARY_DIR}/_deps/ws-error-embedded-c")
    else()
        find_package(ws-error-embedded-c REQUIRED)
    endif()
endif()

add_library(ws-bus-embedded-c STATIC src/bus.c)
add_library(ws-bus::ws-bus-embedded-c ALIAS ws-bus-embedded-c)

target_link_libraries(ws-bus-embedded-c PUBLIC ws-error-embedded-c)
target_include_directories(ws-bus-embedded-c
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
target_compile_options(ws-bus-embedded-c PRIVATE ${WS_BUS_WARNING_FLAGS})

if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    include(CTest)
    if(BUILD_TESTING)
        add_executable(bus_test tests/bus_test.c)
        target_link_libraries(bus_test PRIVATE ws-bus-embedded-c)
        target_compile_options(bus_test PRIVATE ${WS_BUS_WARNING_FLAGS})
        add_test(NAME bus_test COMMAND bus_test)

        add_executable(basic_bus_example examples/basic_bus.c)
        target_link_libraries(basic_bus_example PRIVATE ws-bus-embedded-c)
        target_compile_options(basic_bus_example PRIVATE ${WS_BUS_WARNING_FLAGS})
    endif()
endif()

include(GNUInstallDirs)
install(TARGETS ws-bus-embedded-c
    EXPORT ws-bus-embedded-c-targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT ws-bus-embedded-c-targets
    NAMESPACE ws-bus::
    FILE ws-bus-embedded-c-config.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ws-bus-embedded-c)
