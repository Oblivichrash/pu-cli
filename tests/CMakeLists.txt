find_package(Catch2 3 REQUIRED)

add_executable(pu_tests
    unit/test_ollama_backend.cpp
    unit/test_openai_backend.cpp
    unit/test_codebuddy_backend.cpp
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
    unit/test_mcp_tool.cpp
    unit/test_command_router.cpp
    unit/test_builtin_tools.cpp
    unit/test_executor.cpp
    unit/test_serve_api.cpp
    unit/test_serve_ws.cpp
    unit/test_options.cpp
    unit/test_http_errors.cpp
    unit/test_backend_resolution.cpp
    unit/test_rewind.cpp
)

target_include_directories(pu_tests PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/tests
)

target_link_libraries(pu_tests PRIVATE
    pu_app pu_agent pu_lib
    Catch2::Catch2WithMain
)

include(Catch)
catch_discover_tests(pu_tests)
