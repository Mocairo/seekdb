"""Compare complete TOKENIZE outputs between two locally built servers.

Capture with --record reference.json before switching binaries, then run with
--verify reference.json. Uses only SELECT; no schema or data is changed.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import random
import subprocess
from pathlib import Path


def cases():
    texts = ['', ' ', '\t\r\n', '全文索引性能优化', '中华人民共和国',
             '三千五百二十公斤 10kg 一百个 123.45 3/4',
             '中文EnglishABC123混合 foo-bar test@example.com',
             'ＡＢＣ１２３ 全角，标点。半角!?', '𠀀𠀁𠮷野家😀🚀',
             'かなカナ 한글 日本語', 'café naïve Straße e\u0301',
             '重复 重复重复 全文 全文', 'a\x00b\n中文\t字符',
             'abc', '123', '中', 'a中1文', '中文' * 1500,
             ('数据库索引 中文English123，三百公斤。' * 100),
             'Cafe café CAFE cafe CAFÉ', 'café Cafe café',
             'Résumé resume RÉSUMÉ', 'alpha alpha alpha beta beta',
             'the THE and AND oceanbase OceanBase OCEANBASE']
    rng = random.Random(20261027)
    units = ['数据库', '全文', '索引', '三百', '公斤', 'abc', 'Z9', '12.5',
             '-', '.', '/', '@', ' ', '\n', '\t', '，', '。', '𠀀', '😀',
             'Ａ１', '한글', 'カナ', 'é', '\u0301', '\x00']
    texts.extend(''.join(rng.choice(units) for _ in range(rng.randrange(1, 70)))
                 for _ in range(160))
    for parser in ('ik', 'beng', 'space', 'ngram'):
        for text in texts:
            yield parser, text


def collect(args):
    inputs = list(cases())
    # Hex literals preserve quotes, backslashes, NULs and supplementary Unicode.
    if args.details:
        inputs = [(parser, text, collation) for parser, text in inputs
                  for collation in ('utf8mb4_general_ci', 'utf8mb4_bin')]
        sql = '\n'.join(
            f"SELECT {i}, HEX(TOKENIZE(CONVERT(X'{text.encode().hex()}' USING utf8mb4) "
            f"COLLATE {collation}, '{parser}', '[{{\"output\":\"all\"}}]'));"
            for i, (parser, text, collation) in enumerate(inputs))
    else:
        sql = '\n'.join(f"SELECT {i}, HEX(TOKENIZE(CONVERT(X'{text.encode().hex()}' USING utf8mb4), '{parser}'));"
                        for i, (parser, text) in enumerate(inputs))
    proc = subprocess.run([args.mysql, '--skip-ssl', '-h', args.host, '-P', str(args.port),
                           '-u', args.user, '-N', '-s', '--default-character-set=utf8mb4'],
                          input=sql.encode(), capture_output=True, timeout=180)
    if proc.returncode:
        raise RuntimeError(proc.stderr.decode('utf8', errors='replace'))
    lines = proc.stdout.decode('ascii').splitlines()
    assert len(lines) == len(inputs), (len(lines), len(inputs))
    outputs = []
    for i, line in enumerate(lines):
        index, value = line.split('\t')
        assert int(index) == i
        outputs.append(value)
    return dict(inputs=inputs, outputs=outputs,
                output_sha256=hashlib.sha256('\n'.join(outputs).encode()).hexdigest())


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--mysql', required=True)
    p.add_argument('--host', default='127.0.0.1')
    p.add_argument('--port', type=int, default=2887)
    p.add_argument('--user', default='root')
    p.add_argument('--workers', type=int, default=1,
                   help='Independent concurrent connections when verifying')
    p.add_argument('--details', action='store_true',
                   help='Compare token frequencies and document lengths under two collations')
    mode = p.add_mutually_exclusive_group(required=True)
    mode.add_argument('--record', type=Path)
    mode.add_argument('--verify', type=Path)
    args = p.parse_args()
    if not 1 <= args.workers <= 32:
        p.error('--workers must be between 1 and 32')
    actual = collect(args)
    if args.record:
        with args.record.open('x', encoding='utf8') as f:
            json.dump(actual, f, ensure_ascii=False, indent=2)
        print(f"Recorded {len(actual['outputs'])} complete token streams: {actual['output_sha256']}")
    else:
        expected = json.loads(args.verify.read_text(encoding='utf8'))
        assert expected['inputs'] == [list(x) for x in actual['inputs']], 'Input corpus changed'
        errors = [i for i, (a, b) in enumerate(zip(actual['outputs'], expected['outputs'])) if a != b]
        assert len(actual['outputs']) == len(expected['outputs'])
        assert not errors, f'Token stream mismatch at cases {errors}'
        if args.workers > 1:
            with ThreadPoolExecutor(max_workers=args.workers) as pool:
                for index, concurrent in enumerate(pool.map(collect, [args] * args.workers)):
                    assert concurrent['outputs'] == expected['outputs'], f'Concurrent connection {index} differs'
        print(f"PASS: {len(actual['outputs'])} complete token streams match: {actual['output_sha256']}")
        if args.workers > 1:
            print(f'PASS: {args.workers} concurrent independent connections also match')


if __name__ == '__main__':
    main()
