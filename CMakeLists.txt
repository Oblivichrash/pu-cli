cmake_minimum_required(VERSION 3.16)
project(pu-cli VERSION 0.4.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(CURL REQUIRED)
find_package(nlohmann_json 3.10 REQUIRED)
find_package(spdlog REQUIRED)

# Engine library: infra, session, LLM providers, MCP, core logging, agent
# config/manager, executor, tools, runtime, and command router
add_library(pu_engine STATIC
    # core / infra
    src/infra/curl_http_client.cpp
    src/infra/platform.cpp
    src/core/logging.cpp
    # session
    src/session/workspace.cpp
    src/session/transcript.cpp
    src/session/memory.cpp
    src/session/session.cpp
    src/session_store.cpp
    # llm
    src/llm/ollama_provider.cpp
    src/llm/openai_provider.cpp
    src/llm/streaming_json_parser.cpp
    src/llm/provider_factory.cpp
    # mcp
    src/mcp/stdio_transport.cpp
    src/mcp/json_rpc_client.cpp
    src/mcp/mcp_client.cpp
    # agent
    src/agent_config.cpp
    src/agent_manager.cpp
    src/executor.cpp
    src/tools/toolbox.cpp
    src/tools/builtin_tools.cpp
    src/tools/mcp_tool.cpp
    src/runtime.cpp
    src/command_router.cpp
)
target_include_directories(pu_engine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(pu_engine PUBLIC CURL::libcurl nlohmann_json::nlohmann_json spdlog::spdlog)
add_library(pu::engine ALIAS pu_engine)

# Config-tools library: agent CRUD, interactive wizard, prompt templates, model scanning, system probe
add_library(pu_config_tools STATIC
    src/config_tools/agent_crud.cpp
    src/config_tools/interactive_wizard.cpp
    src/config_tools/model_scanner.cpp
    src/config_tools/provider_registry.cpp
    src/config_tools/system_probe.cpp
)
target_include_directories(pu_config_tools PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(pu_config_tools PUBLIC pu_engine nlohmann_json::nlohmann_json)
add_library(pu::config_tools ALIAS pu_config_tools)

# CLI application: entry point, chat/ask/learn commands, UI helpers, session management
add_executable(pu
    src/main.cpp
    src/cli.cpp
    src/config_cli.cpp
)
target_include_directories(pu PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(pu PRIVATE pu_engine pu_config_tools spdlog::spdlog)

# Compiler warnings
if(MSVC)
    target_compile_options(pu_engine PRIVATE /W4 /D_CRT_SECURE_NO_WARNINGS)
    target_compile_options(pu_config_tools PRIVATE /W4 /D_CRT_SECURE_NO_WARNINGS)
    target_compile_options(pu PRIVATE /W4 /D_CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(pu_engine PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(pu_config_tools PRIVATE -Wall -Wextra -Wpedantic)
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

# Prompt templates data file
set(PU_DATA_DIR "${CMAKE_INSTALL_FULL_DATADIR}/pu")
target_compile_definitions(pu PRIVATE PU_DATA_DIR="${PU_DATA_DIR}")
install(FILES data/prompts.json DESTINATION ${CMAKE_INSTALL_DATADIR}/pu)
