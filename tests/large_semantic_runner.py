#!/usr/bin/env python3
"""Deterministic loopback embedding runner for the 0036-R4/R6 acceptance.

This is a protocol fixture, not a model: it proves the semantic path carries a
1023-slot source into rebuild/search, and it makes the slot-1022 note the clear
top hit without relying on equal-vector tie order.  A real model is the root
reviewer's separate acceptance; no quality claim is made here.

The runner speaks the same `--version` and OpenAI-compatible loopback HTTP
surface as tests/semantic_fake_runner.py.  Each ASCII word maps to a fixed
1024-dimension bucket; the query prefix and the fixture vocabulary are kept in
non-overlapping bucket ranges so a word unique to slot 1022 is also unique in
the vector.

No network beyond 127.0.0.1, no child processes, no uploads.
"""

import argparse
import hashlib
import json
import os
import signal
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
import sys

if os.environ.get("YAN_TEST_CHILD_LOG"):
    with Path(os.environ["YAN_TEST_CHILD_LOG"]).open("a") as log:
        log.write("version\n" if "--version" in sys.argv else "server\n")

if "--version" in sys.argv:
    print("yanos-large-runner 0036-r6-v1")
    raise SystemExit(0)

parser = argparse.ArgumentParser(add_help=False)
parser.add_argument("--model", required=True)
parser.add_argument("--port", type=int, required=True)
parser.add_argument("--host", required=True)
parser.add_argument("--api-key", required=True)
args, _ = parser.parse_known_args()
assert args.host == "127.0.0.1"

if os.environ.get("YAN_TEST_SERVER_PID_FILE"):
    Path(os.environ["YAN_TEST_SERVER_PID_FILE"]).write_text(str(os.getpid()))


def term_bucket(word):
    explicit = {
        "slotlastuniqueword": 1000,
        "notes": 1001,
        "yan": 900, "note": 901, "title": 902, "link": 903, "tag": 904,
        "end": 905, "body": 906, "common": 907, "shared": 908, "txt": 909,
        "anchor": 910, "block": 911, "related": 912, "source": 913,
        "filler": 914, "of": 915, "the": 916, "final": 917, "slot": 918,
        "zslot": 919, "slottag1022": 920, "sharedtag": 921,
    }
    if word in explicit:
        return explicit[word]
    if len(word) == 4 and word[0] == "w" and word[1:].isdigit():
        return 200 + int(word[1:])
    return int(hashlib.sha256(word.encode()).hexdigest()[:8], 16) % 190


def word_vector(text):
    vector = [0.0] * 1024
    word = []
    for char in text:
        if char.isascii() and (char.isalnum() or char == "_"):
            word.append(char.lower())
        elif word:
            vector[term_bucket("".join(word))] = 1.0
            word = []
    if word:
        vector[term_bucket("".join(word))] = 1.0
    if not any(vector):
        vector[0] = 1.0
    return vector


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def reply(self, data, status=200):
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        try:
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def authorized(self):
        return self.headers.get("Authorization") == "Bearer " + args.api_key

    def do_GET(self):
        if not self.authorized():
            self.reply(b"{}", 401)
        else:
            self.reply(b'{"status":"ok"}')

    def do_POST(self):
        if not self.authorized():
            self.reply(b"{}", 401)
            return
        data = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        try:
            value = json.loads(data)
            text = value["input"]
            assert isinstance(text, str)
        except (ValueError, KeyError, AssertionError):
            self.reply(b"{}", 400)
            return
        record = {"object": "embedding", "index": 0,
                  "embedding": word_vector(text)}
        self.reply(json.dumps({"data": [record]}).encode())


def terminate(*_):
    raise SystemExit(0)


signal.signal(signal.SIGTERM, terminate)
HTTPServer(("127.0.0.1", args.port), Handler).serve_forever()
