# Golden baseline for the executor's default path

These two files are the frozen output of `yan_run` on
`tests/guest/golden_probe.c`, the deterministic Guest probe that touches no
device. `tests/guest/run_golden.sh` recompiles the probe, runs it with an
explicit geometry (`--base 0x80000000 --ram 16777216`) and compares both files
byte for byte, printing the first differing line with both sides when they part.

| File | What it is | Size |
| --- | --- | --- |
| `probe.trace.jsonl` | `yan_run --trace` output: one JSON object per executed instruction (pc, instruction word, next pc, status, the 32 registers, mstatus/mepc/mcause/mtval) | 656 lines |
| `probe.signature.hex` | The `--signature` bytes (the image's `begin_signature`..`end_signature` region) as `od -Ax -tx1 -v` hex text | 1028 lines (16420 bytes) |

## How they were generated

```sh
bash tests/guest/run_golden.sh --gcc <riscv-gcc> --run <build>/yan_run \
    --work /tmp/golden --update
```

`--update` runs the probe twice and refuses to install the files when the two
runs disagree, so a frozen baseline is always reproducible. It never runs
automatically, and the script's normal mode only reads these files.

## When to regenerate

Regenerate only when the change is intended and understood:

- `tests/guest/golden_probe.c` changes, which changes the instruction stream by
  definition;
- the linker script or the toolchain version moves addresses or encodings;
- the trace format in `tools/yan_run.c` changes on purpose.

Do **not** regenerate to make a failing check pass: the failure is the signal
that the default execution path changed. Read the diff first — the script prints
the first differing line with both sides.

## What this baseline covers, and what it does not

Covered: the instruction stream, the register writeback after every step, the
CSR state the trace records, and the memory effects the signature captures.

Not covered: `mtime` and any other device state (the probe touches no device on
purpose, so the baseline stays independent of whether a terminal is attached),
the `--terminal` path itself, and anything outside the single image this probe
builds.
