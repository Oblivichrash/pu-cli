cmake_minimum_required(VERSION 3.16)
project(pu-cli VERSION 0.3.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(CURL REQUIRED)
find_package(nlohmann_json 3.10 REQUIRED)

add_library(pu_backend_interface INTERFACE)
target_include_directories(pu_backend_interface INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
add_library(pu::backend_interface ALIAS pu_backend_interface)

add_library(pu_system STATIC
    src/core/system.cpp
)
target_include_directories(pu_system PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
add_library(pu::system ALIAS pu_system)

add_library(pu_http STATIC
    src/http/curl_http_client.cpp
)
target_include_directories(pu_http PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(pu_http PUBLIC CURL::libcurl pu_system)
add_library(pu::http ALIAS pu_http)

# Tool implementations and agent core
add_library(pu_agent STATIC
    src/core/agent_manager.cpp
    src/core/tool_registry.cpp
    src/core/orchestrator.cpp
    src/core/renderer.cpp
    src/core/llm_agent.cpp
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
    PUBLIC  pu_backend_interface pu_system
    PRIVATE pu_http nlohmann_json::nlohmann_json
)
add_library(pu::agent ALIAS pu_agent)

# Backend implementations - depends on pu_agent because agent_loader.cpp creates tool instances
add_library(pu_backend STATIC
    src/backends/ollama/ollama.cpp
    src/backends/openai/openai.cpp
    src/backends/streaming_json_parser.cpp
    src/backends/request_builder.cpp
    src/backends/ollama/ollama_token_adapter.cpp
    src/backends/openai/openai_token_adapter.cpp
    src/core/agent_loader.cpp
    src/core/global_context.cpp
    src/core/call_stack.cpp
    src/core/conversation_store.cpp
)
target_include_directories(pu_backend PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(pu_backend
    PUBLIC  pu_backend_interface
    PRIVATE pu_agent pu_http nlohmann_json::nlohmann_json pu_system
)
add_library(pu::backend ALIAS pu_backend)

add_executable(pu
    src/app/main.cpp
    src/app/ask.cpp
    src/app/chat.cpp
    src/app/learn.cpp
    src/app/common.cpp
)
target_include_directories(pu PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
# Link order: dependents before dependencies
target_link_libraries(pu PRIVATE pu_backend pu_agent pu_http pu_system)

if(MSVC)
    target_compile_options(pu PRIVATE /W4)
    target_compile_options(pu_backend PRIVATE /W4)
    target_compile_options(pu_http PRIVATE /W4)
    target_compile_options(pu_agent PRIVATE /W4)
    target_compile_options(pu_system PRIVATE /W4)
else()
    target_compile_options(pu PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(pu_backend PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(pu_http PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(pu_agent PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(pu_system PRIVATE -Wall -Wextra -Wpedantic)
endif()

option(BUILD_TESTS "Build unit tests" ON)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

include(GNUInstallDirs)
install(TARGETS pu RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/pu)
