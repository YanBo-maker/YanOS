#!/usr/bin/env python3
"""issue61 documentation-alignment controls for tests/mcp_write_docs.py.

These controls support the existing 0035 R2/R9 documentation alignment; they
are pure-Python checker self-tests, not the official-SDK acceptance for
0035-R2-AC1.  Every document/actual mutation below must be a hard ``DocsError``
(which is an ``AssertionError``); there is no skip path and no process is
started.  The checked-in document is only copied into temporary directories,
never edited.
"""

import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
DOCUMENT = HERE.parent / "docs" / "mcp-write-tools.md"
sys.path.insert(0, str(HERE))

import mcp_write_docs


def reformat_json_fences(text):
    """Reformat and reorder every fenced JSON object; array order stays."""
    lines = text.splitlines()
    output = []
    index = 0
    while index < len(lines):
        output.append(lines[index])
        if lines[index].strip() != "```json":
            index += 1
            continue
        end = index + 1
        while end < len(lines) and lines[end].strip() != "```":
            end += 1
        if end >= len(lines):
            raise AssertionError("document fence is not closed")
        value = json.loads("\n".join(lines[index + 1:end]))
        output.extend(json.dumps(value, ensure_ascii=False, indent=1,
                                 sort_keys=True).splitlines())
        output.append("```")
        index = end + 1
    return "\n".join(output) + "\n"


def remove_fence_after(text, label):
    """Remove the fenced JSON block after the last occurrence of ``label``."""
    start = text.rfind(label)
    if start < 0:
        raise AssertionError("label not found: " + label)
    opening = text.find("```json", start)
    if opening < 0:
        raise AssertionError("fence not found after " + label)
    closing = text.find("\n```", opening)
    if closing < 0:
        raise AssertionError("closing fence not found after " + label)
    end = closing + len("\n```")
    if text[end:end + 1] == "\n":
        end += 1
    return text[:opening] + text[end:]


