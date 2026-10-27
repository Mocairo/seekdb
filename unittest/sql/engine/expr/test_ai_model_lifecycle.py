"""Reproduce the first AI-management call without prewarming DBMS_AI_SERVICE.

Run against a disposable seekdb immediately after startup. Unlike the larger
AI regression suite, this keeps the query timeout at the contest's 10 seconds
and does not create any AI resource before the first DROP_AI_MODEL call.
Requires PyMySQL; reads the usual SEEKDB_TEST_* connection variables.
"""
import json
import os
import time
import uuid

import pymysql


def main():
    suffix = uuid.uuid4().hex[:12]
    database = 'ai_lifecycle_' + suffix
    model = 'ai_lifecycle_' + suffix
    db = pymysql.connect(
        host=os.getenv('SEEKDB_TEST_HOST', '127.0.0.1'),
        port=int(os.getenv('SEEKDB_TEST_PORT', '2883')),
        user=os.getenv('SEEKDB_TEST_USER', 'root'),
        password=os.getenv('SEEKDB_TEST_PASSWORD', ''),
        charset='utf8mb4', autocommit=True, connect_timeout=5, read_timeout=20)
    cursor = db.cursor()

    def execute(label, sql, args=None, expected_errno=0):
        started = time.monotonic()
        errno, message = 0, ''
        rows = None
        try:
            cursor.execute(sql, args)
            rows = cursor.fetchall()
        except pymysql.Error as error:
            errno, message = error.args
        print(json.dumps({'case': label, 'seconds': round(time.monotonic() - started, 6),
                          'errno': errno, 'expected_errno': expected_errno,
                          'message': message}, ensure_ascii=False), flush=True)
        if errno != expected_errno:
            raise AssertionError(f'{label}: expected errno {expected_errno}, got {errno}: {message}')
        return rows

    try:
        execute('timeout', 'set ob_query_timeout=10000000')
        execute('create database', 'create database ' + database)
        execute('use database', 'use ' + database)
        execute('JSON prompt', "select ai_prompt('{0}/{0}/{1}',json_array(1,true),json_object('k',2))")
        execute('create LOB table', 'create table prompt_lob(t longtext,a text)')
        execute('insert LOB', "insert into prompt_lob values('{0}-ok','lob')")
        execute('LOB prompt', 'select ai_prompt(t,a) from prompt_lob')
        execute('drop LOB table', 'drop table prompt_lob')
        execute('first missing-model DROP', 'call DBMS_AI_SERVICE.DROP_AI_MODEL(%s)',
                (model,), expected_errno=11118)
        for attempt in range(3):
            execute(f'create model {attempt}', 'call DBMS_AI_SERVICE.CREATE_AI_MODEL(%s,%s)',
                    (model, json.dumps({'type': 'completion', 'model_name': 'lifecycle-test'})))
            execute(f'drop existing model {attempt}', 'call DBMS_AI_SERVICE.DROP_AI_MODEL(%s)', (model,))
            execute(f'drop missing model {attempt}', 'call DBMS_AI_SERVICE.DROP_AI_MODEL(%s)',
                    (model,), expected_errno=11118)
        execute('ordinary SQL after errors', 'select 1')
        execute('drop database', 'drop database ' + database)
    finally:
        # On a failure, retain the isolated objects for inspection rather than
        # hiding the original error with a second failing cleanup operation.
        db.close()


if __name__ == '__main__':
    main()
