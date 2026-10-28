"""Regression for session-sensitive keys and ordinary-column side effects."""
import argparse
import json
from pathlib import Path
import traceback

import pymysql
from ai_column_test_support import FixtureMock
from test_ai_column_worker_live import wait_until


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=2881)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    database, model, endpoint = "ai_column_edges_live", "edges_live_model", "edges_live_ep"
    embed_model, embed_endpoint = "edges_live_embed", "edges_live_embed_ep"
    connection = pymysql.connect(host="127.0.0.1", port=args.port, user="root", charset="utf8mb4",
                                 autocommit=True, read_timeout=30)
    failures = []
    with FixtureMock(root, 19127) as mock, connection.cursor() as q:
        try:
            q.execute(f"DROP DATABASE IF EXISTS {database}")
            q.execute(f"CREATE DATABASE {database}")
            q.execute(f"USE {database}")
            for routine, name, absent in (("DROP_AI_MODEL_ENDPOINT", endpoint, 11112),
                                          ("DROP_AI_MODEL_ENDPOINT", embed_endpoint, 11112),
                                          ("DROP_AI_MODEL", model, 11118),
                                          ("DROP_AI_MODEL", embed_model, 11118)):
                try:
                    q.execute(f"CALL DBMS_AI_SERVICE.{routine}(%s)", (name,))
                except pymysql.MySQLError as error:
                    if error.args[0] != absent:
                        raise
            q.execute("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL(%s,%s)",
                      (model, json.dumps(dict(type="completion", model_name="fixture-model"))))
            q.execute("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)", (endpoint, json.dumps(dict(
                ai_model_name=model, provider="openai", access_key="fixture-key", request_model_name="fixture-model",
                url="http://127.0.0.1:19127/v1/chat/completions"))))
            q.execute("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL(%s,%s)",
                      (embed_model, json.dumps(dict(type="dense_embedding", model_name="fixture-model"))))
            q.execute("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)", (embed_endpoint, json.dumps(dict(
                ai_model_name=embed_model, provider="openai", access_key="fixture-key", request_model_name="fixture-model",
                url="http://127.0.0.1:19127/v1/embeddings"))))

            def one(sql):
                q.execute(sql)
                return q.fetchone()

            def scenario(name, action):
                try:
                    action()
                    print("PASS:", name, flush=True)
                except (AssertionError, pymysql.MySQLError) as error:
                    failures.append((name, str(error)))
                    print("FAIL:", name, str(error), flush=True)
                    traceback.print_exc()

            def timezone_key():
                q.execute(f"CREATE TABLE zoned(id TIMESTAMP(6) PRIMARY KEY,content VARCHAR(100),"
                          f"result VARCHAR(400) AI COLUMN(AI_COMPLETE('{model}',content)))")
                q.execute("SET time_zone='+05:30'")
                q.execute("INSERT INTO zoned(id,content) VALUES('2026-10-27 18:00:00.123456','timezone-key')")
                wait_until("TIMESTAMP key resolved by background session", lambda: one("SELECT result='mock:timezone-key' FROM zoned")[0], timeout=5)
                q.execute("SET time_zone='+00:00'")
                assert one("SELECT id='2026-10-27 12:30:00.123456' FROM zoned")[0] == 1

            def automatic_timestamp():
                q.execute("SET time_zone='+00:00'")
                q.execute(f"CREATE TABLE clocked(id BIGINT PRIMARY KEY,title VARCHAR(100),content VARCHAR(100),"
                          f"stamp TIMESTAMP(6) DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6),"
                          f"result VARCHAR(400) AI COLUMN(AI_COMPLETE('{model}',CONCAT(content,':',DATE_FORMAT(stamp,'%Y-%m-%d %H:%i:%s.%f')))))")
                q.execute("INSERT INTO clocked(id,title,content,stamp) VALUES(1,'original','keep-clock','2000-01-01 00:00:00')")
                wait_until("clock result ready", lambda: one("SELECT result IS NOT NULL FROM clocked WHERE id=1")[0])
                assert one("SELECT stamp='2000-01-01 00:00:00' FROM clocked WHERE id=1")[0] == 1, "AI publication changed an ordinary timestamp"
                q.execute("UPDATE clocked SET title='changed' WHERE id=1")
                wait_until("implicit timestamp dependency regenerated", lambda: one(
                    "SELECT result=CONCAT('mock:',content,':',DATE_FORMAT(stamp,'%Y-%m-%d %H:%i:%s.%f')) FROM clocked WHERE id=1")[0], timeout=5)
                before = mock.accepted
                stamp = one("SELECT stamp FROM clocked WHERE id=1")[0]
                q.execute("UPDATE clocked SET title=title,id=id WHERE id=1")
                assert one("SELECT stamp FROM clocked WHERE id=1")[0] == stamp
                assert mock.accepted == before, "No-op update retried a timestamp-dependent column"

            def update_trigger():
                q.execute(f"CREATE TABLE triggered(id BIGINT PRIMARY KEY,title VARCHAR(100),content VARCHAR(100),"
                          f"result VARCHAR(400) AI COLUMN(AI_COMPLETE('{model}',content)))")
                q.execute("CREATE TRIGGER append_content BEFORE UPDATE ON triggered FOR EACH ROW "
                          "SET NEW.content=CONCAT(NEW.content,'!')")
                q.execute("INSERT INTO triggered(id,title,content) VALUES(1,'title','trigger-input')")
                wait_until("trigger-table result ready", lambda: one("SELECT result IS NOT NULL FROM triggered WHERE id=1")[0])
                assert one("SELECT content,result FROM triggered WHERE id=1") == ("trigger-input", "mock:trigger-input"), "AI publication fired a business trigger"
                q.execute("UPDATE triggered SET title='changed' WHERE id=1")
                wait_until("trigger dependency change regenerated", lambda: one("SELECT result='mock:trigger-input!' FROM triggered WHERE id=1")[0], timeout=5)
                assert one("SELECT content FROM triggered WHERE id=1")[0] == "trigger-input!"
                try:
                    q.execute("ALTER TABLE triggered RENAME COLUMN content TO renamed_content")
                except pymysql.MySQLError as error:
                    assert error.args[0] == 1235, error
                else:
                    raise AssertionError("Dependency rename left an unresolved stored AI expression")

            def model_permission():
                q.execute("DROP USER IF EXISTS 'ai_column_limited'@'%'")
                q.execute("CREATE USER 'ai_column_limited'@'%' IDENTIFIED BY 'fixture-only-password'")
                q.execute(f"GRANT CREATE,INSERT,SELECT,UPDATE ON {database}.* TO 'ai_column_limited'@'%'")
                limited = pymysql.connect(host="127.0.0.1", port=args.port, user="ai_column_limited",
                                          password="fixture-only-password", database=database, autocommit=True)
                try:
                    with limited.cursor() as restricted:
                        denied = None
                        try:
                            restricted.execute("SELECT AI_COMPLETE(%s,'permission-probe')", (model,))
                        except pymysql.MySQLError as error:
                            denied = error.args[0]
                        before = mock.accepted
                        try:
                            restricted.execute(f"CREATE TABLE forbidden(id BIGINT PRIMARY KEY,content VARCHAR(100),"
                                               f"result VARCHAR(400) AI COLUMN(AI_COMPLETE('{model}',content)))")
                        except pymysql.MySQLError as error:
                            assert denied is not None and error.args[0] == denied, (denied, error.args)
                        else:
                            assert denied is None, "AI column definition bypassed the model access privilege"
                        assert mock.accepted == before, "DDL invoked a model"
                finally:
                    limited.close()
                    q.execute("DROP USER 'ai_column_limited'@'%'")

            def generated_dependency():
                q.execute(f"CREATE TABLE derived(id BIGINT PRIMARY KEY,base VARCHAR(100),"
                          f"content VARCHAR(100) GENERATED ALWAYS AS (UPPER(base)) STORED,"
                          f"result VARCHAR(400) AI COLUMN(AI_COMPLETE('{model}',content)))")
                q.execute("INSERT INTO derived(id,base) VALUES(1,'first')")
                wait_until("generated input initially ready", lambda: one("SELECT result='mock:FIRST' FROM derived WHERE id=1")[0])
                q.execute("UPDATE derived SET base='second' WHERE id=1")
                wait_until("generated dependency refreshed", lambda: one("SELECT result='mock:SECOND' FROM derived WHERE id=1")[0], timeout=5)
                try:
                    q.execute("ALTER TABLE derived RENAME COLUMN base TO renamed_base")
                except pymysql.MySQLError as error:
                    assert error.args[0] in (1235, 3108), error
                else:
                    raise AssertionError("Dependency rename left an unresolved stored AI expression")
                q.execute("UPDATE derived SET base='third' WHERE id=1")
                wait_until("rejected rename preserves usable definition", lambda: one("SELECT result='mock:THIRD' FROM derived WHERE id=1")[0])

            def ordinary_ai_functions():
                q.execute("SELECT AI_COMPLETE(%s,AI_PROMPT('repeat:{0}/{0}',%s),"
                          "JSON_OBJECT('temperature',0.2,'max_tokens',16))", (model, "数据库"))
                assert q.fetchone() == ("mock:repeat:数据库/数据库",)
                q.execute("SELECT L2_DISTANCE(AI_EMBED(%s,'scalar-vector',4),[1,0.25,0.5,0.75])", (embed_model,))
                assert q.fetchone() == (0.0,)
                text = "普通 SQL Unicode: 数据库😀\\\"\n" * 300
                q.execute("SELECT AI_COMPLETE(%s,%s,NULL) AS completion", (model, text))
                assert q.fetchone() == ("mock:" + text,)
                before = mock.accepted
                q.execute("SELECT AI_EMBED(%s,NULL,4),1+2,UPPER('sql')", (embed_model,))
                assert q.fetchone() == (None, 3, "SQL")
                try:
                    q.execute("SELECT AI_COMPLETE(%s,CAST(NULL AS CHAR)) AS completion", (model,))
                except pymysql.MySQLError as error:
                    assert error.args[0] == 1210, error
                else:
                    raise AssertionError("Ordinary AI_COMPLETE lost its existing NULL argument error")
                assert mock.accepted == before, "Ordinary NULL propagation invoked HTTP"

            def drop_inflight():
                q.execute(f"CREATE TABLE retiring(id BIGINT PRIMARY KEY,content VARCHAR(100),"
                          f"first_result VARCHAR(400) AI COLUMN(AI_COMPLETE('{model}',content)),"
                          f"second_result VARCHAR(400) AI COLUMN(AI_COMPLETE('{model}',CONCAT('survivor:',content))))")
                table_id = one("SELECT table_id FROM oceanbase.__all_table WHERE table_name='retiring' "
                               f"AND database_id=(SELECT database_id FROM oceanbase.__all_database WHERE database_name='{database}')")[0]
                before = mock.accepted
                q.execute("INSERT INTO retiring(id,content) VALUES(1,'slow-old-retired-column')")
                wait_until("retired column request reaches upstream", lambda: mock.accepted > before)
                q.execute("ALTER TABLE retiring DROP COLUMN first_result")
                wait_until("surviving group member publishes", lambda: one(
                    "SELECT second_result='mock:survivor:slow-old-retired-column' FROM retiring WHERE id=1")[0])
                wait_until("retired column tasks clean up", lambda: one(
                    f"SELECT COUNT(*)=0 FROM oceanbase.__all_ai_column_task WHERE table_id={table_id}")[0])
                assert one(f"SELECT COUNT(*) FROM oceanbase.__all_ai_column WHERE table_id={table_id}")[0] == 1, "Dropped column retained active AI metadata"
                before = mock.accepted
                q.execute("UPDATE retiring SET content='slow-old-retired-table' WHERE id=1")
                wait_until("dropped table request reaches upstream", lambda: mock.accepted > before)
                q.execute("DROP TABLE retiring")
                wait_until("dropped table task and group cleanup", lambda: one(
                    f"SELECT (SELECT COUNT(*) FROM oceanbase.__all_ai_column_task WHERE table_id={table_id})+"
                    f"(SELECT COUNT(*) FROM oceanbase.__all_ai_column_group WHERE table_id={table_id})")[0] == 0)
                assert one(f"SELECT COUNT(*) FROM oceanbase.__all_ai_column WHERE table_id={table_id}")[0] == 0

            scenario("TIMESTAMP primary key across session time zones", timezone_key)
            scenario("publication preserves ordinary automatic timestamps", automatic_timestamp)
            scenario("business triggers run once and their dependency changes regenerate AI results", update_trigger)
            scenario("DDL follows existing AI access policy without calling the model", model_permission)
            scenario("ordinary generated-column dependencies track their source columns", generated_dependency)
            scenario("ordinary AI functions preserve Prompt/config/Unicode/vector/NULL semantics", ordinary_ai_functions)
            scenario("dropping in-flight AI columns and tables retires tasks without losing surviving results", drop_inflight)
            assert not failures, failures
        finally:
            q.execute(f"DROP DATABASE IF EXISTS {database}")
            for routine, name, absent in (("DROP_AI_MODEL_ENDPOINT", endpoint, 11112),
                                          ("DROP_AI_MODEL_ENDPOINT", embed_endpoint, 11112),
                                          ("DROP_AI_MODEL", model, 11118),
                                          ("DROP_AI_MODEL", embed_model, 11118)):
                try:
                    q.execute(f"CALL DBMS_AI_SERVICE.{routine}(%s)", (name,))
                except pymysql.MySQLError as error:
                    if error.args[0] != absent:
                        raise
            connection.close()


if __name__ == "__main__":
    main()
