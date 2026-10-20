cmake_minimum_required(VERSION 3.16)
project(pu-cli VERSION 0.6.0 LANGUAGES CXX)

if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(spdlog REQUIRED)
find_package(Boost REQUIRED COMPONENTS system program_options json)
find_package(OpenSSL REQUIRED)

set(PU_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/pu_build_config.hpp.in
    ${PU_GENERATED_INCLUDE_DIR}/pu/build_config.hpp
)

function(pu_configure_target target)
    target_include_directories(${target} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_include_directories(${target} PRIVATE ${PU_GENERATED_INCLUDE_DIR})

    if(MSVC)
        target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
        target_compile_options(${target} PRIVATE /W4 /D_CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

add_library(pu_core STATIC
    src/session/graph.cpp
    src/core/beast_http_client.cpp
    src/core/platform.cpp
    src/core/sse.cpp
    src/session/session.cpp
    src/session/usage.cpp
    src/session/settings.cpp
    src/serve/peer_read.cpp
    src/core/logging.cpp
    src/llm/ollama_provider.cpp
    src/llm/openai_provider.cpp
    src/llm/openai_protocol.cpp
    src/llm/projection.cpp
    src/llm/model_listing.cpp
    src/llm/probe.cpp
    src/llm/model_verifier.cpp
    src/llm/streaming_provider.cpp
    src/gateways/gateways.cpp
    src/config/backend.cpp
    src/mcp/stdio_transport.cpp
    src/mcp/http_transport.cpp
    src/mcp/client.cpp
    src/config/agents.cpp
    src/config/peer.cpp
    src/config/serve_registry.cpp
    src/runtime/executor.cpp
    src/tools/toolbox.cpp
    src/tools/builtin_tools.cpp
    src/tools/peer_tool.cpp
    src/runtime/runtime.cpp
    src/runtime/command_router.cpp
    src/app/cli.cpp
    src/app/cli_check.cpp
    src/app/cli_agents.cpp
    src/app/cli_sse.cpp
    src/config/agent_summary.cpp
    src/app/serve.cpp
    src/app/serve_http_routes.cpp
    src/serve/board.cpp
)
target_include_directories(pu_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/app)
target_link_libraries(pu_core PUBLIC
    spdlog::spdlog
    Boost::json
    Boost::system
    Boost::program_options
    OpenSSL::SSL
    OpenSSL::Crypto
)
if(WIN32)
    # The Windows root store, read to give OpenSSL the anchors this platform has.
    target_link_libraries(pu_core PRIVATE crypt32)
endif()
pu_configure_target(pu_core)
add_library(pu::core ALIAS pu_core)

add_executable(pu
    src/app/main.cpp
)
target_link_libraries(pu PRIVATE pu_core)
pu_configure_target(pu)

# The page is found relative to the executable, so a build tree without a sibling web/ serves
# nothing. Copying it after every build keeps the binary runnable from where it was built; the
# install rule below places the same files where a packaged layout expects them.
add_custom_command(TARGET pu POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/web
            $<TARGET_FILE_DIR:pu>/web
    COMMENT "Copying web assets beside pu"
    VERBATIM
)

option(BUILD_TESTS "Build unit tests" ON)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

include(GNUInstallDirs)
install(TARGETS pu RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/web/ DESTINATION ${CMAKE_INSTALL_DATADIR}/pu/web)
