"""Compare complete MATCH results on the unchanged official benchmark corpus.

Run after fts_large_bench.sh has populated its dedicated test database. Capture
the unmodified binary with --record, then check each candidate with --verify.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


QUERIES = {
    'cn': ("title, content", "全文索引 分词器"),
    'beng': ("title_en, content_en", "database performance audit"),
    'mixed': ("title, content", "OceanBase 稳定 database"),
    'limit': ("content", "倒排索引 tokenizer"),
}


def collect(args):
    client = [args.mysql, '--skip-ssl', '-h127.0.0.1', f'-P{args.port}', '-uroot',
              '-N', '-s', '-A', '--default-character-set=utf8mb4']

    def query(sql):
        result = subprocess.run(client + ['-e', sql], capture_output=True, timeout=120)
        if result.returncode:
            raise RuntimeError(result.stderr.decode('utf8', errors='replace'))
        return result.stdout.decode('utf8').splitlines()

    results = {}
    for name, (columns, terms) in QUERIES.items():
        match = f"MATCH({columns}) AGAINST('{terms}')"
        table = 'fts_large_bench.docs'
        scored = query(f'SELECT id,{match} FROM {table} WHERE {match} ORDER BY id')
        bare = query(f'SELECT id FROM {table} WHERE {match} ORDER BY id')
        assert bare == [row.split('\t')[0] for row in scored], (name, 'bare/scored IDs differ')
        # Scores and all IDs must match upstream, not just COUNT(*).
        results[name] = scored
        if name == 'limit':
            top_ids = query(f'SELECT id FROM {table} WHERE {match} LIMIT 20')
            scores = {row.split('\t')[0]: float(row.split('\t')[1]) for row in scored}
            expected_scores = sorted(scores.values(), reverse=True)[:20]
            assert len(top_ids) == 20 and len(set(top_ids)) == 20
            assert sorted((scores[i] for i in top_ids), reverse=True) == expected_scores
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mysql', default='/usr/bin/mysql')
    parser.add_argument('--port', type=int, default=2887)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--record', type=Path)
    mode.add_argument('--verify', type=Path)
    args = parser.parse_args()
    actual = collect(args)
    if args.record:
        with args.record.open('x', encoding='utf8') as output:
            json.dump(actual, output, ensure_ascii=False, indent=2)
    else:
        expected = json.loads(args.verify.read_text(encoding='utf8'))
        assert actual.keys() == expected.keys()
        for name in actual:
            assert actual[name] == expected[name], f'{name}: upstream IDs or scores differ'
    print('PASS', {name: len(rows) for name, rows in actual.items()},
          hashlib.sha256(json.dumps(actual, sort_keys=True).encode()).hexdigest())


if __name__ == '__main__':
    main()
