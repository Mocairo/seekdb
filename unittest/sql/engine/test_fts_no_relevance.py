#!/usr/bin/env python3
"""Live SQL regressions for score-free fulltext retrieval.

Run against a disposable seekdb instance, for example:
  python test_fts_no_relevance.py --mysql /path/to/mysql --port 2881
Only the uniquely named database created by this test is removed.
"""

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import unittest
import uuid


class FulltextNoRelevanceTest(unittest.TestCase):
    client = []
    evidence = {}
    expect_score_skipping = True
    database = "fts_relevance_" + uuid.uuid4().hex[:12]
    match = "MATCH(body) AGAINST('oceanbase search')"
    ids = ["1", "2", "3", "7", "8"]

    @classmethod
    def query(cls, sql):
        payload = f"SET NAMES utf8mb4; USE {cls.database};\n{sql};\n"
        result = subprocess.run(cls.client, input=payload.encode("utf-8"),
                                capture_output=True, timeout=60)
        if result.returncode:
            raise AssertionError(result.stderr.decode("utf-8", "replace"))
        return result.stdout.decode("utf-8").strip().splitlines()

    @classmethod
    def setUpClass(cls):
        if not cls.client:
            raise unittest.SkipTest("Run this live regression with --mysql and --port")
        result = subprocess.run(cls.client, input=(
            f"CREATE DATABASE {cls.database};").encode(), capture_output=True, timeout=30)
        if result.returncode:
            raise AssertionError(result.stderr.decode("utf-8", "replace"))
        cls.addClassCleanup(cls.drop_database)
        cls.query("""
          CREATE TABLE docs(id INT PRIMARY KEY, body TEXT,
                            FULLTEXT INDEX ft(body) WITH PARSER space);
          INSERT INTO docs VALUES
            (1,'oceanbase search'), (2,'oceanbase database'), (3,'search engine'),
            (4,'unrelated words'), (5,''), (6,NULL),
            (7,'oceanbase search oceanbase'), (8,'search database')
        """)

    @classmethod
    def drop_database(cls):
        result = subprocess.run(cls.client, input=(
            f"DROP DATABASE {cls.database};").encode(), capture_output=True, timeout=30)
        if result.returncode:
            raise AssertionError(result.stderr.decode("utf-8", "replace"))

    def test_filter_forms(self):
        for predicate in (self.match, f"{self.match}>0", f"0<{self.match}"):
            with self.subTest(predicate=predicate):
                self.assertEqual(self.query(f"SELECT COUNT(*) FROM docs WHERE {predicate}"), ["5"])
        self.assertEqual(self.query(
            f"SELECT id FROM docs WHERE {self.match} ORDER BY id"), self.ids)

    def test_boolean_projection_and_aggregates(self):
        for expr in (f"{self.match}>0", f"0<{self.match}",
                     f"CASE WHEN {self.match}>0 THEN 1 ELSE 0 END"):
            with self.subTest(expr=expr):
                self.assertEqual(self.query(
                    f"SELECT id,({expr}) FROM docs WHERE {self.match} ORDER BY id"),
                    [f"{i}\t1" for i in self.ids])
        self.assertEqual(self.query(
            f"SELECT SUM({self.match}>0),COUNT({self.match}>0) FROM docs WHERE {self.match}"),
            ["5\t5"])

    def test_group_having_and_window(self):
        self.assertEqual(self.query(
            f"SELECT ({self.match}>0) AS hit,COUNT(*) FROM docs WHERE {self.match} "
            "GROUP BY hit HAVING hit>0"), ["1\t5"])
        self.assertEqual(self.query(
            f"SELECT id,SUM({self.match}>0) OVER () FROM docs WHERE {self.match} ORDER BY id"),
            [f"{i}\t5" for i in self.ids])

    def test_nested_predicate_and_join(self):
        self.assertEqual(self.query(
            f"SELECT COUNT(*) FROM docs WHERE {self.match} "
            f"AND CASE WHEN {self.match}>0 THEN id ELSE 0 END>0"), ["5"])
        self.assertEqual(self.query(
            f"SELECT COUNT(*) FROM docs d JOIN (SELECT 1 AS hit) x "
            f"ON ({self.match}>0)=x.hit WHERE {self.match}"), ["5"])

    def test_empty_and_absent_terms(self):
        for term in ("", "zzzzmissingterm", "   "):
            with self.subTest(term=term):
                self.assertEqual(self.query(
                    f"SELECT COUNT(*) FROM docs WHERE MATCH(body) AGAINST('{term}')"), ["0"])

    def test_boolean_mode(self):
        self.assertEqual(self.query(
            "SELECT id FROM docs WHERE MATCH(body) "
            "AGAINST('+oceanbase +search' IN BOOLEAN MODE) ORDER BY id"), ["1", "7"])

    def test_implicit_topk(self):
        implicit = self.query(f"SELECT id FROM docs WHERE {self.match} LIMIT 1")
        scored = self.query(
            f"SELECT id FROM docs WHERE {self.match} ORDER BY {self.match} DESC LIMIT 1")
        self.assertEqual(implicit, scored)
        self.assertEqual(implicit, ["7"])

    def test_taat_many_terms_and_rescan(self):
        # Distinct alphabetic tokens exceed the DAAT token limit (256).
        extra = ["unused" + chr(97+i//26) + chr(97+i%26) for i in range(270)]
        terms = "oceanbase search " + " ".join(extra)
        m = f"MATCH(body) AGAINST('{terms}')"
        rows = self.query(
            f"SELECT COUNT(*) FROM docs WHERE {m};"
            f"SELECT SUM({m}>0) FROM docs WHERE {m};"
            "PREPARE p FROM 'SELECT COUNT(*) FROM docs WHERE MATCH(body) AGAINST(?)';"
            f"SET @q='{terms}'; EXECUTE p USING @q;"
            "SET @q='zzzzmissingterm'; EXECUTE p USING @q;"
            f"SET @q='{terms}'; EXECUTE p USING @q; DEALLOCATE PREPARE p")
        self.assertEqual(rows, ["5", "5", "5", "0", "5"])

    def test_parameterized_thresholds(self):
        prepared = ("SELECT COUNT(*) FROM docs WHERE " + self.match + ">?")
        sql = "PREPARE p FROM '" + prepared.replace("'", "''") + "';"
        expected = []
        for threshold in ("0", "0.5", "999", "0", "NULL", "-1", "0"):
            # Projecting SUM(MATCH) forces scoring for the reference result.
            reference = self.query(
                f"SELECT COUNT(*),SUM({self.match}) FROM docs WHERE {self.match}>{threshold}")
            expected.append(reference[0].split("\t")[0])
            sql += f"SET @threshold={threshold}; EXECUTE p USING @threshold;"
        sql += "DEALLOCATE PREPARE p"
        self.assertEqual(self.query(sql), expected)

    def test_plan_retains_score_only_when_needed(self):
        cases = {
            "count": (f"SELECT COUNT(*) FROM docs WHERE {self.match}", not self.expect_score_skipping),
            "projection": (f"SELECT id,({self.match}>0) FROM docs WHERE {self.match} ORDER BY id", True),
            "group": (f"SELECT ({self.match}>0) AS hit,COUNT(*) FROM docs WHERE {self.match} GROUP BY hit", True),
            "topk": (f"SELECT id FROM docs WHERE {self.match} LIMIT 1", True),
        }
        for name, (sql, scoring) in cases.items():
            with self.subTest(name=name):
                plan = "\n".join(self.query("EXPLAIN " + sql))
                self.evidence[name] = plan
                self.assertIn("calc_relevance=" + str(scoring).lower(), plan)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mysql", default=os.environ.get("MYSQL_CLIENT", "mysql"))
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=2881)
    parser.add_argument("--user", default="root")
    parser.add_argument("--evidence", type=Path)
    parser.add_argument("--scored-baseline", action="store_true",
                        help="Expect the unmodified optimizer to retain scoring for COUNT")
    args = parser.parse_args()
    FulltextNoRelevanceTest.expect_score_skipping = not args.scored_baseline
    FulltextNoRelevanceTest.client = [args.mysql, "--skip-ssl", "--connect-timeout=5",
        f"--host={args.host}", f"--port={args.port}", f"--user={args.user}",
        "--batch", "--raw", "--skip-column-names", "--default-character-set=utf8mb4"]
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(FulltextNoRelevanceTest)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if args.evidence:
        args.evidence.write_text(json.dumps({"tests": result.testsRun,
            "successful": result.wasSuccessful(), "failures": [text for _, text in result.failures],
            "errors": [text for _, text in result.errors], "plans": FulltextNoRelevanceTest.evidence},
            ensure_ascii=False, indent=2), encoding="utf-8")
    sys.exit(0 if result.wasSuccessful() else 1)
