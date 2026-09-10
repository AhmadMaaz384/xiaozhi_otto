#!/usr/bin/env python3
"""Manually trigger any of Otto's MCP tools over the board's WebSocket
control server (port 8080), bypassing voice/LLM entirely.

This talks to WebSocketControlServer::HandleMessage() (websocket_control_server.cc),
which accepts a bare MCP JSON-RPC "tools/call" payload sent directly as the
websocket message body (no {"type":"mcp","payload":{...}} wrapper needed -
see the "简化格式" branch in HandleMessage()) and forwards it straight to
McpServer::ParseMessage(), skipping the LLM/voice pipeline entirely. Tool
responses come back over the same connection (Application's MCP broadcast
callback re-broadcasts them to every connected control-server client).

The TOOLS registry below documents every self.otto.*/self.battery.* tool
currently registered in otto_controller.cc (RegisterMcpTools()) - keep it in
sync by hand if tools are added/renamed/removed there; `list` only prints
this registry, it doesn't query the device for it.

Requires: pip install websocket-client

Usage:
    python3 trigger_tictactoe.py <device-ip> [--port 8080]

Then, interactively:
    list                         - show every known tool, its params and defaults
    call <tool_name> [k=v ...]   - call ANY tool by name with arbitrary arguments
    action <name> [k=v ...]      - shortcut for self.otto.action
                                   e.g. action walk steps=5 speed=700 direction=1
    seq <sequence-json>          - shortcut for self.otto.servo_sequences
    trim <servo_type> <value>    - shortcut for self.otto.set_trim
    trims                        - self.otto.get_trims
    status                       - self.otto.get_status
    battery                      - self.battery.get_level
    ip                           - self.otto.get_ip
    stop                         - self.otto.stop (halt everything, then re-home)
    ttt start                    - self.otto.tictactoe_start
    ttt <1-9>                    - self.otto.tictactoe_move with that cell
    ttt stop                     - self.otto.tictactoe_stop
    quit / exit                  - close and exit
"""

import argparse
import json
import shlex
import threading
from typing import Optional

import websocket  # pip install websocket-client

# name -> (description, {param_name: (kind, default, extra)})
# kind is just for the `list` printout; every actual argument is sent as
# whatever Python type build_request()/parse_kv() produces (int/bool/str).
TOOLS = {
    "self.otto.action": (
        "Run a built-in movement. action picks which one; the rest are only "
        "used by actions that need them.",
        {
            "action": ("string", "sit",
                       "walk/turn/jump/swing/moonwalk/bend/shake_leg/updown/"
                       "whirlwind_leg/sit/showcase/home/hands_up/hands_down/"
                       "hand_wave/windmill/takeoff/fitness/greeting/shy/"
                       "radio_calisthenics/magic_circle"),
            "steps": ("int", 3, "1-100"),
            "speed": ("int", 700, "100-3000, smaller = faster"),
            "direction": ("int", 1, "-1..1"),
            "amount": ("int", 30, "0-170"),
            "arm_swing": ("int", 50, "0-170"),
        },
    ),
    "self.otto.servo_sequences": (
        "AI-programmed raw servo sequence (see otto_controller.cc for the full "
        "'s'/'osc' mode syntax) - use the `seq` shortcut instead of `call` for this one.",
        {"sequence": ("string", '{"a":[{"s":{"ll":90,"rl":90},"v":1000}]}', "sequence JSON")},
    ),
    "self.otto.stop": ("Immediately stop all motion and re-home.", {}),
    "self.otto.set_trim": (
        "Persist a per-servo trim offset.",
        {
            "servo_type": ("string", "left_leg",
                           "left_leg/right_leg/left_foot/right_foot/left_hand/right_hand"),
            "trim_value": ("int", 0, "-50..50"),
        },
    ),
    "self.otto.get_trims": ("Read back the current saved trims.", {}),
    "self.otto.get_status": ("Returns 'moving' or 'idle'.", {}),
    "self.battery.get_level": ("Returns battery level + charging state.", {}),
    "self.otto.get_ip": ("Returns the device's Wi-Fi IP.", {}),
    "self.otto.tictactoe_start": ("Start a new tic-tac-toe game.", {}),
    "self.otto.tictactoe_move": (
        "Play cell (1-9) as X, then Otto replies as O.",
        {"cell": ("int", 5, "1-9")},
    ),
    "self.otto.tictactoe_stop": ("End the current tic-tac-toe game.", {}),
}


def build_request(request_id: int, tool_name: str, arguments: Optional[dict] = None) -> str:
    return json.dumps({
        "jsonrpc": "2.0",
        "method": "tools/call",
        "params": {
            "name": tool_name,
            "arguments": arguments or {},
        },
        "id": request_id,
    })


