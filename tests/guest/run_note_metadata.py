"""0027 observations through the production RV32 terminal and real images."""
import argparse
import subprocess
import sys
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_search_terms as terms

base = terms.base
INPUTS = tuple(terms.INPUTS) + (
    "tests/guest/run_note_metadata.py", "os/note_metadata_format.h",
    "os/note_metadata_format.c", "os/note_metadata.h", "os/note_metadata.c",
)


def head(*fields, body=b""):
    return b"@yan-note 1\n" + b"".join(field + b"\n" for field in fields) + b"@end\n" + body


def show(name, *, title=b"UNKNOWN", created=b"UNKNOWN", modified=b"UNKNOWN",
         tags=(), links=()):
    output = b"META " + name.encode() + b"\r\nTITLE " + base.display(title)
    output += b"\r\nCREATED " + created + b"\r\nMODIFIED " + modified + b"\r\n"
    output += (b"".join(b"TAG " + base.display(tag) + b"\r\n" for tag in tags)
               if tags else b"TAG NONE\r\n")
    output += (b"".join(b"LINK " + name.encode() + b" " + state + b"\r\n"
                        for name, state in links) if links else b"LINK NONE\r\n")
    return output + b"OK meta show\r\n"


def contents(disk, name):
    raw = disk.read_bytes()
    entries = base.parse_entries(raw, len(raw) // 4096)
    for _slot, entry_name, size, start, _count in entries:
        if entry_name == name:
            return raw[start * 4096:start * 4096 + size]
    raise AssertionError("missing persisted file: " + name)


def case_observation_and_tag_identity(args, work):
    files = [("z", b"\0\xffbinary", 1),
             ("a", head(b"title: note", b"tag: os", b"tag: OS",
                        b"link: z", b"link: missing", body=b"hit\n"), 2)]
    terms.run_commands(args, work, "observe", files, [
        (b"meta show z", show("z")),
        (b"meta show a", show("a", title=b"note", tags=(b"os", b"OS"),
                              links=(("z", b"OK"), ("missing", b"DANGLING")))),
        (b"meta list", b"a\r\nz\r\nOK meta list shown=2\r\n"),
        (b"meta list tag:os tag:OS", b"a\r\nOK meta list shown=1\r\n"),
        (b"meta list tag:Os", b"OK meta list shown=0\r\n"),
        (b"search tag:os hit", terms.row("a", 8, b"hit") + terms.summary(1, skipped=1)),
    ])


def case_update_body_and_restart(args, work):
    body = "interrupt 中断\r\nno final newline".encode()
    disk, _ = terms.run_commands(args, work, "update", [("irq.md", body, 1)], [
        (b"meta tag irq.md add os", b"OK meta tag\r\n"),
        (b"search tag:os interrupt", terms.row("irq.md", 4, "interrupt 中断".encode()) + terms.summary(1)),
        (b"meta set irq.md title " + "中断笔记".encode(), b"OK meta set\r\n"),
        (b"meta set irq.md created 2000-02-29", b"OK meta set\r\n"),
        (b"meta link irq.md add absent", b"OK meta link\r\n"),
        (b"meta show irq.md", show("irq.md", title="中断笔记".encode(), created=b"2000-02-29",
                                   tags=(b"os",), links=(("absent", b"DANGLING"),))),
    ], readonly=False)
    expected = head("title: 中断笔记".encode(), b"created: 2000-02-29", b"tag: os",
                    b"link: absent", body=body)
    if contents(disk, "irq.md") != expected:
        raise AssertionError("metadata update changed body bytes or canonical header")
    terms.run_commands(args, work, "restart", [], [
        (b"meta show irq.md", show("irq.md", title="中断笔记".encode(), created=b"2000-02-29",
                                   tags=(b"os",), links=(("absent", b"DANGLING"),))),
        (b"search tag:os interrupt", terms.row("irq.md", 7, "interrupt 中断".encode()) + terms.summary(1)),
    ], disk=disk)


def case_filter_before_top20(args, work):
    files = [("untagged", b"hit hit hit\n" * 30, 1),
             ("eligible", head(b"tag: os", body=b"hit\n"), 2)]
    terms.run_commands(args, work, "rank", files, [
        (b"search tag:os hit", terms.row("eligible", 4, b"hit") + terms.summary(1)),
        (b"search tag:os hit", terms.row("eligible", 4, b"hit") + terms.summary(1)),
        (b"index clear", b"OK index\r\n"),
        (b"search tag:os hit", terms.row("eligible", 4, b"hit") + terms.summary(1)),
    ])


def case_late_bad_header_no_partial_output(args, work):
    files = [("eligible", head(b"tag: os", body=b"hit\n"), 1)]
    files += [("empty%d" % n, b"", 0) for n in range(61)]
    files += [("bad", b"@yan-note 9\n@end\nhit\n", 2)]
    terms.run_commands(args, work, "bad", files, [
        (b"meta list tag:os", b"ERROR METADATA\r\n"),
        (b"search tag:os hit", b"ERROR METADATA\r\n"),
        (b"meta show bad", b"ERROR METADATA\r\n"),
        (b"search hit", terms.row("eligible", 4, b"hit") + terms.row("bad", 3, b"hit") + terms.summary(2)),
    ])


def case_large_binary_noop_and_change(args, work):
    files = [("large", head(b"tag: os", body=b"x" * 20000), 1),
             ("binary", head(b"tag: os", body=b"\0\xff"), 8)]
    terms.run_commands(args, work, "noops", files, [
        (b"meta show large", show("large", tags=(b"os",))),
        (b"meta tag large add os", b"OK meta tag\r\n"),
        (b"meta tag large del missing", b"OK meta tag\r\n"),
        (b"meta tag large add new", b"ERROR TOO_LARGE\r\n"),
        (b"meta tag binary add os", b"OK meta tag\r\n"),
        (b"meta tag binary add new", b"ERROR INVALID_TEXT\r\n"),
    ])


def case_filename_links_follow_existence(args, work):
    files = [("note", head(b"link: target", body=b"hit"), 1),
             ("target", b"", 2)]
    terms.run_commands(args, work, "links", files, [
        (b"meta show note", show("note", links=(("target", b"OK"),))),
        (b"mv target renamed", b"OK mv\r\n"),
        (b"meta show note", show("note", links=(("target", b"DANGLING"),))),
        (b"cp note copy", b"OK cp\r\n"),
        (b"meta show copy", show("copy", links=(("target", b"DANGLING"),))),
    ], readonly=False)


def case_raw_grammar_and_title_spaces(args, work):
    fields=(b'tag: os', b'tag: a"b', b'tag: a\\b')
    body=b'alpha beta\n'
    query=lambda line: terms.row('n',line,b'alpha beta')+terms.summary(1)
    duplicate=b' '.join([b'tag:os']*17)
    different=b' '.join(b'tag:t%d'%n for n in range(17))
    disk,_=terms.run_commands(args,work,'grammar',[('n',head(*fields,body=body),1)],[
        (b'search alpha tag:os beta',query(6)),
        (b'search tag:a"b alpha',query(6)),
        (b'search tag:a\\b alpha',query(6)),
        (b'search '+duplicate+b' alpha',query(6)),
        (b'meta list '+duplicate,b'n\r\nOK meta list shown=1\r\n'),
        (b'search '+different+b' alpha',b'ERROR USAGE\r\n'),
        (b'search alpha"beta tag:os',b'ERROR USAGE\r\n'),
        (b'search -alpha',b'ERROR USAGE\r\n'),
        (b'search tag:os -alpha',b'ERROR USAGE\r\n'),
        (b'search alpha tag:os -beta',query(6)),
        (b'search "-alpha"',query(6)),
        (b'search "tag:os alpha"',terms.summary(0)),
        (b'meta show n',show('n',tags=(b'os',b'a"b',b'a\\b'))),
        (b'meta set n title  a b ',b'OK meta set\r\n'),
        (b'meta show n',show('n',title=b' a b ',tags=(b'os',b'a"b',b'a\\b'))),
        (b'search alpha tag:os beta',query(7)),
        (b'meta set n created 1900-02-29',b'ERROR USAGE\r\n'),
        (b'meta show bad/name',b'ERROR USAGE\r\n'),
    ],readonly=False)
    if contents(disk,'n')!=head(b'title:  a b ',*fields,body=body):
        raise AssertionError('raw tag identity or title whitespace changed on disk')


CASES = [case_raw_grammar_and_title_spaces, case_observation_and_tag_identity, case_update_body_and_restart,
         case_filter_before_top20, case_late_bad_header_no_partial_output,
         case_large_binary_noop_and_change, case_filename_links_follow_existence]


def main():
    parser = argparse.ArgumentParser()
    for flag in ("source", "work", "run", "mkfs", "guest"):
        parser.add_argument("--" + flag, required=True, type=Path)
    args = parser.parse_args()
    missing = [args.source / path for path in INPUTS if not (args.source / path).is_file()]
    missing += [path for path in (args.run, args.mkfs, args.guest) if not path.is_file()]
    if missing:
        for path in missing:
            print(":FAIL: missing required input:", path)
        return 1
    failed = harness = 0
    for case in CASES:
        work = args.work / case.__name__
        work.mkdir(parents=True, exist_ok=True)
        try:
            case(args, work)
            print("PASS", case.__name__, flush=True)
        except AssertionError as error:
            failed += 1
            print(":FAIL:", case.__name__, error, flush=True)
        except (base.HarnessError, OSError, subprocess.TimeoutExpired) as error:
            harness += 1
            print(":HARNESS-ERROR:", case.__name__, error, flush=True)
    print("note_metadata Guest: %d passed, %d failed, %d harness errors" %
          (len(CASES) - failed - harness, failed, harness))
    return 2 if harness else (1 if failed else 0)


if __name__ == "__main__":
    sys.exit(main())
