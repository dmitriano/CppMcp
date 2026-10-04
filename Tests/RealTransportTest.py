"""Integration tests against a separately linked C++ host and actual OS pipes/TCP."""
import base64
import concurrent.futures
import http.client
import json
import queue
import socket
import subprocess
import sys
import threading
import time

EXE, CASE = sys.argv[1:]
VERSION = "2026-07-28"


def rpc(method="test/echo", params=None, request_id=1):
    params = dict(params or {})
    params["_meta"] = {"io.modelcontextprotocol/protocolVersion": VERSION,
                       "io.modelcontextprotocol/clientCapabilities": {}}
    if method == "test/slow" or params.get("progress"):
        params["_meta"]["progressToken"] = "progress-test"
    return {"jsonrpc": "2.0", "id": request_id, "method": method, "params": params}


def encoded(value):
    return json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def headers(value):
    result = {"Content-Type": "application/json", "Accept": "application/json, text/event-stream"}
    if isinstance(value, dict) and "id" in value:
        result.update({"MCP-Protocol-Version": value.get("params", {}).get("_meta", {}).get(
            "io.modelcontextprotocol/protocolVersion", VERSION), "Mcp-Method": value.get("method", "")})
        if value.get("method") in ("tools/call", "resources/read", "prompts/get"):
            name = value.get("params", {}).get("uri" if value["method"] == "resources/read" else "name", "")
            if not name.isascii() or name != name.strip() or (name.startswith("=?base64?") and name.endswith("?=")):
                name = "=?base64?" + base64.b64encode(name.encode("utf-8")).decode("ascii") + "?="
            result["Mcp-Name"] = name
    return result


