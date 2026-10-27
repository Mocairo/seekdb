"""Native seekdb acceptance tests. Requires an isolated, already-started server.

Start with --variable secure_file_priv=<root>; never run against a shared server.
PyArrow creates test files only; every SQL statement executes in seekdb.
"""
import argparse
import base64
import csv
import datetime
import json
import pathlib
import threading
import time
import pymysql

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--port', type=int, default=2883)
p.add_argument('--root', type=pathlib.Path, required=True)
p.add_argument('--report', type=pathlib.Path, required=True)
args = p.parse_args()
root = args.root.resolve()
root.mkdir(parents=True, exist_ok=True)
repo = pathlib.Path(__file__).resolve().parents[2]
public = repo / 'tools/deploy/mysql_test/test_suite/file_sql/data'
for stale in ('nested.jsonl','broken.csv','badutf.csv'):
    (root / stale).unlink(missing_ok=True)
for name in ('basic.csv', 'basic.jsonl', 'invalid.txt'):
    (root / name).write_bytes((public / name).read_text(encoding="utf-8").encode("utf-8"))
(root / 'simple.parquet').write_bytes(base64.b64decode((public / 'simple.parquet.b64').read_bytes()))
for fixture in ('plain','dictionary','v2','empty'):
    source = repo / f'unittest/sql/engine/basic/data/{fixture}.parquet.b64'
    (root / f'{fixture}.parquet').write_bytes(base64.b64decode(source.read_bytes()))
(root / 'left.csv').write_text('id,label\n1,one\n2,two\n3,three\n4,four\n', encoding='utf-8')
(root / 'right.jsonl').write_text(
    '{"id":2,"name":"二"}\n{"id":3,"name":"三"}\n{"id":3,"name":"third"}\n', encoding='utf-8')
(root / 'types.csv').write_text(
    'id,wide,allnull,invalidday,at\n9223372036854775807,9223372036854775808,NULL,2023-02-29,2024-02-29 12:34:56.123456\n'
    '-9223372036854775808,18446744073709551615,NULL,2024-13-01,2024-03-01\n', encoding='utf-8')
(root / 'changing.csv').write_text('id\n1\n', encoding='utf-8')
outside = root.parent / 'file_sql_outside.csv'
outside.write_text('id\n999\n', encoding='utf-8')
connection = pymysql.connect(host='127.0.0.1', port=args.port, user='root', database='test',
                             autocommit=True, charset='utf8mb4', read_timeout=30)
results = []
def literal(path):
    return "'" + str(path).replace('\\', '/').replace("'", "''") + "'"
def file(name, function='read_csv', options=''):
    return f'{function}({literal(root / name)}{options})'
def query(sql):
    with connection.cursor() as cursor:
        cursor.execute(sql)
        return cursor.fetchall()
def equal(name, sql, expected):
    actual = query(sql)
    if actual != tuple(expected):
        raise AssertionError(f'{name}: {actual!r} != {expected!r}\n{sql}')
    results.append({'case': name, 'status': 'pass'})
    print('PASS', name, flush=True)
def error(name, sql, code=None):
    try:
        query(sql)
    except pymysql.MySQLError as exc:
        if code is not None and exc.args[0] != code:
            raise AssertionError(f'{name}: expected error {code}, got {exc!r}')
        results.append({'case': name, 'status': 'pass', 'error': str(exc)})
        print('PASS', name, exc.args[0], flush=True)
    else:
        raise AssertionError(f'{name}: statement unexpectedly succeeded: {sql}')

