#!/usr/bin/env python3
"""Owned local HTTP fixture. No external network, real model, or uploads."""
import argparse
import json
import os
import signal
from pathlib import Path
import sys
import time
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer

if os.environ.get('YAN_TEST_CHILD_LOG'):
    with Path(os.environ['YAN_TEST_CHILD_LOG']).open('a') as log:
        log.write('version\n' if '--version' in sys.argv else 'server\n')
if '--version' in sys.argv:
    # Explicit MCP integration control. A runner must inherit EOF on stdin,
    # and no private result/control pipe may survive exec. Other suites do
    # not enable this check or change their fixed runner version string.
    if os.environ.get('YAN_MCP_FD_CHECK'):
        assert os.read(0, 1) == b'', 'runner inherited MCP input'
        for entry in Path('/proc/self/fd').iterdir():
            if int(entry.name) < 3:
                continue
            try:
                target = entry.readlink()
            except FileNotFoundError:
                continue  # the directory enumeration's own fd
            assert not str(target).startswith('pipe:'), 'runner inherited private pipe'
    version_mode = os.environ.get('YAN_TEST_VERSION_MODE','normal')
    if version_mode == 'empty': raise SystemExit(0)
    if version_mode == 'long': print('x'*2048); raise SystemExit(0)
    if version_mode == 'timeout':
        Path(os.environ['YAN_TEST_PID_FILE']).write_text(str(os.getpid()))
        def term(*_):
            Path(os.environ['YAN_TEST_PID_FILE']+'.term').write_text('TERM')
            raise SystemExit(0)
        signal.signal(signal.SIGTERM,term)
        time.sleep(10)
    print('yanos-test-runner 0029-v1')
    raise SystemExit(0)
parser = argparse.ArgumentParser(add_help=False)
parser.add_argument('--model', required=True)
parser.add_argument('--port', type=int, required=True)
parser.add_argument('--host', required=True)
parser.add_argument('--api-key', required=True)
args, _ = parser.parse_known_args()
assert args.host == '127.0.0.1'
if os.environ.get('YAN_TEST_SERVER_PID_FILE'):
    Path(os.environ['YAN_TEST_SERVER_PID_FILE']).write_text(str(os.getpid()))
if os.environ.get('YAN_TEST_FORK_TREE'):
    child = os.fork()
    if child == 0:
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        grandchild = os.fork()
        fd = os.open(os.environ['YAN_TEST_DESCENDANTS_FILE'], os.O_WRONLY|os.O_APPEND|os.O_CREAT, 0o600)
        os.write(fd, (str(os.getpid())+'\n').encode()); os.close(fd)
        if grandchild == 0:
            signal.signal(signal.SIGTERM, signal.SIG_IGN)
        while True:
            time.sleep(30)
mode = Path(args.model).read_text().strip()
if mode == 'ignore_term' or os.environ.get('YAN_TEST_IGNORE_TERM'):
    signal.signal(signal.SIGTERM,signal.SIG_IGN)
if mode == 'early_exit': raise SystemExit(9)
if mode == 'startup_timeout': time.sleep(10)

class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_): pass
    def reply(self, data, status=200, location=None):
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        if location: self.send_header('Location', location)
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        try: self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError): pass
    def authorized(self):
        return self.headers.get('Authorization') == 'Bearer ' + args.api_key
    def do_GET(self):
        if not self.authorized() or mode == 'wrong_auth': self.reply(b'{}',401)
        else: self.reply(b'{"status":"ok"}')
    def do_POST(self):
        if not self.authorized(): self.reply(b'{}',401); return
        data = self.rfile.read(int(self.headers.get('Content-Length','0')))
        try:
            value = json.loads(data)
            assert isinstance(value['input'], str)
            if mode not in ('universal', 'precision'): assert value['input'] == 'hello中'
        except (ValueError, KeyError, AssertionError):
            self.reply(b'{}',400); return
        if mode == 'timeout': time.sleep(10)
        if os.environ.get('YAN_TEST_EMBED_FAILURE'):
            self.reply(b'{bad json'); return
        if os.environ.get('YAN_TEST_EMBED_DELAY'):
            time.sleep(float(os.environ['YAN_TEST_EMBED_DELAY']))
        if mode == 'redirect': self.reply(b'{}',302,'http://127.0.0.1:9/no'); return
        if mode == 'http_error': self.reply(b'{}',500); return
        if mode == 'malformed': self.reply(b'{not json'); return
        if mode == 'oversize': self.reply(b' '*(1024*1024+1)); return
        vector = [0.0]*1024
        vector[0] = 1.0
        if mode == 'precision' and not value['input'].startswith('Instruct:'):
            vector[1] = 2.0
        if mode == 'dimension': vector.pop()
        if mode == 'zero': vector[0] = 0.0
        if mode == 'nan': vector[0] = float('nan')
        if mode == 'huge': vector[0] = 1e300
        if mode == 'underflow': vector[0] = 1e-100
        record = {'object':'embedding','index':1 if mode=='index' else 0, 'embedding':vector}
        records = [record, record] if mode=='extra' else [record]
        self.reply(json.dumps({'data':records}).encode())
        if mode == 'exit_after_response': os._exit(9)
        if mode == 'delayed_exit': threading.Timer(0.05, lambda: os._exit(9)).start()

HTTPServer(('127.0.0.1',args.port),Handler).serve_forever()
