"""Exercise the AI COLUMN outbox in real SQL transactions, without a real model.

The supplied Mock handler runs in this test process, avoiding platform-specific
PID management. All business changes in the assertions are rolled back.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import threading
from http.server import ThreadingHTTPServer

import pymysql


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=2881)
    parser.add_argument("--mock-port", type=int, default=19121)
    parser.add_argument("--debug", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    mock_file = root / "tools/deploy/mysql_test/test_suite/ai_column/include/ai_column_mock.py"
    spec = importlib.util.spec_from_file_location("ai_column_fixture_mock", mock_file)
    mock = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mock)
    state = root / "build_ai_column_tests" / f"mock-{args.mock_port}"
    state.mkdir(parents=True, exist_ok=True)
    (state / "events.jsonl").write_text("", encoding="utf-8")
    mock.state_dir = lambda port: str(state)
    ready = threading.Event()
    server_holder, server_errors = [], []

    def create_server(address, handler):
        server = ThreadingHTTPServer(address, handler)
        server.daemon_threads = True
        server_holder.append(server)
        ready.set()
        return server

    def serve():
        try:
            mock.serve(args.mock_port)
        except Exception as error:
            server_errors.append(error)
            ready.set()

    mock.ThreadingHTTPServer = create_server
    thread = threading.Thread(target=serve, daemon=True)
    thread.start()
    if not ready.wait(5) or server_errors:
        raise RuntimeError(f"Mock startup failed: {server_errors}")
    options = dict(host="127.0.0.1", port=args.port, user="root", charset="utf8mb4",
                   autocommit=True, read_timeout=30, write_timeout=10)
    business = pymysql.connect(**options)
    observer = pymysql.connect(**options)
    database = "ai_column_t1_live"
    models = ("t1_live_text", "t1_live_embed")
    endpoints = ("t1_live_text_ep", "t1_live_embed_ep")
    try:
        with business.cursor() as q, observer.cursor() as outside:
            q.execute(f"DROP DATABASE IF EXISTS {database}")
            q.execute(f"CREATE DATABASE {database}")
            q.execute(f"USE {database}")
            if args.debug:
                q.execute("SET ob_log_level='DEBUG'")
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
            for name, kind, endpoint, route in (
                (models[0], "completion", endpoints[0], "chat/completions"),
                (models[1], "dense_embedding", endpoints[1], "embeddings"),
            ):
                q.execute("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL(%s,%s)",
                          (name, json.dumps(dict(type=kind, model_name="fixture-model"))))
                q.execute("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)", (endpoint,
                          json.dumps(dict(ai_model_name=name, provider="openai", access_key="fixture-key",
                                          request_model_name="fixture-model",
                                          url=f"http://127.0.0.1:{args.mock_port}/v1/{route}"))))
            q.execute(f"""CREATE TABLE docs(
                id BIGINT PRIMARY KEY, title VARCHAR(100), content TEXT,
                tags JSON AI COLUMN(AI_COMPLETE('{models[0]}', CONCAT('Classify: ',content))),
                emb VECTOR(4) AI COLUMN(AI_EMBED('{models[1]}',content))
            )""")
            q.execute("SELECT table_id FROM oceanbase.__all_table WHERE table_name='docs' "
                      "AND database_id=(SELECT database_id FROM oceanbase.__all_database WHERE database_name=%s)",
                      (database,))
            table_id = q.fetchone()[0]

            def tasks(cursor=q):
                cursor.execute("SELECT request_id,column_id,rowkey_predicate,input_snapshot,state,row_incarnation "
                               "FROM oceanbase.__all_ai_column_task WHERE table_id=%s ORDER BY task_id", (table_id,))
                return cursor.fetchall()

            def groups(cursor=q):
                cursor.execute("SELECT request_id,member_column_ids FROM oceanbase.__all_ai_column_group "
                               "WHERE table_id=%s ORDER BY request_id", (table_id,))
                return cursor.fetchall()

            business.begin()
            q.execute("INSERT INTO docs(id,title,content) VALUES(1,'one',%s),(2,'two',NULL)",
                      ('数据库 "quoted" \\ payload',))
            initial = tasks()
            assert len(initial) == 4 and len(groups()) == 2, initial
            assert len({row[0] for row in initial}) == 2, initial
            for _, members in groups():
                assert len(json.loads(members)) == 2
            assert {row[2] for row in initial} == {"`id` = 1", "`id` = 2"}, initial
            for row in initial:
                assert row[0] == row[5], "Initial row incarnation differs across its AI columns"
                captured = json.loads(row[3])["args"]
                assert captured[0] in models and len(captured) == 2, captured
                if row[2] == "`id` = 2":
                    assert captured[1] is None, captured
                else:
                    assert '数据库 "quoted" \\ payload' in captured[1], captured
            assert tasks(outside) == () and groups(outside) == ()
            outside.execute(f"SELECT COUNT(*) FROM {database}.docs")
            assert outside.fetchone()[0] == 0
            assert mock.read_events(args.mock_port) == [], "Model request occurred before commit"
            business.rollback()
            assert tasks() == () and groups() == ()
            assert mock.read_events(args.mock_port) == [], "Rolled-back transaction invoked a model"
            print("PASS: batch groups, typed/NULL/Unicode snapshots, isolation, no precommit HTTP, rollback", flush=True)

            business.begin()
            q.execute("INSERT INTO docs(id,title,content) VALUES(1,'title','original')")
            original = tasks()
            assert len(original) == 2
            incarnation = original[0][5]
            q.execute("INSERT IGNORE INTO docs(id,title,content) VALUES(1,'ignored','ignored')")
            assert tasks() == original, "INSERT IGNORE created ghost tasks"
            q.execute("UPDATE docs SET title='unrelated' WHERE id=1")
            assert tasks() == original, "Unrelated update created tasks"
            q.execute("UPDATE docs SET content=content, id=id WHERE id=1")
            assert tasks() == original, "No-op dependency/key update created tasks"
            q.execute("SAVEPOINT before_change")
            q.execute("UPDATE docs SET content='new body' WHERE id=1")
            changed = tasks()
            active = [row for row in changed if row[4] != 5]
            assert len(active) == 2 and len(changed) == 4, changed
            assert all(row[5] == incarnation and row[0] != incarnation for row in active), active
            assert len({row[0] for row in active}) == 1
            assert all('new body' in json.loads(row[3])["args"][1] for row in active)
            q.execute("ROLLBACK TO SAVEPOINT before_change")
            assert tasks() == original, "Savepoint rollback did not restore the outbox"
            q.execute("UPDATE docs SET id=5 WHERE id=1")
            moved = tasks()
            assert len(moved) == 2 and all(row[2] == "`id` = 5" for row in moved), moved
            assert all(row[5] == incarnation for row in moved), moved
            q.execute("SAVEPOINT before_delete")
            q.execute("DELETE FROM docs WHERE id=5")
            assert tasks() == () and groups() == (), "DELETE left tasks or group manifests"
            q.execute("INSERT INTO docs(id,title,content) VALUES(5,'new lifetime','replacement')")
            assert len(tasks()) == 2 and all(row[5] != incarnation for row in tasks())
            q.execute("ROLLBACK TO SAVEPOINT before_delete")
            assert tasks() == moved
            business.rollback()
            assert tasks() == () and groups() == ()
            print("PASS: IGNORE, unrelated/dependent/PK updates, savepoints, deletion and rollback", flush=True)

            business.begin()
            q.executemany("INSERT INTO docs(id,title,content) VALUES(%s,'batch',%s)",
                          [(i, f"input-{i}") for i in range(10, 26)])
            batch = tasks()
            assert len(batch) == 32 and len({row[0] for row in batch}) == 16, batch
            business.rollback()
            assert tasks() == () and groups() == ()
            assert mock.read_events(args.mock_port) == []
            print("PASS: 16-row batch, unique row groups, complete rollback and zero model calls", flush=True)
    finally:
        business.rollback()
        with business.cursor() as q:
            q.execute(f"DROP DATABASE IF EXISTS {database}")
            for endpoint in endpoints:
                q.execute("CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT(%s)", (endpoint,))
            for model in models:
                q.execute("CALL DBMS_AI_SERVICE.DROP_AI_MODEL(%s)", (model,))
        business.close()
        observer.close()
        server_holder[0].shutdown()
        server_holder[0].server_close()
        thread.join(5)


if __name__ == "__main__":
    main()
