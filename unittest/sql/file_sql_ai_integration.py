"""Check AI expressions and COPY over native file scans with a local HTTP fixture."""
import argparse
import json
from pathlib import Path
import sys
import threading
import uuid

import pymysql

sys.path.insert(0, str(Path(__file__).parent/'engine/expr'))
from ai_mock_service import MockAIService, embedding


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, required=True)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    assert root.is_dir(), 'Use the dedicated secure_file_priv directory'
    suffix = uuid.uuid4().hex[:12]
    source, target = (root/f'combined-{suffix}.{ext}' for ext in ('csv','out.csv'))
    source.write_text('id,content\n1,alpha\n2,beta\n3,NULL\n', encoding='utf-8')
    mock = MockAIService(('127.0.0.1', 0))
    thread = threading.Thread(target=mock.serve_forever, daemon=True)
    thread.start()
    connection = pymysql.connect(host='127.0.0.1', port=args.port, user='root',
                                 database='test', autocommit=True, charset='utf8mb4', read_timeout=65)
    model, endpoint = 'merge_'+suffix, 'merge_ep_'+suffix
    created_model = created_endpoint = False
    results = []

    def literal(value):
        return "'"+str(value).replace('\\','/').replace("'","''")+"'"

    try:
        with connection.cursor() as cursor:
            cursor.execute('set session enable_file_sql=ON')
            cursor.execute('set ob_query_timeout=60000000')
            scan = f'read_csv({literal(source)})'
            cursor.execute(f"select id,ai_prompt('row {{0}}: {{1}}',cast(id as char),content) from {scan} "
                           'where content is not null order by id')
            expected = [(1,{'template':'row {0}: {1}','args':['1','alpha']}),
                        (2,{'template':'row {0}: {1}','args':['2','beta']})]
            assert [(row_id,json.loads(prompt)) for row_id,prompt in cursor.fetchall()] == expected
            results.append('AI_PROMPT over CSV columns')
            cursor.execute('call DBMS_AI_SERVICE.CREATE_AI_MODEL(%s,%s)',
                           (model,json.dumps({'type':'dense_embedding','model_name':'merge-fixture'})))
            created_model = True
            cursor.execute('call DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)',
                           (endpoint,json.dumps({'ai_model_name':model,'scope':'all','provider':'openai',
                            'url':f'http://127.0.0.1:{mock.server_port}/ok/embeddings','access_key':'fixture-key'})))
            created_endpoint = True
            cursor.execute(f'select id,ai_embed(%s,content,4) from {scan} order by id',(model,))
            actual = [(row_id,None if vector is None else json.loads(vector))
                      for row_id,vector in cursor.fetchall()]
            assert actual == [(1,embedding('alpha',4)),(2,embedding('beta',4)),(3,None)]
            results.append('AI_EMBED over CSV columns with NULL')
            requested = [text for event in mock.events for text in event['body']['input']]
            assert sorted(requested) == ['alpha','beta']
            results.append('HTTP inputs preserve file-row mapping and skip NULL')
            cursor.execute(f"copy (select id,ai_prompt('row {{0}}: {{1}}',cast(id as char),content) as label "
                           f'from {scan} where content is not null order by id) '
                           f'to {literal(target)} (header)')
            cursor.execute(f'select id,label from read_csv({literal(target)}) order by id')
            assert [(row_id,json.loads(prompt)) for row_id,prompt in cursor.fetchall()] == expected
            results.append('COPY of AI expressions and CSV readback')
    finally:
        with connection.cursor() as cursor:
            if created_endpoint:
                cursor.execute('call DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT(%s)',(endpoint,))
            if created_model:
                cursor.execute('call DBMS_AI_SERVICE.DROP_AI_MODEL(%s)',(model,))
        connection.close()
        mock.shutdown()
        mock.server_close()
        thread.join(timeout=5)
        source.unlink(missing_ok=True)
        target.unlink(missing_ok=True)
        args.report.write_text(json.dumps({'cases_passed':len(results),'cases':results},indent=2),encoding='utf-8')
    print('PASS',len(results),'combined AI/File SQL cases',flush=True)


if __name__ == '__main__':
    main()
