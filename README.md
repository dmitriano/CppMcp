# CppMcp

C++23 MCP framework library using AWL, Boost.Asio and Boost.Beast, without Qt.
The static library `CppMcp` provides transport orchestration and a JSON-RPC dispatcher
with handlers, tools and resources, real stdio and Streamable HTTP transports.

AWL is a submodule in `lib/Awl`, pinned to the same revision as the reference
`tradeclient` project. Boost and OpenSSL are discovered by AWL's CMake scripts,
with the static runtime and static Boost libraries used by `tradeclient`.

Reference development guidelines: [AGENTS.md](../tradeclient/AGENTS.md) in the `tradeclient` project.

## Windows build

Initialize the bundled AWL checkout:

```powershell
git submodule update --init --recursive
```

Configure and build outside the source directory (adapt the paths to your machine):

```powershell
$cmake = "C:/dev/tools/cmake-4.4.3-windows-x86_64/bin/cmake.exe"

& $cmake -S C:/dev/repos/CppMcp -B C:/dev/build/cppmcp `
    -G "Visual Studio 18 2026" -A x64 `
    -DCMAKE_PREFIX_PATH=C:/dev/libs/boost_1_89_0 `
    -DOPENSSL_ROOT_DIR=C:/dev/libs/OpenSSL `
    -DOPENSSL_USE_STATIC_LIBS=ON

& $cmake --build C:/dev/build/cppmcp --target CppMcp --config RelWithDebInfo
```

This builds `CppMcp.lib` on Windows. The library has no `main()`; the application
provides its own entry point. AWL and Boost.JSON implementations are compiled
once into the library. The test executable has a separate AWL test entry point.
Use `-DBUILD_TESTING=OFF` to configure a library-only build.

## Using the library

Add this repository to an application's CMake build and link the public target:

```cmake
add_subdirectory(path/to/CppMcp)
add_executable(MyServer Main.cpp)
target_link_libraries(MyServer PRIVATE CppMcp::CppMcp)
if(MSVC)
    set_property(TARGET MyServer PROPERTY MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
```

The target supplies C++23, public header paths, dependencies and the AWL character
configuration. Tests default to enabled for standalone builds and disabled when
CppMcp is added as a subdirectory, unless `BUILD_TESTING` is already set.
On MSVC, applications must use the same static runtime as the library, as above.
`CppMcpCore` and `CppMcpTransport` are compatibility aliases for the same library.

## External AWL checkout

`AWL_ROOT_DIR` is a CMake `CACHE PATH` parameter, as in `SQLiteWrapper`.
It defaults to `lib/Awl` in this repository. To use an external checkout, add this
argument to the configure command:

```powershell
-DAWL_ROOT_DIR=C:/dev/repos/tradeclient/lib/Awl
```

The path must contain `CMake/AwlConfig.cmake` and `CMake/AwlLink.cmake`.
An external checkout does not require initializing the bundled AWL submodule.
Use `-DAWL_ROOT_DIR=...` again to change the path in an existing build directory.

## Transport contracts and tests

`CppMcp` defines the transport interface and `TransportServer::addTransport`.
Transports are ordinary objects; there is no static factory registration.
The application handler reads a shared `InputChannel` and sends replies to the
originating transport's `OutputChannel`, carried by each incoming message.
`routeId` identifies a destination within that output channel, independently of
the JSON-RPC ID. All queues use `mcp::Channel`, an Asio `concurrent_channel` alias.

Call `asyncRun()` once after adding transports. The server uses a caller-supplied
strand executor and runs all transports concurrently. Cancellation or a child
failure cancels and joins the child coroutines. Transports do not close the shared
input channel. The server and executors must outlive the running coroutines.

`Tests/TestTransports.cpp` implements two in-memory transports that model stdio
and multiple HTTP response routes. These are channel-based test doubles; real
standard-stream I/O and Beast HTTP are covered by separate integration tests. The tests cover
requests, progress, responses, acceptance without a JSON body, route isolation,
HTTP operation after stdio closes, and cancellation during idle I/O or a blocked
send to a full channel. They run the Asio context on two threads.

```powershell
# Integration tests require Python 3; set its path if automatic discovery fails.
& $cmake -S C:/dev/repos/CppMcp -B C:/dev/build/cppmcp `
    -DPython3_EXECUTABLE=C:/dev/tools/Python313/python.exe
& $cmake --build C:/dev/build/cppmcp --config RelWithDebInfo
& C:/dev/tools/cmake-4.4.3-windows-x86_64/bin/ctest.exe `
    --test-dir C:/dev/build/cppmcp -C RelWithDebInfo --output-on-failure
```

Tests are enabled by the standard CMake `BUILD_TESTING` option (default: `ON`).

The application supplies the root AWL logger. For stdio operation, configure its
sink to write to stderr; stdout is reserved for protocol messages.
`Server`, `TransportServer` and `TypedTool`
take a non-null `std::shared_ptr<awl::ILogger>` through their constructors.
Create component loggers with `logger->createLogger("Component")`; they share
the root sink and level filter. Typed tools expose a protected `logger()` getter.
Handlers and resources can accept a logger in their own constructors when needed;
`RequestContext` carries only request data and notification routing.
Tests use `context.logger` as their root.

