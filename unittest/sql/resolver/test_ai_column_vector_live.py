"""Native VSAG/HNSW regression: AI vector publication, invalidation and rollback."""
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
    database, model, endpoint = "ai_column_vector_live", "vector_live_model", "vector_live_ep"
    connection = pymysql.connect(host="127.0.0.1", port=args.port, user="root", charset="utf8mb4",
                                 autocommit=True, read_timeout=120)
    with FixtureMock(root, 19123) as mock, connection.cursor() as q:
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
                      (model, json.dumps(dict(type="dense_embedding", model_name="fixture-model"))))
            q.execute("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)", (endpoint, json.dumps(dict(
                ai_model_name=model, provider="openai", access_key="fixture-key",
                url="http://127.0.0.1:19123/v1/embeddings", request_model_name="fixture-model"))))
            q.execute(f"""CREATE TABLE docs(id BIGINT PRIMARY KEY,content VARCHAR(100),
                emb VECTOR(4) AI COLUMN(AI_EMBED('{model}',content)))""")

            def one(sql):
                q.execute(sql)
                return q.fetchone()

            def ann(limit=3):
                q.execute(f"SELECT id FROM docs ORDER BY L2_DISTANCE(emb,[0,0,0,0]) APPROXIMATE LIMIT {limit}")
                return [r[0] for r in q.fetchall()]

            q.execute("INSERT INTO docs(id,content) VALUES(1,'vec-zero'),(2,'vec-ten'),(3,'vec-twenty')")
            wait_until("embeddings ready", lambda: one("SELECT COUNT(*) FROM docs WHERE emb IS NOT NULL")[0] == 3)
            q.execute("CREATE VECTOR INDEX idx_ai_emb ON docs(emb) "
                      "WITH(distance=l2,type=hnsw,lib=vsag,m=10,ef_construction=12,ef_search=40)")
            assert ann() == [1, 2, 3]
            q.execute("EXPLAIN SELECT id FROM docs ORDER BY L2_DISTANCE(emb,[0,0,0,0]) APPROXIMATE LIMIT 3")
            plan = "\n".join(str(r) for r in q.fetchall())
            assert "VECTOR" in plan.upper() or "idx_ai_emb" in plan, plan
            print("PASS: generated vectors are searched through the native VSAG/HNSW index", flush=True)

            before = mock.accepted
            connection.begin()
            q.execute("UPDATE docs SET content='vec-zero' WHERE id=2")
            assert one("SELECT emb IS NULL FROM docs WHERE id=2")[0] == 1
            assert set(ann()) == {1, 3}, "Invalidated vector remained in ANN within its transaction"
            assert mock.accepted == before, "Model call before commit"
            connection.rollback()
            assert one("SELECT content='vec-ten' AND emb=[10,0,0,0] FROM docs WHERE id=2")[0] == 1
            assert ann() == [1, 2, 3]
            assert mock.accepted == before, "Rolled-back invalidation called a model"
            print("PASS: invalidation removes the old ANN entry; rollback restores data and index without HTTP", flush=True)

            mock.response_status = 503
            q.execute("UPDATE docs SET content='vec-zero' WHERE id=2")
            assert set(ann()) == {1, 3}, "Committed invalidation retained the old vector"
            mock.response_status = None
            wait_until("vector regenerated", lambda: one("SELECT emb=[0,0,0,0] FROM docs WHERE id=2")[0])
            assert set(ann(2)) == {1, 2}
            connection.begin()
            q.execute("DELETE FROM docs WHERE id=3")
            assert set(ann()) == {1, 2}
            connection.rollback()
            assert set(ann()) == {1, 2, 3}
            print("PASS: regenerated vectors rejoin ANN; delete rollback preserves index visibility", flush=True)
        finally:
            connection.rollback()
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
