"""Check actual async AI COLUMN execution, atomic publication and stale-result fencing."""
import argparse
import json
from pathlib import Path
import time

import pymysql
from ai_column_test_support import FixtureMock


def wait_until(description, function, timeout=15):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        value = function()
        if value:
            return value
        time.sleep(0.02)
    raise AssertionError(f"Timed out: {description}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=2881)
    parser.add_argument("--mock-port", type=int, default=19122)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    connection = pymysql.connect(host="127.0.0.1", port=args.port, user="root", charset="utf8mb4",
                                 autocommit=True, connect_timeout=5, read_timeout=30)
    database = "ai_column_worker_live"
    models = ("worker_live_text", "worker_live_embed", "worker_live_fail")
    endpoints = tuple(model + "_ep" for model in models)
    with FixtureMock(root, args.mock_port) as mock, connection.cursor() as q:
        try:
            q.execute(f"DROP DATABASE IF EXISTS {database}")
            q.execute(f"CREATE DATABASE {database}")
            q.execute(f"USE {database}")
            for endpoint in endpoints:
                try:
                    q.execute("CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT(%s)", (endpoint,))
                except pymysql.MySQLError as error:
                    if error.args[0] != 11112:
                        raise
            for model in models:
                try:
                    q.execute("CALL DBMS_AI_SERVICE.DROP_AI_MODEL(%s)", (model,))
                except pymysql.MySQLError as error:
                    if error.args[0] != 11118:
                        raise
            for name, endpoint, kind, route in (
                (models[0], endpoints[0], "completion", "chat/completions"),
                (models[1], endpoints[1], "dense_embedding", "embeddings"),
                (models[2], endpoints[2], "completion", "always-fail/completions"),
            ):
                q.execute("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL(%s,%s)",
                          (name, json.dumps(dict(type=kind, model_name="fixture-model"))))
                q.execute("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)", (endpoint,
                          json.dumps(dict(ai_model_name=name, provider="openai", access_key="fixture-key",
                                          request_model_name="fixture-model",
                                          url=f"http://127.0.0.1:{args.mock_port}/v1/{route}"))))

            def one(sql, params=()):
                q.execute(sql, params)
                return q.fetchone()

            def no_tasks():
                return one("SELECT COUNT(*) FROM oceanbase.__all_ai_column_task task "
                           "JOIN oceanbase.__all_table t ON t.table_id=task.table_id "
                           "JOIN oceanbase.__all_database d ON d.database_id=t.database_id "
                           "WHERE d.database_name=%s", (database,))[0] == 0

            q.execute(f"""CREATE TABLE typed_docs(id BIGINT PRIMARY KEY, content TEXT,
                tags JSON AI COLUMN(AI_COMPLETE('{models[0]}',CONCAT('Classify: ',content))),
                score BIGINT AI COLUMN(AI_COMPLETE('{models[0]}',CONCAT('Score: ',content))),
                emb VECTOR(4) AI COLUMN(AI_EMBED('{models[1]}',content)))""")
            connection.begin()
            q.execute("INSERT INTO typed_docs(id,content) VALUES(1,'typed-prompts')")
            assert one("SELECT tags IS NULL AND score IS NULL AND emb IS NULL FROM typed_docs WHERE id=1")[0] == 1
            assert mock.events() == []
            connection.commit()

            def typed_ready():
                count = one("SELECT (tags IS NOT NULL)+(score IS NOT NULL)+(emb IS NOT NULL) FROM typed_docs WHERE id=1")[0]
                assert count in (0, 3), f"Partial group became visible: {count}"
                return count == 3

            try:
                wait_until("three-column atomic publication", typed_ready)
            except AssertionError:
                q.execute("SELECT column_id,state,attempt_count,retry_count,error_code FROM "
                          "oceanbase.__all_ai_column_task WHERE table_id=(SELECT table_id "
                          "FROM oceanbase.__all_table WHERE table_name='typed_docs' AND database_id="
                          "(SELECT database_id FROM oceanbase.__all_database WHERE database_name=%s))",
                          (database,))
                print("Unpublished tasks:", q.fetchall(), "Mock events:", mock.events(), flush=True)
                raise
            assert one("SELECT score,JSON_UNQUOTE(JSON_EXTRACT(tags,'$.input')),"
                       "L2_DISTANCE(emb,[1,0.25,0.5,0.75]) FROM typed_docs WHERE id=1") == (
                           7, "Classify: typed-prompts", 0.0)
            wait_until("typed task cleanup", no_tasks)
            events = mock.events()
            assert len(events) == 3, events
            assert any("valid JSON value" in e["system"] for e in events)
            assert any("base-10 integer" in e["system"] for e in events)
            print("PASS: real HTTP, typed JSON/integer/vector, one atomic group, and task cleanup", flush=True)

            before = mock.accepted
            q.execute("INSERT INTO typed_docs(id,content) VALUES(2,NULL)")
            wait_until("NULL tasks finish", no_tasks)
            assert one("SELECT tags IS NULL AND score IS NULL AND emb IS NULL FROM typed_docs WHERE id=2")[0] == 1
            assert mock.accepted == before, "NULL input sent an HTTP request"
            print("PASS: READY NULL propagation without HTTP", flush=True)

            q.execute(f"""CREATE TABLE retry_docs(id BIGINT PRIMARY KEY, content VARCHAR(100),
                result VARCHAR(400) AI COLUMN(AI_COMPLETE('{models[0]}',content)) WITH(max_retries=2))""")
            q.execute("INSERT INTO retry_docs(id,content) VALUES(1,'retry-once')")
            wait_until("finite automatic retry", lambda: one("SELECT result='mock:retry-once' FROM retry_docs WHERE id=1")[0])
            wait_until("retry cleanup", no_tasks)
            retried = [e for e in mock.events() if e["user"] == "retry-once"]
            assert [e["status"] for e in retried] == [400, 200], retried
            print("PASS: exactly two upstream calls for one recoverable failure", flush=True)

            q.execute(f"""CREATE TABLE mixed_docs(id BIGINT PRIMARY KEY, content VARCHAR(100),
                good VARCHAR(400) AI COLUMN(AI_COMPLETE('{models[0]}',content)),
                bad VARCHAR(400) AI COLUMN(AI_COMPLETE('{models[2]}',content)) WITH(max_retries=1))""")
            q.execute("INSERT INTO mixed_docs(id,content) VALUES(1,'mixed-terminal')")

            def mixed_ready():
                row = one("SELECT good,bad FROM mixed_docs WHERE id=1")
                assert row[1] is None
                return row[0] == "mock:mixed-terminal"

            wait_until("mixed success/failure publication", mixed_ready)
            wait_until("mixed task cleanup", no_tasks)
            mixed = [e for e in mock.events() if e["user"] == "mixed-terminal"]
            assert len([e for e in mixed if e["status"] == 400]) == 2, mixed
            assert len([e for e in mixed if e["status"] == 200]) == 1, mixed
            print("PASS: successful member publishes with terminal failure after its finite retry budget", flush=True)

            q.execute(f"""CREATE TABLE racing_docs(id BIGINT PRIMARY KEY, content VARCHAR(100),
                result VARCHAR(400) AI COLUMN(AI_COMPLETE('{models[0]}',content)))""")
            before = mock.accepted
            q.execute("INSERT INTO racing_docs(id,content) VALUES(1,'slow-old')")
            wait_until("old request reaches upstream", lambda: mock.accepted > before)
            q.execute("UPDATE racing_docs SET content='fast-new' WHERE id=1")
            wait_until("new request wins", lambda: one("SELECT result='mock:fast-new' FROM racing_docs WHERE id=1")[0])
            wait_until("late old response returns", lambda: any(e["user"] == "slow-old" for e in mock.events()))
            wait_until("stale task cleanup", no_tasks)
            assert one("SELECT result FROM racing_docs WHERE id=1")[0] == "mock:fast-new"
            completed = [e["user"] for e in mock.events() if e["user"] in ("slow-old", "fast-new")]
            assert completed == ["fast-new", "slow-old"], completed
            print("PASS: genuinely reordered upstream responses cannot overwrite newer content", flush=True)

            before = mock.accepted
            q.execute("INSERT INTO racing_docs(id,content) VALUES(2,'slow-old-deleted')")
            wait_until("deleted row's request reaches upstream", lambda: mock.accepted > before)
            q.execute("DELETE FROM racing_docs WHERE id=2")
            q.execute("INSERT INTO racing_docs(id,content) VALUES(2,'new-incarnation')")
            wait_until("same-key new row ready", lambda: one("SELECT result='mock:new-incarnation' FROM racing_docs WHERE id=2")[0])
            wait_until("deleted lifetime's response returns", lambda: any(e["user"] == "slow-old-deleted" for e in mock.events()))
            wait_until("deleted lifetime cleanup", no_tasks)
            assert one("SELECT result FROM racing_docs WHERE id=2")[0] == "mock:new-incarnation"
            print("PASS: deletion/reinsertion fences the previous row lifetime", flush=True)

            completion = lambda content: json.dumps({"choices": [{"message": {"content": content}}]}).encode()
            faults = (
                ("invalid_json", "JSON", models[0], completion("{broken")),
                ("invalid_integer", "BIGINT", models[0], completion("not-an-integer")),
                ("wrong_dimension", "VECTOR(4)", models[1], b'{"data":[{"index":0,"embedding":[1,2,3]}]}'),
                ("nonfinite_vector", "VECTOR(4)", models[1], b'{"data":[{"index":0,"embedding":[1,2,3,1e999]}]}'),
                ("malformed_response", "VARCHAR(400)", models[0], b'{'),
                ("empty_response", "VARCHAR(400)", models[0], b''),
            )
            for name, result_type, chosen_model, body in faults:
                function = "AI_EMBED" if result_type.startswith("VECTOR") else "AI_COMPLETE"
                q.execute(f"CREATE TABLE {name}(id BIGINT PRIMARY KEY,content VARCHAR(100),"
                          f"result {result_type} AI COLUMN({function}('{chosen_model}',content)) WITH(max_retries=0))")
                before = mock.accepted
                mock.response_body = body
                q.execute(f"INSERT INTO {name}(id,content) VALUES(1,'typed-failure-{name}')")
                wait_until(name + " reaches final state", no_tasks)
                assert mock.accepted == before + 1, (name, mock.accepted - before)
                assert one(f"SELECT result IS NULL FROM {name}")[0] == 1, name
                q.execute("SET SESSION ai_column_strict_read=ON")
                try:
                    q.execute(f"SELECT result FROM {name}")
                except pymysql.MySQLError as error:
                    assert error.args[0] == 11120, (name, error)
                else:
                    raise AssertionError(f"Invalid result became READY: {name}")
                finally:
                    q.execute("SET SESSION ai_column_strict_read=OFF")
                mock.response_body = None
                q.execute(f"ALTER TABLE {name} RETRY AI COLUMN result WHERE status='FAILED'")
                wait_until(name + " recovers after explicit retry", lambda: one(f"SELECT result IS NOT NULL FROM {name}")[0])
                wait_until(name + " cleanup after retry", no_tasks)
                assert mock.accepted == before + 2, (name, mock.accepted - before)
            print("PASS: malformed/empty responses, JSON/integer conversion, vector dimension/finiteness, and FAILED-only recovery", flush=True)
        finally:
            mock.response_body = None
            connection.rollback()
            q.execute(f"DROP DATABASE IF EXISTS {database}")
            for endpoint in endpoints:
                try:
                    q.execute("CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT(%s)", (endpoint,))
                except pymysql.MySQLError as error:
                    if error.args[0] != 11112:
                        raise
            for model in models:
                try:
                    q.execute("CALL DBMS_AI_SERVICE.DROP_AI_MODEL(%s)", (model,))
                except pymysql.MySQLError as error:
                    if error.args[0] != 11118:
                        raise
            connection.close()


if __name__ == "__main__":
    main()
