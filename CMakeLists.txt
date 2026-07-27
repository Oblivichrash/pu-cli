cmake_minimum_required(VERSION 3.16)
project(pu-cli VERSION 0.3.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(CURL REQUIRED)
find_package(nlohmann_json 3.10 REQUIRED)

# Core library: HTTP client, platform utilities, Workspace/Assignment/CallStack, and LLM providers
add_library(pu_core STATIC
    src/infra/curl_http_client.cpp
    src/infra/platform.cpp
    src/session/workspace.cpp
    src/session/transcript.cpp
    src/session/memory.cpp
    src/session/revision_graph.cpp
    src/session/artifact.cpp
    src/session/assignment.cpp
    src/session/handoff_receipt.cpp
    src/session/call_stack.cpp
    src/session/runtime_spec.cpp
    src/session/session.cpp
    src/storage/session_store.cpp
    src/core/fact_extractor.cpp
    src/core/fork_merge_service.cpp
    src/core/summary_generator.cpp
    src/llm/providers/ollama_provider.cpp
    src/llm/providers/openai_provider.cpp
    src/llm/common/streaming_json_parser.cpp
)
target_include_directories(pu_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(pu_core PUBLIC CURL::libcurl nlohmann_json::nlohmann_json)
add_library(pu::core ALIAS pu_core)

# Agent library: Agent manager, executor, tools, toolbox, and conversation store
add_library(pu_agent STATIC
    src/agent/agent_config.cpp
    src/agent/agent_manager.cpp
    src/executor/executor.cpp
    src/tools/toolbox.cpp
    src/tools/command_executor.cpp
    src/tools/builtin_tools.cpp
    src/tools/fork_tools.cpp
    src/tools/python_tool.cpp
    src/conversation/conversation_store.cpp
    src/runtime/runtime.cpp
    src/runtime/command_router.cpp
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
