"""Measure publication latency and actual upstream calls using the supplied Mock."""
import argparse
import json
from pathlib import Path
import statistics
import time

import pymysql
from ai_column_test_support import FixtureMock
from test_ai_column_worker_live import wait_until


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=2881)
    parser.add_argument("--rows", type=int, default=50)
    parser.add_argument("--failure-samples", type=int, default=4)
    parser.add_argument("--label", default="local")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    assert 1 <= args.rows <= 1000 and 1 <= args.failure_samples <= 20
    root = Path(__file__).resolve().parents[3]
    database, model, endpoint = "ai_column_benchmark_live", "benchmark_live_model", "benchmark_live_ep"
    connection = pymysql.connect(host="127.0.0.1", port=args.port, user="root", charset="utf8mb4",
                                 autocommit=True, read_timeout=30)
    metrics = dict(label=args.label, clock="client commit return to committed publication", rows=args.rows,
                   mock_success_delay_seconds=0.05, failure_status=503, failure_samples=args.failure_samples,
                   write_latency_ms=[], failure_completion_ms=[])
    with FixtureMock(root, 19128) as mock, connection.cursor() as q:
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
                url="http://127.0.0.1:19128/v1/chat/completions"))))
            q.execute(f"CREATE TABLE docs(id BIGINT PRIMARY KEY,content VARCHAR(100),"
                      f"result VARCHAR(400) AI COLUMN(AI_COMPLETE('{model}',content)) WITH(max_retries=0))")

            def no_tasks():
                q.execute("SELECT COUNT(*) FROM oceanbase.__all_ai_column_task task JOIN oceanbase.__all_table t "
                          "ON t.table_id=task.table_id JOIN oceanbase.__all_database d ON d.database_id=t.database_id "
                          "WHERE d.database_name=%s", (database,))
                return q.fetchone()[0] == 0

            mock.response_status = 503
            for i in range(args.failure_samples):
                before = mock.accepted
                begin = time.perf_counter()
                q.execute("INSERT INTO docs(id,content) VALUES(%s,%s)", (-(i + 1), f"failure-{i}"))
                committed = time.perf_counter()
                metrics["write_latency_ms"].append((committed - begin) * 1000)
                wait_until("terminal 503 failure published", lambda: no_tasks() and mock.accepted == before + 1, timeout=10)
                metrics["failure_completion_ms"].append((time.perf_counter() - committed) * 1000)
            metrics["failure_completion_median_ms"] = statistics.median(metrics["failure_completion_ms"])
            metrics["failure_actual_calls"] = mock.accepted
            mock.response_status = None
            before = mock.accepted
            begin = time.perf_counter()
            q.executemany("INSERT INTO docs(id,content) VALUES(%s,%s)", [(i, f"throughput-{i}") for i in range(1, args.rows + 1)])
            committed = time.perf_counter()
            metrics["batch_write_ms"] = (committed - begin) * 1000

            def all_ready():
                q.execute("SELECT COUNT(*) FROM docs WHERE id>0 AND result=CONCAT('mock:',content)")
                return q.fetchone()[0] == args.rows and no_tasks()

            wait_until("complete batch publication", all_ready, timeout=90)
            elapsed = time.perf_counter() - committed
            metrics["batch_completion_ms"] = elapsed * 1000
            metrics["completed_rows_per_second"] = args.rows / elapsed
            metrics["success_actual_calls"] = mock.accepted - before
            metrics["peak_upstream_concurrency"] = mock.peak_active
            assert metrics["failure_actual_calls"] == args.failure_samples, metrics
            assert metrics["success_actual_calls"] == args.rows, metrics
            assert metrics["peak_upstream_concurrency"] <= 2, metrics
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(metrics, ensure_ascii=False, indent=2), encoding="utf8")
            print(json.dumps(metrics, ensure_ascii=False), flush=True)
        finally:
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
