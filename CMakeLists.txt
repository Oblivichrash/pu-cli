cmake_minimum_required(VERSION 3.16)
project(pu-cli VERSION 0.3.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(CURL REQUIRED)
find_package(nlohmann_json 3.10 REQUIRED)
find_package(spdlog REQUIRED)

# Core library: HTTP client, platform utilities, Workspace, LLM providers, and MCP
add_library(pu_core STATIC
    src/infra/curl_http_client.cpp
    src/infra/platform.cpp
    src/session/workspace.cpp
    src/session/transcript.cpp
    src/session/memory.cpp
    src/session/session.cpp
    src/storage/session_store.cpp
    src/core/logging.cpp
    src/llm/providers/ollama_provider.cpp
    src/llm/providers/openai_provider.cpp
    src/llm/common/streaming_json_parser.cpp
    src/mcp/stdio_transport.cpp
    src/mcp/json_rpc_client.cpp
    src/mcp/mcp_client.cpp
)
target_include_directories(pu_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(pu_core PUBLIC CURL::libcurl nlohmann_json::nlohmann_json spdlog::spdlog)
add_library(pu::core ALIAS pu_core)

# Agent library: Agent manager, executor, tools, toolbox, MCP tool
add_library(pu_agent STATIC
    src/agent/agent_config.cpp
    src/agent/agent_manager.cpp
    src/executor/executor.cpp
    src/tools/toolbox.cpp
    src/tools/command_executor.cpp
    src/tools/builtin_tools.cpp
    src/tools/mcp_tool.cpp
    src/runtime/runtime.cpp
    src/runtime/command_router.cpp
)
target_include_directories(pu_agent PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(pu_agent PUBLIC pu_core nlohmann_json::nlohmann_json spdlog::spdlog)
add_library(pu::agent ALIAS pu_agent)

# CLI application: entry point, chat/ask/learn commands, UI helpers, session management
add_executable(pu
    src/app/main.cpp
    src/app/cli.cpp
    src/app/ui.cpp
)
target_include_directories(pu PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/src/app)
target_link_libraries(pu PRIVATE pu_agent pu_core spdlog::spdlog)

# Compiler warnings
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
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/pu)
