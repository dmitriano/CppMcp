# Real transports

Create transports directly, give each its own output channel and transfer it to
`Server::addTransport`. Both transports use the same server strand and input:

```cpp
#include "Server/Server.h"
#include "Transport/StdioTransport.h"
#include "Transport/HttpTransport.h"
#include "BoostExtras/StopToken.h"
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/thread_pool.hpp>

// io and logger belong to the application; configure the logger sink as stderr.
const auto executor = boost::asio::make_strand(io);
boost::asio::thread_pool blocking_io(2); // External workers for Windows stdio.
std::shared_ptr<mcp::InputChannel> input = std::make_shared<mcp::InputChannel>(executor, 64);
mcp::Server server(executor, input, logger->createLogger("Server"));
std::shared_ptr<mcp::OutputChannel> stdio_output = std::make_shared<mcp::OutputChannel>(executor, 64);
server.addTransport(std::make_unique<mcp::StdioTransport>(executor,
    mcp::openStdioStreams(executor, blocking_io.get_executor()), input, stdio_output,
    logger->createLogger("Stdio")));
std::shared_ptr<mcp::OutputChannel> http_output = std::make_shared<mcp::OutputChannel>(executor, 64);
mcp::HttpOptions http_options;
http_options.endpoint.port(8080); // Loopback by default; 0 selects a free port.
server.addTransport(std::make_unique<mcp::HttpTransport>(executor, input, http_output,
    logger->createLogger("Http"), http_options));

std::stop_source stop_source;
boost::asio::post(executor, [&]
{
    // Construct and spawn on the same strand; retain the adapter until completion.
    std::shared_ptr<awl::StopToken> cancellation = std::make_shared<awl::StopToken>(
        executor, stop_source.get_token());
    boost::asio::co_spawn(executor, server.asyncRun(),
        boost::asio::bind_cancellation_slot(cancellation->slot(),
            [cancellation](std::exception_ptr error)
            {
                // Application completion handler observes error and cancellation.
            }));
});
// Register application handlers before running io. A shutdown callback/thread
// calls stop_source.request_stop(); do not stop io prematurely.
io.run();
blocking_io.join();
```

The library creates no threads, pools or strands. Keep the server, channels,
logger and execution resources alive until `asyncRun()` joins its children.
An AWL stop adapter connects `std::stop_token` to Asio cancellation. Each request
has a separate adapter; `RequestContext::stopToken()` exposes its combined
request/shutdown token for cooperative synchronous work. Async handlers use
Asio's default cancellation checks and must await their child work. Blocking
CPU work must run on an appropriate external executor; do not block the strand.

## Stdio

`StdioTransport` reads UTF-8 JSON lines, supports partial/multiple frames and CRLF,
and writes only compact JSON plus a newline to stdout. Stderr carries logging.
`StdioOptions` bounds frame size and pending work; duplicate active IDs and
unterminated EOF frames fail the transport. `notifications/cancelled` stops the
matching active request without a final reply. EOF drains accepted work for up
to `shutdownTimeout` (default: one second), then cancels remaining handlers and
blocked writes. An EOF on stdio leaves HTTP running.

Native stdio code lives in `Platform/Windows` and `Platform/Posix`; CMake selects
one implementation. Windows duplicates standard handles and performs synchronous
`ReadFile`/`WriteFile` on the external pool, with `CancelSynchronousIo` and joined
completion. Supply at least two available workers on a separate blocking executor.
POSIX uses duplicated descriptors and cancellable Asio descriptor operations;
the blocking executor is unused there. POSIX applications should configure their
SIGPIPE policy before using output pipes. Closing a transport releases its own
handles, leaving the application's original standard handles open.

## HTTP

`HttpTransport` binds/listens at construction; `localEndpoint()` reports the actual
port. It serves POST `/mcp` on loopback by default, using Boost.Beast. JSON replies
use `application/json`; request-scoped notifications switch a request to chunked
SSE, followed by its final JSON-RPC response. Notifications receive empty HTTP 202.
Both media types must be acceptable to the client. HTTP request headers
`MCP-Protocol-Version`, `Mcp-Method` and, where applicable, `Mcp-Name` must match the
body; names support the protocol's Base64 sentinel encoding. Invalid/missing
headers return HTTP 400 / `-32020`; unknown RPC methods return HTTP 404 / `-32601`.
Origin headers are checked against `HttpOptions::allowedOrigins`, including empty
or `null` origins; absent Origin is accepted. Configure the allowlist explicitly
for browser callers.

