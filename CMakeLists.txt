cmake_minimum_required(VERSION 3.16)
project(pu-cli VERSION 0.4.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(CURL REQUIRED)
find_package(spdlog REQUIRED)
find_package(Boost REQUIRED COMPONENTS system program_options json)

find_path(HTTPLIB_INCLUDE_DIR NAMES httplib.h)
if(NOT HTTPLIB_INCLUDE_DIR)
  message(FATAL_ERROR "cpp-httplib headers not found")
endif()

add_library(pu_core STATIC
    src/infra/curl_http_client.cpp
    src/infra/platform.cpp
    src/session/workspace.cpp
    src/session/transcript.cpp
    src/session/memory.cpp
    src/session/session.cpp
    src/core/logging.cpp
    src/llm/ollama_provider.cpp
    src/llm/openai_provider.cpp
    src/llm/streaming_json_parser.cpp
    src/mcp/stdio_transport.cpp
    src/mcp/http_transport.cpp
    src/mcp/json_rpc_client.cpp
    src/mcp/mcp_client.cpp
)
target_include_directories(pu_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(pu_core PUBLIC
    CURL::libcurl
    spdlog::spdlog
    Boost::json
    Boost::system
    Boost::program_options
)
add_library(pu::core ALIAS pu_core)

add_library(pu_agent STATIC
    src/agent_config.cpp
    src/agent_manager.cpp
    src/executor.cpp
    src/tools/toolbox.cpp
    src/tools/builtin_tools.cpp
    src/tools/mcp_tool.cpp
    src/runtime.cpp
    src/command_router.cpp
)
target_include_directories(pu_agent PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(pu_agent PUBLIC pu_core spdlog::spdlog)
add_library(pu::agent ALIAS pu_agent)

add_executable(pu
    src/app/main.cpp
    src/app/cli.cpp
    src/app/serve.cpp
)
target_include_directories(pu PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app
    ${HTTPLIB_INCLUDE_DIR}
)
target_link_libraries(pu PRIVATE pu_agent pu_core spdlog::spdlog)

if(MSVC)
    target_compile_options(pu_core PRIVATE /W4 /D_CRT_SECURE_NO_WARNINGS)
    target_compile_options(pu_agent PRIVATE /W4 /D_CRT_SECURE_NO_WARNINGS)
    target_compile_options(pu PRIVATE /W4 /D_CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(pu_core PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(pu_agent PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(pu PRIVATE -Wall -Wextra -Wpedantic)
endif()

option(BUILD_TESTS "Build unit tests" ON)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

include(GNUInstallDirs)
install(TARGETS pu RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/web/ DESTINATION ${CMAKE_INSTALL_DATADIR}/pu/web)