class Host:
    def __init__(self, mode="stdio", stop_after=0, max_bytes=1048576, read_output=True):
        self.mode = mode
        self.process = subprocess.Popen([EXE, mode, str(stop_after), str(max_bytes)],
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.lines = queue.Queue()
        self.ready = queue.Queue()
        self.errors = []
        self.threads = []

        def read_stderr():
            for line in iter(self.process.stderr.readline, b""):
                self.errors.append(line.decode("utf-8", errors="replace"))
                if line.startswith(b"READY "):
                    self.ready.put(int(line.split()[1]))

        def read_stdout():
            for line in iter(self.process.stdout.readline, b""):
                self.lines.put(line)
            self.lines.put(None)

        for target in ([read_stderr, read_stdout] if read_output else [read_stderr]):
            thread = threading.Thread(target=target, daemon=True)
            thread.start()
            self.threads.append(thread)
        self.port = self.ready.get(timeout=5)

    def __enter__(self):
        return self

    def __exit__(self, *_):
        if self.process.poll() is None:
            self.process.kill()
        self.process.wait(timeout=5)
        for pipe in (self.process.stdin, self.process.stdout, self.process.stderr):
            pipe.close()
        for thread in self.threads:
            thread.join(timeout=1)

    def send(self, value):
        self.process.stdin.write(encoded(value) + b"\n")
        self.process.stdin.flush()

    def receive(self):
        line = self.lines.get(timeout=5)
        assert line is not None, self.errors
        return json.loads(line)

    def wait(self, expected=0):
        code = self.process.wait(timeout=5)
        assert code == expected, (code, self.errors)

    def http(self, value, overrides=None, method="POST", path="/mcp", raw=None):
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
        try:
            request_headers = headers(value)
            request_headers.update(overrides or {})
            connection.request(method, path, raw if raw is not None else encoded(value), request_headers)
            response = connection.getresponse()
            body = response.read()
            return response.status, dict(response.getheaders()), body
        finally:
            connection.close()

    def http_json(self, value, **kwargs):
        status, response_headers, body = self.http(value, **kwargs)
        return status, json.loads(body) if body else None

    def stop_http(self):
        status, result = self.http_json(rpc("test/stop"))
        assert status == 200 and "result" in result, result
        self.wait()


def stdio_real():
    with Host() as host:
        first = rpc(params={"text": "\u041f\u0440\u0438\u0432\u0435\u0442\nembedded", "value": 42})
        second = rpc("server/discover", request_id="second")
        notice = {"jsonrpc": "2.0", "method": "test/echo", "params": rpc()["params"]}
        data = encoded(first) + b"\r\n" + encoded(notice) + b"\n" + encoded(second) + b"\n"
        for index in range(0, len(data), 3):
            host.process.stdin.write(data[index:index+3])
            host.process.stdin.flush()
        host.process.stdin.close()
        replies = {message["id"]: message for message in (host.receive(), host.receive())}
        assert replies[1]["result"] == {"text": "\u041f\u0440\u0438\u0432\u0435\u0442\nembedded", "value": 42}
        assert replies["second"]["result"]["supportedVersions"] == [VERSION, "2025-06-18"]
        host.wait()
        assert host.lines.get(timeout=2) is None  # Notification wrote no stdout.


def stdio_errors():
    with Host() as host:
        host.process.stdin.write(b"{bad json}\n")
        host.process.stdin.flush()
        assert host.receive()["error"]["code"] == -32700
        invalid = rpc()
        invalid["params"]["_meta"]["io.modelcontextprotocol/protocolVersion"] = "1900-01-01"
        host.send(invalid)
        assert host.receive()["error"]["code"] == -32022
        host.send({"jsonrpc": "2.0", "id": 3, "method": "test/echo"})
        assert host.receive()["error"]["code"] == -32602
        host.process.stdin.close()
        host.wait()
    for data in (b"x" * 129, encoded(rpc())):
        with Host(max_bytes=128) as host:
            host.process.stdin.write(data)
            host.process.stdin.close()
            host.wait(expected=1)


def stdio_request_cancel():
    with Host() as host:
        host.send(rpc("test/slow", request_id="slow"))
        assert host.receive()["method"] == "notifications/progress"
        # Another request succeeds while the slow request is still running.
        host.send(rpc(params={"value": "parallel"}, request_id="parallel"))
        assert host.receive()["result"] == {"value": "parallel"}
        host.send({"jsonrpc": "2.0", "method": "notifications/cancelled", "params": {"requestId": "slow"}})
        for _ in range(10):
            host.send(rpc("test/stats", request_id="stats"))
            result = host.receive()
            assert result["id"] == "stats"  # No final response for cancelled work.
            if result["result"]["cancelled"] == 1:
                break
            time.sleep(0.01)
        else:
            raise AssertionError("Cancellation did not join the handler")
        host.process.stdin.close()
        host.wait()


def stdio_cancel_idle():
    with Host(stop_after=200) as host:
        host.wait()  # stdin stays open: cancellation must interrupt the read.
        assert host.lines.get(timeout=2) is None


def stdio_cancel_write():
    with Host(stop_after=350, read_output=False) as host:
        host.send(rpc("test/large"))
        host.wait()  # stdout is deliberately never drained; write must cancel.


def stdio_eof_cancel():
    with Host() as host:
        host.send(rpc("test/slow"))
        assert host.receive()["method"] == "notifications/progress"
        host.process.stdin.close()
        host.wait()  # EOF drain deadline cancels and joins the 60-second handler.
        assert host.lines.get(timeout=2) is None
    with Host(read_output=False) as host:
        host.send(rpc("test/large"))
        host.process.stdin.close()
        host.wait()  # EOF also interrupts a native write to an unread stdout pipe.


def http_timeout():
    with Host("http") as host:
        connection = http.client.HTTPConnection("127.0.0.1", host.port, timeout=5)
        value = rpc("test/slow")
        connection.request("POST", "/mcp", encoded(value), headers(value))
        response = connection.getresponse()
        assert response.readline().startswith(b"event:")
        assert response.readline().startswith(b"data:")
        try:
            response.read()  # Request timeout interrupts SSE before a final response.
        except (http.client.IncompleteRead, ConnectionResetError):
            pass
        else:
            raise AssertionError("Timed-out SSE must close before the last chunk")
        finally:
            response.close()
            connection.close()
        assert host.http_json(rpc("test/stats"))[1]["result"]["cancelled"] == 1
        host.stop_http()
    with Host("http", stop_after=200) as host:
        connection = http.client.HTTPConnection("127.0.0.1", host.port, timeout=5)
        value = rpc("test/slow")
        connection.request("POST", "/mcp", encoded(value), headers(value))
        response = connection.getresponse()
        assert response.readline().startswith(b"event:")
        host.wait()  # Root StopToken joins a live HTTP request and its handler.
        response.close()
        connection.close()


def http_real():
    with Host("http") as host:
        status, reply = host.http_json(rpc("server/discover"))
        assert status == 200 and reply["result"]["supportedVersions"] == [VERSION, "2025-06-18"]
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            futures = [pool.submit(host.http_json, rpc(params={"value": value}, request_id=1)) for value in ("a", "b")]
            for future, value in zip(futures, ("a", "b")):
                status, reply = future.result(timeout=5)
                assert status == 200 and reply["id"] == 1 and reply["result"] == {"value": value}
        notice = {"jsonrpc": "2.0", "method": "test/echo", "params": rpc(params={"progress": True})["params"]}
        status, _, body = host.http(notice)
        assert status == 202 and body == b""
        host.stop_http()


def http_sse():
    with Host("http") as host:
        status, response_headers, body = host.http(rpc(params={"progress": True, "text": "\u041c\u0438\u0440"}))
        assert status == 200 and response_headers["Content-Type"] == "text/event-stream"
        events = [json.loads(line[6:]) for line in body.splitlines() if line.startswith(b"data: ")]
        assert len(events) == 2 and events[0]["method"] == "notifications/progress"
        assert events[1]["result"] == {"progress": True, "text": "\u041c\u0438\u0440"}
        host.stop_http()


def http_errors():
    with Host("http") as host:
        cases = [({"Origin": "http://denied.example"}, 403), ({"Origin": ""}, 403),
                 ({"Mcp-Method": "wrong"}, 400), ({"MCP-Protocol-Version": "wrong"}, 400),
                 ({"Accept": "application/json"}, 406),
                 ({"Accept": "application/json-extra, text/event-stream"}, 406),
                 ({"Accept": "application/json;q=0, text/event-stream"}, 406), ({"Content-Type": "text/plain"}, 415)]
        for overrides, expected in cases:
            status, _ = host.http_json(rpc(), overrides=overrides)
            assert status == expected, (overrides, status)
        assert host.http_json(rpc(), overrides={"Origin": "http://allowed.example"})[0] == 200
        assert host.http_json(rpc(), overrides={"Content-Type": "Application/JSON; charset=UTF-8",
                                              "Accept": "Application/JSON;q=0.5, Text/Event-Stream;q=1"})[0] == 200
        assert host.http_json(rpc(), raw=b"[]")[1]["error"]["code"] == -32600
        for name in ("\u041c\u0438\u0440", "=?base64?literal?=", " padded "):
            status, reply = host.http_json(rpc("tools/call", {"name": name}))
            assert status == 200 and reply["error"]["code"] == -32602  # Header decoded; tool is absent.
        for bad_header in ("=?base64?@#$!?=", "=?base64?Zh==?=", "=?base64?Zg=?="):
            assert host.http_json(rpc("tools/call", {"name": "f"}), overrides={"Mcp-Name": bad_header})[0] == 400
        missing_capabilities = rpc()
        del missing_capabilities["params"]["_meta"]["io.modelcontextprotocol/clientCapabilities"]
        assert host.http_json(missing_capabilities)[1]["error"]["code"] == -32602
        status, response_headers, _ = host.http(rpc(), method="GET")
        assert status == 405 and response_headers["Allow"] == "POST"
        assert host.http_json(rpc(), path="/wrong")[0] == 404
        value = rpc()
        with socket.create_connection(("127.0.0.1", host.port), timeout=3) as client:
            body = encoded(value)
            lines = ["POST /mcp HTTP/1.1", "Host: localhost", "Content-Type: application/json",
                     "Accept: application/json, text/event-stream", "Mcp-Method: test/echo", "Mcp-Method: wrong",
                     "MCP-Protocol-Version: " + VERSION, "Content-Length: " + str(len(body)), "", ""]
            client.sendall("\r\n".join(lines).encode("ascii") + body)
            response = http.client.HTTPResponse(client)
            response.begin()
            assert response.status == 400 and json.loads(response.read())["error"]["code"] == -32020
            response.close()
        assert host.http_json(rpc(), raw=b"{bad json}")[0] == 400
        assert host.http_json(rpc("unknown"))[0] == 404
        wrong_version = rpc()
        wrong_version["params"]["_meta"]["io.modelcontextprotocol/protocolVersion"] = "1900-01-01"
        status, reply = host.http_json(wrong_version)
        assert status == 400 and reply["error"]["code"] == -32022
        assert host.http_json(rpc("tools/call", {"name": "missing"}), overrides={"Mcp-Name": "wrong"})[0] == 400
        with socket.create_connection(("127.0.0.1", host.port), timeout=3) as client:
            client.sendall(b"POST /mcp HTTP/1.1\r\n")
            time.sleep(0.7)
            try:
                assert client.recv(1) == b""  # Header read deadline.
            except ConnectionResetError:
                pass  # Windows may report a reset when unread input is discarded.
        host.stop_http()
    with Host("http", max_bytes=256) as host:
        assert host.http_json(rpc(), raw=b"x" * 1024)[0] == 413
        host.stop_http()


def http_disconnect():
    with Host("http") as host:
        connection = http.client.HTTPConnection("127.0.0.1", host.port, timeout=5)
        request = rpc("test/slow", request_id="slow")
        connection.request("POST", "/mcp", encoded(request), headers(request))
        response = connection.getresponse()
        assert response.getheader("Content-Type") == "text/event-stream"
        assert response.readline().startswith(b"event:")
        assert response.readline().startswith(b"data:")
        # A second client must not wait for the first handler to complete.
        assert host.http_json(rpc(params={"value": 42}))[1]["result"] == {"value": 42}
        response.close()
        connection.close()
        for _ in range(30):
            status, reply = host.http_json(rpc("test/stats"))
            if reply["result"]["cancelled"] == 1:
                break
            time.sleep(0.01)
        else:
            raise AssertionError("Disconnect did not cancel the request")
        host.stop_http()


def combined():
    with Host("both") as host:
        host.send(rpc(params={"source": "stdio"}))
        assert host.receive()["result"] == {"source": "stdio"}
        assert host.http_json(rpc(params={"source": "http"}))[1]["result"] == {"source": "http"}
        host.process.stdin.close()
        # Standard handles belong to the application, which remains alive for
        # HTTP. EOF drains the stdio transport, not the process's original stdout.
        time.sleep(0.05)
        assert host.process.poll() is None
        assert host.http_json(rpc(params={"after": "stdio EOF"}))[1]["result"] == {"after": "stdio EOF"}
        host.stop_http()


def legacy_request(method, params=None, request_id=1):
    return {"jsonrpc": "2.0", "id": request_id, "method": method, "params": params or {}}


def initialize_request(version="2025-06-18"):
    return legacy_request("initialize", {"protocolVersion": version, "capabilities": {},
                                         "clientInfo": {"name": "integration-test", "version": "1.0"}})


def check_echo_definition(result):
    assert len(result["tools"]) == 1
    definition = result["tools"][0]
    assert definition["name"] == "echo"
    assert definition["inputSchema"]["properties"]["text"]["type"] == "string"
    assert definition["inputSchema"]["required"] == ["text"]
    assert definition["outputSchema"]["properties"]["text"]["type"] == "string"


def check_echo_result(result):
    assert result["isError"] is False
    expected = {"text": "\u041f\u0440\u0438\u0432\u0435\u0442 from MCP"}
    assert result["structuredContent"] == expected
    assert json.loads(result["content"][0]["text"]) == expected


def echo_request(modern=True):
    make = rpc if modern else legacy_request
    return make("tools/call", {"name": "echo", "arguments": {"text": "\u041f\u0440\u0438\u0432\u0435\u0442 from MCP"}})


def tool_real():
    with Host("both") as host:
        host.send(rpc("tools/list"))
        check_echo_definition(host.receive()["result"])
        host.send(echo_request())
        check_echo_result(host.receive()["result"])
        check_echo_definition(host.http_json(rpc("tools/list"))[1]["result"])
        check_echo_result(host.http_json(echo_request())[1]["result"])
        invalid = rpc("tools/call", {"name": "echo", "arguments": {"text": 42}})
        result = host.http_json(invalid)[1]["result"]
        assert result["isError"] is True and "structuredContent" not in result
        host.process.stdin.close()
        host.stop_http()


def legacy_stdio():
    with Host() as host:
        host.send(legacy_request("initialize"))
        assert host.receive()["error"]["code"] == -32602
        host.send(legacy_request("tools/list"))
        assert host.receive()["error"]["code"] == -32602  # Failed init did not disable modern validation.
        host.send(initialize_request())
        initialized = host.receive()["result"]
        assert initialized["protocolVersion"] == "2025-06-18" and "tools" in initialized["capabilities"]
        assert initialized["serverInfo"]["name"] == "CppMcp"
        host.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
        host.send(legacy_request("ping"))
        assert host.receive()["result"] == {}
        host.send(legacy_request("tools/list"))
        result = host.receive()["result"]
        check_echo_definition(result)
        assert "resultType" not in result
        host.send(echo_request(modern=False))
        result = host.receive()["result"]
        check_echo_result(result)
        assert "resultType" not in result
        host.process.stdin.close()
        host.wait()


def legacy_http():
    legacy_headers = {"MCP-Protocol-Version": "2025-06-18"}
    with Host("http") as host:
        # The first request has no MCP headers or per-request metadata.
        connection = http.client.HTTPConnection("127.0.0.1", host.port, timeout=5)
        connection.request("POST", "/mcp", encoded(initialize_request()), {
            "Content-Type": "application/json", "Accept": "application/json, text/event-stream"})
        response = connection.getresponse()
        assert response.status == 200
        initialized = json.loads(response.read())["result"]
        assert initialized["protocolVersion"] == "2025-06-18"
        connection.close()
        notice = {"jsonrpc": "2.0", "method": "notifications/initialized"}
        assert host.http(notice, overrides=legacy_headers)[0] == 202
        for version in ("2025-06-18", "unknown-version"):
            result = host.http_json(initialize_request(version))[1]["result"]
            assert result["protocolVersion"] == "2025-06-18"  # Offered fallback for unknown versions.
        status, reply = host.http_json(legacy_request("tools/list"), overrides=legacy_headers)
        assert status == 200
        check_echo_definition(reply["result"])
        assert "resultType" not in reply["result"]
        status, reply = host.http_json(echo_request(modern=False), overrides=legacy_headers)
        assert status == 200
        check_echo_result(reply["result"])
        assert "resultType" not in reply["result"]
        assert host.http_json(legacy_request("ping"), overrides=legacy_headers)[1]["result"] == {}
        # Initialization must not globally relax validation for other HTTP peers.
        assert host.http_json(legacy_request("tools/list"), overrides={"MCP-Protocol-Version": ""})[0] == 400
        assert host.http_json(rpc("tools/list"), overrides=legacy_headers)[0] == 400
        assert host.http_json(rpc("tools/list"))[1]["result"]["resultType"] == "complete"
        host.stop_http()


TESTS = {"StdioReal": stdio_real, "StdioFramingErrors": stdio_errors,
         "StdioRequestCancel": stdio_request_cancel, "StdioCancelIdle": stdio_cancel_idle,
         "StdioCancelWrite": stdio_cancel_write, "StdioEofCancel": stdio_eof_cancel,
         "HttpTimeout": http_timeout, "HttpReal": http_real, "HttpSse": http_sse,
         "ToolReal": tool_real, "LegacyStdio": legacy_stdio, "LegacyHttp": legacy_http,
         "HttpErrors": http_errors, "HttpDisconnect": http_disconnect, "CombinedTransports": combined}
if __name__ == "__main__":
    TESTS[CASE]()
