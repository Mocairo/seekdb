"""Deterministic AI protocol fixture; never calls an external model.

Run standalone on port 8000 or import MockAIService from the SQL integration tests.
Faults are selected by URL path so database code never needs fixture-specific logic.
"""
import hashlib
import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def embedding(text, dimension):
    seed = int.from_bytes(hashlib.sha256(text.encode('utf-8')).digest()[:4], 'big')
    return [((seed + i) % 8192) / 8192 for i in range(dimension)]


class MockAIService(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, address=('127.0.0.1', 8000)):
        super().__init__(address, Handler)
        self.events = []
        self.default_dimension = 8
        self.lock = threading.Lock()

    def clear(self):
        with self.lock:
            self.events.clear()


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *args):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        with self.server.lock:
            self.server.events.append({'path': self.path, 'body': body,
                                       'authorization': self.headers.get('Authorization'),
                                       'content_type': self.headers.get('Content-Type')})
            attempt = sum(event['path'] == self.path for event in self.server.events)
        fault = self.path.split('/')[1]
        if fault == 'http-error' or (fault == 'second-batch-failure' and attempt == 2):
            self.respond({'error': {'message': 'fixture failure'}}, 400)
            return
        if fault == 'disconnect':
            self.close_connection = True
            self.connection.close()
            return
        if fault == 'truncated-once' and attempt == 1:
            self.send_response(200)
            self.send_header('Content-Length', '10000')
            self.end_headers()
            self.wfile.write(b'{"partial":')
            self.wfile.flush()
            self.close_connection = True
            return
        if fault == 'invalid-json':
            self.respond(b'{broken')
            return
        if fault == 'wrong-root':
            self.respond([])
            return
        if self.path.endswith('/embeddings') or self.path.endswith('/api/embed'):
            inputs = body['input']
            if not isinstance(inputs, list):
                inputs = [inputs]
            dimension = body.get('dimensions', self.server.default_dimension)
            vectors = [embedding(text, dimension) for text in inputs]
            if fault == 'wrong-dimension':
                vectors[0].append(0)
            if fault == 'inconsistent-dimensions' and len(vectors) > 1:
                vectors[-1].append(0)
            if fault == 'empty-vector':
                vectors[0] = []
            if fault == 'string-element':
                vectors[0][0] = 'bad'
            if fault == 'null-element':
                vectors[0][0] = None
            if fault == 'bool-element':
                vectors[0][0] = True
            if fault == 'infinite-element':
                vectors[0][0] = float('inf')
            if self.path.endswith('/api/embed'):
                response = {'embeddings': vectors}
            else:
                data = [{'index': i, 'embedding': v} for i, v in enumerate(vectors)]
                if fault == 'duplicate-index' and len(data) > 1:
                    data[-1]['index'] = 0
                if fault == 'bad-index':
                    data[0]['index'] = len(data)
                if fault == 'missing-index':
                    del data[0]['index']
                if fault == 'string-index':
                    data[0]['index'] = '0'
                if fault == 'missing-vector':
                    del data[0]['embedding']
                if fault == 'wrong-vector':
                    data[0]['embedding'] = {'x': 1}
                if fault == 'missing-result':
                    data.pop()
                response = {'data': list(reversed(data))}
                if fault == 'wrong-data':
                    response['data'] = {}
        elif self.path.endswith('/rerank'):
            docs = body['documents']
            # Equal-length documents deliberately tie. Return unsorted items.
            results = [{'index': i, 'relevance_score': (10 if text == body['query'] else 0) + len(text) % 5}
                       for i, text in enumerate(docs)]
            if fault == 'duplicate-index' and len(results) > 1:
                results[-1]['index'] = 0
            if fault == 'bad-index':
                results[0]['index'] = -1
            if fault == 'bad-score':
                results[0]['relevance_score'] = 'bad'
            if fault == 'missing-result':
                results.pop()
            response = {'results': list(reversed(results))}
        else:
            ollama = self.path.endswith('/api/generate')
            prompt = body['prompt'] if ollama else body['messages'][-1]['content']
            content = prompt
            if fault == 'empty-content':
                content = ''
            if fault == 'null-content':
                content = None
            if fault == 'wrong-content':
                content = {'text': 'bad'}
            response = {'response': content} if ollama else {'choices': [{'message': {'content': content}}]}
            if fault == 'missing-content':
                response = {}
        self.respond(response)

    def respond(self, response, status=200):
        payload = response if isinstance(response, bytes) else json.dumps(response, ensure_ascii=False).encode('utf-8')
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)


if __name__ == '__main__':
    MockAIService(('0.0.0.0', 8000)).serve_forever()
