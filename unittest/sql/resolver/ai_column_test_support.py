"""Portable lifecycle for the supplied deterministic AI COLUMN Mock handler."""
import importlib.util
from pathlib import Path
import threading
from http.server import ThreadingHTTPServer


class FixtureMock:
    def __init__(self, root: Path, port: int):
        self.port = port
        self.accepted = 0
        self.active = 0
        self.peak_active = 0
        self.response_status = None
        self.response_body = None
        self._lock = threading.Lock()
        self._ready = threading.Event()
        self._server = None
        self._errors = []
        path = root / "tools/deploy/mysql_test/test_suite/ai_column/include/ai_column_mock.py"
        spec = importlib.util.spec_from_file_location(f"ai_column_mock_{port}", path)
        self.module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.module)
        state = root / "build_ai_column_tests" / f"mock-{port}"
        state.mkdir(parents=True, exist_ok=True)
        (state / "events.jsonl").write_text("", encoding="utf-8")
        self.module.state_dir = lambda _: str(state)

    def __enter__(self):
        owner = self

        def factory(address, handler):
            class ObservedHandler(handler):
                def do_POST(self):
                    with owner._lock:
                        owner.accepted += 1
                        owner.active += 1
                        owner.peak_active = max(owner.peak_active, owner.active)
                    try:
                        self._serve_fixture()
                    finally:
                        with owner._lock:
                            owner.active -= 1

                def _serve_fixture(self):
                    if owner.response_status is not None or owner.response_body is not None:
                        self.rfile.read(int(self.headers.get("Content-Length", "0")))
                        response = owner.response_body if owner.response_body is not None else b'{"error":{"message":"injected test outage"}}'
                        self.send_response(owner.response_status if owner.response_status is not None else 200)
                        self.send_header("Content-Type", "application/json")
                        self.send_header("Content-Length", str(len(response)))
                        self.send_header("Connection", "close")
                        self.end_headers()
                        self.wfile.write(response)
                        self.close_connection = True
                        return
                    try:
                        super().do_POST()
                    except (BrokenPipeError, ConnectionResetError):
                        # A recovery test deliberately terminates the SQL client
                        # after the upstream has accepted its inference request.
                        self.close_connection = True

            self._server = ThreadingHTTPServer(address, ObservedHandler)
            self._server.daemon_threads = True
            self._ready.set()
            return self._server

        def serve():
            try:
                self.module.serve(self.port)
            except Exception as error:
                self._errors.append(error)
                self._ready.set()

        self.module.ThreadingHTTPServer = factory
        self._thread = threading.Thread(target=serve, daemon=True)
        self._thread.start()
        if not self._ready.wait(5) or self._errors:
            raise RuntimeError(f"Mock could not start: {self._errors}")
        return self

    def __exit__(self, *ignored):
        if self._server:
            self._server.shutdown()
            self._server.server_close()
        self._thread.join(5)

    def events(self):
        return self.module.read_events(self.port)