This implementation uses one POST per TCP connection and sends `Connection: close`.
Each request has its own route, so identical JSON-RPC IDs from different clients
remain independent. Disconnect, request timeout, queue overflow or shutdown
cancels that request. `HttpOptions` bounds body size, connections and queued
responses, and sets read/request deadlines. It provides plain HTTP; TLS and
application authentication are not supplied. Optional schema-directed
`Mcp-Param-*` header mirroring is not implemented, so do not advertise
`x-mcp-header` annotations. Legacy GET/DELETE/session/resumable SSE mechanisms
are not supported by this transport. These behaviors follow the modern
[stdio](https://modelcontextprotocol.io/specification/2026-07-28/basic/transports/stdio)
and [Streamable HTTP](https://modelcontextprotocol.io/specification/2026-07-28/basic/transports/streamable-http)
transport model.

## Protocol metadata

Real transports target MCP `2026-07-28`: each request carries
`params._meta["io.modelcontextprotocol/protocolVersion"]` and object-valued
`params._meta["io.modelcontextprotocol/clientCapabilities"]`; optional `clientInfo`
contains string `name` and `version`. `server/discover` advertises the supported
version, tools/resources capabilities and server identity from `ServerOptions`.
Unsupported versions return `-32022` with the requested/supported versions.

The compatibility dialect `2025-06-18` implements `initialize`,
`notifications/initialized` and `ping`. Initialization validates the client's
version, capabilities and identity, then offers `2025-06-18`. Stdio remembers the
negotiated version after a successful initialization response; a failed response
does not change its dialect. HTTP remains stateless: initialization needs no
version header, while later requests carry `MCP-Protocol-Version: 2025-06-18`.
That dialect does not require modern metadata or mirrored method/name headers,
and returns the legacy result shape without `resultType`. Supplied mirrored
headers are still validated. Initialization of one HTTP client does not change
the dialect or relax validation for other clients. Session IDs, subscriptions,
prompts, pagination and server-to-client requests are not implemented.

## Connect the test host to Codex

The test host registers a `TypedTool` named `echo`, accepting `{ "text": "..." }`
and returning both structured `{ "text": "..." }` and a text block with its JSON.
Its input and output schemas come from AWL reflection. Invalid inputs return a
tool error. The existing `test/echo` handler remains a separate test RPC method.

The host arguments are mode, shutdown timer in milliseconds, message/body byte
limit, and HTTP port. Port 0 (the default) chooses a free port. To reuse an existing
Codex connection to `http://127.0.0.1:52034/mcp`, launch in PowerShell:

```powershell
& C:/dev/build/cppmcp/Tests/RelWithDebInfo/CppMcpTransportHost.exe both 0 1048576 52034
```

The process prints `READY 52034` to stderr. Restart the Codex extension after
starting the host so it discovers the `echo` tool. With stdio, configure Codex to
launch the executable with `args = ["stdio"]` instead of starting it manually.
Actual initialization, discovery and tool calls were verified through the MCP
client in Codex 0.160.0 using the `2025-06-18` dialect over HTTP and stdio.

## Tests

`Tests/RealTransportTest.py` launches `CppMcpTransportHost`, linked against the
library, and exercises actual OS pipes and loopback TCP: UTF-8/framing, JSON/SSE,
notifications, discovery, header/error handling, body limits, concurrent routes,
EOF and explicit cancellation, unread stdout, client disconnect, deadlines and
combined stdio/HTTP shutdown. Python 3 is required when `BUILD_TESTING=ON`.
The regular set has 43 CTest cases: 28 unit tests and 15 integration scenarios,
including modern and legacy tool listing/calls and initialization failures.
An optional `CodexMcpInterop` test runs the installed Codex app-server with isolated
temporary configuration/state. It lists and calls `echo` over HTTP and stdio,
without starting a model turn or changing the user's Codex configuration. Enable
it with `-DCPPMCP_TEST_CODEX=ON`; supply `-DCODEX_EXECUTABLE=<path>` if Codex is not
in PATH. The check uses experimental app-server APIs tested with Codex 0.160.0.
Windows tests are verified; the POSIX source has not been compiled or executed
in this Windows environment.
