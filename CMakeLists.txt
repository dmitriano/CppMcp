cmake_minimum_required(VERSION 3.24.2...4.4.3)

project(CppMcp LANGUAGES CXX)

set(AWL_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib/Awl"
    CACHE PATH "Path to AWL root directory")

if(NOT EXISTS "${AWL_ROOT_DIR}/CMake/AwlConfig.cmake"
    OR NOT EXISTS "${AWL_ROOT_DIR}/CMake/AwlTargets.cmake")
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
include("${AWL_ROOT_DIR}/CMake/AwlTargets.cmake")
awl_add_libraries()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(${PROJECT_NAME} STATIC
    src/Common/Protocol.h
    src/Common/JsonSchema.h
    src/Transport/StdioStreams.h
    src/Transport/StdioTransport.h
    src/Transport/StdioTransport.cpp
    src/Transport/HttpTransport.h
    src/Transport/HttpTransport.cpp
    src/Transport/ITransport.h
    src/Transport/TransportChannels.h
    src/Transport/TransportServer.h
    src/Transport/TransportServer.cpp
    src/Server/IHandler.h
    src/Server/ITool.h
    src/Server/TypedTool.h
    src/Server/IResource.h
    src/Server/RpcError.h
    src/Server/RequestContext.h
    src/Server/RequestContext.cpp
    src/Server/Server.h
    src/Server/Server.cpp)
if(WIN32)
    target_sources(${PROJECT_NAME} PRIVATE Platform/Windows/StdioStreams.cpp)
elseif(UNIX)
    target_sources(${PROJECT_NAME} PRIVATE Platform/Posix/StdioStreams.cpp)
else()
    message(FATAL_ERROR "No stdio implementation for this platform")
endif()

add_library(CppMcp::CppMcp ALIAS ${PROJECT_NAME})
add_library(CppMcpCore ALIAS ${PROJECT_NAME})
add_library(CppMcpTransport ALIAS ${PROJECT_NAME})

target_include_directories(${PROJECT_NAME} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_23)
include("${AWL_ROOT_DIR}/CMake/AwlLink.cmake")

option(BUILD_TESTING "Build CppMcp tests" ${PROJECT_IS_TOP_LEVEL})
include(CTest)
if(BUILD_TESTING)
    add_subdirectory(Tests)
endif()
