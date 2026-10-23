#!/usr/bin/env python3
"""issue61 / 0035 R2/R9 documentation alignment support.

Compare the public write-tool document with actual ``tools/list`` fields.  This
supports the existing R2 annotations/schema and R9 description constraints; it
is not itself the official-SDK acceptance for 0035-R2-AC1 (``mcp_sdk`` remains
that acceptance).

The document is the only source of documented values.  This module never reads
``tools/mcp_note.c`` and never carries a copied implementation expectation.
The document's two explicit ``###`` tool sections are parsed as data; the
create tool reuses the replace inputSchema only through its explicit shared
reference, never by silently filling a missing block.

This is a pure-Python helper used by ``tests/test_mcp_write_docs.py`` and by
the live ``tests/mcp_note_checks.py`` run.  It has no skip/failure-code path.
"""

import json
from pathlib import Path

DEFAULT_DOCUMENT = Path(__file__).resolve().parent.parent / "docs" / "mcp-write-tools.md"
TOOL_NAMES = ("yanos_note_create", "yanos_note_replace")
CREATE_INPUT_REFERENCE = "`inputSchema` 与 `yanos_note_replace` 相同，见下一节。"


class DocsError(AssertionError):
    """A documented/actual mismatch or an unusable document."""


def _fail(message):
    raise DocsError(message)


def _document_text(path):
    path = Path(path)
    try:
        return path.read_text(encoding="utf-8")
    except FileNotFoundError:
        _fail("write-tool document not found: %s" % path)
    except (OSError, UnicodeError) as error:
        _fail("write-tool document unreadable: %s: %s" % (path, error))


def _section(lines, tool, path):
    heading = "### `%s`" % tool
    starts = [index for index, line in enumerate(lines)
              if line.strip() == heading]
    if not starts:
        _fail("missing documented section %r in %s" % (heading, path))
    if len(starts) > 1:
        _fail("duplicate documented section %r in %s" % (heading, path))
    start = starts[0] + 1
    end = len(lines)
    for index in range(start, len(lines)):
        stripped = lines[index].lstrip()
        if stripped.startswith("## ") or stripped.startswith("### "):
            end = index
            break
    return lines[start:end]


def _label_index(section, tool, label, path, prefix=False):
    forms = (label + "：", label + ":")
    matches = [index for index, line in enumerate(section)
               if (line.strip().startswith(label) if prefix
                   else line.strip() in forms)]
    if not matches:
        _fail("tool %r: missing label %r in %s" % (tool, label, path))
    if len(matches) > 1:
        _fail("tool %r: duplicate label %r in %s" % (tool, label, path))
    return matches[0]


def _description(section, tool, path):
    index = _label_index(section, tool, "description", path)
    cursor = index + 1
    while cursor < len(section) and not section[cursor].strip():
        cursor += 1
    if cursor >= len(section) or not section[cursor].lstrip().startswith(">"):
        _fail("tool %r: missing description blockquote in %s" % (tool, path))
    quoted = []
    while cursor < len(section) and section[cursor].lstrip().startswith(">"):
        line = section[cursor].lstrip()[1:]
        if line.startswith(" "):
            line = line[1:]
        quoted.append(line)
        cursor += 1
    return "\n".join(quoted)


def _reject_constant(value):
    raise ValueError("invalid JSON constant %s" % value)


