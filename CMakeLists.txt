cmake_minimum_required(VERSION 3.16)
project(pu-cli VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(CURL REQUIRED)
find_package(nlohmann_json 3.10 REQUIRED)

add_executable(pu src/main.cpp)
target_link_libraries(pu PRIVATE CURL::libcurl nlohmann_json::nlohmann_json)

if(MSVC)
    target_compile_options(pu PRIVATE /W4)
else()
    target_compile_options(pu PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(GNUInstallDirs)
install(TARGETS pu RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
