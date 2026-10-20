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
    unit/test_peer_serve.cpp
    unit/test_board.cpp
    unit/test_board_http.cpp
    unit/test_budget.cpp
    unit/test_peer_read.cpp
    unit/test_board_liveness.cpp
    unit/test_serve_registry.cpp
    unit/test_usage_total.cpp
    unit/test_capabilities.cpp
    unit/test_model_verifier.cpp
    unit/test_approval_gate.cpp
    unit/test_transcript_chain.cpp
    unit/test_mcp_stdio_client.cpp
    unit/test_cli_exit.cpp
    unit/test_approval_refusal.cpp
    unit/test_cli_check.cpp
    unit/test_cli_help.cpp
    unit/test_agents_summary.cpp
    unit/test_usage_ledger.cpp
    unit/test_serve_usage.cpp
    unit/test_billing.cpp
    unit/test_cli_usage.cpp
    unit/test_model_caps.cpp
    unit/test_sse.cpp
    unit/test_cli_sse.cpp
    unit/test_project_rules.cpp
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

# The page rule in unit/test_project_rules.cpp reads the tree this build was configured from
# rather than a copy of it.
target_compile_definitions(pu_tests PRIVATE
    PU_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
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

# The page's transcript fold is plain JavaScript with no DOM in it, so its cases can be
# checked without a browser. They are one file of input/output pairs; what runs them differs.
#
# Where node exists the page's own history.js is loaded and run, which is the only check
# that proves the JavaScript itself still does this. Where it does not, the Python stand-in
# runs the same cases against a second implementation of the same fold: it catches a case
# that no longer describes the page, but not a page that no longer runs.
find_program(PU_NODE node)
if(PU_NODE)
    add_test(NAME web/groupHistory
             COMMAND ${PU_NODE} ${CMAKE_CURRENT_SOURCE_DIR}/js/check-group-history.cjs)
    set_tests_properties(web/groupHistory PROPERTIES TIMEOUT 60)
    # The header strip and the panel's spend figure, loaded the same way. Both are shapes the
    # page drew wrong without failing to load, so nothing but running the modules catches them.
    add_test(NAME web/pagePanels
             COMMAND ${PU_NODE} ${CMAKE_CURRENT_SOURCE_DIR}/js/check-page-panels.cjs)
    set_tests_properties(web/pagePanels PROPERTIES TIMEOUT 60)
else()
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME web/groupHistory
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/js/check_group_history.py)
        set_tests_properties(web/groupHistory PROPERTIES TIMEOUT 60)
        message(STATUS "node not found: the transcript fold's cases run against the stand-in")
    else()
        message(STATUS "no node and no python3: the transcript fold's cases are not run here")
    endif()
endif()