class McpWriteDocsGate(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.document = mcp_write_docs.load_documented(DOCUMENT)
        cls.text = DOCUMENT.read_text(encoding="utf-8")

    def actual(self):
        return [copy.deepcopy(self.document[name])
                for name in mcp_write_docs.TOOL_NAMES]

    def temp_document(self, text):
        directory = tempfile.TemporaryDirectory(prefix="mcp-write-docs-")
        self.addCleanup(directory.cleanup)
        path = Path(directory.name) / "mcp-write-tools.md"
        path.write_text(text, encoding="utf-8")
        return path

    def replace_once(self, text, old, new):
        self.assertEqual(1, text.count(old), old)
        return text.replace(old, new, 1)

    def replace_first(self, text, old, new):
        self.assertGreaterEqual(text.count(old), 1, old)
        return text.replace(old, new, 1)

    def assert_document_rejected(self, text, *needles):
        path = self.temp_document(text)
        with self.assertRaises(mcp_write_docs.DocsError) as caught:
            mcp_write_docs.compare_tools(self.actual(), path=path)
        message = str(caught.exception)
        self.assertIsInstance(caught.exception, AssertionError)
        for needle in needles:
            self.assertIn(needle, message)
        return message

    def assert_actual_rejected(self, actual, *needles):
        with self.assertRaises(mcp_write_docs.DocsError) as caught:
            mcp_write_docs.compare_tools(actual, path=DOCUMENT)
        message = str(caught.exception)
        self.assertIsInstance(caught.exception, AssertionError)
        for needle in needles:
            self.assertIn(needle, message)
        return message

    # ------------------------------------------------------------ controls

    def test_current_document_passes_against_documented_actual(self):
        self.assertIsNone(
            mcp_write_docs.compare_tools(self.actual(), path=DOCUMENT))

    def test_object_order_and_indentation_are_irrelevant(self):
        path = self.temp_document(reformat_json_fences(self.text))
        self.assertIsNone(
            mcp_write_docs.compare_tools(self.actual(), path=path))
        reversed_actual = [dict(reversed(list(tool.items())))
                           for tool in self.actual()]
        self.assertIsNone(
            mcp_write_docs.compare_tools(reversed_actual, path=DOCUMENT))

    def test_missing_document_fails(self):
        with tempfile.TemporaryDirectory(prefix="mcp-write-docs-") as root:
            path = Path(root) / "absent.md"
            with self.assertRaises(mcp_write_docs.DocsError) as caught:
                mcp_write_docs.compare_tools(self.actual(), path=path)
        self.assertIn("not found", str(caught.exception))
        self.assertIn(str(path), str(caught.exception))

    def test_malformed_json_fails(self):
        text = self.replace_once(
            self.text, '"const": "note_create"\n',
            '"const": "note_create",\n')
        self.assert_document_rejected(
            text, "yanos_note_create", "outputSchema", "malformed JSON")

    def test_duplicate_json_key_fails(self):
        text = self.replace_once(
            self.text, '"const": "note_create"\n',
            '"const": "note_create",\n          "const": "note_create"\n')
        self.assert_document_rejected(
            text, "yanos_note_create", "duplicate JSON key", "const")

    def test_duplicate_documented_section_fails(self):
        text = self.text + "\n### `yanos_note_create`\n"
        self.assert_document_rejected(
            text, "duplicate documented section", "yanos_note_create")

    def test_missing_documented_section_fails(self):
        start = self.text.index("### `yanos_note_replace`")
        end = self.text.index("## 学习笔记的五个块")
        self.assert_document_rejected(
            self.text[:start] + self.text[end:],
            "missing documented section", "yanos_note_replace")

    def test_missing_documented_schema_fence_fails(self):
        text = remove_fence_after(self.text, "outputSchema：")
        self.assert_document_rejected(
            text, "yanos_note_replace", "outputSchema",
            "missing fenced JSON block")

    def test_missing_create_shared_input_reference_fails(self):
        text = self.replace_once(
            self.text, mcp_write_docs.CREATE_INPUT_REFERENCE,
            "`inputSchema` 见下一节。")
        self.assert_document_rejected(
            text, "yanos_note_create", "shared inputSchema reference")

    def test_create_extra_input_schema_declaration_fails(self):
        extra = (
            "inputSchema：\n\n"
            "```json\n"
            "{\n"
            '  "type": "object",\n'
            '  "required": ["name"],\n'
            '  "properties": {"name": {"type": "string"}}\n'
            "}\n"
            "```\n\n")
        text = self.replace_once(
            self.text, "### `yanos_note_replace`",
            extra + "### `yanos_note_replace`")
        self.assert_document_rejected(
            text, "yanos_note_create", "inputSchema", "ambiguous",
            "shared reference")

    def test_changed_documented_description_fails(self):
        text = self.replace_once(
            self.text, "新建一个 YanFS 笔记文件", "新建一个 YanFS 笔记")
        self.assert_document_rejected(
            text, "yanos_note_create", "description",
            "新建一个 YanFS 笔记文件")

    def test_changed_documented_annotations_fail(self):
        text = self.replace_once(
            self.text, '"destructiveHint": false',
            '"destructiveHint": true')
        self.assert_document_rejected(
            text, "yanos_note_create", "annotations.destructiveHint",
            "False", "True")

    def test_documented_false_does_not_equal_actual_zero(self):
        text = self.replace_first(self.text, '"readOnlyHint": false',
                                  '"readOnlyHint": 0')
        self.assert_document_rejected(
            text, "yanos_note_create", "annotations.readOnlyHint",
            "False", "0")

    def test_changed_documented_nested_schema_fails(self):
        cases = (
            ('"maxLength": 31', '"maxLength": 32',
             "outputSchema.oneOf[0].properties.name.maxLength"),
            ('  "required": [\n    "name",\n    "text"\n  ],',
             '  "required": [\n    "name"\n  ],',
             "inputSchema.required"),
            ('    "text": {\n      "type": "string"\n    },',
             '    "text": {\n      "type": "integer"\n    },',
             "inputSchema.properties.text.type"),
        )
        for old, new, field in cases:
            with self.subTest(field=field):
                self.assert_document_rejected(
                    self.replace_first(self.text, old, new), field)

    def test_changed_actual_description_or_annotations_fail(self):
        actual = self.actual()
        actual[0]["description"] += " 篡改"
        self.assert_actual_rejected(
            actual, "yanos_note_create", "description")

        actual = self.actual()
        actual[1]["annotations"]["readOnlyHint"] = True
        self.assert_actual_rejected(
            actual, "yanos_note_replace", "annotations.readOnlyHint",
            "False", "True")

    def test_changed_actual_nested_schema_fails(self):
        actual = self.actual()
        actual[0]["inputSchema"]["properties"]["dry_run"]["type"] = "string"
        self.assert_actual_rejected(
            actual, "inputSchema.properties.dry_run.type",
            "boolean", "string")

        actual = self.actual()
        actual[0]["outputSchema"]["oneOf"][0]["properties"][
            "schema_version"]["const"] = True
        self.assert_actual_rejected(
            actual, "outputSchema.oneOf[0].properties.schema_version.const",
            "1", "True")

    def test_missing_or_duplicate_actual_tool_fails(self):
        actual = [tool for tool in self.actual()
                  if tool["name"] != "yanos_note_create"]
        self.assert_actual_rejected(
            actual, "missing actual tool", "yanos_note_create")

        actual = self.actual()
        actual.append(copy.deepcopy(actual[1]))
        self.assert_actual_rejected(
            actual, "duplicate actual tool", "yanos_note_replace")

    def test_actual_tool_missing_required_field_fails(self):
        actual = self.actual()
        del actual[0]["annotations"]
        self.assert_actual_rejected(
            actual, "yanos_note_create", "annotations",
            "missing from actual tools/list")


if __name__ == "__main__":
    unittest.main()
