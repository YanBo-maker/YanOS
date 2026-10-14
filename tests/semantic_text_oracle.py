"""Independent Unicode/byte-boundary/SHA reference; never imports C logic."""
import hashlib
from pathlib import Path
import random
import struct
import subprocess
import sys

WHITE = set(range(9, 14)) | {32, 0x85, 0xA0, 0x1680, 0x2028, 0x2029,
                            0x202F, 0x205F, 0x3000} | set(range(0x2000, 0x200B))
def eligible(data, query=False):
    try:
        text = data.decode('utf-8', errors='strict')
    except UnicodeDecodeError:
        return False
    if query and (not 1 <= len(data) <= 1023 or all(ord(c) in WHITE for c in text)):
        return False
    for i, c in enumerate(text):
        n = ord(c)
        if n == 13 and not query:
            if i + 1 == len(text) or text[i + 1] != '\n': return False
        elif n < 32 and n not in ({9} if query else {9, 10}): return False
        elif 0x7F <= n <= 0x9F: return False
    return True

def chunks(data, offset, line):
    text = data.decode('utf-8')
    positions = [offset]
    for c in text: positions.append(positions[-1] + len(c.encode('utf-8')))
    lines = [line]
    for c in text: lines.append(lines[-1] + (c == '\n'))
    start = 0; rows = []
    while start < len(text):
        end = min(start + 512, len(text))
        if end < len(text) and text[end-1:end+1] == '\r\n': end -= 1
        part = text[start:end]
        if any(ord(c) not in WHITE for c in part):
            rows.append('ROW %d %d %d %d %d %s' %
                        (positions[start], positions[end], lines[start], lines[end-1],
                         end-start, hashlib.sha256(part.encode('utf-8')).hexdigest()))
        if end == len(text): break
        following = end - 64
        if text[following-1:following+1] == '\r\n': following -= 1
        assert following > start
        start = following
    return rows

def main():
    executable = Path(sys.argv[1])
    if not executable.is_file():
        raise RuntimeError('FAIL the implementation under test is missing: ' + str(executable))
    rng = random.Random(290014)
    count = 0
    def check(mode, data, feed=1, offset=0, line=1):
        nonlocal count
        record = struct.pack('<IIII', len(data), feed, offset, line) + data
        run = subprocess.run([str(executable), mode], input=record, capture_output=True, timeout=10)
        assert run.returncode == 0 and not run.stderr, (mode, run.returncode, run.stderr)
        expected = chunks(data, offset, line) if mode == 'chunk' else []
        if mode == 'sha': expected = ['HASH ' + hashlib.sha256(data).hexdigest()]
        status = 0 if mode in ('sha','chunk') or eligible(data, mode == 'query') else 1
        expected.append('RESULT %d' % status)
        assert run.stdout.decode('ascii').splitlines() == expected, (count, mode, feed, expected, run.stdout)
        count += 1
    samples = [b'', b'a', b'\r\n', b'a\n', b'x'*511, b'x'*512, b'x'*513,
               ('中🙂é\r\n'*600).encode(), (' '*513+'x'*600).encode(),
               b'x'*446+b'\r\n'+b'x'*63+b'\r\n'+b'x'*300]
    for _ in range(50):
        text = ''.join(rng.choice(['a','中','🙂','é','\r\n','\n','\t','\u3000','\ufeff'])
                       for _ in range(rng.randrange(1, 4500)))
        samples.append(text.encode('utf-8'))
    for data in samples:
        for feed in (1, 3, 64, 4096):
            check('chunk', data, feed, 123, 7)
            check('text', data, feed)
        check('sha', data, rng.randrange(1, 300))
        check('query', data)
    invalid = [b'\0', b'\r', b'\rX', b'\xff', b'\xc0\x80', b'\xed\xa0\x80',
               b'\xf4\x90\x80\x80', b'\xe4\xb8', b'\xc2\x85', b'\x7f']
    invalid += [bytes([i]) for i in range(32) if i not in (9, 10, 13)]
    for data in invalid:
        check('text', data); check('query', data)
    for size in (55,56,63,64,65,111,112,127,128,1023,1024,65536):
        check('sha', rng.randbytes(size), 7)
    print('PASS semantic text/SHA independent oracle: %d cases' % count)
if __name__ == '__main__': main()
