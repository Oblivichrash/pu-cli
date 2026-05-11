cmake_minimum_required(VERSION 3.16)
project(pu-cli VERSION 0.1.0 LANGUAGES CXX)

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
if(WIN32)
    set(PLATFORM_SRCS src/platform/platform_win.cpp)
else()
    set(PLATFORM_SRCS src/platform/platform_linux.cpp)
endif()
add_library(pu_platform STATIC ${PLATFORM_SRCS})
target_include_directories(pu_platform PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
add_library(pu::platform ALIAS pu_platform)

# Backend implementation library
add_library(pu_backend STATIC
    src/backends/ollama/ollama_backend.cpp
    src/backends/openai/openai_backend.cpp
    src/expert_config.cpp
    src/conversation_store.cpp
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

# Expert library
add_library(pu_expert STATIC
    src/expert/expert_manager.cpp
    src/experts/chat/chat_expert.cpp
    src/experts/bash/bash_expert.cpp
    src/executor/command_executor.cpp
    src/expert_factory.cpp
    src/proactive_engine.cpp
)
target_include_directories(pu_expert PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(pu_expert
    PUBLIC  pu_backend_interface pu_platform
    PRIVATE pu_backend pu_http nlohmann_json::nlohmann_json
)
add_library(pu::expert ALIAS pu_expert)

# Main executable
add_executable(pu
    src/main.cpp
    src/cli_ask.cpp
    src/cli_chat.cpp
    src/cli_app_setup.cpp
    src/renderer.cpp
)
target_include_directories(pu PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(pu PRIVATE pu_expert pu_backend pu_http pu_platform)

if(MSVC)
    target_compile_options(pu PRIVATE /W4)
    target_compile_options(pu_backend PRIVATE /W4)
    target_compile_options(pu_http PRIVATE /W4)
    target_compile_options(pu_expert PRIVATE /W4)
    target_compile_options(pu_platform PRIVATE /W4)
else()
    target_compile_options(pu PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(pu_backend PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(pu_http PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(pu_expert PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(pu_platform PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Testing
option(BUILD_TESTS "Build unit tests" ON)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

include(GNUInstallDirs)
install(TARGETS pu RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/pu)