def parse_value(raw: str):
    """Best-effort str -> int/bool/str, for the k=v arguments on the command line."""
    if raw.lower() in ("true", "false"):
        return raw.lower() == "true"
    try:
        return int(raw)
    except ValueError:
        return raw


def parse_kv_args(tokens) -> dict:
    args = {}
    for token in tokens:
        if "=" not in token:
            print(f"  (ignoring '{token}' - expected key=value)")
            continue
        key, _, value = token.partition("=")
        args[key] = parse_value(value)
    return args


def print_tool_list():
    for name, (description, params) in TOOLS.items():
        print(f"\n{name}")
        print(f"  {description}")
        for param_name, (kind, default, extra) in params.items():
            print(f"    {param_name}: {kind} (default {default!r}) - {extra}")
    print()


def on_message(ws, message):
    print(f"\n<< {message}")
    print("> ", end="", flush=True)


def on_error(ws, error):
    print(f"\n[error] {error}")


def on_close(ws, close_status_code, close_msg):
    print("\n[connection closed]")


def main():
    parser = argparse.ArgumentParser(description="Manually trigger Otto's MCP tools")
    parser.add_argument("host", help="Device IP address, e.g. 192.168.1.42")
    parser.add_argument("--port", type=int, default=8080,
                        help="WebSocket control server port (default: 8080)")
    args = parser.parse_args()

    url = f"ws://{args.host}:{args.port}/ws"
    print(f"Connecting to {url} ...")

    connected = threading.Event()

    def on_open(ws):
        connected.set()

    ws = websocket.WebSocketApp(
        url,
        on_open=on_open,
        on_message=on_message,
        on_error=on_error,
        on_close=on_close,
    )

    thread = threading.Thread(target=ws.run_forever, daemon=True)
    thread.start()

    # Don't claim "Connected" (or let the first send() race the handshake)
    # until on_open has actually fired.
    if not connected.wait(timeout=10):
        print("Failed to connect within 10s - check the IP/port and that the "
              "device's WebSocketControlServer is running.")
        return

    request_id = 1

    def send(tool_name, arguments=None):
        nonlocal request_id
        request = build_request(request_id, tool_name, arguments)
        request_id += 1
        # Printed immediately, separately from whatever on_message later
        # prints - makes it obvious whether a reply is just slow (the
        # response is scheduled onto the device's main task, then has to
        # round-trip back over Wi-Fi, so it's never instantaneous) versus
        # never arriving at all.
        print(f">> {request}")
        try:
            ws.send(request)
        except websocket.WebSocketException as e:
            print(f"[send failed] {e}")

    print("Connected. Type 'list' to see every available command, or 'quit' to exit.")

    try:
        while True:
            line = input("> ").strip()
            if not line:
                continue
            tokens = shlex.split(line)
            command, rest = tokens[0].lower(), tokens[1:]

            if command in ("quit", "exit"):
                break

            elif command == "list":
                print_tool_list()

            elif command == "call":
                if not rest:
                    print("Usage: call <tool_name> [key=value ...]")
                    continue
                send(rest[0], parse_kv_args(rest[1:]))

            elif command == "action":
                if not rest:
                    print("Usage: action <name> [key=value ...]")
                    continue
                arguments = {"action": rest[0]}
                arguments.update(parse_kv_args(rest[1:]))
                send("self.otto.action", arguments)

            elif command == "seq":
                if not rest:
                    print("Usage: seq <sequence-json>")
                    continue
                send("self.otto.servo_sequences", {"sequence": " ".join(rest)})

            elif command == "trim":
                if len(rest) != 2:
                    print("Usage: trim <servo_type> <value>")
                    continue
                send("self.otto.set_trim", {"servo_type": rest[0], "trim_value": int(rest[1])})

            elif command == "trims":
                send("self.otto.get_trims")

            elif command == "status":
                send("self.otto.get_status")

            elif command == "battery":
                send("self.battery.get_level")

            elif command == "ip":
                send("self.otto.get_ip")

            elif command == "stop":
                send("self.otto.stop")

            elif command == "ttt":
                if not rest:
                    print("Usage: ttt start | ttt <1-9> | ttt stop")
                    continue
                sub = rest[0].lower()
                if sub == "start":
                    send("self.otto.tictactoe_start")
                elif sub == "stop":
                    send("self.otto.tictactoe_stop")
                elif sub.isdigit() and 1 <= int(sub) <= 9:
                    send("self.otto.tictactoe_move", {"cell": int(sub)})
                else:
                    print("Usage: ttt start | ttt <1-9> | ttt stop")

            else:
                print(f"Unknown command '{command}'. Type 'list' to see what's available.")
    except (KeyboardInterrupt, EOFError):
        pass
    finally:
        ws.close()


if __name__ == "__main__":
    main()
