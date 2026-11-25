#!/usr/bin/env python3
"""Exercise the embedded CA against a real, isolated TLS listener."""
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
from pathlib import Path


def make_cert(directory, name):
    cert = directory / f"{name}.pem"
    key = directory / f"{name}.key"
    subprocess.run([
        "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
        "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost",
        "-addext", "basicConstraints=critical,CA:TRUE", "-keyout", str(key), "-out", str(cert),
    ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return cert, key


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: http_ca_blob_test.py PATH_TO_COMPILED_TEST")
    binary = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="streetless-ca-") as temp:
        path = Path(temp)
        trusted, key = make_cert(path, "trusted")
        untrusted, _ = make_cert(path, "untrusted")
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(trusted, key)
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listener.bind(("127.0.0.1", 0))
        listener.listen(2)
        listener.settimeout(15)
        port = str(listener.getsockname()[1])
        errors = []

        def serve():
            try:
                for _ in range(2):
                    conn, _ = listener.accept()
                    conn.settimeout(10)
                    try:
                        with context.wrap_socket(conn, server_side=True) as tls:
                            request = tls.recv(4096)
                            if not request.startswith(b"GET /healthz HTTP/1.1\r\n"):
                                raise AssertionError("unexpected HTTP request")
                            tls.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 3\r\nConnection: close\r\n\r\nok\n")
                    except ssl.SSLError:
                        # The untrusted-CA scenario aborts during TLS handshake.
                        pass
            except Exception as exc:
                errors.append(exc)
            finally:
                listener.close()

        thread = threading.Thread(target=serve)
        thread.start()
        try:
            subprocess.run([binary, str(trusted), port, "trusted"], check=True, timeout=15)
            subprocess.run([binary, str(untrusted), port, "untrusted"], check=True, timeout=15)
        finally:
            thread.join(timeout=20)
        if thread.is_alive() or errors:
            raise AssertionError(f"TLS server failed: {errors}")
    print("Embedded CA accepts the intended TLS server and rejects another CA")


if __name__ == "__main__":
    main()
