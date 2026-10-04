"""Exercise the installed Codex MCP client without starting a model turn."""
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading

from RealTransportTest import Host


class CodexClient:
    def __init__(self, home):
        # Isolate configuration and state; never modify the user's Codex config.
        environment = dict(os.environ, CODEX_HOME=str(home))
        self.process = subprocess.Popen([sys.argv[2], "app-server"], env=environment,
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                        text=True, encoding="utf-8")
        self.messages = queue.Queue()
        self.errors = []
        self.threads = []

        def read_stdout():
            for line in self.process.stdout:
                self.messages.put(json.loads(line))
            self.messages.put(None)

        def read_stderr():
            for line in self.process.stderr:
                self.errors.append(line.rstrip())

        for target in (read_stdout, read_stderr):
            thread = threading.Thread(target=target, daemon=True)
            thread.start()
            self.threads.append(thread)

    def __enter__(self):
        try:
            self.request(0, "initialize", {"clientInfo": {"name": "cppmcp_test", "version": "0.1.0"},
                                          "capabilities": {"experimentalApi": True}})
            self.send({"method": "initialized", "params": {}})
        except BaseException:
            self.__exit__()
            raise
        return self

    def __exit__(self, *_):
        self.process.terminate()
        self.process.wait(timeout=5)
        for pipe in (self.process.stdin, self.process.stdout, self.process.stderr):
            pipe.close()
        for thread in self.threads:
            thread.join(timeout=1)

    def send(self, message):
        self.process.stdin.write(json.dumps(message) + "\n")
        self.process.stdin.flush()

    def request(self, request_id, method, params):
        self.send({"id": request_id, "method": method, "params": params})
        for _ in range(200):
            message = self.messages.get(timeout=20)
            assert message is not None, self.errors
            if message.get("id") == request_id:
                assert "error" not in message, (method, message, self.errors)
                return message["result"]
        raise AssertionError("Too many unrelated Codex notifications")


def check_codex(home):
    with CodexClient(home) as client:
        thread = client.request(1, "thread/start", {"cwd": str(home), "ephemeral": True,
                                                   "approvalPolicy": "never", "sandbox": "read-only"})
        thread_id = thread["thread"]["id"]
        inventory = client.request(2, "mcpServerStatus/list", {"threadId": thread_id,
                                                              "serverName": "cppmcp",
                                                              "detail": "toolsAndAuthOnly"})
        assert len(inventory["data"]) == 1, inventory
        server = inventory["data"][0]
        assert not server.get("toolsError"), (server, client.errors)
        assert any(tool["name"] == "echo" for tool in server["tools"].values()), server
        result = client.request(3, "mcpServer/tool/call", {"threadId": thread_id, "server": "cppmcp",
                                                          "tool": "echo", "arguments": {"text": "hello from Codex"}})
        assert result["isError"] is False, result
        assert result["structuredContent"] == {"text": "hello from Codex"}, result
        assert json.loads(result["content"][0]["text"]) == {"text": "hello from Codex"}, result


with tempfile.TemporaryDirectory(prefix="cppmcp-codex-") as temporary:
    root = Path(temporary)
    with Host("http") as host:
        http_home = root / "http"
        http_home.mkdir()
        (http_home / "config.toml").write_text(
            '[mcp_servers.cppmcp]\nurl = "http://127.0.0.1:' + str(host.port) + '/mcp"\n'
            'startup_timeout_sec = 10\ndefault_tools_approval_mode = "auto"\n', encoding="utf-8")
        check_codex(http_home)
        host.stop_http()
    stdio_home = root / "stdio"
    stdio_home.mkdir()
    # JSON quoting also produces a valid TOML basic string for a Windows path.
    (stdio_home / "config.toml").write_text(
        '[mcp_servers.cppmcp]\ncommand = ' + json.dumps(str(Path(sys.argv[1]).resolve())) + '\n'
        'args = ["--run=McpServer_Example", "--output=all", "--output_stream=stderr", "--transport=stdio"]\nstartup_timeout_sec = 10\ndefault_tools_approval_mode = "auto"\n', encoding="utf-8")
    check_codex(stdio_home)
print("Codex discovered and called echo over HTTP and stdio; no model turn was started.")
