"""Independent 0032 oracle: Python UTF-8, byte lines and regular expressions.

This module never imports or calls a C parser. Raw files are the only input;
the existing independent Python 0027 model interprets a recognized header.
"""
import re
from note_metadata_reference import MetadataError, parse_header

ID = rb'[A-Za-z0-9_-]{1,31}'
NAME = rb'[A-Za-z0-9._-]{1,31}'
MARKER = re.compile(rb'@block (' + ID + rb')\Z')
REFERENCE = re.compile(rb'\[\[(' + NAME + rb')#(' + ID + rb')\]\]')
ADDRESS = re.compile(rb'(' + NAME + rb')#(' + ID + rb')\Z')


class BlockError(ValueError):
    def __init__(self, token):
        super().__init__(token)
        self.token = token


def qualified(data):
    try:
        text = data.decode('utf-8', 'strict')
    except UnicodeError:
        return False
    for at, char in enumerate(text):
        scalar = ord(char)
        if char == '\r':
            if at + 1 == len(text) or text[at + 1] != '\n':
                return False
        elif (scalar < 32 and char not in '\t\n') or 0x7f <= scalar <= 0x9f:
            return False
    return True


def physical_lines(data, offset=0):
    line = data.count(b'\n', 0, offset) + 1
    while offset < len(data):
        lf = data.find(b'\n', offset)
        end = len(data) if lf < 0 else lf + 1
        raw = data[offset:end]
        content = raw[:-2] if raw.endswith(b'\r\n') else raw[:-1] if raw.endswith(b'\n') else raw
        yield line, offset, end, content
        offset = end
        line += 1


def scan(data):
    if not qualified(data):
        raise BlockError('INVALID_TEXT')
    try:
        body = parse_header(data)['header_length']
    except MetadataError as error:
        raise BlockError('METADATA') from error
    blocks, refs, seen = [], [], set()
    last_line = data.count(b'\n') + (0 if not data or data.endswith(b'\n') else 1)

    def close(end, last):
        if blocks:
            block = blocks[-1]
            block.update(end=end, first=block['marker'] + 1 if end > block['start'] else 0,
                         last=last if end > block['start'] else 0)

    for line, start, end, content in physical_lines(data, body):
        reserved = content.startswith(b'@block') and (len(content) == 6 or content[6:7] in (b' ', b'\t'))
        if reserved:
            match = MARKER.fullmatch(content)
            if not match:
                raise BlockError('BLOCK_FORMAT')
            identity = match[1].decode('ascii')
            if identity in seen:
                raise BlockError('AMBIGUOUS')
            if len(seen) == 64:
                raise BlockError('LIMIT')
            close(start, line - 1)
            seen.add(identity)
            blocks.append(dict(id=identity, marker=line, first=0, last=0, start=end, end=end))
        else:
            for match in REFERENCE.finditer(content):
                if match[1] in (b'.', b'..'):
                    continue
                refs.append(dict(name=match[1].decode('ascii'), id=match[2].decode('ascii'),
                    source_id=blocks[-1]['id'] if blocks else None, line=line,
                    start=start + match.start(), end=start + match.end()))
    close(len(data), last_line)
    return blocks, refs


def target(files, name, identity):
    if name not in files:
        return 'DANGLING_FILE'
    blocks, _ = scan(files[name])
    return 'OK' if any(block['id'] == identity for block in blocks) else 'DANGLING_BLOCK'


def display(data):
    return data.decode('utf-8').replace('\\', '\\\\').replace('\t', '\\x09').encode('utf-8')


def source_token(reference):
    return 'NONE' if reference['source_id'] is None else '#' + reference['source_id']


def query(files, command):
    """Return the exact command result, excluding echo and next prompt."""
    try:
        parts = command.split(b' ')
        parts = [part for part in parts if part]
        if len(parts) != 3 or parts[0] != b'block':
            raise BlockError('USAGE')
        verb, argument = parts[1:]
        if verb in (b'list', b'refs'):
            if re.fullmatch(NAME, argument) is None or argument in (b'.', b'..'):
                raise BlockError('USAGE')
            name = argument.decode('ascii')
            if name not in files:
                raise BlockError('NOT_FOUND')
            blocks, refs = scan(files[name])
        elif verb in (b'show', b'backlinks'):
            parsed = ADDRESS.fullmatch(argument)
            if parsed is None or parsed[1] in (b'.', b'..'):
                raise BlockError('USAGE')
            name, identity = (part.decode('ascii') for part in parsed.groups())
        else:
            raise BlockError('USAGE')
        if verb == b'list':
            out = ''.join('BLOCK %s#%s marker=%d first=%d last=%d bytes=%d:%d\r\n' %
                (name, block['id'], block['marker'], block['first'], block['last'], block['start'], block['end'])
                for block in blocks).encode()
            return out + ('OK block list total=%d shown=%d\r\n' % (len(blocks), len(blocks))).encode()
        if verb == b'show':
            if name not in files:
                raise BlockError('NOT_FOUND')
            blocks, _ = scan(files[name])
            block = next((block for block in blocks if block['id'] == identity), None)
            if block is None:
                raise BlockError('NOT_FOUND')
            selected = files[name][block['start']:block['end']]
            rows = list(physical_lines(selected))
            out = b''.join(str(block['first'] + i).encode() + b' ' + display(row[3]) + b'\r\n'
                           for i, row in enumerate(rows))
            return out + ('OK block show total=%d shown=%d\r\n' % (len(rows), len(rows))).encode()
        if verb == b'refs':
            out = b''
            for ref in refs[:20]:
                state = target(files, ref['name'], ref['id'])
                out += ('REF source=%s block=%s line=%d bytes=%d:%d target=%s#%s state=%s\r\n' %
                    (name, source_token(ref), ref['line'], ref['start'], ref['end'],
                     ref['name'], ref['id'], state)).encode()
            return out + ('OK block refs total=%d shown=%d\r\n' % (len(refs), min(20, len(refs)))).encode()
        state = target(files, name, identity)
        matches, skipped = [], 0
        for source in sorted(files):
            try:
                _, refs = scan(files[source])
            except BlockError as error:
                if error.token == 'INVALID_TEXT':
                    skipped += 1
                    continue
                raise
            matches.extend((source, ref) for ref in refs if (ref['name'], ref['id']) == (name, identity))
        out = b''.join(('BACKLINK source=%s block=%s line=%d bytes=%d:%d target=%s#%s\r\n' %
            (source, source_token(ref), ref['line'], ref['start'], ref['end'], name, identity)).encode()
            for source, ref in matches[:20])
        return out + ('OK block backlinks total=%d shown=%d skipped=%d target=%s\r\n' %
            (len(matches), min(20, len(matches)), skipped, state)).encode()
    except BlockError as error:
        return ('ERROR %s\r\n' % error.token).encode()
