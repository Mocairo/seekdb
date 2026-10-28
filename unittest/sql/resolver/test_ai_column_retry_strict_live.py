"""Real SQL regression for target-only retry and column-wide strict reads."""
import argparse
import json
from pathlib import Path

import pymysql
from ai_column_test_support import FixtureMock
from test_ai_column_worker_live import wait_until
from test_ai_column_ddl_live import assert_rejected


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=2881)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    database, model, endpoint = "ai_column_retry_live", "retry_live_model", "retry_live_ep"
    connection = pymysql.connect(host="127.0.0.1", port=args.port, user="root", charset="utf8mb4",
                                 autocommit=True, read_timeout=30)
    with FixtureMock(root, 19124) as mock, connection.cursor() as q:
        try:
            q.execute(f"DROP DATABASE IF EXISTS {database}")
            q.execute(f"CREATE DATABASE {database}")
            q.execute(f"USE {database}")
            for routine, name, absent in (("DROP_AI_MODEL_ENDPOINT", endpoint, 11112), ("DROP_AI_MODEL", model, 11118)):
                try:
                    q.execute(f"CALL DBMS_AI_SERVICE.{routine}(%s)", (name,))
                except pymysql.MySQLError as error:
                    if error.args[0] != absent:
                        raise
            q.execute("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL(%s,%s)",
                      (model, json.dumps(dict(type="completion", model_name="fixture-model"))))
            q.execute("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)", (endpoint, json.dumps(dict(
                ai_model_name=model, provider="openai", access_key="fixture-key",
                url="http://127.0.0.1:19124/v1/chat/completions", request_model_name="fixture-model"))))
            q.execute(f"""CREATE TABLE docs(id BIGINT PRIMARY KEY, category VARCHAR(20), status VARCHAR(20),
                content VARCHAR(100), result VARCHAR(400) AI COLUMN(AI_COMPLETE('{model}',content)) WITH(max_retries=0),
                label VARCHAR(400) AI COLUMN(AI_COMPLETE('{model}',CONCAT('Label: ',content))) WITH(max_retries=0))""")

            def one(sql, params=()):
                q.execute(sql, params)
                return q.fetchone()

            def tasks_empty():
                return one("SELECT COUNT(*) FROM oceanbase.__all_ai_column_task task JOIN oceanbase.__all_table t "
                           "ON t.table_id=task.table_id JOIN oceanbase.__all_database d ON d.database_id=t.database_id "
                           "WHERE d.database_name=%s", (database,))[0] == 0

            q.execute("INSERT INTO docs(id,category,status,content) VALUES(1,'news','active','one'),"
                      "(2,'sports','active','two'),(3,'news','inactive','three')")
            wait_until("initial results ready", lambda: one("SELECT COUNT(*) FROM docs WHERE result IS NOT NULL AND label IS NOT NULL")[0] == 3)
            assert one("SELECT @@ai_column_strict_read")[0] == 0
            # Warm a plan while strictness is OFF. Its references must survive cache reuse.
            assert one("SELECT result FROM docs WHERE id=2")[0] == "mock:two"
            mock.response_status = 503
            q.execute("ALTER TABLE docs RETRY AI COLUMN result WHERE id IN (1,3)")
            q.execute("SELECT id,result IS NULL,label IS NOT NULL FROM docs ORDER BY id")
            assert q.fetchall() == ((1, 1, 1), (2, 0, 1), (3, 1, 1))
            q.execute("SET SESSION ai_column_strict_read=ON")
            assert one("SELECT COUNT(*) FROM docs WHERE label IS NOT NULL")[0] == 3
            assert one("SELECT COUNT(*) FROM docs")[0] == 3
            for sql in (
                "SELECT result FROM docs WHERE id=2",
                "SELECT result FROM docs WHERE 0",
                "SELECT * FROM docs WHERE id=999",
                "SELECT id FROM docs WHERE result IS NULL",
                "SELECT id FROM docs WHERE id IN (SELECT id FROM docs WHERE result IS NULL)",
                "UPDATE docs SET category=category WHERE result IS NULL",
                "DELETE FROM docs WHERE result IS NULL",
            ):
                assert_rejected(q, sql, 11120)
            assert one("SELECT COUNT(*) FROM docs")[0] == 3
            q.execute("SET SESSION ai_column_strict_read=OFF")
            wait_until("failed tasks cleaned", tasks_empty)
            q.execute("SET SESSION ai_column_strict_read=ON")
            assert_rejected(q, "SELECT result FROM docs WHERE id=1", 11120)
            q.execute("SET SESSION ai_column_strict_read=OFF")
            print("PASS: strict default, cached plans, column scope, empty filters, subqueries and write errors", flush=True)

            q.execute("ALTER TABLE docs RETRY AI COLUMN result WHERE status='FAILED' AND category='news'")
            wait_until("failed-only retry cleaned", tasks_empty)
            mock.response_status = None
            q.execute("ALTER TABLE docs RETRY AI COLUMN result")
            wait_until("full retry ready", lambda: one("SELECT COUNT(*) FROM docs WHERE result IS NOT NULL")[0] == 3)
            wait_until("full retry cleaned", tasks_empty)
            q.execute("SET SESSION ai_column_strict_read=ON")
            assert one("SELECT COUNT(*) FROM docs WHERE result IS NOT NULL")[0] == 3
            q.execute("SET SESSION ai_column_strict_read=OFF")
            before = mock.accepted
            q.execute("ALTER TABLE docs RETRY AI COLUMN result WHERE status='READY' AND id=999")
            assert tasks_empty() and mock.accepted == before
            mock.response_status = 503
            q.execute("ALTER TABLE docs RETRY AI COLUMN result WHERE status='READY' AND category='sports'")
            q.execute("SELECT id,result IS NULL,label IS NOT NULL FROM docs ORDER BY id")
            assert q.fetchall() == ((1, 0, 1), (2, 1, 1), (3, 0, 1))
            wait_until("READY retry cleaned", tasks_empty)
            mock.response_status = None
            q.execute("ALTER TABLE docs RETRY AI COLUMN result")
            wait_until("restored ready", lambda: one("SELECT COUNT(*) FROM docs WHERE result IS NOT NULL")[0] == 3)
            mock.response_status = 503
            q.execute("ALTER TABLE docs RETRY AI COLUMN result WHERE `status`='active' AND id=1")
            q.execute("SELECT id,result IS NULL FROM docs ORDER BY id")
            assert q.fetchall() == ((1, 1), (2, 0), (3, 0))
            assert_rejected(q, "ALTER TABLE docs RETRY AI COLUMN content WHERE id=1")
            assert_rejected(q, "ALTER TABLE docs RETRY AI COLUMN result WHERE id=1, ADD COLUMN extra INT")
            print("PASS: FAILED/READY pseudo status, business status, zero matches, target-only retry and invalid syntax", flush=True)
        finally:
            q.execute("SET SESSION ai_column_strict_read=OFF")
            q.execute(f"DROP DATABASE IF EXISTS {database}")
            for routine, name, absent in (("DROP_AI_MODEL_ENDPOINT", endpoint, 11112), ("DROP_AI_MODEL", model, 11118)):
                try:
                    q.execute(f"CALL DBMS_AI_SERVICE.{routine}(%s)", (name,))
                except pymysql.MySQLError as error:
                    if error.args[0] != absent:
                        raise
            connection.close()


if __name__ == "__main__":
    main()
