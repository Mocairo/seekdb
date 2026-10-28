"""Restart the dedicated test instance to verify durable leases and publication.

Only port 28751 and restart_local_instance.ps1's verified process manifest are
supported. This deliberately terminates that dedicated test process three times.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys

import pymysql
from ai_column_test_support import FixtureMock
from test_ai_column_worker_live import wait_until


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=28751, choices=(28751,))
    parser.add_argument("--staged-only", action="store_true")
    parser.add_argument("--keep-on-failure", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    options = dict(host="127.0.0.1", port=args.port, user="root", charset="utf8mb4",
                   autocommit=True, connect_timeout=2, read_timeout=15)
    database, model, endpoint = "ai_column_recovery_live", "recovery_live_model", "recovery_live_ep"
    connection = pymysql.connect(**options)
    abandoned = None

    def reconnect():
        nonlocal connection
        try:
            fresh = pymysql.connect(**options)
            with fresh.cursor() as q:
                q.execute(f"USE {database}")
            connection = fresh
            return True
        except pymysql.MySQLError as error:
            if error.args[0] not in (2003, 2013, 4012, 4624, 5001):
                raise
            return False

    def one(sql, params=()):
        with connection.cursor() as q:
            q.execute(sql, params)
            return q.fetchone()

    def execute(sql, params=()):
        with connection.cursor() as q:
            q.execute(sql, params)

    def restart():
        # A native service can inherit a subprocess PIPE on Windows even with
        # redirected console output. A file keeps communicate() independent of
        # the lifetime of the restarted server.
        output_path = root / "build_ai_column_tests/recovery-restart.log"
        with output_path.open("w", encoding="utf8") as output:
            result = subprocess.run(["pwsh", "-NoProfile", "-File", str(root / "unittest/share/ai_column/restart_local_instance.ps1"), "-LogLevel", "WDIAG"],
                                    stdout=output, stderr=subprocess.STDOUT, timeout=30, cwd=root)
        if result.returncode:
            raise RuntimeError("Guarded restart failed: " + output_path.read_text(encoding="utf8"))
        try:
            connection.close()
        except pymysql.MySQLError:
            pass
        wait_until("dedicated instance recovery", reconnect, timeout=30)

    def no_tasks():
        return one("SELECT COUNT(*) FROM oceanbase.__all_ai_column_task task JOIN oceanbase.__all_table t "
                   "ON t.table_id=task.table_id JOIN oceanbase.__all_database d ON d.database_id=t.database_id "
                   "WHERE d.database_name=%s", (database,))[0] == 0

    with FixtureMock(root, 19125) as mock:
        try:
            execute(f"DROP DATABASE IF EXISTS {database}")
            execute(f"CREATE DATABASE {database}")
            execute(f"USE {database}")
            for routine, name, absent in (("DROP_AI_MODEL_ENDPOINT", endpoint, 11112), ("DROP_AI_MODEL", model, 11118)):
                try:
                    execute(f"CALL DBMS_AI_SERVICE.{routine}(%s)", (name,))
                except pymysql.MySQLError as error:
                    if error.args[0] != absent:
                        raise
            execute("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL(%s,%s)",
                    (model, json.dumps(dict(type="completion", model_name="fixture-model"))))
            execute("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)", (endpoint, json.dumps(dict(
                ai_model_name=model, provider="openai", access_key="fixture-key", request_model_name="fixture-model",
                url="http://127.0.0.1:19125/v1/chat/completions"))))
            execute(f"CREATE TABLE docs(id BIGINT PRIMARY KEY,content VARCHAR(100),"
                    f"result VARCHAR(400) AI COLUMN(AI_COMPLETE('{model}',content)) WITH(max_retries=2))")
            if not args.staged_only:
                execute("INSERT INTO docs(id,content) VALUES(1,'slow-old-recovery')")
                wait_until("request accepted before interruption", lambda: mock.accepted == 1)
                abandoned = pymysql.connect(**options, database=database)
                abandoned.begin()
                with abandoned.cursor() as q:
                    q.execute("INSERT INTO docs(id,content) VALUES(2,'must-rollback-on-crash')")
                print("STAGE: request accepted; killing the dedicated instance with one uncommitted insert", flush=True)
                restart()
                assert one("SELECT COUNT(*) FROM docs WHERE id=2")[0] == 0
                wait_until("expired lease reclaimed", lambda: one("SELECT result='mock:slow-old-recovery' FROM docs WHERE id=1")[0], timeout=55)
                wait_until("recovered group cleaned", no_tasks)
                assert mock.accepted == 2, f"Unexpected upstream call count after response loss: {mock.accepted}"
                print("PASS: crash rollback makes no request; expired lease recovers with exactly one duplicate inference", flush=True)

                execute(f"CREATE TABLE exhausted(id BIGINT PRIMARY KEY,content VARCHAR(100),"
                        f"result VARCHAR(400) AI COLUMN(AI_COMPLETE('{model}',content)) WITH(max_retries=0))")
                execute("INSERT INTO exhausted(id,content) VALUES(1,'slow-old-exhausted')")
                wait_until("zero-budget request accepted", lambda: mock.accepted == 3)
                print("STAGE: interrupting a zero-retry task after upstream acceptance", flush=True)
                restart()
                wait_until("exhausted crash budget published", no_tasks, timeout=55)
                assert mock.accepted == 3 and one("SELECT result IS NULL FROM exhausted WHERE id=1")[0] == 1
                print("PASS: persisted attempt budget prevents an extra model call after a crash", flush=True)

            before = mock.accepted
            execute("INSERT INTO docs(id,content) VALUES(3,'staged-before-crash')")
            blocker = pymysql.connect(**options, database=database)
            blocker.begin()
            with blocker.cursor() as q:
                q.execute("SELECT id FROM docs WHERE id=3 FOR UPDATE")
            wait_until("durable result staged behind the row lock", lambda: one(
                "SELECT COUNT(*) FROM oceanbase.__all_ai_column_task WHERE state=2 AND rowkey_predicate='`id` = 3'")[0] == 1)
            staged = one("SELECT result_value IS NULL,CAST(result_value AS CHAR) "
                         "FROM oceanbase.__all_ai_column_task WHERE state=2 AND rowkey_predicate='`id` = 3'")
            assert staged[0] == 0 and staged[1], staged
            print("STAGE: validated non-NULL durable SQL result", flush=True)
            assert mock.accepted == before + 1
            print("STAGE: result durably staged; interrupting before its blocked publication", flush=True)
            restart()
            try:
                blocker.close()
            except pymysql.MySQLError:
                pass
            try:
                wait_until("staged result published after restart", lambda: one("SELECT result='mock:staged-before-crash' FROM docs WHERE id=3")[0])
            except AssertionError:
                with connection.cursor() as q:
                    q.execute("SELECT task_id,state,attempt_count,claim_epoch,CAST(result_value AS CHAR) "
                              "FROM oceanbase.__all_ai_column_task WHERE rowkey_predicate='`id` = 3'")
                    print("Recovery tasks:", q.fetchall(), flush=True)
                print("Row:", one("SELECT id,content,result FROM docs WHERE id=3"), flush=True)
                raise
            wait_until("staged group cleaned", no_tasks)
            assert mock.accepted == before + 1, "Publication recovery repeated a completed inference"
            print("PASS: staged results publish after restart without another upstream call", flush=True)
        finally:
            if args.keep_on_failure and sys.exc_info()[0] is not None:
                print("Preserving the dedicated recovery database for diagnosis", flush=True)
                connection.close()
                raise
            if abandoned is not None:
                try:
                    abandoned.close()
                except pymysql.MySQLError:
                    pass
            execute(f"DROP DATABASE IF EXISTS {database}")
            for routine, name, absent in (("DROP_AI_MODEL_ENDPOINT", endpoint, 11112), ("DROP_AI_MODEL", model, 11118)):
                try:
                    execute(f"CALL DBMS_AI_SERVICE.{routine}(%s)", (name,))
                except pymysql.MySQLError as error:
                    if error.args[0] != absent:
                        raise
            connection.close()


if __name__ == "__main__":
    main()
