"""Independent byte-line and calendar reference for the 0027 note header.

This module does not import or translate the C parser. Guest and native probes
can compare their observations with Python's strict UTF-8/calendar libraries.
The returned body is the original byte suffix, including its line endings.
"""
import datetime
import re


WHITE_SPACE = {9, 10, 11, 12, 13, 32, 0x85, 0xA0, 0x1680,
               *range(0x2000, 0x200B), 0x2028, 0x2029, 0x202F,
               0x205F, 0x3000}
NAME = re.compile(rb"[A-Za-z0-9._-]{1,31}\Z")


class MetadataError(ValueError):
    """Recognizable metadata which violates the bounded header contract."""


def text_value(raw, maximum, *, tag=False):
    if not 1 <= len(raw) <= maximum:
        raise MetadataError("field byte length")
    try:
        text = raw.decode("utf-8", errors="strict")
    except UnicodeError as error:
        raise MetadataError("UTF-8") from error
    if any(ord(c) == 0xFEFF or ord(c) < 32 or 0x7F <= ord(c) <= 0x9F or
           (tag and ord(c) in WHITE_SPACE) for c in text):
        raise MetadataError("field control/space")
    return text


def date_value(raw):
    if re.fullmatch(rb"[0-9]{4}-[0-9]{2}-[0-9]{2}", raw) is None:
        raise MetadataError("date syntax")
    try:
        datetime.date.fromisoformat(raw.decode("ascii"))
    except ValueError as error:
        raise MetadataError("calendar date") from error
    return raw.decode("ascii")


def parse_header(data):
    result = dict(present=False, title=None, created=None, modified=None,
                  tags=[], links=[], header_length=0, body=data)
    if not data.startswith(b"@yan-note"):
        return result
    if not data.startswith(b"@yan-note 1\n"):
        raise MetadataError("magic/version")
    position = len(b"@yan-note 1\n")
    result["present"] = True
    while position < min(len(data), 4096):
        end = data.find(b"\n", position, 4096)
        if end < 0:
            raise MetadataError("unfinished header")
        line = data[position:end]
        position = end + 1
        if line == b"@end":
            result.update(header_length=position, body=data[position:])
            return result
        if b": " not in line:
            raise MetadataError("field syntax")
        key, value = line.split(b": ", 1)
        if key == b"title":
            if result["title"] is not None:
                raise MetadataError("duplicate title")
            result["title"] = text_value(value, 256)
        elif key in (b"created", b"modified"):
            name = key.decode("ascii")
            if result[name] is not None:
                raise MetadataError("duplicate date")
            result[name] = date_value(value)
        elif key == b"tag":
            tag = text_value(value, 63, tag=True)
            if tag in result["tags"] or len(result["tags"]) == 16:
                raise MetadataError("duplicate/excess tag")
            result["tags"].append(tag)
        elif key == b"link":
            if NAME.fullmatch(value) is None or value in (b".", b".."):
                raise MetadataError("link name")
            link = value.decode("ascii")
            if link in result["links"] or len(result["links"]) == 16:
                raise MetadataError("duplicate/excess link")
            result["links"].append(link)
        else:
            raise MetadataError("unknown field")
    raise MetadataError("missing terminator/oversize header")


def filtered_reference(query, files, tags, term_reference):
    """Filter whole valid text files before invoking the independent term model.

    Invalid UTF-8/NUL files contribute one skip each. A valid text file's bad
    header aborts the operation even if its tags could not have matched.
    Physical slots and all header bytes are retained in the eligible corpus.
    """
    selected, skipped = [], 0
    for slot, name, data in files:
        try:
            if b"\0" in data:
                raise ValueError("NUL")
            data.decode("utf-8", errors="strict")
        except (UnicodeError, ValueError):
            skipped += 1
            continue
        metadata = parse_header(data)
        if all(tag in metadata["tags"] for tag in tags):
            selected.append((slot, name, data))
    answer = term_reference(query, selected)
    answer["skipped"] += skipped
    return answer


def canonical_header(info):
    lines = [b"@yan-note 1"]
    for key in ("title", "created", "modified"):
        if info[key] is not None:
            lines.append(key.encode() + b": " + info[key].encode("utf-8"))
    lines += [b"tag: " + tag.encode("utf-8") for tag in info["tags"]]
    lines += [b"link: " + link.encode("ascii") for link in info["links"]]
    lines.append(b"@end")
    return b"\n".join(lines) + b"\n"
