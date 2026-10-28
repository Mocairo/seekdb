"""Run the supplied AI COLUMN DDL contract against a local seekdb instance.

Usage: python test_ai_column_ddl_live.py --port 28751
Requires PyMySQL. This executes the existing mysqltest input without recording
new expected results and checks every result set and expected error.
"""
import argparse
from pathlib import Path

import pymysql


def statements(path):
    sql, errors = [], {0}
    for line in path.read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        if stripped.startswith("--error "):
            errors = {int(code) for code in stripped[8:].split(",")}
        elif stripped in ("--disable_warnings", "--enable_warnings", "connection default;"):
            continue
        elif stripped.startswith("--"):
            raise ValueError(f"Unsupported mysqltest directive: {stripped}")
        else:
            sql.append(line)
            if stripped.endswith(";"):
                yield "\n".join(sql), errors
                sql, errors = [], {0}
    if sql:
        raise ValueError("Unterminated SQL statement")


def assert_rejected(cursor, sql, code=1235):
    try:
        cursor.execute(sql)
    except pymysql.MySQLError as error:
        if error.args[0] != code:
            raise AssertionError(f"Expected {code}: {sql}\n{error}") from error
    else:
        raise AssertionError(f"Expected rejection: {sql}")


def extended_contract(cursor):
    cursor.execute("SHOW AI COLUMNS FROM ai_column_p0_ddl.docs")
    rows = cursor.fetchall()
    if [row[0] for row in rows] != ["tags", "emb"] or [row[4] for row in rows] != [2, 3]:
        raise AssertionError(f"SHOW AI COLUMNS differs: {rows}")
    cursor.execute("SHOW CREATE TABLE ai_column_p0_ddl.docs")
    ddl = cursor.fetchone()[1]
    if ddl.count(" AI COLUMN (") != 2 or "max_retries=3" not in ddl:
        raise AssertionError(f"SHOW CREATE lost AI definitions: {ddl}")
    cursor.execute("USE ai_column_p0_ddl")
    cursor.execute(ddl.replace("CREATE TABLE `docs`", "CREATE TABLE `docs_roundtrip`", 1))
    cursor.execute("SHOW AI COLUMNS FROM docs_roundtrip")
    if cursor.fetchall() != rows:
        raise AssertionError("SHOW CREATE round trip changed AI definitions")

    cursor.execute("INSERT INTO docs_roundtrip VALUES (1, 'first'), (2, NULL)")
    cursor.execute("SELECT * FROM docs_roundtrip ORDER BY id")
    if cursor.fetchall() != ((1, "first", None, None), (2, None, None, None)):
        raise AssertionError("Implicit INSERT or default NULL/hidden-column visibility differs")
    for sql in (
        "INSERT INTO docs_roundtrip(id, tags) VALUES (3, NULL)",
        "INSERT INTO docs_roundtrip(id, tags) SELECT 3, NULL",
        "INSERT INTO docs_roundtrip SET id=3, tags=DEFAULT",
        "UPDATE docs_roundtrip SET tags=DEFAULT WHERE id=1",
        "UPDATE docs_roundtrip SET emb=NULL WHERE 1=0",
        "INSERT INTO docs_roundtrip(id) VALUES(1) ON DUPLICATE KEY UPDATE tags=NULL",
    ):
        assert_rejected(cursor, sql)
    cursor.execute("CREATE VIEW docs_view AS SELECT id, content, tags FROM docs_roundtrip")
    assert_rejected(cursor, "UPDATE docs_view SET tags=NULL WHERE id=1")
    cursor.execute("UPDATE docs_roundtrip SET content='changed' WHERE id=1")
    cursor.execute("START TRANSACTION")
    cursor.execute("UPDATE docs_roundtrip SET id=9, content='rolled back' WHERE id=1")
    cursor.execute("ROLLBACK")
    cursor.execute("SELECT id, content FROM docs_roundtrip ORDER BY id")
    if cursor.fetchall() != ((1, "changed"), (2, None)):
        raise AssertionError("AI table update rollback changed business values")
    cursor.execute("DROP VIEW docs_view")
    cursor.execute("DROP TABLE docs_roundtrip")
    print("PASS: SHOW round trip, implicit/batch INSERT, assignment guards, view guard, and rollback", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=2881)
    parser.add_argument("--user", default="root")
    parser.add_argument("--extended", action="store_true", help="Also check SHOW and DML resolver contracts")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    fixture = root / "tools/deploy/mysql_test/test_suite/ai_column/t/ai_column_ddl.test"
    expected = [((1,),), (("emb", 3), ("tags", 2)), ((1,),), ((1,),), ((1,),), ((1,),)]
    results = []
    accepted_errors = []
    connection = pymysql.connect(host=args.host, port=args.port, user=args.user,
                                 charset="utf8mb4", autocommit=True,
                                 connect_timeout=5, read_timeout=30, write_timeout=10)
    try:
        with connection.cursor() as cursor:
            for sql, errors in statements(fixture):
                try:
                    cursor.execute(sql)
                except pymysql.MySQLError as error:
                    if error.args[0] not in errors:
                        raise AssertionError(f"Unexpected SQL failure: {sql}\n{error}") from error
                    accepted_errors.append(error.args[0])
                else:
                    if 0 not in errors:
                        raise AssertionError(f"Expected errors {errors}, SQL succeeded: {sql}")
                    if cursor.description:
                        rows = cursor.fetchall()
                        print(f"{cursor.description[0][0]}: {rows}", flush=True)
                        results.append(rows)
                        if args.extended and len(results) == len(expected):
                            extended_contract(cursor)
            if results != expected:
                raise AssertionError(f"DDL results differ: expected={expected}, actual={results}")
            if accepted_errors[-2:] != [7600, 1235]:
                raise AssertionError(f"Definition validation errors differ: {accepted_errors}")
    finally:
        connection.close()
    print("PASS: supplied DDL fixture, metadata, hidden columns, ALTER ADD, and invalid definitions")


if __name__ == "__main__":
    main()
