find_package(Catch2 3 REQUIRED)

# The stdio cases start a real MCP server. It is built here rather than run out of the test
# binary: a program with one job cannot be confused with the runner, and it needs no
# interpreter, so the same case runs on a machine that has none.
add_executable(pu_echo_server mocks/echo_server_main.cpp)
target_include_directories(pu_echo_server PRIVATE ${CMAKE_SOURCE_DIR}/tests/mocks)
target_link_libraries(pu_echo_server PRIVATE Boost::json)

add_executable(pu_tests
    unit/test_ollama_backend.cpp
    unit/test_openai_backend.cpp
    unit/test_codebuddy_backend.cpp
    unit/test_codebuddy_catalog.cpp
    unit/test_projection.cpp
    unit/test_agent_manager.cpp
    unit/test_agent_config.cpp
    unit/test_streaming_json_parser.cpp
    unit/test_conversation.cpp
    unit/test_session_schema.cpp
    unit/test_message.cpp
    unit/test_graph.cpp
    unit/test_request.cpp
    unit/test_encoding.cpp
    unit/test_command_execution.cpp
    unit/test_logging.cpp
    unit/test_mcp_client.cpp
    unit/test_mcp_tool.cpp
    unit/test_mcp_registration.cpp
    unit/test_command_router.cpp
    unit/test_builtin_tools.cpp
    unit/test_executor.cpp
    unit/test_serve_api.cpp
    unit/test_serve_ws.cpp
    unit/test_options.cpp
    unit/test_http_errors.cpp
    unit/test_backend_resolution.cpp
    unit/test_listen_options.cpp
    unit/test_rewind.cpp
    unit/test_list_models.cpp
    unit/test_probe.cpp
    unit/test_peer_config.cpp
    unit/test_peer_tool.cpp
    unit/test_capabilities.cpp
    unit/test_model_verifier.cpp
    unit/test_approval_gate.cpp
    unit/test_transcript_chain.cpp
    unit/test_mcp_stdio_client.cpp
    unit/test_cli_exit.cpp
)

target_include_directories(pu_tests PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/tests
)

# A captured response is read from the source tree rather than copied, so the
# fixture a test names stays the one file it was taken from.
target_compile_definitions(pu_tests PRIVATE
    PU_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures"
)

# The program a stdio case starts. A generator expression rather than a searched path, so a
# copy of the test binary still names the server ctest built beside it.
target_compile_definitions(pu_tests PRIVATE
    PU_ECHO_SERVER_PATH="$<TARGET_FILE:pu_echo_server>"
)
add_dependencies(pu_tests pu_echo_server)

target_link_libraries(pu_tests PRIVATE
    pu_core
    Catch2::Catch2WithMain
)

include(Catch)
catch_discover_tests(pu_tests)
