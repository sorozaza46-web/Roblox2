cmake_minimum_required(VERSION 3.15)
project(RobloxDumper LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 1.0.1
)
FetchContent_MakeAvailable(glm)

FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.12.0
)
FetchContent_MakeAvailable(spdlog)

add_executable(roblox-dumper main.cpp dumper_engine.h)

target_link_libraries(roblox-dumper PRIVATE glm::glm spdlog::spdlog)
target_include_directories(roblox-dumper PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