## Real transports

`StdioTransport` and `HttpTransport` share the server input channel and each own
an output channel. Both support cancellation through `awl::StopToken`, including
per-request cancellation and joined shutdown. Native stdio implementations live
in `Platform/Windows` and `Platform/Posix`, selected by CMake.
The host runs as `McpServer_Example` inside `CppMcpTest`, using named AWL
attributes. For stdio, select `--output_stream=stderr` so the AWL console leaves
stdout for protocol messages:

```powershell
& C:/dev/build/cppmcp/Tests/RelWithDebInfo/CppMcpTest.exe `
    --run=McpServer_Example --output=all --output_stream=stderr `
    --transport=both --http_port=52034
```

See [RealTransports.md](doc/RealTransports.md) for parameters, cancellation, limits and tests.

## Server handlers, tools and resources

`CppMcp` provides `mcp::Server`. Construct it with a caller-supplied strand
executor, a shared `InputChannel` and a non-null `awl::ILogger`, then register
transports and handlers before calling `asyncRun()` once. All transports must
send requests to that input channel.
The server owns handlers through `std::unique_ptr`:

- `addHandler(method, handler)` registers an `IHandler` for a custom JSON-RPC method.
- `addTool(tool_handler)` registers an `ITool` by its definition's name.
- `addResource(resource_handler)` registers an `IResource` by its definition's URI.
- `addTransport(transport)` takes ownership through `std::unique_ptr<ITransport>`.

`server/discover`, `tools/list`, `tools/call`, `resources/list` and `resources/read` are built-in server
methods. Tools are selected by `params.name`, resources by `params.uri`; custom
handlers have a separate method registry. Duplicate keys, empty identifiers,
reserved methods and registration after startup are rejected. Definitions are
copied at registration, and lists are sorted by tool name or resource URI.

Handlers receive object parameters and `RequestContext` with the original request
ID, transport route and `_meta`. They can await `asyncNotify(method, params)` to
send notifications through the same route. Tool calls return content blocks,
optional structured content and `isError`; resources return text or base64 blobs.
The server builds final JSON-RPC responses and preserves string/integer IDs.
Notifications receive `MessageKind::Accepted` with an empty body, including on
handler failure. Explicit `RpcError` exceptions become protocol errors; unexpected
handler errors become generic internal errors, and tool execution failures become
`isError` results. Cancellation propagates without sending an error response.

Requests run concurrently on the supplied strand, bounded by
`ServerOptions::maxConcurrentRequests` (default: 64). An await permits other requests
to enter the same handler. Cancellation or child failure cancels and joins the dispatcher and
transports. On normal completion of all transports, `Server` closes its input
channel. Keep the server and executors alive until `asyncRun()` completes; handlers
must await their child work and use request contexts only during the invocation.

