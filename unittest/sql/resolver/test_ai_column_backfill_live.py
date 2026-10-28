"""ADD AI columns over existing rows, multiple seed batches and definition refresh."""
import argparse
import json
from pathlib import Path

import pymysql
from ai_column_test_support import FixtureMock
from test_ai_column_worker_live import wait_until


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=2881)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    database, model, endpoint = "ai_column_backfill_live", "backfill_live_model", "backfill_live_ep"
    connection = pymysql.connect(host="127.0.0.1", port=args.port, user="root", charset="utf8mb4",
                                 autocommit=True, read_timeout=30)
    with FixtureMock(root, 19126) as mock, connection.cursor() as q:
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
                ai_model_name=model, provider="openai", access_key="fixture-key", request_model_name="fixture-model",
                url="http://127.0.0.1:19126/v1/chat/completions"))))
            q.execute("CREATE TABLE docs(id BIGINT PRIMARY KEY,content TEXT,title VARCHAR(100))")
            q.executemany("INSERT INTO docs VALUES(%s,%s,'original')", [(i, f"existing-数据库-{i}") for i in range(1, 73)])
            q.execute("INSERT INTO docs VALUES(73,NULL,'nullable')")
            q.execute(f"ALTER TABLE docs ADD COLUMN tags JSON AI COLUMN(AI_COMPLETE('{model}',content)), "
                      f"ADD COLUMN score BIGINT AI COLUMN(AI_COMPLETE('{model}',content))")

            def one(sql):
                q.execute(sql)
                return q.fetchone()

            def ready():
                assert one("SELECT COUNT(*) FROM docs WHERE content IS NOT NULL AND "
                           "((tags IS NOT NULL)+(score IS NOT NULL)) NOT IN (0,2)")[0] == 0, "Partial backfill group published"
                return one("SELECT COUNT(*) FROM docs WHERE tags IS NOT NULL AND score=7")[0] == 72

            wait_until("existing rows filled across seed batches", ready, timeout=30)
            q.execute("SELECT id,JSON_UNQUOTE(JSON_EXTRACT(tags,'$.input')),score FROM docs WHERE id<=72 ORDER BY id")
            assert q.fetchall() == tuple((i, f"existing-数据库-{i}", 7) for i in range(1, 73))
            assert mock.accepted == 144, "Backfill repeated or omitted inference"
            # NULL is a completed result too; strict reads must accept it.
            q.execute("SET SESSION ai_column_strict_read=ON")
            wait_until("NULL seed ready", lambda: one("SELECT tags IS NULL AND score IS NULL FROM docs WHERE id=73")[0])
            q.execute("SET SESSION ai_column_strict_read=OFF")
            print("PASS: two AI columns on 73 existing rows, multiple seed batches, atomic groups and READY NULL", flush=True)

            before = mock.accepted
            q.execute(f"ALTER TABLE docs MODIFY tags JSON AI COLUMN(AI_COMPLETE('{model}',CONCAT('redefined: ',content)))")
            wait_until("definition refresh", lambda: one("SELECT COUNT(*) FROM docs WHERE "
                       "JSON_UNQUOTE(JSON_EXTRACT(tags,'$.input'))=CONCAT('redefined: ',content)")[0] == 72, timeout=30)
            assert one("SELECT COUNT(*) FROM docs WHERE score=7")[0] == 72
            assert mock.accepted == before + 72, "Definition refresh repeated an unchanged AI column"
            print("PASS: changed definition regenerates only its target column", flush=True)
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
