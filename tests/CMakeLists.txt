find_package(Catch2 3 REQUIRED)

add_executable(pu_tests
    unit/test_ollama_backend.cpp
    unit/test_openai_backend.cpp
    unit/test_conversation_store.cpp
    unit/test_agent_manager.cpp
    unit/test_agent_config.cpp
    unit/test_streaming_json_parser.cpp
)

target_include_directories(pu_tests PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/tests
)

target_link_libraries(pu_tests PRIVATE
    pu::backend
    pu::agent
    pu::http
    Catch2::Catch2WithMain
    nlohmann_json::nlohmann_json
)

include(Catch)
catch_discover_tests(pu_tests)
