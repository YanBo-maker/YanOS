"""0030 independent JSON/identity/raw-byte oracle against the production CLI."""
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path
import re
import signal
import struct
import subprocess
import sys
import tempfile
import time
from semantic_fixture import image, note


def logical_hash(files):
    h = hashlib.sha256()
    entries = [(name, slot, data) for slot, item in enumerate(files) if item
               for name, data in [item]]
    for name, slot, data in sorted(entries):
        name = name.encode('ascii')
        h.update(struct.pack('<II', slot, len(name)))
        h.update(name)
        h.update(struct.pack('<I', len(data)))
        h.update(data)
    return h.hexdigest()


def unique_object(pairs):
    value = {}
    for key, item in pairs:
        assert key not in value, ('duplicate JSON key', key)
        value[key] = item
    return value


def main():
    tool, runner, memory_tool = map(Path, sys.argv[1:])
    for p in (tool, runner, memory_tool):
        if not p.is_file(): raise RuntimeError('FAIL implementation/fixture missing: ' + str(p))
    cases = 0
    with tempfile.TemporaryDirectory(prefix='yan-context-json-') as tmp:
        root = Path(tmp)
        disk, index, model = [root / n for n in ('source.img', 'index', 'model')]
        child_log, pid_file = root / 'children', root / 'server.pid'
        env = dict(os.environ, YAN_TEST_CHILD_LOG=str(child_log),
                   YAN_TEST_SERVER_PID_FILE=str(pid_file))
        files = []

        def args(command, extra=()):
            return [str(tool), command, '--disk-image', str(disk), '--index', str(index),
                    '--model', str(model), '--runner', str(runner), *extra]

        def snapshots():
            return tuple(hashlib.sha256(p.read_bytes()).digest() if p.exists() else None
                         for p in (disk, model, runner, index))

        def call(command, *extra, rc=0, error=b'', overrides=None):
            nonlocal cases
            before = snapshots()
            old_log = child_log.read_text().splitlines() if child_log.exists() else []
            p = subprocess.run(args(command, extra), capture_output=True, timeout=20,
                               env=dict(env, **(overrides or {})))
            assert (p.returncode, p.stderr) == (rc, error), (command, extra, p.returncode, p.stdout, p.stderr)
            after = snapshots()
            assert before[:3] == after[:3], 'source/model/runner modified'
            if command != 'rebuild' or rc != 0: assert before[3] == after[3], 'index modified'
            new_log = child_log.read_text().splitlines() if child_log.exists() else []
            added = new_log[len(old_log):]
            if rc == 2: assert not added, ('USAGE started child', added)
            if rc != 0: assert p.stdout == b'', ('failure published JSON', p.stdout)
            if rc == 3: assert 'server' not in added, ('invalid index started inference', added)
            cases += 1
            return p.stdout

        def install(new_files, mode='universal'):
            nonlocal files
            files = new_files
            disk.write_bytes(image(files))
            model.write_text(mode)
            call('rebuild')

        def check(data, query):
            assert data.startswith(b'{') and data.endswith(b'}\n') and data.count(b'\n') == 1, data
            obj = json.loads(data, object_pairs_hook=unique_object,
                             parse_constant=lambda x: (_ for _ in ()).throw(AssertionError(x)))
            assert set(obj) == {'schema_version', 'query', 'source_sha256', 'model_sha256',
                               'runner_sha256', 'config_sha256', 'segments', 'skipped', 'shown', 'results'}
            assert type(obj['schema_version']) is int and obj['schema_version'] == 1
            assert obj['query'].encode('utf-8') == query.encode('utf-8')
            header = index.read_bytes()[:256]
            for key, off in [('source', 32), ('model', 64), ('runner', 96), ('config', 128)]:
                text = obj[key + '_sha256']
                assert re.fullmatch('[0-9a-f]{64}', text), (key, text)
                assert text == header[off:off+32].hex(), ('index identity', key)
            assert obj['source_sha256'] == logical_hash(files), 'logical-source oracle'
            assert obj['model_sha256'] == hashlib.sha256(model.read_bytes()).hexdigest()
            assert obj['runner_sha256'] == hashlib.sha256(runner.read_bytes()).hexdigest()
            for key in ('segments', 'skipped', 'shown'):
                assert type(obj[key]) is int and obj[key] >= 0
            assert obj['shown'] == len(obj['results']) == min(20, obj['segments'])
            originals = dict(item for item in files if item)
            for row in obj['results']:
                assert set(row) == {'file', 'first_line', 'last_line', 'start', 'end', 'score', 'text'}
                assert type(row['score']) in (int, float) and math.isfinite(row['score'])
                for key in ('first_line', 'last_line', 'start', 'end'): assert type(row[key]) is int
                raw = originals[row['file']]
                start, end = row['start'], row['end']
                assert 0 <= start < end <= len(raw)
                assert row['text'].encode('utf-8') == raw[start:end], 'complete original byte range'
                assert len(row['text']) <= 512 and len(raw[start:end]) <= 2048
                assert row['first_line'] == raw[:start].count(b'\n') + 1
                assert row['last_line'] == raw[:end-1].count(b'\n') + 1
            return obj

        body = '前'*190 + '"\\\t\r\n' + '😀'*280 + '\ufeff\n'
        initial = [('z', note('标题不是正文', body)), None, ('emoji', ('😀'*512).encode()),
                   ('long', ('界'*1100).encode()), ('binary', b'bad\0tail'),
                   ('empty', b''), ('title', note('仅标题', ''))]
        install(initial)
        query = '问"\\\t😀\ufeff'
        obj = check(call('search', '--query', query, '--format', 'json'), query)
        assert obj['segments'] == 5 and obj['skipped'] == 1
        assert [(r['file'], r['start']) for r in obj['results']] == sorted((r['file'], r['start']) for r in obj['results'])
        assert any(len(r['text']) > 160 for r in obj['results'])
        assert any(len(r['text'].encode()) == 2048 for r in obj['results'])
        assert call('search', '--query', query) == call('search', '--query', query, '--format', 'text')
        assert call('status') == b'STATUS READY segments=5 skipped=1\n'
        for extra in [('--format',), ('--format', ''), ('--format', 'JSON'), ('--format', 'other'),
                      ('--format', 'json', '--format', 'text')]:
            call('search', '--query', 'q', *extra, rc=2, error=b'ERROR USAGE\n')
        for command in ('status', 'rebuild'):
            for fmt in ('text', 'json'):
                call(command, '--format', fmt, rc=2, error=b'ERROR USAGE\n')
        for extra in [(), ('--query', ''), ('--query', ' '), ('--query', '\n'), ('--query', 'q'*1024)]:
            call('search', '--format', 'json', *extra, rc=2, error=b'ERROR USAGE\n')
        # Missing all dependencies still gives USAGE before opening anything.
        call('search', '--query', 'q', '--format', 'bad', '--model', '/no/such/model',
             rc=2, error=b'ERROR USAGE\n')
        install([('b', b'beta'), ('a', b'alpha')], 'precision')
        precise = check(call('search', '--query', 'q', '--format', 'json'), 'q')
        for row in precise['results']:
            assert row['score'] == 1 / math.sqrt(5), ('double round-trip', row)
            assert row['score'] != round(row['score'], 6)
        install([(f'f{i:02}', b'note') for i in range(26, -1, -1)])
        many = check(call('search', '--query', 'q', '--format', 'json'), 'q')
        assert many['segments'] == 27 and [r['file'] for r in many['results']] == [f'f{i:02}' for i in range(20)]
        install([])
        empty = check(call('search', '--query', 'q', '--format', 'json'), 'q')
        assert empty['results'] == [] and empty['source_sha256'] == hashlib.sha256(b'').hexdigest()
        install([('a', b'hello\n')])
        saved = index.read_bytes()
        index.unlink(); call('search', '--query', 'q', '--format', 'json', rc=3, error=b'ERROR MISSING\n')
        index.write_bytes(b'x' + saved[1:]); call('search', '--query', 'q', '--format', 'json', rc=3, error=b'ERROR CORRUPT\n')
        index.write_bytes(saved)
        disk.write_bytes(image([('a', b'changed\n')]))
        call('search', '--query', 'q', '--format', 'json', rc=3, error=b'ERROR STALE\n')
        disk.write_bytes(image(files))
        model.write_text('changed')
        call('search', '--query', 'q', '--format', 'json', rc=3, error=b'ERROR STALE\n')
        model.write_text('universal')
        disk.write_bytes(image([('bad', b'@yan-note 1\nbad field\n@end\ntext')]))
        call('search', '--query', 'q', '--format', 'json', rc=3, error=b'ERROR METADATA\n')
        disk.write_bytes(image(files))
        call('search', '--query', 'q', '--format', 'json', rc=4, error=b'ERROR PROVIDER\n',
             overrides={'YAN_TEST_EMBED_FAILURE': '1'})
        before = snapshots()
        with open('/dev/full', 'wb') as full:
            p = subprocess.run(args('search', ['--query', 'q', '--format', 'json']),
                               stdout=full, stderr=subprocess.PIPE, env=env, timeout=20)
        assert p.returncode == 5 and p.stderr == b'ERROR OUTPUT\n', p
        assert snapshots() == before
        cases += 1

        memory_args = args('search', ['--query', 'q', '--format', 'json'])
        memory_args[0] = str(memory_tool)
        log_before = child_log.read_bytes()
        p = subprocess.run(memory_args, capture_output=True, timeout=10, env=env)
        assert (p.returncode, p.stdout, p.stderr) == (5, b'', b'ERROR IO\n'), p
        assert snapshots() == before and child_log.read_bytes() == log_before
        cases += 1
        memory_args = args('search', ['--query', 'q'])
        memory_args[0] = str(memory_tool)
        p = subprocess.run(memory_args, capture_output=True, timeout=20, env=env)
        assert p.returncode == 0 and p.stderr == b'' and p.stdout.endswith(b'OK search shown=1 segments=1 skipped=0\n'), p
        assert snapshots() == before
        cases += 1

        # Cancel while the owned child exists, before any JSON output.
        old = child_log.read_text().splitlines()
        pid_file.unlink(missing_ok=True)
        p = subprocess.Popen(args('search', ['--query', 'q', '--format', 'json']),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=dict(env, YAN_TEST_EMBED_DELAY='10'))
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if pid_file.exists() and 'server' in child_log.read_text().splitlines()[len(old):]: break
            time.sleep(0.01)
        else:
            p.kill(); p.communicate(); raise AssertionError('owned server not started')
        p.send_signal(signal.SIGINT)
        out, err = p.communicate(timeout=10)
        assert (p.returncode, out, err) == (5, b'', b'ERROR CANCELLED\n'), (p.returncode, out, err)
        server = int(pid_file.read_text())
        try: os.kill(server, 0)
        except ProcessLookupError: pass
        else: raise AssertionError('cancelled command left owned child alive')
        assert snapshots() == before
        cases += 1

        # A bounded pipe forces a real partial write; neither OUTPUT nor
        # post-write cancellation may be mistaken for a consumable success.
        install([(f'f{i:02}', ('中'*450).encode()) for i in range(21)])
        for cancel in (False, True):
            before = snapshots()
            read_fd, write_fd = os.pipe()
            fcntl.fcntl(write_fd, fcntl.F_SETPIPE_SZ, 4096)
            p = subprocess.Popen(args('search', ['--query', 'q', '--format', 'json']),
                                 stdout=write_fd, stderr=subprocess.PIPE, env=env)
            os.close(write_fd)
            prefix = os.read(read_fd, 256)
            assert prefix.startswith(b'{') and not prefix.endswith(b'}\n'), prefix
            if cancel: p.send_signal(signal.SIGINT)
            os.close(read_fd)
            _, err = p.communicate(timeout=10)
            assert p.returncode == 5 and err == (b'ERROR CANCELLED\n' if cancel else b'ERROR OUTPUT\n'), (p.returncode, err)
            assert snapshots() == before
            cases += 1
    print('PASS semantic JSON oracle: %d cases' % cases)


if __name__ == '__main__': main()
