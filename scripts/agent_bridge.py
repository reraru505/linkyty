#!/usr/bin/env python3
"""
LinKyty Agent IPC Bridge (UNIX Domain Sockets)
Enables bi-directional communication between the Coordinator (Antigravity) and
the Implementor (external Pi agent).

Default socket: /tmp/linkyty_agent.sock
Log / bus file: /tmp/linkyty_agent_bus.jsonl
"""

import argparse
import json
import os
import select
import socket
import subprocess
import sys
import time
from typing import Any, Dict, List, Optional

SOCKET_PATH = os.environ.get("LINKYTY_IPC_SOCK", "/tmp/linkyty_agent.sock")
LOG_PATH = os.environ.get("LINKYTY_IPC_LOG", "/tmp/linkyty_agent_bus.jsonl")


def log_message(msg: Dict[str, Any]) -> None:
    try:
        with open(LOG_PATH, "a", encoding="utf-8") as f:
            f.write(json.dumps(msg) + "\n")
    except Exception:
        pass


class BridgeServer:
    def __init__(self, sock_path: str = SOCKET_PATH):
        self.sock_path = sock_path
        self.inbox: Dict[str, List[Dict[str, Any]]] = {"pi": [], "coordinator": []}
        self.waiters: Dict[str, List[socket.socket]] = {"pi": [], "coordinator": []}
        self.running = True

    def run(self):
        if os.path.exists(self.sock_path):
            try:
                test_s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                test_s.connect(self.sock_path)
                test_s.close()
                print(f"[BridgeServer] Already running on {self.sock_path}")
                return
            except OSError:
                os.remove(self.sock_path)

        server_sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        server_sock.bind(self.sock_path)
        server_sock.listen(16)
        print(f"[BridgeServer] Listening on {self.sock_path}")

        # Set permissions so any user/agent can access
        try:
            os.chmod(self.sock_path, 0o777)
        except OSError:
            pass

        epoll = select.epoll()
        epoll.register(server_sock.fileno(), select.EPOLLIN)
        connections: Dict[int, socket.socket] = {}
        buffers: Dict[int, bytearray] = {}

        try:
            while self.running:
                events = epoll.poll(0.5)
                for fd, event in events:
                    if fd == server_sock.fileno():
                        client_sock, _ = server_sock.accept()
                        client_sock.setblocking(False)
                        cfd = client_sock.fileno()
                        epoll.register(cfd, select.EPOLLIN)
                        connections[cfd] = client_sock
                        buffers[cfd] = bytearray()
                    elif event & select.EPOLLIN:
                        client_sock = connections[fd]
                        try:
                            data = client_sock.recv(65536)
                            if not data:
                                self._cleanup_client(fd, epoll, connections, buffers)
                                continue
                            buffers[fd].extend(data)
                            while b"\n" in buffers[fd]:
                                line, _, rest = buffers[fd].partition(b"\n")
                                buffers[fd] = bytearray(rest)
                                if line.strip():
                                    self._handle_request(client_sock, line.decode("utf-8", "replace"))
                        except (ConnectionResetError, BrokenPipeError):
                            self._cleanup_client(fd, epoll, connections, buffers)
                    elif event & (select.EPOLLHUP | select.EPOLLERR):
                        self._cleanup_client(fd, epoll, connections, buffers)
        finally:
            server_sock.close()
            if os.path.exists(self.sock_path):
                os.remove(self.sock_path)

    def _cleanup_client(self, fd: int, epoll, connections, buffers):
        if fd in connections:
            sock = connections[fd]
            for role in self.waiters:
                if sock in self.waiters[role]:
                    self.waiters[role].remove(sock)
            try:
                epoll.unregister(fd)
            except Exception:
                pass
            try:
                sock.close()
            except Exception:
                pass
            del connections[fd]
        if fd in buffers:
            del buffers[fd]

    def _handle_request(self, client_sock: socket.socket, raw_line: str):
        try:
            req = json.loads(raw_line)
        except json.JSONDecodeError as e:
            self._send_reply(client_sock, {"status": "error", "error": f"Invalid JSON: {e}"})
            return

        action = req.get("action")

        if action == "send":
            sender = req.get("from", "unknown")
            to = req.get("to", "coordinator" if sender == "pi" else "pi")
            content = req.get("content", "")
            msg_type = req.get("type", "message")
            metadata = req.get("metadata", {})

            packet = {
                "id": int(time.time() * 1000),
                "timestamp": time.time(),
                "from": sender,
                "to": to,
                "type": msg_type,
                "content": content,
                "metadata": metadata,
            }
            log_message(packet)

            # Check if anyone is waiting
            if self.waiters.get(to):
                waiter_sock = self.waiters[to].pop(0)
                try:
                    self._send_reply(waiter_sock, {"status": "ok", "messages": [packet]})
                except Exception:
                    self.inbox.setdefault(to, []).append(packet)
            else:
                self.inbox.setdefault(to, []).append(packet)

            self._send_reply(client_sock, {"status": "ok", "delivered": True, "id": packet["id"]})

        elif action == "recv":
            for_role = req.get("for", "pi")
            wait = req.get("wait", False)

            if self.inbox.get(for_role):
                msgs = self.inbox[for_role]
                self.inbox[for_role] = []
                self._send_reply(client_sock, {"status": "ok", "messages": msgs})
            elif wait:
                self.waiters.setdefault(for_role, []).append(client_sock)
            else:
                self._send_reply(client_sock, {"status": "ok", "messages": []})

        elif action == "status":
            self._send_reply(
                client_sock,
                {
                    "status": "ok",
                    "inbox_counts": {k: len(v) for k, v in self.inbox.items()},
                    "waiter_counts": {k: len(v) for k, v in self.waiters.items()},
                },
            )

        elif action == "clear":
            self.inbox = {"pi": [], "coordinator": []}
            self._send_reply(client_sock, {"status": "ok", "cleared": True})

        else:
            self._send_reply(client_sock, {"status": "error", "error": f"Unknown action: {action}"})

    def _send_reply(self, sock: socket.socket, reply: Dict[str, Any]):
        try:
            sock.sendall((json.dumps(reply) + "\n").encode("utf-8"))
        except Exception:
            pass


