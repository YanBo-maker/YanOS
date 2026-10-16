"""Mutation-CLI fixtures copy named repository inputs, never unrelated work."""
from pathlib import Path
import os
import shutil
import tempfile
from unittest.mock import patch


def copy_named_inputs(source, target, relative_paths):
    paths = sorted({Path(p) for p in relative_paths}, key=lambda p: p.as_posix())
    if any(p.is_absolute() or '..' in p.parts for p in paths):
        raise ValueError('fixture inputs must be repository-relative')
    target.mkdir(parents=True)
    for relative in paths:
        destination = target / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source / relative, destination)


def self_test_named_copy():
    with tempfile.TemporaryDirectory(prefix='named-fixture-control-') as tmp:
        root = Path(tmp); source = root / 'source'
        (source / 'os').mkdir(parents=True)
        (source / 'os' / 'needed.c').write_bytes(b'needed input')
        (source / 'unrelated').mkdir()
        (source / 'unrelated' / 'changing.tmp').write_bytes(b'outside fixture scope')
        target = root / 'target'
        # The old broad copy reproduces the fault here without ever reading
        # unrelated real workspace data. A named copy needs no directory walk.
        with patch.object(os, 'scandir', side_effect=AssertionError('fixture scanned an unrelated directory')):
            copy_named_inputs(source, target, ['os/needed.c'])
        assert (target / 'os' / 'needed.c').read_bytes() == b'needed input'
        assert not (target / 'unrelated').exists()
        try: copy_named_inputs(source, root / 'missing', ['os/missing.c'])
        except FileNotFoundError: pass
        else: raise AssertionError('missing repository input was silently ignored')
        try: copy_named_inputs(source, root / 'escape', ['../outside'])
        except ValueError: pass
        else: raise AssertionError('fixture allowed path escape')
    print('PASS named fixture copy: 3 controls')


if __name__ == '__main__': self_test_named_copy()
