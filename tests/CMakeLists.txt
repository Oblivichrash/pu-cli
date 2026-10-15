find_package(Catch2 3 REQUIRED)

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
    unit/test_capabilities.cpp
    unit/test_model_verifier.cpp
    unit/test_approval_gate.cpp
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

target_link_libraries(pu_tests PRIVATE
    pu_core
    Catch2::Catch2WithMain
)

include(Catch)
catch_discover_tests(pu_tests)
