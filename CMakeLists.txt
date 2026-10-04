cmake_minimum_required(VERSION 3.24.2...4.4.3)

project(CppMcp LANGUAGES CXX)

set(AWL_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib/Awl"
    CACHE PATH "Path to AWL root directory")

if(NOT EXISTS "${AWL_ROOT_DIR}/CMake/AwlConfig.cmake"
    OR NOT EXISTS "${AWL_ROOT_DIR}/CMake/AwlLink.cmake")
    message(FATAL_ERROR
        "AWL not found in '${AWL_ROOT_DIR}'. Run git submodule update --init --recursive "
        "or set -DAWL_ROOT_DIR=<path to AWL>.")
endif()

set(AWL_FIND_BOOST ON)
set(AWL_FIND_OPENSSL ON)
set(AWL_FIND_QT OFF CACHE BOOL "Build without Qt" FORCE)
set(AWL_COMPILE_TESTS OFF CACHE BOOL "Do not compile AWL tests" FORCE)
set(AWL_COMPILE_MAIN OFF)
set(AWL_STATIC_RUNTIME ON)
set(AWL_ANSI_CMD_CHAR ON)
set(AWL_BIGOBJ ON)

include("${AWL_ROOT_DIR}/CMake/AwlConfig.cmake")
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(${PROJECT_NAME} src/Main.cpp src/Common/Channel.h)
target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# AWL compiles Boost.JSON's implementation in BoostExtras/Json/Json.cpp.
target_compile_definitions(${PROJECT_NAME} PRIVATE BOOST_JSON_NO_LIB)
include("${AWL_ROOT_DIR}/CMake/AwlLink.cmake")

if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE ws2_32)
endif()
