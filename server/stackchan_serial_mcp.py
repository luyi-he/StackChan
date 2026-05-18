"""
Serial MCP server for StackChan.
Bridges Hermes Agent MCP tools to StackChan via USB serial (COM4).

Usage:
    python serial_mcp_server.py

Environment variables:
    STACKCHAN_PORT - Serial port (default: COM4)
    STACKCHAN_BAUD - Baud rate (default: 115200)
"""

import json
import os
import sys
import time
import threading
import serial
from typing import Any


# ── Configuration ──────────────────────────────────────────────────────────
SERIAL_PORT = os.environ.get("STACKCHAN_PORT", "COM4")
SERIAL_BAUD = int(os.environ.get("STACKCHAN_BAUD", "115200"))


# ── Serial Connection ──────────────────────────────────────────────────────
class StackChanSerial:
    """Manages the USB serial connection to StackChan."""

    def __init__(self, port: str, baud: int):
        self.port = port
        self.baud = baud
        self._serial: serial.Serial | None = None
        self._lock = threading.Lock()
        self._response_event = threading.Event()
        self._last_response: str | None = None
        self._buffer = ""
        self._reader_thread: threading.Thread | None = None

    def connect(self) -> bool:
        try:
            self._serial = serial.Serial(
                port=self.port,
                baudrate=self.baud,
                timeout=0.1,
                write_timeout=1,
            )
            # Start reader thread
            self._reader_thread = threading.Thread(target=self._reader_loop, daemon=True)
            self._reader_thread.start()
            # Flush any stale data
            time.sleep(0.5)
            self._serial.reset_input_buffer()
            return True
        except Exception as e:
            print(f"Failed to connect to {self.port}: {e}", file=sys.stderr)
            return False

    def disconnect(self):
        with self._lock:
            if self._serial:
                self._serial.close()
                self._serial = None

    def send_command(self, command: dict, timeout: float = 3.0) -> dict:
        """Send a JSON command and wait for response."""
        payload = json.dumps(command) + "\n"
        self._response_event.clear()
        self._last_response = None

        with self._lock:
            if not self._serial:
                return {"ok": False, "error": "not connected"}
            self._serial.write(payload.encode())

        # Wait for response
        if self._response_event.wait(timeout):
            try:
                return json.loads(self._last_response or "{}")
            except json.JSONDecodeError:
                return {"ok": False, "error": f"invalid response: {self._last_response}"}
        return {"ok": False, "error": "timeout"}

    def _reader_loop(self):
        while True:
            try:
                with self._lock:
                    if not self._serial or not self._serial.is_open:
                        break
                    if self._serial.in_waiting:
                        data = self._serial.read(self._serial.in_waiting).decode(errors="replace")
                        self._buffer += data

                        # Process complete lines
                        while "\n" in self._buffer:
                            line, self._buffer = self._buffer.split("\n", 1)
                            line = line.strip()
                            if line and line.startswith("{"):
                                self._last_response = line
                                self._response_event.set()
            except Exception:
                break
            time.sleep(0.01)

    @property
    def is_connected(self) -> bool:
        return self._serial is not None and self._serial.is_open


