cmake_minimum_required(VERSION 3.16)
project(pu-cli VERSION 0.3.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(CURL REQUIRED)
find_package(nlohmann_json 3.10 REQUIRED)

# Core library: HTTP client, platform utilities, Context/Delegation, and LLM backends
add_library(pu_core STATIC
    src/infra/curl_http_client.cpp
    src/infra/platform.cpp
    src/runtime/context.cpp
    src/runtime/delegation.cpp
    src/runtime/delegation_stack.cpp
    src/runtime/orchestrator.cpp
    src/backends/ollama/ollama_backend.cpp
    src/backends/openai/openai_backend.cpp
    src/backends/common/streaming_json_parser.cpp
)
target_include_directories(pu_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(pu_core PUBLIC CURL::libcurl nlohmann_json::nlohmann_json)
add_library(pu::core ALIAS pu_core)

# Agent library: Agent manager, executor, tools, tool registry, and conversation store
add_library(pu_agent STATIC
    src/agent/agent_config.cpp
    src/agent/agent_manager.cpp
    src/agent/executor.cpp
    src/agent/agent_factory.cpp
    src/agent/tool_registry.cpp
    src/agent/llm_agent.cpp
    src/tools/command_executor.cpp
    src/tools/builtin_tools.cpp
    src/tools/fork_tools.cpp          # ← NEW: Fork-Merge tools
    src/tools/python_tool.cpp
    src/conversation/conversation_store.cpp
)
target_include_directories(pu_agent PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(pu_agent PUBLIC pu_core nlohmann_json::nlohmann_json)
add_library(pu::agent ALIAS pu_agent)

# CLI application: entry point, chat/ask/learn commands, UI helpers, session management
add_executable(pu
    src/app/main.cpp
    src/app/cli.cpp
    src/app/ui.cpp
    src/app/session.cpp
    src/app/renderer.cpp
)
target_include_directories(pu PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/src/app)
target_link_libraries(pu PRIVATE pu_agent pu_core)

# Compiler warnings
if(MSVC)
    target_compile_options(pu_core PRIVATE /W4)
    target_compile_options(pu_agent PRIVATE /W4)
    target_compile_options(pu PRIVATE /W4)
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