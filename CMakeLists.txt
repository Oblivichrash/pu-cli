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
        # The sources are UTF-8 and carry typographic punctuation in comments; without this MSVC
        # reads them in the system code page and warns C4819 on every file that has any.
        # /bigobj: an object file holds at most 65535 sections, and a Debug build of the unit that
        # instantiates Boost.Beast's templates passes that — C1128, on a file nobody edited.
        target_compile_options(${target} PRIVATE /W4 /utf-8 /bigobj /D_CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

# The layer with no dependencies of its own: an HTTP client, the SSE framing, the JSON and
# text helpers, logging and the platform's token. A tool that talks to a server and prints
# what it reads needs this and nothing else, which is why it is a target of its own rather
# than the first entries of `pu_core`. A standalone tool links this; it does not link the
# session, the pager or the tool box it has no use for.
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

# Everything that talks to a model: the providers, the protocols that frame a request, the
# projection that reads a reply back, and the factory that picks one from a gateway. It reads
# the wire vocabulary out of `llm_types` and knows nothing about a session, a turn or a tool
# box, so it sits below the modules that own those.
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

# What a gateway is: the products `agents.json` may name, the host each answers on, the wire it
# speaks and how it bills. No factories here — the row that says which provider a gateway opens
# is written in `pu_llm`, which is why `pu_llm` is linked in.
add_library(pu_model STATIC
    src/model/backend.cpp
    src/model/gateway.cpp
)
target_link_libraries(pu_model PUBLIC pu_llm)
pu_configure_target(pu_model)
add_library(pu::model ALIAS pu_model)

# The agent table, the list of servers a turn may reach and the probe. It links `pu_model`
# because that is where a gateway's fact lives and `pu_llm` because the probe reads one.
add_library(pu_config STATIC
    src/config/agents.cpp
    src/config/serve_registry.cpp
    src/tools_cli/probe.cpp
)
target_link_libraries(pu_config PUBLIC pu_model)
pu_configure_target(pu_config)
add_library(pu::config ALIAS pu_config)

# What a page is and when it is elided: a budget, a rank and the rule that a second answer to
# one question replaces the first. It reads `core`'s JSON and nothing else, so it sits at the
# bottom beside `core_util` — every layer that holds a transcript links it.
add_library(pu_paging STATIC
    src/paging/paging.cpp
)
target_link_libraries(pu_paging PUBLIC pu_core_util)
pu_configure_target(pu_paging)
add_library(pu::paging ALIAS pu_paging)

# The conversation itself: what a message is, what a request carries, the graph of turns and
# what one cost. It reads `llm` for the wire vocabulary it stores and `model` for the gateway a
# request names, and it knows nothing about how a turn is run.
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

# The client for a server a turn may reach: the transports that carry the protocol, and the
# tools it publishes read back as the shape a provider already speaks. It links `config` for
# the server entry it is handed and nothing above it.
add_library(pu_mcp STATIC
    src/mcp/client.cpp
    src/mcp/http_transport.cpp
    src/mcp/stdio_transport.cpp
)
target_link_libraries(pu_mcp PUBLIC pu_config)
pu_configure_target(pu_mcp)
add_library(pu::mcp ALIAS pu_mcp)

# What a turn may call: the builtins, the policy that refuses a call, and the box that holds
# them. `ask_dir` is here because it states a question, and reaches the runtime that answers one
# through `RunDelegatedTurn` rather than by linking it — which is what keeps this layer below
# the runtime that builds it.
add_library(pu_tools STATIC
    src/tools/ask_dir_tool.cpp
    src/tools/bash.cpp
    src/tools/delegation.cpp
    src/tools/policy.cpp
    src/tools/toolbox.cpp
    src/tools/write_file.cpp
)
target_link_libraries(pu_tools PUBLIC pu_mcp pu_session)
pu_configure_target(pu_tools)
add_library(pu::tools ALIAS pu_tools)

# The turn: the router that reads a slash line, the executor that runs the loop, and the runtime
# that owns a workspace, a backend and a tool box. It sits above `tools` because it builds the
# box — and `tools` reaches back into it only through `RunDelegatedTurn`, whose definition lives
# in `runtime.cpp`, so the link direction stays the one the includes already have.
add_library(pu_runtime STATIC
    src/runtime/command_router.cpp
    src/runtime/executor.cpp
    src/runtime/runtime.cpp
)
target_link_libraries(pu_runtime PUBLIC pu_tools)
pu_configure_target(pu_runtime)
add_library(pu::runtime ALIAS pu_runtime)

add_library(pu_core STATIC
    src/app/ask.cpp
    src/app/cli.cpp
    src/app/cli_agents.cpp
    src/app/cli_check.cpp
    src/app/cli_output.cpp
    src/app/cli_serve.cpp
    src/app/cli_sse.cpp
    src/app/models.cpp
    src/app/probe.cpp
    src/config/agent_summary.cpp
    src/serve/serve_http_routes.cpp
    src/serve/serve_runner.cpp
    src/serve/serve_websocket.cpp
)
target_link_libraries(pu_core PUBLIC
    pu_runtime
    Boost::program_options
)
pu_configure_target(pu_core)
add_library(pu::core ALIAS pu_core)

add_executable(pu
    src/app/main.cpp
)
target_link_libraries(pu PRIVATE pu_core)
pu_configure_target(pu)

# The standalone reader. It links the layer with no dependencies of its own, which is what
# reading a feed needs: no session, no configuration, no pager. `pu sse` runs the same
# implementation, so the two never disagree about what an event looks like.
add_executable(pu-sse
    src/tools_cli/sse_main.cpp
)
target_link_libraries(pu-sse PRIVATE pu_core_util Boost::program_options)
pu_configure_target(pu-sse)
add_executable(pu::sse ALIAS pu-sse)

# The standalone probe. It needs the agent table to find a gateway and nothing else: no
# session, no transcript, no pager. `pu probe` runs the same implementation, so a field
# name read by one is a field name read by the other.
add_executable(pu-probe
    src/tools_cli/probe_main.cpp
)
target_link_libraries(pu-probe PRIVATE pu_config Boost::program_options)
pu_configure_target(pu-probe)
add_executable(pu::probe ALIAS pu-probe)

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
# The tools go in beside the command that owns them: one package to install, and a reader who
# has `pu` has `pu-sse`, whether they reach it as `pu sse` or as its own name.
install(TARGETS pu pu-sse pu-probe RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(WIN32)
    # A packaged Windows binary needs the DLLs it was linked against beside it: spdlog and the
    # Boost libraries are on PATH for a build tree, not for a layout that was unzipped.
    install(FILES $<TARGET_RUNTIME_DLLS:pu> DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/web/ DESTINATION ${CMAKE_INSTALL_DATADIR}/pu/web)
