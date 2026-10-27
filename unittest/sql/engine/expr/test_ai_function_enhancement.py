"""SQL integration regressions against a locally built seekdb and HTTP fixture.

Requires PyMySQL. Run with SEEKDB_TEST_PORT (default 2883), SEEKDB_TEST_USER
(default root) and optional SEEKDB_TEST_PASSWORD. Uses a dedicated database.
"""
import json
import os
import threading
import unittest
from pathlib import Path
import pymysql
from ai_mock_service import MockAIService, embedding


class AIFunctionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.mock = MockAIService(('127.0.0.1', 0))
        cls.thread = threading.Thread(target=cls.mock.serve_forever, daemon=True)
        cls.thread.start()
        cls.db = pymysql.connect(host='127.0.0.1', port=int(os.getenv('SEEKDB_TEST_PORT', '2883')),
                                 user=os.getenv('SEEKDB_TEST_USER', 'root'),
                                 password=os.getenv('SEEKDB_TEST_PASSWORD', ''),
                                 charset='utf8mb4', autocommit=True, read_timeout=60)
        cls.cursor = cls.db.cursor()
        cls.cursor.execute('create database if not exists ai_enhancement_regression')
        cls.cursor.execute('use ai_enhancement_regression')
        cls.cursor.execute('set ob_query_timeout=60000000')
        for name, kind in [('complete', 'completion'), ('embed', 'dense_embedding'), ('rerank', 'rerank')]:
            for method, suffix in [('DROP_AI_MODEL_ENDPOINT', '_ep'), ('DROP_AI_MODEL', '')]:
                try:
                    cls.cursor.execute('call DBMS_AI_SERVICE.' + method + '(%s)', ('reg_' + name + suffix,))
                except pymysql.Error:
                    pass
            cls.cursor.execute('call DBMS_AI_SERVICE.CREATE_AI_MODEL(%s,%s)',
                               ('reg_' + name, json.dumps({'type': kind, 'model_name': 'fixture-' + name})))
            cls.cursor.execute('call DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)',
                               ('reg_' + name + '_ep', json.dumps({'ai_model_name': 'reg_' + name,
                                'scope': 'all', 'url': 'http://127.0.0.1:' + str(cls.mock.server_port) + '/v1/' + name,
                                'access_key': 'test-key', 'provider': 'siliconflow' if name == 'rerank' else 'openai'})))

    @classmethod
    def tearDownClass(cls):
        for name in ['complete', 'embed', 'rerank']:
            cls.cursor.execute('call DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT(%s)', ('reg_' + name + '_ep',))
            cls.cursor.execute('call DBMS_AI_SERVICE.DROP_AI_MODEL(%s)', ('reg_' + name,))
        cls.cursor.execute('drop database ai_enhancement_regression')
        cls.db.close()
        cls.mock.shutdown()
        cls.mock.server_close()
        cls.thread.join()

    def setUp(self):
        for kind, suffix in [('complete', '/chat/completions'), ('embed', '/embeddings'), ('rerank', '/rerank')]:
            self.endpoint(kind, 'ok', suffix)
        self.mock.clear()

    def endpoint(self, kind, fault, suffix=None, provider=None, key='test-key'):
        suffix = suffix or {'complete': '/chat/completions', 'embed': '/embeddings', 'rerank': '/rerank'}[kind]
        config = {'url': 'http://127.0.0.1:' + str(self.mock.server_port) + '/' + fault + suffix, 'access_key': key,
                  'provider': provider or ('siliconflow' if kind == 'rerank' else 'openai')}
        self.cursor.execute('call DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT(%s,%s)',
                            ('reg_' + kind + '_ep', json.dumps(config)))

    def scalar(self, sql, args=None):
        self.cursor.execute(sql, args)
        return self.cursor.fetchone()[0]

    def completion(self, prompt_expr='%s', args=('hello',), config='null'):
        value = self.scalar("select ai_complete('reg_complete', " + prompt_expr + ', ' + config + ')', args)
        return value

    def test_prompt_json_repeat_nested_unicode(self):
        self.assertEqual('你好/你好/x', self.completion("ai_prompt('{0}/{0}/{1}', '你好', 'x')", None))
        result = self.completion("ai_prompt('{0}|{1}', json_array(1,true), json_object('k',2))", None)
        left, right = result.split('|')
        self.assertEqual([1, True], json.loads(left))
        self.assertEqual({'k': 2}, json.loads(right))
        self.assertEqual('outer inner value', self.completion("ai_prompt('outer {0}', ai_prompt('inner {0}', 'value'))", None))
        text = '引号"\\换行\n\x00尾'
        self.assertEqual(text + text, self.completion("ai_prompt('{0}{0}', %s)", (text,)))

    def test_prompt_longtext_and_repeated_expansion(self):
        self.cursor.execute('create table prompt_lob(t longtext, a longtext)')
        try:
            text = '中文' * 20000
            self.cursor.execute('insert into prompt_lob values(%s,%s)', ('{0}/{0}/{0}', text))
            output = self.scalar("select ai_complete('reg_complete', ai_prompt(t,a)) from prompt_lob")
            self.assertEqual('/'.join([text] * 3), output)
        finally:
            self.cursor.execute('drop table prompt_lob')

    def test_prompt_invalid_indices(self):
        for template in ['{1}', '{9999999999999999999999999999999}']:
            with self.subTest(template=template), self.assertRaises(pymysql.Error):
                self.completion("ai_prompt(%s,'x')", (template,))
        stored = json.loads(self.scalar("select ai_prompt('{0}/{1}','x')"))
        self.assertEqual({'template': '{0}/{1}', 'args': ['x']}, stored)

    def test_completion_configs(self):
        self.assertEqual('hello', self.completion())
        for config in ["json_object('temperature',0.2,'max_tokens',16)", "'{\"temperature\":0.2,\"max_tokens\":16}'"]:
            self.assertEqual('hello', self.completion(config=config))
            body = self.mock.events[-1]['body']
            self.assertEqual(0.2, body['temperature'])
            self.assertEqual(16, body['max_tokens'])
        for config in ["json_array(1)", "'bad-json'"]:
            with self.subTest(config=config), self.assertRaises(pymysql.Error):
                self.completion(config=config)

    def test_completion_errors(self):
        for fault in ['empty-content', 'null-content', 'wrong-content', 'missing-content',
                      'wrong-root', 'invalid-json', 'http-error', 'disconnect']:
            self.endpoint('complete', fault)
            with self.subTest(fault=fault), self.assertRaises(pymysql.Error):
                self.completion()
        self.assertEqual(2, self.scalar('select 1+1'))
        self.endpoint('complete', 'truncated-once')
        self.assertEqual('hello', self.completion())
        self.assertEqual(2, sum(e['path'].startswith('/truncated-once/') for e in self.mock.events))

    def test_embedding_dimensions_and_null(self):
        for dim in [None, 1, 7, 768]:
            sql = "select ai_embed('reg_embed','hello'" + ('' if dim is None else ',' + str(dim)) + ')'
            result = json.loads(self.scalar(sql))
            self.assertEqual(embedding('hello', dim or 8), result)
        before = len(self.mock.events)
        self.assertIsNone(self.scalar("select ai_embed('reg_embed',null)"))
        self.assertEqual(before, len(self.mock.events))
        for dim in ['0', '-1', '1.5', 'null']:
            with self.subTest(dimension=dim), self.assertRaises(pymysql.Error):
                self.scalar("select ai_embed('reg_embed','hello'," + dim + ')')

    def test_embedding_invalid_responses(self):
        for fault in ['wrong-dimension', 'empty-vector', 'string-element', 'null-element', 'bool-element',
                      'infinite-element', 'bad-index', 'missing-index', 'string-index', 'missing-vector',
                      'wrong-vector', 'missing-result', 'wrong-data', 'wrong-root', 'invalid-json', 'http-error']:
            self.endpoint('embed', fault)
            with self.subTest(fault=fault), self.assertRaises(pymysql.Error):
                self.scalar("select ai_embed('reg_embed','hello',8)")

    def test_embedding_batch_mapping_filters_limit(self):
        self.cursor.execute('create table batch_input(id int primary key, content text, dim int)')
        rows = [(i, None if i % 7 == 0 else 'row-' + str(i), 3 + i % 2) for i in range(150)]
        try:
            self.cursor.executemany('insert into batch_input values(%s,%s,%s)', rows)
            for clause in ['', ' where id % 3 = 0', ' where id >= 17 limit 19']:
                self.mock.clear()
                self.cursor.execute("select id,content,dim,ai_embed('reg_embed',content,dim) from batch_input" + clause)
                output = self.cursor.fetchall()
                expected_ids = list(range(150)) if not clause else (
                    list(range(0, 150, 3)) if '% 3' in clause else list(range(17, 36)))
                self.assertEqual(expected_ids, [row[0] for row in output])
                for _, content, dim, vector in output:
                    self.assertEqual(None if content is None else embedding(content, dim),
                                     None if vector is None else json.loads(vector))
                self.assertTrue(any(len(e['body']['input']) > 1 for e in self.mock.events), self.mock.events)
                self.assertTrue(all(None not in e['body']['input'] for e in self.mock.events))
                requested = [text for e in self.mock.events for text in e['body']['input']]
                self.assertCountEqual([row[1] for row in output if row[1] is not None], requested)
            self.endpoint('embed', 'duplicate-index')
            with self.assertRaises(pymysql.Error):
                self.cursor.execute("select ai_embed('reg_embed',content) from batch_input")
            self.endpoint('embed', 'inconsistent-dimensions')
            with self.assertRaises(pymysql.Error):
                self.cursor.execute("select ai_embed('reg_embed',content) from batch_input")
            self.mock.clear()
            self.endpoint('embed', 'second-batch-failure')
            with self.assertRaises(pymysql.Error):
                self.cursor.execute("select ai_embed('reg_embed',content) from batch_input")
            self.assertEqual(2, len(self.mock.events))
            self.endpoint('embed', 'ok')
            self.mock.clear()
            self.cursor.execute("select ai_embed('reg_embed',content) from batch_input where content is null")
            self.assertEqual([(None,)] * 22, list(self.cursor.fetchall()))
            self.cursor.execute("select ai_embed('reg_embed',content) from batch_input limit 0")
            self.assertEqual((), self.cursor.fetchall())
            self.assertEqual([], self.mock.events)
            self.assertEqual(embedding('recovery', 8), json.loads(self.scalar("select ai_embed('reg_embed','recovery')")))
        finally:
            self.cursor.execute('drop table batch_input')

    def test_rerank_global_order_and_ids(self):
        texts = ['x' * (1 + i % 11) for i in range(47)]
        expected = sorted(range(len(texts)), key=lambda i: (-(len(texts[i]) % 5), i))
        output = json.loads(self.scalar("select ai_rerank('reg_rerank','query',%s,null)", (json.dumps(texts),)))
        self.assertEqual(expected, [item['index'] for item in output])
        docs = [{'id': 'id-' + str(i), 'body': text, 'other': i} for i, text in enumerate(texts)]
        output = json.loads(self.scalar("select ai_rerank('reg_rerank','query',%s,'body')", (json.dumps(docs),)))
        self.assertEqual(['id-' + str(i) for i in expected], [item['id'] for item in output])
        self.assertEqual(expected, [item['other'] for item in output])
        self.assertEqual([20, 20, 7, 20, 20, 7], [len(e['body']['documents']) for e in self.mock.events])

    def test_embedding_multiple_sql_batches_and_models(self):
        self.cursor.execute("call DBMS_AI_SERVICE.CREATE_AI_MODEL('reg_embed_alt','{\"type\":\"dense_embedding\",\"model_name\":\"fixture-alternate\"}')")
        config = {'ai_model_name': 'reg_embed_alt', 'scope': 'all', 'access_key': 'key', 'provider': 'OPENAI',
                  'url': 'http://127.0.0.1:' + str(self.mock.server_port) + '/ok/embeddings'}
        self.cursor.execute('call DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)',
                            ('reg_embed_alt_ep', json.dumps(config)))
        self.cursor.execute('create table batch_models(id int primary key, model varchar(64), content text, dim int)')
        try:
            rows = [(i, 'reg_embed_alt' if i % 3 else 'reg_embed',
                     None if i % 11 == 0 else 'multi-' + str(i), 2 + i % 4) for i in range(1153)]
            self.cursor.executemany('insert into batch_models values(%s,%s,%s,%s)', rows)
            self.mock.clear()
            self.cursor.execute('select id, ai_embed(model,content,dim) from batch_models order by id')
            output = self.cursor.fetchall()
            self.assertEqual(list(range(1153)), [row[0] for row in output])
            for (i, vector), (_, model, content, dim) in zip(output, rows):
                self.assertEqual(None if content is None else embedding(content, dim),
                                 None if vector is None else json.loads(vector))
            for event in self.mock.events:
                self.assertLessEqual(len(event['body']['input']), 64)
                for content in event['body']['input']:
                    i = int(content.split('-')[1])
                    self.assertEqual('fixture-alternate' if i % 3 else 'fixture-embed', event['body']['model'])
                    self.assertEqual(2 + i % 4, event['body']['dimensions'])
            sizes = [len(event['body']['input']) for event in self.mock.events]
            self.assertEqual(1048, sum(sizes))
            self.assertGreater(max(sizes), 1)
            print('\n1153 rows: %d requests, %d non-NULL inputs, max %d inputs/request' % (len(sizes), sum(sizes), max(sizes)))
        finally:
            self.cursor.execute('drop table batch_models')
            self.cursor.execute("call DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('reg_embed_alt_ep')")
            self.cursor.execute("call DBMS_AI_SERVICE.DROP_AI_MODEL('reg_embed_alt')")

    def test_rerank_invalid_responses(self):
        for fault in ['duplicate-index', 'bad-index', 'bad-score', 'missing-result', 'wrong-root']:
            self.endpoint('rerank', fault)
            with self.subTest(fault=fault), self.assertRaises(pymysql.Error):
                self.scalar("select ai_rerank('reg_rerank','query','[\"one\",\"two\"]',null)")

    def test_ollama_no_key_and_protocol(self):
        for provider in ['ollama', 'Ollama', 'OLLAMA']:
            self.endpoint('complete', 'ok', '/api/generate', provider, '')
            self.assertEqual('hello', self.completion())
            event = self.mock.events[-1]
            self.assertIs(event['body']['stream'], False)
            self.assertIsNone(event['authorization'])
            self.assertEqual('application/json', event['content_type'])
        self.endpoint('embed', 'ok', '/api/embed', 'OLLAMA', '')
        self.assertEqual(embedding('hello', 8), json.loads(self.scalar("select ai_embed('reg_embed','hello')")))
        self.assertEqual('hello', self.completion(config="json_object('stream',true)"))
        self.assertIs(self.mock.events[-1]['body']['stream'], False)
        self.assertEqual('hello', self.completion(config="json_object('temperature',0.2,'max_tokens',16)"))
        self.assertEqual({'temperature': 0.2, 'num_predict': 16}, self.mock.events[-1]['body']['options'])
        for fault in ['empty-content', 'wrong-content', 'wrong-root']:
            self.endpoint('complete', fault, '/api/generate', 'ollama', '')
            with self.subTest(fault=fault), self.assertRaises(pymysql.Error):
                self.completion()
        for fault in ['string-element', 'empty-vector', 'wrong-root']:
            self.endpoint('embed', fault, '/api/embed', 'ollama', '')
            with self.subTest(fault=fault), self.assertRaises(pymysql.Error):
                self.scalar("select ai_embed('reg_embed','hello')")

    def test_endpoint_create_without_key_and_change_provider(self):
        config = {'ai_model_name': 'reg_keyless_model', 'scope': 'all',
                  'url': 'http://127.0.0.1:' + str(self.mock.server_port) + '/ok/api/generate',
                  'provider': 'oLlAmA'}
        self.cursor.execute("call DBMS_AI_SERVICE.CREATE_AI_MODEL('reg_keyless_model','{\"type\":\"completion\",\"model_name\":\"fixture-keyless\"}')")
        self.cursor.execute('call DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)',
                            ('reg_keyless_ep', json.dumps(config)))
        try:
            self.assertEqual('hello', self.scalar("select ai_complete('reg_keyless_model','hello')"))
            self.assertEqual('oLlAmA', self.scalar("select provider from oceanbase.DBA_OB_AI_MODEL_ENDPOINTS where endpoint_name='reg_keyless_ep'"))
            with self.assertRaises(pymysql.Error):
                self.cursor.execute("call DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT('reg_keyless_ep','{\"provider\":\"openai\"}')")
            self.cursor.execute("call DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT('reg_keyless_ep','{\"provider\":\"openai\",\"access_key\":\"key\"}')")
            self.assertEqual('openai', self.scalar("select provider from oceanbase.DBA_OB_AI_MODEL_ENDPOINTS where endpoint_name='reg_keyless_ep'"))
        finally:
            self.cursor.execute("call DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('reg_keyless_ep')")
            self.cursor.execute("call DBMS_AI_SERVICE.DROP_AI_MODEL('reg_keyless_model')")

    def test_existing_mysqltest_statements(self):
        """Execute the two supplied SQL suites, honoring their expected errors.

        This is a PyMySQL adapter, not the mysqltest executable. Only the
        connection/warning/error directives used by these suites are supported.
        """
        suite = Path(__file__).resolve().parents[4] / 'tools/deploy/mysql_test/test_suite/ai_function'
        self.mock.default_dimension = 768
        try:
            for name in ['ai_prompt', 'ai_function_enhancement']:
                pending = []
                expected_errors = {0}
                count = 0
                for line in (suite / 't' / (name + '.test')).read_text(encoding='utf-8').splitlines():
                    line = line.strip()
                    if not line or line.startswith('#') or line == 'connection default;':
                        continue
                    if line.startswith('--error '):
                        expected_errors = {int(value) for value in line[8:].split(',')}
                        continue
                    if line in ['--disable_warnings', '--enable_warnings']:
                        continue
                    self.assertFalse(line.startswith('--'), 'Unsupported mysqltest directive: ' + line)
                    pending.append(line)
                    if not line.endswith(';'):
                        continue
                    statement = '\n'.join(pending)
                    pending.clear()
                    statement = statement.replace('http://0.0.0.0:8000', 'http://127.0.0.1:' + str(self.mock.server_port))
                    with self.subTest(suite=name, sql=statement):
                        try:
                            self.cursor.execute(statement)
                            self.cursor.fetchall()
                        except pymysql.Error as error:
                            self.assertIn(error.args[0], expected_errors)
                        else:
                            self.assertIn(0, expected_errors)
                    expected_errors = {0}
                    count += 1
                self.assertFalse(pending)
                print('\n%s: %d SQL statements checked' % (name, count))
        finally:
            self.mock.default_dimension = 8


if __name__ == '__main__':
    unittest.main(verbosity=2)