# ── Serial MCP Server (stdio) ──────────────────────────────────────────────
class SerialMcpServer:
    """Acts as an MCP server via stdio, bridging to StackChan over serial."""

    def __init__(self, serial_dev: StackChanSerial):
        self.serial = serial_dev
        self.request_id = 0

        # Tool definitions (MCP tool schema)
        self.tools = [
            {
                "name": "get_status",
                "description": "Check StackChan serial connection status",
                "inputSchema": {"type": "object", "properties": {}},
            },
            {
                "name": "move_head",
                "description": "Move StackChan's head. Yaw: horizontal (-90..90), Pitch: vertical (5..85), Speed: 100-1000",
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "yaw": {"type": "integer", "description": "Horizontal angle (-90 to 90)", "default": -9999},
                        "pitch": {"type": "integer", "description": "Vertical angle (5 to 85)", "default": -9999},
                        "speed": {"type": "integer", "description": "Speed (100-1000)", "default": 150},
                    },
                },
            },
            {
                "name": "get_head_angles",
                "description": "Get current head yaw/pitch angles",
                "inputSchema": {"type": "object", "properties": {}},
            },
            {
                "name": "set_led",
                "description": "Set one of the 12 RGB LEDs by index",
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "index": {"type": "integer", "description": "LED index (0-11)"},
                        "r": {"type": "integer", "description": "Red (0-255)"},
                        "g": {"type": "integer", "description": "Green (0-255)"},
                        "b": {"type": "integer", "description": "Blue (0-255)"},
                    },
                    "required": ["index", "r", "g", "b"],
                },
            },
            {
                "name": "set_all_leds",
                "description": "Set all 12 RGB LEDs to the same color",
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "r": {"type": "integer", "description": "Red (0-255)"},
                        "g": {"type": "integer", "description": "Green (0-255)"},
                        "b": {"type": "integer", "description": "Blue (0-255)"},
                    },
                    "required": ["r", "g", "b"],
                },
            },
            {
                "name": "clear_leds",
                "description": "Turn all RGB LEDs off",
                "inputSchema": {"type": "object", "properties": {}},
            },
            {
                "name": "get_battery",
                "description": "Get battery level and charging status",
                "inputSchema": {"type": "object", "properties": {}},
            },
            {
                "name": "set_volume",
                "description": "Set speaker volume (0-100)",
                "inputSchema": {
                    "type": "object",
                    "properties": {"volume": {"type": "integer", "description": "Volume (0-100)"}},
                    "required": ["volume"],
                },
            },
            {
                "name": "set_servo_power",
                "description": "Enable or disable servo power",
                "inputSchema": {
                    "type": "object",
                    "properties": {"on": {"type": "boolean", "description": "True to enable, false to disable"}},
                    "required": ["on"],
                },
            },
        ]

    def _send_jsonrpc(self, method: str, params: dict | None = None, result: Any = None, error: dict | None = None):
        """Send a JSON-RPC message over stdio."""
        msg: dict = {"jsonrpc": "2.0"}
        if method:
            msg["method"] = method
            if params:
                msg["params"] = params
        if result is not None:
            self.request_id += 1
            msg["id"] = self.request_id
            msg["result"] = result
        if error:
            self.request_id += 1
            msg["id"] = self.request_id
            msg["error"] = error
        sys.stdout.write(json.dumps(msg) + "\n")
        sys.stdout.flush()

    def _handle_tool_call(self, name: str, args: dict) -> dict:
        """Handle a tool call by sending a serial command to StackChan."""
        cmd_map = {
            "move_head": "set_head",
            "get_head_angles": "get_head",
            "set_led": "set_led",
            "set_all_leds": "set_all_leds",
            "clear_leds": "clear_leds",
            "get_battery": "get_battery",
            "set_volume": "set_volume",
            "set_servo_power": "set_servo_power",
        }

        cmd_name = cmd_map.get(name)
        if not cmd_name:
            return {"ok": False, "error": f"unknown tool: {name}"}

        command = {"cmd": cmd_name}
        for k, v in args.items():
            if v is not None:
                command[k] = v

        # Handle defaults for move_head
        if name == "move_head":
            if "yaw" not in args or args["yaw"] is None:
                command["yaw"] = -9999
            if "pitch" not in args or args["pitch"] is None:
                command["pitch"] = -9999

        result = self.serial.send_command(command)
        return result

    def run(self):
        """Main loop: read JSON-RPC from stdin, dispatch."""

        # Send capabilities (MCP server info)
        self._send_jsonrpc("capabilities", {
            "tools": self.tools,
        })

        buffer = ""
        while True:
            try:
                chunk = sys.stdin.read(4096)
                if not chunk:
                    break
                buffer += chunk

                while "\n" in buffer:
                    line, buffer = buffer.split("\n", 1)
                    line = line.strip()
                    if not line:
                        continue

                    try:
                        msg = json.loads(line)
                    except json.JSONDecodeError:
                        continue

                    method = msg.get("method", "")
                    msg_id = msg.get("id")

                    if method == "tools/list":
                        self._send_jsonrpc("", result=self.tools)

                    elif method == "tools/call":
                        params = msg.get("params", {})
                        name = params.get("name", "")
                        args = params.get("arguments", {})

                        if name == "get_status":
                            result = {
                                "ok": True,
                                "connected": self.serial.is_connected,
                                "port": SERIAL_PORT,
                            }
                            self._send_jsonrpc("", result=result)
                        else:
                            result = self._handle_tool_call(name, args)
                            self._send_jsonrpc("", result=result)

                    elif method == "initialize":
                        self._send_jsonrpc("", result={
                            "protocolVersion": "2024-11-05",
                            "serverInfo": {"name": "stackchan-serial-mcp", "version": "1.0.0"},
                        })

                    elif method == "notifications/initialized":
                        pass

            except (EOFError, KeyboardInterrupt):
                break
            except Exception as e:
                error_msg = {"code": -32603, "message": str(e)}
                self._send_jsonrpc("", error=error_msg)


# ── Main ───────────────────────────────────────────────────────────────────
def main():
    serial_dev = StackChanSerial(SERIAL_PORT, SERIAL_BAUD)
    connected = serial_dev.connect()

    if not connected:
        print(f"Warning: Could not connect to StackChan on {SERIAL_PORT}", file=sys.stderr)
        print("Make sure StackChan is connected via USB and the COM port is correct.", file=sys.stderr)
        print(f"You can set STACKCHAN_PORT environment variable to change the port.", file=sys.stderr)
        # Still start the server - user can diagnose connection issues
    else:
        print(f"Connected to StackChan on {SERIAL_PORT}", file=sys.stderr)

    server = SerialMcpServer(serial_dev)
    try:
        server.run()
    finally:
        serial_dev.disconnect()


if __name__ == "__main__":
    main()
