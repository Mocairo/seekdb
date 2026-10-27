"""Exercise the first AI-management CALL on a fresh Linux seekdb instance.

Uses the production server binary and test_ai_model_lifecycle.py at the default
10-second SQL deadline. Optional competing processes share only the server's
single pinned CPU. Every child is stopped; logs and the new data directory are
retained. Requires PyMySQL. Never point this at a production instance.
"""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import time

import pymysql


def stop(process):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=40)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            raise RuntimeError(f'child {process.pid} needed SIGKILL; inspect its logs')


def run_phase(args, phase, root, cpu):
    results = root / phase
    results.mkdir()
    command = [str(args.binary.resolve()), '--nodaemon',
               '--base-dir=' + str(root / 'data'), '--port=' + str(args.port),
               '--parameter', 'memory_budget=4G', '--parameter', 'cpu_count=1',
               '--parameter', 'datafile_size=1G', '--parameter', 'datafile_maxsize=2G',
               '--parameter', 'log_disk_size=2G']
    if args.sync_packages:
        command += ['--parameter', '_enable_async_load_sys_package=false']
    (results / 'command.json').write_text(json.dumps(command, indent=2))
    record = {'phase': phase, 'cpu': cpu, 'workers': args.workers}
    workers = []
    server = None
    started = time.monotonic()
    with (results / 'server-console.log').open('w') as console:
        try:
            server = subprocess.Popen(command, stdout=console, stderr=subprocess.STDOUT,
                                      preexec_fn=lambda: os.sched_setaffinity(0, {cpu}))
            record['server_pid'] = server.pid
            while time.monotonic() - started < 90:
                if server.poll() is not None:
                    raise RuntimeError(f'server exited: {server.returncode}')
                try:
                    # Connecting does not call a system package or prewarm it.
                    db = pymysql.connect(host='127.0.0.1', port=args.port, user='root',
                                         connect_timeout=1, read_timeout=5)
                    db.close()
                    break
                except pymysql.Error:
                    time.sleep(0.05)
            else:
                raise RuntimeError('server did not accept connections within 90 seconds')
            record['first_connection_seconds'] = round(time.monotonic() - started, 6)
            for _ in range(args.workers):
                workers.append(subprocess.Popen(
                    [sys.executable, '-c', 'while True: pass'],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                    preexec_fn=lambda: os.sched_setaffinity(0, {cpu})))
            # Explicit settings prevent inherited test credentials from selecting
            # another database server. This new instance uses the default root.
            env = dict(os.environ, SEEKDB_TEST_HOST='127.0.0.1',
                       SEEKDB_TEST_PORT=str(args.port), SEEKDB_TEST_USER='root',
                       SEEKDB_TEST_PASSWORD='')
            lifecycle = Path(__file__).with_name('test_ai_model_lifecycle.py')
            with (results / 'lifecycle.log').open('w') as output:
                completed = subprocess.run([sys.executable, str(lifecycle)], env=env,
                                           stdout=output, stderr=subprocess.STDOUT,
                                           timeout=90)
            record['lifecycle_exit'] = completed.returncode
        except Exception as error:
            record['error'] = str(error)
            record['lifecycle_exit'] = 1
        finally:
            # Always release the competing processes before stopping the server.
            for worker in workers:
                if worker.poll() is None:
                    worker.terminate()
            for worker in workers:
                worker.wait(timeout=10)
            if server is not None:
                try:
                    stop(server)
                except RuntimeError as error:
                    record['shutdown_error'] = str(error)
                    record['lifecycle_exit'] = 1
            record['elapsed_seconds'] = round(time.monotonic() - started, 6)
            (results / 'summary.json').write_text(json.dumps(record, indent=2))
    print(json.dumps(record), flush=True)
    print((results / 'lifecycle.log').read_text() if (results / 'lifecycle.log').exists()
          else 'No lifecycle log: inspect server-console.log', flush=True)
    return record['lifecycle_exit']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path,
                        help='new directory for isolated data and results; must not exist')
    parser.add_argument('--port', type=int, default=2885)
    parser.add_argument('--workers', type=int, default=0,
                        help='competing processes on the same CPU (20 reproduces the slow-start case)')
    parser.add_argument('--cpu', type=int, help='allowed CPU; defaults to the lowest allowed CPU')
    parser.add_argument('--restart', action='store_true', help='also test restart with persisted packages')
    parser.add_argument('--sync-packages', action='store_true',
                        help='also exercise bootstrap with asynchronous package loading disabled')
    args = parser.parse_args()
    if sys.platform != 'linux':
        parser.error('requires Linux CPU affinity support')
    if not args.binary.is_file() or not os.access(args.binary, os.X_OK):
        parser.error('--binary must be an executable seekdb binary')
    if not 0 <= args.workers <= 64 or not 1 <= args.port <= 65535:
        parser.error('workers must be 0..64 and port must be 1..65535')
    allowed = os.sched_getaffinity(0)
    cpu = min(allowed) if args.cpu is None else args.cpu
    if cpu not in allowed:
        parser.error('--cpu is outside the current allowed CPU set')
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', args.port))
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=False)
    result = run_phase(args, 'cold', root, cpu)
    if args.restart:
        result |= run_phase(args, 'restart', root, cpu)
    return 1 if result else 0


if __name__ == '__main__':
    sys.exit(main())
