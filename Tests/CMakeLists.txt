project(CppMcpTest LANGUAGES CXX)

add_executable(${PROJECT_NAME} TestTransports.h TestTransports.cpp CancellationProbe.h
    TransportTest.cpp ServerTest.cpp McpServerExample.cpp TypedToolTest.cpp DurationTest.cpp JsonSchemaTest.cpp JsonSchemaTestData.h
    "${AWL_ROOT_DIR}/Main/main.cpp")
target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(${PROJECT_NAME} PRIVATE CppMcp::CppMcp)
include("${AWL_ROOT_DIR}/CMake/AwlCompilerOptions.cmake")

foreach(test_name TransportFullDuplex TransportCancelIdle TransportCancelBlockedSend
    ServerRegistration ServerFullDuplex ServerErrors ServerCancelHandler ServerCancelTool
    ServerCancelOutput ServerOutputFailure ServerEmptyRegistries TypedToolSchema TypedToolValidation
    TypedToolAllTypes TypedToolFullDuplex TypedToolInputErrors TypedToolOutputErrors TypedToolCancel
    DurationFormatting DurationParsing DurationErrors JsonDuration
    JsonSchemaAllTypes JsonSchemaShapes JsonSchemaValidation JsonSchemaWrappers JsonSchemaUnicode JsonSchemaCustomSerializer)
    add_test(NAME ${test_name}
        COMMAND ${PROJECT_NAME} --run=${test_name}_Test --output=all)
    set_tests_properties(${test_name} PROPERTIES
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
        TIMEOUT 15)
endforeach()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
foreach(test_name HostOptions StdioReal StdioFramingErrors StdioRequestCancel StdioCancelIdle StdioCancelWrite StdioEofCancel
    ToolReal LegacyStdio LegacyHttp HttpTimeout HttpReal HttpSse HttpErrors HttpDisconnect CombinedTransports)
    add_test(NAME ${test_name} COMMAND Python3::Interpreter
        "${CMAKE_CURRENT_SOURCE_DIR}/RealTransportTest.py" "$<TARGET_FILE:${PROJECT_NAME}>" ${test_name})
    set_tests_properties(${test_name} PROPERTIES TIMEOUT 15)
endforeach()

# Opt in to checking an installed Codex version; no model turn is started.
option(CPPMCP_TEST_CODEX "Run the interoperability test with the installed Codex MCP client" OFF)
if(CPPMCP_TEST_CODEX)
    find_program(CODEX_EXECUTABLE NAMES codex REQUIRED)
    add_test(NAME CodexMcpInterop COMMAND Python3::Interpreter
        "${CMAKE_CURRENT_SOURCE_DIR}/CodexMcpTest.py" "$<TARGET_FILE:${PROJECT_NAME}>" "${CODEX_EXECUTABLE}")
    set_tests_properties(CodexMcpInterop PROPERTIES TIMEOUT 60)
endif()