try:
    equal('default disabled', 'select @@session.enable_file_sql', [(0,)])
    error('disabled read', f'select * from {file("basic.csv")}', 1235)
    equal('ordinary expression', 'select 1+2,concat("a","b")', [(3, 'ab')])
    query('set session enable_file_sql=ON')
    csv_source = file('basic.csv')
    json_source = file('basic.jsonl', 'read_json', ",format='newline_delimited'")
    equal('CSV aggregate', f'select count(*),sum(id),round(avg(score),2),sum(active) from {csv_source}',
          [(3, 6, 91.92, 2)])
    equal('CSV projection', f'select id__2,name from {csv_source} order by id',
          [(101, 'Alice'), (102, 'Bob'), (103, 'Chen')])
    equal('CSV multiline', f"select replace(note,char(10),'<NL>') from {csv_source} where id=1",
          [('hello,<NL>world',)])
    equal('CSV quote', f'select note from {csv_source} where id=3', [('say "hi"',)])
    equal('filter/order/limit/alias', f'select id*10 as v from {csv_source} where active order by score limit 1', [(30,)])
    equal('group/having', f'select active,count(*),sum(id),min(id),max(id) from {csv_source} group by active having count(*)>1',
          [(1, 2, 4, 1, 3)])
    equal('JSON union', f'select id,name,note from {json_source} order by id',
          [(1, 'Alice', None), (2, 'Bob', None), (3, 'Chen', 'ok')])
    equal('ndjson synonym', f'select count(*) from {file("right.jsonl","read_ndjson")}', [(3,)])
    equal('Parquet public', f'select count(*),min(c1),max(c1),sum(c1) from {file("simple.parquet","read_parquet")}',
          [(7, 0, 6, 21)])
    equal('empty Parquet row group', f'select count(*) from {file("empty.parquet","read_parquet")}', [(0,)])
    for fixture in ('plain','dictionary','v2'):
        source=file(f'{fixture}.parquet','read_parquet')
        equal(f'Parquet {fixture} row groups',
              f'select count(*),sum(id),sum(active),count(name),min(day) from {source}',
              [(257,32896,128,220,datetime.date(2024,2,29))])
        equal(f'Parquet {fixture} column projection',
              f'select name,id from {source} where id<3 order by id',
              [(None,0),('value-1',1),('value-2',2)])
    for function, filename in [('read_csv', 'basic.csv'), ('read_json', 'basic.jsonl'), ('read_parquet', 'simple.parquet')]:
        schema = query(f'describe select * from {file(filename, function)}')
        if not schema or schema[0][0] not in ('id', 'c1'):
            raise AssertionError(('dynamic metadata', schema))
        results.append({'case': f'{function} DESCRIBE', 'status': 'pass', 'schema': schema})
    schema = query(f'describe select * from {csv_source}')
    if [row[1] for row in schema] != ['BIGINT','VARCHAR','DOUBLE','BOOLEAN','DATE','DATETIME','VARCHAR','BIGINT']:
        raise AssertionError(('inferred types', schema))
    equal('inner cross format join',
          f'select a.id,a.label,b.name from {file("left.csv")} a inner join {file("right.jsonl","read_json")} b on a.id=b.id order by a.id,b.name',
          [(2, 'two', '二'), (3, 'three', 'third'), (3, 'three', '三')])
    equal('left join NULLs',
          f'select a.id,count(b.name) from {file("left.csv")} a left join {file("right.jsonl","read_json")} b on a.id=b.id group by a.id order by a.id',
          [(1, 0), (2, 1), (3, 2), (4, 0)])
    equal('CSV Parquet join',
          f'select a.id,p.c1 from {file("left.csv")} a join {file("simple.parquet","read_parquet")} p on a.id=p.c1 order by a.id',
          [(1, 1), (2, 2), (3, 3), (4, 4)])
    equal('nested rescan',
          f'select a.id,(select count(*) from {file("right.jsonl","read_json")} b where b.id=a.id) from {file("left.csv")} a order by a.id',
          [(1, 0), (2, 1), (3, 2), (4, 0)])
    plan = query(f'explain select a.id,b.name from {file("left.csv")} a left join {file("right.jsonl","read_json")} b on a.id=b.id')
    plan_text = str(plan).upper()
    if 'FUNCTION' not in plan_text or 'JOIN' not in plan_text:
        raise AssertionError(('native plan', plan))
    results.append({'case': 'native JOIN plan', 'status': 'pass', 'plan': plan})
    equal('numeric boundaries', f'select id,wide,allnull,invalidday from {file("types.csv")} order by id',
          [(-9223372036854775808,'18446744073709551615',None,'2024-13-01'),
           (9223372036854775807,'9223372036854775808',None,'2023-02-29')])
    equal('fractional datetime', f'select at from {file("types.csv")} where id>0',
          [(datetime.datetime(2024,2,29,12,34,56,123456),)])
    with connection.cursor() as cursor:
        cursor.execute(f'select created from {csv_source} where id=1')
        if cursor._result.fields[0].scale != 0:
            raise AssertionError('whole-second datetime reported fractional precision')
        cursor.execute(f'select at from {file("types.csv")} where id>0')
        if cursor._result.fields[0].scale != 6:
            raise AssertionError('fractional datetime precision was lost')
    results.append({'case': 'datetime wire precision', 'status': 'pass'})
    print('PASS datetime wire precision', flush=True)
    equal('glob', f'select count(*) from glob({literal(root / "*.jsonl")})', [(2,)])
    error('sample_size guard', f'select * from {file("basic.csv", options=",sample_size=10")}', 1235)
    error('positional format rejected', f'select * from {file("basic.csv", options=",\'xml\'")}', 1064)
    error('unknown option', f'select * from {file("basic.csv", options=",unknown=true")}')
    error('path escape', f'select * from read_csv({literal(outside)})')
    error('dotdot escape', f'select * from read_csv({literal(root / ".." / outside.name)})')
    error('missing file', f'select * from {file("missing.csv")}')
    error('forged descriptor', "select * from table('FSQ1 invalid')")
    for name, content, function in [('broken.csv','a,b\n1\n','read_csv'), ('nested.jsonl','{"a":[]}\n','read_json'),
                                     ('badutf.csv',b'a\n\xff\n','read_csv')]:
        (root / name).write_bytes(content.encode() if isinstance(content,str) else content)
        error(name, f'select * from {file(name,function)}')
    equal('first bind', f'select count(*) from {file("changing.csv")}', [(1,)])
    (root / 'changing.csv').write_text('id\n1\n2\n', encoding='utf-8')
    equal('rebind changed file', f'select count(*) from {file("changing.csv")}', [(2,)])
    for name, options in [('export.csv', '(format csv,header,nullstr \'NULL\')'),
                           ('custom.csv', '(header,delimiter \';\',quote \'"\',escape \'"\',nullstr \'NA\')')]:
        target = root / name
        target.unlink(missing_ok=True)
        query(f'copy (select id,name,score from {csv_source} order by id) to {literal(target)} {options}')
        opts = '' if name=='export.csv' else ",delim=';',nullstr='NA'"
        equal(name+' round trip', f'select id,name,score from {file(name, options=opts)} order by id',
              [(1,'Alice',98.5),(2,'Bob',87.0),(3,'Chen',90.25)])
        before = target.read_bytes()
        error(name+' overwrite', f'copy (select 99 as id) to {literal(target)} (header)')
        if target.read_bytes()!=before: raise AssertionError('existing target changed')
    target = root / 'union.csv'; target.unlink(missing_ok=True)
    query(f'copy (select 1 as id union all select 2) to {literal(target)} (header)')
    equal('COPY UNION', f'select sum(id) from {file("union.csv")}', [(3,)])
    target = root / 'duplicate.csv'; target.unlink(missing_ok=True)
    query(f'copy (select id,id+10 as id from {csv_source} order by 1 desc) '
          f'to {literal(target)} (header)')
    with target.open(encoding='utf-8', newline='') as exported:
        actual = list(csv.reader(exported))
    if actual != [['id','id'],['3','13'],['2','12'],['1','11']]:
        raise AssertionError(f'COPY duplicate names/order: {actual!r}')
    results.append({'case': 'COPY duplicate names and descending order', 'status': 'pass'})
    print('PASS COPY duplicate names and descending order', flush=True)
    equal('COPY duplicate header readback',
          f'select id,id__2 from {file("duplicate.csv")} order by id', [(1,11),(2,12),(3,13)])
    target = root / 'duplicate_union.csv'; target.unlink(missing_ok=True)
    query(f'copy (select 1 as x,2 as x union all select 3,4) to {literal(target)} (header)')
    equal('COPY UNION duplicate names',
          f'select x,x__2 from {file("duplicate_union.csv")} order by x', [(1,2),(3,4)])
    inner = root / 'inner.csv'; inner.unlink(missing_ok=True)
    error('COPY source INTO', f'copy (select 1 into outfile {literal(inner)}) '
          f'to {literal(root / "outer.csv")}', 1064)
    if inner.exists(): raise AssertionError('COPY executed source INTO')
    query('drop table if exists file_sql_export_test')
    query('create table file_sql_export_test(id bigint,name varchar(50))')
    query("insert into file_sql_export_test values(1,'local'),(2,NULL)")
    target = root / 'table.csv'; target.unlink(missing_ok=True)
    query(f'copy file_sql_export_test to {literal(target)} (header)')
    equal('COPY table', f'select id,name from {file("table.csv")} order by id', [(1,'local'),(2,None)])
    forbidden = root.parent / 'file_sql_forbidden_export.csv'
    forbidden.unlink(missing_ok=True)
    error('COPY path escape', f'copy (select 1) to {literal(forbidden)}')
    if forbidden.exists(): raise AssertionError('COPY escaped secure root')
    aborted = root / 'aborted.csv'; aborted.unlink(missing_ok=True)
    error('COPY query error', f'copy (select missing_column from {csv_source}) to {literal(aborted)}')
    if aborted.exists() or list(root.glob('*.tmp')): raise AssertionError('failed COPY left output')
    (root / '数据.csv').write_text('编号,名称\n1,海洋\n2,数据库\n', encoding='utf-8')
    equal('UTF8 file and column names', f'select 名称 from {file("数据.csv")} where 编号=2', [('数据库',)])
    (root / 'semicolon.csv').write_bytes(b'1;"";NULL\r\n2;"NULL";3\r\n')
    equal('CSV options and quoted empty',
          f'select column1,column2,column3 from {file("semicolon.csv",options=",header=false,delimiter=\';\'")} order by column1',
          [(1,'',None),(2,'NULL',3)])
    (root / 'empty.csv').write_text('id,name\n', encoding='utf-8')
    equal('empty CSV with header', f'select count(*) from {file("empty.csv")}', [(0,)])
    null_schema = query(f'describe select allnull from {file("types.csv")}')
    if null_schema[0][1:3] != ('NULL','YES'): raise AssertionError(null_schema)
    results.append({'case':'all NULL metadata','status':'pass'})
    for copy_query in (False, True):
        mutable = root / 'mutable.csv'
        mutable.write_text('id\n'+''.join(f'{i}\n' for i in range(1000)), encoding='utf-8')
        partial = root / 'partial.csv'
        partial.unlink(missing_ok=True)
        mutation_errors=[]
        def mutate():
            try:
                with mutable.open('a',encoding='utf-8') as out: out.write('1001\n')
            except Exception as exc: mutation_errors.append(exc)
        timer=threading.Timer(0.3,mutate)
        sql=f'select id,sleep(0.002) as pause from {file("mutable.csv")}'
        if copy_query: sql=f'copy ({sql}) to {literal(partial)} (header)'
        timer.start()
        try: error('COPY mid-query cleanup' if copy_query else 'input mutation during scan', sql)
        finally: timer.join()
        if mutation_errors: raise mutation_errors[0]
        if partial.exists() or list(root.glob('*.tmp')): raise AssertionError('partial export leaked')
    large = root / 'oversize.csv'
    large.write_bytes(b'a\n'+b'x'*(16*1024*1024+1)+b'\n')
    error('record resource limit', f'select * from {file("oversize.csv")}')
    large.unlink()
    equal('server healthy after malformed input', 'select 7*9', [(63,)])
    for _ in range(50): query(f'select name from {csv_source} where id=1')
    results.append({'case':'50 consecutive scans','status':'pass'})
    query('set session enable_file_sql=OFF')
    error('gate after previous success', f'select count(*) from {csv_source}', 1235)
    error('COPY gate', f'copy (select 1) to {literal(root / "disabled.csv")}', 1235)
    equal('ordinary SQL after errors', 'select count(*),sum(id) from file_sql_export_test', [(2,3)])
finally:
    connection.close()
    args.report.write_text(json.dumps({'cases_passed':len(results),'results':results},
                                      ensure_ascii=False,default=str,indent=2), encoding='utf-8')
print('PASS',len(results),'native SQL cases')
