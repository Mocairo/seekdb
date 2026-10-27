"""Check score-free TAAT results and timings on the official 20k-row corpus.

This is a separate regression diagnostic, not part of the scored benchmark.
"""
import argparse
import json
from pathlib import Path
import subprocess
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--port', type=int, default=2887)
p.add_argument('--reference', required=True, type=Path)
p.add_argument('--record', action='store_true')
p.add_argument('--output', required=True, type=Path)
args = p.parse_args()
client = ['/usr/bin/mysql', '--skip-ssl', '-h127.0.0.1', f'-P{args.port}', '-uroot',
          '-N', '-s', '-A', '--default-character-set=utf8mb4']
terms = 'database performance audit ' + ' '.join(
    'unused'+chr(97+i//26)+chr(97+i%26)+'qx' for i in range(400))
match = f"MATCH(title_en,content_en) AGAINST('{terms}')"
table = 'fts_large_bench.docs'


def query(sql):
    start = time.perf_counter()
    result = subprocess.run(client, input=sql.encode(), capture_output=True, timeout=180)
    elapsed = time.perf_counter()-start
    if result.returncode:
        raise RuntimeError(result.stderr.decode(errors='replace'))
    return result.stdout.decode().splitlines(), elapsed


tokens, _ = query(f"SELECT JSON_LENGTH(TOKENIZE('{terms}','beng'));")
assert int(tokens[0]) > 256, tokens
scored, _ = query(f'SELECT id,{match} FROM {table} WHERE {match} ORDER BY id;')
bare, _ = query(f'SELECT id FROM {table} WHERE {match} ORDER BY id;')
assert bare == [row.split('\t')[0] for row in scored]
if args.record:
    with args.reference.open('x') as output:
        json.dump(scored, output)
else:
    assert scored == json.loads(args.reference.read_text()), 'TAAT IDs or scores differ'
timings = {'count': [], 'score': []}
for _ in range(3):
    for label, projection in [('count', 'COUNT(*)'), ('score', f'COUNT(*), SUM({match})')]:
        sql = f'SELECT {projection} FROM {table} WHERE {match};'
        query(sql)
        rows, elapsed = query(sql*10)
        assert len(rows) == 10
        assert all(int(row.split('\t')[0]) == len(scored) for row in rows)
        timings[label].append(elapsed*1000/10)
plan, _ = query(f'EXPLAIN SELECT COUNT(*) FROM {table} WHERE {match};')
result = dict(tokens=int(tokens[0]), matched_rows=len(scored), average_ms_samples=timings, plan=plan)
args.output.write_text(json.dumps(result, indent=2))
print('PASS TAAT', len(scored), 'rows;', int(tokens[0]), 'tokens;', timings)
