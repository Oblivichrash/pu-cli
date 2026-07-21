cmake_minimum_required(VERSION 3.16)
project(pu-cli VERSION 0.3.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(CURL REQUIRED)
find_package(nlohmann_json 3.10 REQUIRED)

# Interface library – public type definitions only
add_library(pu_backend_interface INTERFACE)
target_include_directories(pu_backend_interface INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
add_library(pu::backend_interface ALIAS pu_backend_interface)

# HTTP abstraction library
add_library(pu_http STATIC
    src/http/curl_http_client.cpp
)
target_include_directories(pu_http PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(pu_http PUBLIC CURL::libcurl pu_platform)
add_library(pu::http ALIAS pu_http)

# Platform abstraction library
add_library(pu_platform STATIC src/platform/platform.cpp)
target_include_directories(pu_platform PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
add_library(pu::platform ALIAS pu_platform)

# Backend implementation library
add_library(pu_backend STATIC
    src/backends/ollama/ollama_backend.cpp
    src/backends/openai/openai_backend.cpp
    src/config/agent_config.cpp
    src/conversation/conversation_store.cpp
    src/backends/common/streaming_json_parser.cpp
)
target_include_directories(pu_backend PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(pu_backend
    PUBLIC  pu_backend_interface
    PRIVATE pu_http nlohmann_json::nlohmann_json
)
add_library(pu::backend ALIAS pu_backend)

# Context and call stack library
add_library(pu_context STATIC
    src/context/global_context.cpp
    src/orchestrator/call_stack.cpp
    src/orchestrator/orchestrator.cpp
)
target_include_directories(pu_context PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(pu_context PUBLIC nlohmann_json::nlohmann_json)
add_library(pu::context ALIAS pu_context)

# Agent library
add_library(pu_agent STATIC
    src/agent/agent_manager.cpp
    src/agent/agent_executor.cpp
    src/executor/command_executor.cpp
    src/agent/agent_factory.cpp
    src/renderer/renderer.cpp
    src/agent/tool_registry.cpp
    src/agents/llm/llm_agent.cpp
    src/tools/execute_bash_tool.cpp
    src/tools/python_tool.cpp
    src/tools/create_tool.cpp
    src/tools/write_file_tool.cpp
)
target_include_directories(pu_agent PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(pu_agent
    PUBLIC  pu_backend_interface pu_platform
    PRIVATE pu_backend pu_http nlohmann_json::nlohmann_json pu_context
)
add_library(pu::agent ALIAS pu_agent)

# Main executable - unified CLI
add_executable(pu
    src/app/main.cpp
    src/app/cli.cpp
    src/app/ui.cpp
    src/app/session.cpp
)
target_include_directories(pu PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app
)
target_link_libraries(pu PRIVATE pu_agent pu_context pu_backend pu_http pu_platform)

if(MSVC)
    target_compile_options(pu PRIVATE /W4)
    target_compile_options(pu_backend PRIVATE /W4)
    target_compile_options(pu_http PRIVATE /W4)
    target_compile_options(pu_agent PRIVATE /W4)
    target_compile_options(pu_platform PRIVATE /W4)
    target_compile_options(pu_context PRIVATE /W4)
else()
    target_compile_options(pu PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(pu_backend PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(pu_http PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(pu_agent PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(pu_platform PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(pu_context PRIVATE -Wall -Wextra -Wpedantic)
endif()

option(BUILD_TESTS "Build unit tests" ON)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

include(GNUInstallDirs)
install(TARGETS pu RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/pu)
