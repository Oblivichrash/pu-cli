cmake_minimum_required(VERSION 3.21)
project(pu-cli VERSION 0.7.1 LANGUAGES CXX)

if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(spdlog REQUIRED)
find_package(Boost 1.84 REQUIRED COMPONENTS system program_options json)
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
        # /utf-8: the sources carry typographic punctuation in comments; without it MSVC reads
        # them in the system code page and warns C4819. /bigobj: an object holds at most 65535
        # sections, and a Debug build of the unit that instantiates Boost.Beast's templates
        # passes that.
        target_compile_options(${target} PRIVATE /W4 /utf-8 /bigobj /D_CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

add_library(pu_core_util STATIC
    src/core/beast_http_client.cpp
    src/core/logging.cpp
    src/core/platform.cpp
    src/core/process.cpp
    src/core/sse.cpp
    src/tools_cli/sse.cpp
)
target_link_libraries(pu_core_util PUBLIC
    spdlog::spdlog
    Boost::json
    Boost::system
    OpenSSL::SSL
    OpenSSL::Crypto
)
if(WIN32)
    target_link_libraries(pu_core_util PRIVATE crypt32)
endif()
pu_configure_target(pu_core_util)
add_library(pu::core_util ALIAS pu_core_util)

add_library(pu_llm STATIC
    src/llm/field_capture.cpp
    src/llm/gateway_table.cpp
    src/llm/model_check.cpp
    src/llm/model_factory.cpp
    src/llm/model_listing.cpp
    src/llm/ollama_provider.cpp
    src/llm/openai_protocol.cpp
    src/llm/openai_provider.cpp
    src/llm/projection.cpp
    src/llm/streaming_provider.cpp
)
target_link_libraries(pu_llm PUBLIC pu_core_util)
pu_configure_target(pu_llm)
add_library(pu::llm ALIAS pu_llm)

add_library(pu_model STATIC
    src/model/backend.cpp
    src/model/gateway.cpp
)
target_link_libraries(pu_model PUBLIC pu_llm)
pu_configure_target(pu_model)
add_library(pu::model ALIAS pu_model)

add_library(pu_config STATIC
    src/config/agents.cpp
    src/config/backend_from_table.cpp
    src/config/serve_registry.cpp
)
target_link_libraries(pu_config PUBLIC pu_model)
pu_configure_target(pu_config)
add_library(pu::config ALIAS pu_config)

# `pu probe` and the standalone `pu-probe` share this one implementation.
add_library(pu_probe STATIC
    src/tools_cli/probe.cpp
)
target_link_libraries(pu_probe PUBLIC pu_config)
pu_configure_target(pu_probe)

# `pu models` and the standalone `pu-models` share this one implementation.
add_library(pu_models STATIC
    src/tools_cli/agents_source.cpp
    src/tools_cli/models.cpp
)
target_link_libraries(pu_models PUBLIC pu_config)
pu_configure_target(pu_models)

add_library(pu_paging STATIC
    src/paging/paging.cpp
)
target_link_libraries(pu_paging PUBLIC pu_core_util)
pu_configure_target(pu_paging)
add_library(pu::paging ALIAS pu_paging)

add_library(pu_session STATIC
    src/session/graph.cpp
    src/session/request.cpp
    src/session/session.cpp
    src/session/settings.cpp
    src/session/usage.cpp
)
target_link_libraries(pu_session PUBLIC pu_config pu_paging)
pu_configure_target(pu_session)
add_library(pu::session ALIAS pu_session)

add_library(pu_mcp STATIC
    src/mcp/client.cpp
    src/mcp/http_transport.cpp
    src/mcp/stdio_transport.cpp
)
target_link_libraries(pu_mcp PUBLIC pu_config)
pu_configure_target(pu_mcp)
add_library(pu::mcp ALIAS pu_mcp)

add_library(pu_tools STATIC
    src/tools/ask_dir_tool.cpp
    src/tools/bash.cpp
    src/tools/delegation.cpp
    src/tools/policy.cpp
    src/tools/toolbox.cpp
    src/tools/write_file.cpp
)
target_link_libraries(pu_tools PUBLIC pu_session)
pu_configure_target(pu_tools)
add_library(pu::tools ALIAS pu_tools)

add_library(pu_runtime STATIC
    src/runtime/answer.cpp
    src/runtime/command_router.cpp
    src/runtime/executor.cpp
    src/runtime/runtime.cpp
)
target_link_libraries(pu_runtime PUBLIC pu_tools pu_mcp)
pu_configure_target(pu_runtime)
add_library(pu::runtime ALIAS pu_runtime)

add_library(pu_serve STATIC
    src/serve/serve_http_routes.cpp
    src/serve/serve_runner.cpp
    src/serve/serve_websocket.cpp
)
target_link_libraries(pu_serve PUBLIC pu_runtime)
pu_configure_target(pu_serve)
add_library(pu::serve ALIAS pu_serve)

add_library(pu_core STATIC
    src/app/agent_summary.cpp
    src/app/ask.cpp
    src/app/cli.cpp
    src/app/cli_agents.cpp
    src/app/cli_check.cpp
    src/app/cli_output.cpp
    src/app/cli_serve.cpp
    src/app/cli_sse.cpp
    src/app/models.cpp
    src/app/probe.cpp
)
target_link_libraries(pu_core PUBLIC
    pu_serve
    pu_probe
    pu_models
    Boost::program_options
)
pu_configure_target(pu_core)
add_library(pu::core ALIAS pu_core)

add_executable(pu
    src/app/main.cpp
)
target_link_libraries(pu PRIVATE pu_core)
pu_configure_target(pu)

add_executable(pu-sse
    src/tools_cli/sse_main.cpp
)
target_link_libraries(pu-sse PRIVATE pu_core_util Boost::program_options)
pu_configure_target(pu-sse)
add_executable(pu::sse ALIAS pu-sse)

add_executable(pu-probe
    src/tools_cli/probe_main.cpp
)
target_link_libraries(pu-probe PRIVATE pu_probe Boost::program_options)
pu_configure_target(pu-probe)
add_executable(pu::probe ALIAS pu-probe)

add_executable(pu-models
    src/tools_cli/models_main.cpp
)
target_link_libraries(pu-models PRIVATE pu_models Boost::program_options)
pu_configure_target(pu-models)

# The page is found relative to the executable, so a build tree without a sibling web/ serves
# nothing.
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
install(TARGETS pu pu-sse pu-probe pu-models RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(WIN32)
    # A packaged Windows binary needs its DLLs beside it: spdlog and Boost are on PATH for a
    # build tree, not for a layout that was unzipped.
    install(FILES $<TARGET_RUNTIME_DLLS:pu> DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/web/ DESTINATION ${CMAKE_INSTALL_DATADIR}/pu/web)
