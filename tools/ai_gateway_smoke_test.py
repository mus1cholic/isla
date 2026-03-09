#!/usr/bin/env python3
"""Phase 2.5 AI gateway WebSocket smoke test.

This script assumes a gateway server is already running and validates:
 - session.start -> session.started
 - text.input -> text.output -> turn.completed
 - text.input + turn.cancel -> turn.cancelled
 - session.end -> session.ended
"""

from __future__ import annotations

import argparse
import asyncio
import json
import sys
from typing import Any

try:
    import websockets
except ImportError as exc:  # pragma: no cover - runtime dependency check
    print(
        "Missing dependency: websockets\n"
        "Install it with: python -m pip install websockets",
        file=sys.stderr,
    )
    raise SystemExit(2) from exc


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1", help="Gateway host")
    parser.add_argument("--port", type=int, default=8080, help="Gateway port")
    parser.add_argument("--path", default="/", help="WebSocket path")
    parser.add_argument(
        "--timeout-seconds",
        type=float,
        default=2.0,
        help="Per-frame receive timeout in seconds",
    )
    parser.add_argument(
        "--normal-text",
        default="hello gateway",
        help="Text payload to use for the normal completion flow",
    )
    parser.add_argument(
        "--cancel-text",
        default="cancel me",
        help="Text payload to use for the cancellation flow",
    )
    parser.add_argument(
        "--response-prefix",
        default="stub echo: ",
        help="Expected stub responder text prefix",
    )
    return parser.parse_args()


def make_url(host: str, port: int, path: str) -> str:
    normalized_path = path if path.startswith("/") else f"/{path}"
    return f"ws://{host}:{port}{normalized_path}"


async def send_json(socket: Any, payload: dict[str, Any]) -> None:
    await socket.send(json.dumps(payload))


async def recv_json(socket: Any, timeout_seconds: float) -> dict[str, Any]:
    raw_message = await asyncio.wait_for(socket.recv(), timeout=timeout_seconds)
    if not isinstance(raw_message, str):
        raise AssertionError(f"expected text frame, got {type(raw_message).__name__}")
    parsed = json.loads(raw_message)
    if not isinstance(parsed, dict):
        raise AssertionError(f"expected JSON object, got {type(parsed).__name__}")
    return parsed


def require_message_type(message: dict[str, Any], expected_type: str) -> None:
    actual_type = message.get("type")
    if actual_type != expected_type:
        raise AssertionError(
            f"expected message type {expected_type!r}, got {actual_type!r}: {message}"
        )


async def run_smoke_test(args: argparse.Namespace) -> None:
    url = make_url(args.host, args.port, args.path)
    print(f"Connecting to {url}")

    async with websockets.connect(url, open_timeout=args.timeout_seconds) as socket:
        print("Starting session")
        await send_json(socket, {"type": "session.start"})
        started = await recv_json(socket, args.timeout_seconds)
        require_message_type(started, "session.started")
        session_id = started.get("session_id")
        if not isinstance(session_id, str) or not session_id:
            raise AssertionError(f"session.started missing session_id: {started}")
        print(f"Session started: {session_id}")

        print("Testing normal text turn")
        await send_json(
            socket,
            {
                "type": "text.input",
                "turn_id": "turn_1",
                "text": args.normal_text,
            },
        )
        text_output = await recv_json(socket, args.timeout_seconds)
        require_message_type(text_output, "text.output")
        expected_reply = f"{args.response_prefix}{args.normal_text}"
        if text_output.get("turn_id") != "turn_1" or text_output.get("text") != expected_reply:
            raise AssertionError(
                "unexpected text.output payload: "
                f"expected turn_id='turn_1' text={expected_reply!r}, got {text_output}"
            )

        turn_completed = await recv_json(socket, args.timeout_seconds)
        require_message_type(turn_completed, "turn.completed")
        if turn_completed.get("turn_id") != "turn_1":
            raise AssertionError(f"unexpected turn.completed payload: {turn_completed}")
        print("Normal text turn passed")

        print("Testing cancellation flow")
        await send_json(
            socket,
            {
                "type": "text.input",
                "turn_id": "turn_2",
                "text": args.cancel_text,
            },
        )
        await send_json(socket, {"type": "turn.cancel", "turn_id": "turn_2"})
        turn_cancelled = await recv_json(socket, args.timeout_seconds)
        require_message_type(turn_cancelled, "turn.cancelled")
        if turn_cancelled.get("turn_id") != "turn_2":
            raise AssertionError(f"unexpected turn.cancelled payload: {turn_cancelled}")
        print("Cancellation flow passed")

        print("Ending session")
        await send_json(socket, {"type": "session.end", "session_id": session_id})
        session_ended = await recv_json(socket, args.timeout_seconds)
        require_message_type(session_ended, "session.ended")
        if session_ended.get("session_id") != session_id:
            raise AssertionError(f"unexpected session.ended payload: {session_ended}")
        print("Session end passed")

    print("Smoke test passed")


def main() -> int:
    args = parse_args()
    try:
        asyncio.run(run_smoke_test(args))
    except KeyboardInterrupt:
        return 130
    except Exception as exc:
        print(f"Smoke test failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
