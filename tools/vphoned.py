#!/usr/bin/env python3
"""Talk to vphoned (vphone-cli's guest daemon, vsock port 1339) through hengevm's vsock socket.

  tools/vphoned.py GET /v1/health
  tools/vphoned.py POST /v1/rpc '{"method":"screen.unlock","params":{},"id":1}'

The socket is $HV_VSOCK_DIR/3.sock (default /tmp/hengevm-vsock/3.sock): write "CONNECT 1339",
read "OK <port>", then plain HTTP. The response body goes to stdout.
"""
import os
import socket
import sys


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    method, path = sys.argv[1], sys.argv[2]
    body = sys.argv[3].encode() if len(sys.argv) > 3 else b""
    path_sock = os.path.join(os.environ.get("HV_VSOCK_DIR", "/tmp/hengevm-vsock"), "3.sock")
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(float(os.environ.get("VPHONED_TIMEOUT", "120")))
    s.connect(path_sock)
    s.sendall(b"CONNECT 1339\n")
    line = b""
    while not line.endswith(b"\n"):
        c = s.recv(1)
        if not c:
            sys.exit("vphoned.py: the connection closed before OK")
        line += c
    if not line.startswith(b"OK"):
        sys.exit("vphoned.py: " + line.decode(errors="replace").strip())
    head = f"{method} {path} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n"
    if body:
        head += f"Content-Type: application/json\r\nContent-Length: {len(body)}\r\n"
    s.sendall(head.encode() + b"\r\n" + body)
    data = b""
    while True:
        try:
            chunk = s.recv(65536)
        except socket.timeout:
            break
        if not chunk:
            break
        data += chunk
    _, _, payload = data.partition(b"\r\n\r\n")
    sys.stdout.buffer.write(payload or data)
    sys.stdout.write("\n")


if __name__ == "__main__":
    main()
