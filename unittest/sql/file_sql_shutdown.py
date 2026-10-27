"""Stop an owned POSIX seekdb child and report observed shutdown facts.

The server handles SIGUSR1 as its normal stop request. Its legacy SIGTERM
handler sends SIGKILL to itself; a negative exit code alone proves no timeout.
"""
import signal
import subprocess
import time


def stop_owned_server(process, timeout=15.0, stop_signal=None):
    """Only accepts a Popen child, never a PID discovered on the host."""
    if stop_signal is None:
        stop_signal = signal.SIGUSR1
    started = time.monotonic()
    already_exited = process.poll() is not None
    timed_out = False
    forced_kill = False
    if not already_exited:
        try:
            process.send_signal(stop_signal)
        except ProcessLookupError:
            pass  # The owned child exited between poll() and send_signal().
        try:
            process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            forced_kill = True
            process.kill()
            process.wait(timeout=10)
    returncode = process.returncode
    return {
        'requested_signal': None if already_exited else signal.Signals(stop_signal).name,
        'elapsed_seconds': round(time.monotonic() - started, 6),
        'timeout_seconds': timeout,
        'timed_out': timed_out,
        'forced_kill_by_harness': forced_kill,
        'already_exited': already_exited,
        'exit_code': returncode,
        'exit_signal': signal.Signals(-returncode).name if returncode < 0 else None,
        'normal_exit': returncode == 0 and not timed_out and not forced_kill,
    }