def ensure_server_running(sock_path: str = SOCKET_PATH):
    if os.path.exists(sock_path):
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(sock_path)
            s.close()
            return
        except OSError:
            try:
                os.remove(sock_path)
            except OSError:
                pass

    # Launch daemon in background
    cmd = [sys.executable, os.path.abspath(__file__), "server"]
    subprocess.Popen(
        cmd,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        stdin=subprocess.DEVNULL,
        start_new_session=True,
    )
    # Wait up to 2 seconds for server to be responsive
    for _ in range(20):
        time.sleep(0.1)
        if os.path.exists(sock_path):
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(sock_path)
                s.close()
                return
            except OSError:
                pass


def send_rpc(req: Dict[str, Any], sock_path: str = SOCKET_PATH, timeout: Optional[float] = 10.0) -> Dict[str, Any]:
    ensure_server_running(sock_path)
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    if timeout:
        s.settimeout(timeout)
    s.connect(sock_path)
    s.sendall((json.dumps(req) + "\n").encode("utf-8"))

    buf = bytearray()
    while True:
        chunk = s.recv(4096)
        if not chunk:
            break
        buf.extend(chunk)
        if b"\n" in buf:
            line, _, _ = buf.partition(b"\n")
            s.close()
            return json.loads(line.decode("utf-8"))
    s.close()
    return {"status": "error", "error": "No response"}


# Public helper functions
def send_msg(sender: str, to: str, content: str, msg_type: str = "message", metadata: Optional[dict] = None) -> bool:
    resp = send_rpc(
        {
            "action": "send",
            "from": sender,
            "to": to,
            "content": content,
            "type": msg_type,
            "metadata": metadata or {},
        }
    )
    return resp.get("status") == "ok"


def recv_msgs(for_role: str, wait: bool = False, timeout: Optional[float] = 60.0) -> List[Dict[str, Any]]:
    resp = send_rpc(
        {"action": "recv", "for": for_role, "wait": wait},
        timeout=timeout if wait else 5.0,
    )
    return resp.get("messages", [])


def main():
    parser = argparse.ArgumentParser(description="LinKyty Agent IPC Bridge (UDS)")
    subparsers = parser.add_subparsers(dest="command")

    # server
    subparsers.add_parser("server", help="Run the socket server broker")

    # send
    send_p = subparsers.add_parser("send", help="Send a message")
    send_p.add_argument("--from", dest="from_role", default="coordinator", help="Sender role (coordinator|pi)")
    send_p.add_argument("--to", dest="to_role", default=None, help="Recipient role (pi|coordinator)")
    send_p.add_argument("--type", dest="msg_type", default="task", help="Message type (task|status|result|chat)")
    send_p.add_argument("content", nargs="?", default="", help="Message text / prompt")

    # recv
    recv_p = subparsers.add_parser("recv", help="Receive messages")
    recv_p.add_argument("--for", dest="for_role", default="coordinator", help="Role checking inbox (coordinator|pi)")
    recv_p.add_argument("--wait", action="store_true", help="Block until a message arrives")
    recv_p.add_argument("--timeout", type=float, default=60.0, help="Wait timeout in seconds")

    # status
    subparsers.add_parser("status", help="Show bridge status & queued message counts")

    # clear
    subparsers.add_parser("clear", help="Clear all pending messages")

    # log
    subparsers.add_parser("log", help="Display full audit log of messages")

    args = parser.parse_args()

    if args.command == "server":
        server = BridgeServer()
        server.run()
    elif args.command == "send":
        content = args.content
        if not content and not sys.stdin.isatty():
            content = sys.stdin.read().strip()
        to_role = args.to_role or ("pi" if args.from_role == "coordinator" else "coordinator")
        ok = send_msg(args.from_role, to_role, content, msg_type=args.msg_type)
        if ok:
            print(f"[Bridge] Message sent from '{args.from_role}' to '{to_role}'.")
        else:
            print("[Bridge] Failed to send message.", file=sys.stderr)
            sys.exit(1)
    elif args.command == "recv":
        try:
            msgs = recv_msgs(args.for_role, wait=args.wait, timeout=args.timeout)
            if not msgs:
                if not args.wait:
                    print(f"[Bridge] No pending messages for '{args.for_role}'.")
            else:
                for m in msgs:
                    print(json.dumps(m, indent=2))
        except socket.timeout:
            print(f"[Bridge] Timeout waiting for messages for '{args.for_role}'.", file=sys.stderr)
            sys.exit(2)
    elif args.command == "status":
        resp = send_rpc({"action": "status"})
        print(json.dumps(resp, indent=2))
    elif args.command == "clear":
        resp = send_rpc({"action": "clear"})
        print(json.dumps(resp, indent=2))
    elif args.command == "log":
        if os.path.exists(LOG_PATH):
            with open(LOG_PATH, "r", encoding="utf-8") as f:
                print(f.read())
        else:
            print(f"[Bridge] No log file at {LOG_PATH}")
    else:
        parser.print_help()


if __name__ == "__main__":
    main()
