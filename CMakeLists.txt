cmake_minimum_required(VERSION 3.16)
project(pu-cli VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(CURL REQUIRED)
find_package(nlohmann_json 3.10 REQUIRED)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# HTTP abstraction library
add_library(pu_http STATIC
    src/http/curl_http_client.cpp
)
target_include_directories(pu_http PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(pu_http PRIVATE CURL::libcurl)

# Backends library
add_library(pu_backend STATIC
    src/backends/ollama/ollama_backend.cpp
)
target_include_directories(pu_backend PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(pu_backend PRIVATE pu_http nlohmann_json::nlohmann_json)

# Main executable
add_executable(pu src/main.cpp)
target_link_libraries(pu PRIVATE pu_backend)

if(MSVC)
    target_compile_options(pu PRIVATE /W4)
    target_compile_options(pu_backend PRIVATE /W4)
    target_compile_options(pu_http PRIVATE /W4)
else()
    target_compile_options(pu PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(pu_backend PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(pu_http PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Testing
option(BUILD_TESTS "Build unit tests" ON)
if(BUILD_TESTS)
    add_subdirectory(tests)
endif()

include(GNUInstallDirs)
install(TARGETS pu RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