Real transports target MCP `2026-07-28`, with per-request protocol version and client
capabilities in `params._meta`, and server identity/discovery from `ServerOptions`.
The legacy MCP `2025-06-18` initialization handshake is also supported for clients
such as Codex. Subscriptions, prompts and pagination remain unsupported.
Batches and array parameters are unsupported. Plain `ITool` implementations
validate their arguments and results; the server checks schema root types but does not implement
a JSON Schema validator. Built-in payloads use the `complete` result form from the
[tools](https://modelcontextprotocol.io/specification/2026-07-28/server/tools) and
[resources](https://modelcontextprotocol.io/specification/2026-07-28/server/resources)
specifications.

`Tests/ServerTest.cpp` exercises the core through both test transports: concurrent
clients with overlapping IDs and routes, notifications, metadata, tool schemas and
results, text/blob resources, malformed requests, protocol and execution errors,
registration rules, empty registries, normal shutdown and cancellation in a
handler, a tool or a blocked output send. Tests use two Asio threads and watchdogs.

## Typed tools

`TypedTool<Input, Output>` implements `ITool` using AWL reflection and JSON
serializers. Both root types must be reflected objects; input must be default
constructible and movable. For atomic members, supply a move constructor;
reference wrappers must stay bound to live storage after moves. Supply the name,
description and logger, then implement `asyncExecute()`:

```cpp
#include "Server/TypedTool.h"

struct SumInput
{
    std::int32_t a{};
    std::int32_t b{};

    AWL_REFLECT(a, b)
};

struct SumOutput
{
    std::int64_t sum{};

    AWL_REFLECT(sum)
};

class SumTool : public mcp::TypedTool<SumInput, SumOutput>
{
public:

    using TypedTool::TypedTool;

protected:

    boost::asio::awaitable<SumOutput> asyncExecute(
        SumInput input, mcp::RequestContext) override
    {
        co_return SumOutput{static_cast<std::int64_t>(input.a) + input.b};
    }
};

// Register before server.asyncRun().
server.addTool(std::make_unique<SumTool>("sum", "Sum two integers", logger->createLogger("SumTool")));
```

Schemas are generated once at construction from `AWL_REFLECT` member names and
types. Schemas cover the type families provided by AWL's
`BoostExtras/Json/Json.h`: booleans, integers up to 64 bits, floating point types
(including `long double`, within the finite `double` wire range), `std::string`
and `std::wstring`, AWL enums and decimals, chrono durations and time points,
reflected objects, string-keyed maps, tuples, AWL insertable sequences,
`optional<T>`, `atomic<T>`, `reference_wrapper<T>` and Boost.JSON value/object/array.
Wide strings use UTF-8 on the wire. Enum values use their AWL names; decimals
and durations use strings. Time points keep integer milliseconds since their
clock's epoch; there is no implicit UTC conversion.

Tuples use `prefixItems` with an exact length, maps use typed
`additionalProperties`, and recursive types use `$defs` and `$ref` in
[JSON Schema 2020-12](https://json-schema.org/draft/2020-12/json-schema-core).
An empty tuple is an empty array. For an empty reflected root object, derive from
`awl::EmptyStringizable`. Containers must have the operations required by their
AWL serializer; `std::array`, arbitrary ranges and maps with non-string keys have
no such built-in serializer. Set insertion follows its normal duplicate policy;
multimaps can only serialize distinct keys, with duplicate output keys rejected.

A custom `awl::JsonSerializer<T>` is accepted too: by default its schema is `{}`
and validation calls its `fromJson` on a temporary value. Specialize
`mcp::JsonSchemaTraits<T>` with a static `schema()` returning a JSON object to
provide its actual shape. An optional static `validate(const boost::json::value&)`
can replace deserialization-based validation, including for types without a default
constructor; report errors with `awl::JsonException`. Arbitrary custom schemas are
advertised to clients, not interpreted by a general-purpose validator.

Every non-optional field is required, even if its C++ declaration has a default.
Optional fields may be absent or `null`; AWL serializes an empty optional as `null`.
Unknown fields are rejected in reflected objects; map keys and opaque JSON
objects are unrestricted. Transparent wrappers retain the underlying type rules.
`makeJsonSchema<T>()` and
`validateJson<T>()` in `Common/JsonSchema.h` share these type rules. This validates
the supported C++ shapes, not arbitrary user-provided JSON Schema documents.
Arithmetic fields must be JSON numbers within the C++ type's range; numeric
string coercion is rejected. Decimal and duration fields use their string
serializers for syntax, range and precision checks. Time points check target
duration range and reject precision loss on input.
Integral floating point values such as `4.0` are accepted for integers
and normalized safely before AWL deserialization. Opaque JSON and floating point
fields retain their number kinds. Non-finite numbers and invalid UTF-8 are rejected
throughout the JSON document, including opaque DOM fields.

`asyncCall()` validates arguments, uses `awl::fromJson`, awaits `asyncExecute`, then
uses `awl::toJson` and validates the output. Successful results contain both
`structuredContent` and a text block with its serialized JSON. Argument errors
return `isError=true` with a field path and no structured content. Execution/output
exceptions follow the server's existing tool error handling; cancellation propagates.
Custom business failures can throw AWL exceptions; use plain `ITool` when returning
custom content blocks or structured error results directly.

`Tests/TypedToolTest.cpp` checks schemas, strict validation, nested serialization,
optional/null values, enum names, numeric boundaries, progress metadata, stdio/HTTP
route isolation, input/output failures and cancellation with joined child work.
`Tests/JsonSchemaTest.cpp` adds coverage of all built-in families, Unicode,
recursive schemas, wrappers and custom serializers. `TypedToolAllTypes` exercises
expanded input/output types and invalid requests through both test transports.

## AWL duration helpers

`Awl/Duration.h` provides `format_duration(stream, value)`,
`duration_to_string<Character>(value)` and `parse_duration<Duration>(text)`.
These remain available through `Awl/Time.h`. Formatting and parsing support
narrow and wide text, with the same portable extended format on all compilers:

```cpp
#include "Awl/Time.h"

const auto text = awl::duration_to_string<char>(std::chrono::milliseconds{1500});
// "1s.5"
const auto value = awl::parse_duration<std::chrono::milliseconds>(text);
// 1500 milliseconds
```

The format uses days, hours, minutes, seconds and an optional fractional second,
for example `2d.3h.4m.5s.006`, `-0.001` or `0`. Helpers use nanosecond resolution
and the signed 64-bit nanosecond range; they reject malformed text, overflow and
precision loss during conversion. The existing `part_count` parameter remains
for source compatibility and does not truncate the result.

The AWL JSON duration serializer delegates to these helpers and retains the
existing string representation. Supporting the expanded schemas also fixes wide
Unicode conversion, integer character parsing and checked time-point conversion,
and enables list/deque/multimap deserialization. Review findings and remaining
limitations are recorded in [JsonSerializerReview.md](doc/JsonSerializerReview.md).
`Tests/DurationTest.cpp` covers formatting, parsing, range/precision errors and
JSON round trips, including durations inside reflected objects and containers.