def _object_without_duplicates(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON key %r" % key)
        result[key] = value
    return result


def _fenced_json(section, tool, label, path, prefix=False):
    index = _label_index(section, tool, label, path, prefix=prefix)
    cursor = index + 1
    while cursor < len(section) and not section[cursor].strip():
        cursor += 1
    if cursor >= len(section) or not section[cursor].strip().startswith("```"):
        _fail("tool %r field %r: missing fenced JSON block in %s"
              % (tool, label, path))
    opening = section[cursor].strip()
    if opening not in ("```", "```json"):
        _fail("tool %r field %r: unsupported fence %r in %s"
              % (tool, label, opening, path))
    cursor += 1
    body = []
    while cursor < len(section) and section[cursor].strip() != "```":
        body.append(section[cursor])
        cursor += 1
    if cursor >= len(section):
        _fail("tool %r field %r: missing closing fence in %s"
              % (tool, label, path))
    raw = "\n".join(body)
    try:
        return json.loads(raw, object_pairs_hook=_object_without_duplicates,
                          parse_constant=_reject_constant)
    except ValueError as error:
        _fail("tool %r field %r: malformed JSON in %s: %s"
              % (tool, label, path, error))


def _input_schema_labels(section):
    """Return explicit inputSchema labels outside fenced example JSON."""
    labels = []
    inside_fence = False
    for line in section:
        stripped = line.strip()
        if stripped.startswith("```"):
            inside_fence = not inside_fence
            continue
        if inside_fence or not stripped.startswith("inputSchema"):
            continue
        rest = stripped[len("inputSchema"):]
        if rest.startswith("（"):
            end = rest.find("）")
            if end < 0:
                continue
            rest = rest[end + 1:]
        if rest.lstrip().startswith(("：", ":")):
            labels.append(stripped)
    return labels


def _parse_tool(section, tool, path, create=False, replace_input=None):
    documented = {
        "name": tool,
        "description": _description(section, tool, path),
        "annotations": _fenced_json(section, tool, "annotations", path),
        "outputSchema": _fenced_json(section, tool, "outputSchema", path),
    }
    if create:
        references = [line.strip() for line in section
                      if line.strip() == CREATE_INPUT_REFERENCE]
        if not references:
            _fail("tool %r: missing create shared inputSchema reference %r in %s"
                  % (tool, CREATE_INPUT_REFERENCE, path))
        if len(references) > 1:
            _fail("tool %r: duplicate create shared inputSchema reference in %s"
                  % (tool, path))
        declarations = _input_schema_labels(section)
        if declarations:
            _fail(
                "tool %r field 'inputSchema': ambiguous extra declaration %r "
                "conflicts with the shared reference %r in %s"
                % (tool, declarations[0], CREATE_INPUT_REFERENCE, path))
        documented["inputSchema"] = replace_input
    else:
        documented["inputSchema"] = _fenced_json(
            section, tool, "inputSchema", path, prefix=True)
    return documented


def load_documented(path=DEFAULT_DOCUMENT):
    """Parse the two documented write tools, rejecting every malformed form."""
    path = Path(path)
    lines = _document_text(path).splitlines()
    replace = _parse_tool(_section(lines, "yanos_note_replace", path),
                          "yanos_note_replace", path)
    create = _parse_tool(_section(lines, "yanos_note_create", path),
                         "yanos_note_create", path, create=True,
                         replace_input=replace["inputSchema"])
    return {"yanos_note_create": create, "yanos_note_replace": replace}


def _json_type(value):
    if value is None:
        return "null"
    if isinstance(value, bool):
        return "boolean"
    if isinstance(value, int):
        return "integer"
    if isinstance(value, float):
        return "number"
    if isinstance(value, str):
        return "string"
    if isinstance(value, list):
        return "array"
    if isinstance(value, dict):
        return "object"
    return type(value).__name__


def _short(value, limit=180):
    rendered = repr(value)
    if len(rendered) > limit:
        rendered = rendered[:limit] + "...<%d chars>" % len(rendered)
    return rendered


def _mismatch(tool, field, documented, actual, path, note=""):
    message = (
        "tool %r field %r differs from tools/list [doc: %s]: "
        "documented=%s (%s) actual=%s (%s)"
        % (tool, field, path, _short(documented), _json_type(documented),
           _short(actual), _json_type(actual)))
    if note:
        message += " " + note
    _fail(message)


def _compare_value(tool, field, documented, actual, path):
    if type(documented) is not type(actual):
        _mismatch(tool, field, documented, actual, path)
    if isinstance(documented, dict):
        documented_keys = set(documented)
        actual_keys = set(actual)
        if documented_keys != actual_keys:
            _fail(
                "tool %r field %r differs from tools/list [doc: %s]: "
                "documented keys %r actual keys %r"
                % (tool, field, path, sorted(documented_keys),
                   sorted(actual_keys)))
        for key in documented:
            _compare_value(tool, "%s.%s" % (field, key), documented[key],
                           actual[key], path)
    elif isinstance(documented, list):
        if len(documented) != len(actual):
            _mismatch(tool, field, documented, actual, path,
                      "list length differs")
        for index, (left, right) in enumerate(zip(documented, actual)):
            _compare_value(tool, "%s[%d]" % (field, index), left, right, path)
    elif documented != actual:
        _mismatch(tool, field, documented, actual, path)


def compare_tools(tools, path=DEFAULT_DOCUMENT):
    """Compare actual raw tools/list entries; return None or raise DocsError."""
    path = Path(path)
    documented = load_documented(path)
    if not isinstance(tools, list):
        _fail("tools/list result is not an array [doc: %s]: %r"
              % (path, tools))
    actual = {}
    for index, tool in enumerate(tools):
        if not isinstance(tool, dict):
            _fail("tools/list entry %d is not an object [doc: %s]: %r"
                  % (index, path, tool))
        name = tool.get("name")
        if not isinstance(name, str):
            _fail("tools/list entry %d has no string name [doc: %s]: %r"
                  % (index, path, tool))
        if name in actual:
            _fail("duplicate actual tool %r in tools/list [doc: %s]"
                  % (name, path))
        actual[name] = tool
    for name in TOOL_NAMES:
        if name not in actual:
            _fail("missing actual tool %r in tools/list [doc: %s]"
                  % (name, path))
        for field in ("name", "description", "annotations", "inputSchema",
                      "outputSchema"):
            if field not in actual[name]:
                _fail("tool %r field %r missing from actual tools/list "
                      "[doc: %s]" % (name, field, path))
            _compare_value(name, field, documented[name][field],
                           actual[name][field], path)
    return None
