"""Regression tests distinguish a signal exit from an actual stop timeout."""
import signal
import subprocess
import sys
import unittest

from file_sql_shutdown import stop_owned_server


@unittest.skipUnless(hasattr(signal, 'SIGUSR1'), 'POSIX signals required')
class ShutdownReportingTest(unittest.TestCase):
    def child(self, behavior):
        process = subprocess.Popen(
            [sys.executable, '-u', '-c',
             'import os,signal,sys\n' + behavior +
             '\nprint("ready",flush=True)\nwhile True: signal.pause()\n'],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

        def cleanup():
            if process.poll() is None:
                process.kill()
            process.wait(timeout=5)
            process.stdout.close()
            process.stderr.close()

        self.addCleanup(cleanup)
        self.assertEqual('ready', process.stdout.readline().strip())
        return process

    def test_normal_stop_has_zero_exit_without_escalation(self):
        process = self.child('signal.signal(signal.SIGUSR1,lambda *_: sys.exit(0))')
        result = stop_owned_server(process, timeout=2)
        self.assertTrue(result['normal_exit'])
        self.assertEqual(0, result['exit_code'])
        self.assertFalse(result['timed_out'])
        self.assertFalse(result['forced_kill_by_harness'])

    def test_self_sigkill_is_not_reported_as_timeout(self):
        process = self.child(
            'signal.signal(signal.SIGTERM,lambda *_: os.kill(os.getpid(),signal.SIGKILL))')
        result = stop_owned_server(process, timeout=2, stop_signal=signal.SIGTERM)
        self.assertEqual(-signal.SIGKILL, result['exit_code'])
        self.assertEqual('SIGKILL', result['exit_signal'])
        self.assertFalse(result['timed_out'])
        self.assertFalse(result['forced_kill_by_harness'])
        self.assertFalse(result['normal_exit'])

    def test_real_timeout_records_forced_cleanup(self):
        process = self.child('signal.signal(signal.SIGUSR1,signal.SIG_IGN)')
        result = stop_owned_server(process, timeout=0.15)
        self.assertTrue(result['timed_out'])
        self.assertTrue(result['forced_kill_by_harness'])
        self.assertEqual(-signal.SIGKILL, result['exit_code'])
        self.assertFalse(result['normal_exit'])

    def test_already_failed_child_keeps_actual_exit_code(self):
        process = subprocess.Popen([sys.executable, '-c', 'raise SystemExit(7)'])
        process.wait(timeout=5)
        result = stop_owned_server(process)
        self.assertTrue(result['already_exited'])
        self.assertIsNone(result['requested_signal'])
        self.assertEqual(7, result['exit_code'])
        self.assertFalse(result['normal_exit'])
        self.assertFalse(result['timed_out'])


if __name__ == '__main__':
    unittest.main(verbosity=2)
