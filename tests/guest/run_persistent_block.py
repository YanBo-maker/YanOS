"""Cross-process persistence acceptance, with independent host byte checks."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def run(command, expected=0, env=None):
    result = subprocess.run(command, capture_output=True, text=True, timeout=60, env=env)
    if result.returncode < 0 or "Sanitizer" in result.stderr or "runtime error:" in result.stderr:
        raise RuntimeError(f"process crashed or sanitizer failed: {command}\n{result.stderr}")
    require(result.returncode == expected,
            f"{command}: exit {result.returncode}, expected {expected}\n"
            f"{result.stdout}{result.stderr}")
    require("Sanitizer" not in result.stderr and "runtime error:" not in result.stderr,
            result.stderr)
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--work", required=True, type=Path)
    parser.add_argument("--run", required=True)
    parser.add_argument("--run-close-fault")
    parser.add_argument("--gcc", required=True)
    args = parser.parse_args()
    if not shutil.which(args.gcc):
        print("SKIP: missing external cross compiler")
        return 77
    source, work = args.source.resolve(), args.work.resolve()
    guest = source / "tests/guest"
    required = [guest / name for name in ("persistent_block_check.c", "mtrap_entry.S",
                "mtrap.c", "guest_lib.c", "link.ld")]
    required += [source / "os/block.c", Path(args.run)]
    if args.run_close_fault:
        required.append(Path(args.run_close_fault))
    for path in required:
        require(path.is_file(), f"source or executable under test missing: {path}")
    work.mkdir(parents=True, exist_ok=True)
    # Every fixture lives in a fresh directory; reruns never overwrite user disks.
    import tempfile
    with tempfile.TemporaryDirectory(prefix="case-", dir=work) as directory:
        case = Path(directory)
        initial = bytes((i * 7 + i // 4096 * 11 + 93) & 255 for i in range(32768))
        for lba in (0, 3, 7):
            disk = case / f"disk-{lba}.img"
            disk.write_bytes(initial)
            images = []
            for write in (1, 0):
                elf = case / f"guest-{lba}-{write}.elf"
                run([args.gcc, "-march=rv32im", "-mabi=ilp32", "-mcmodel=medany",
                     "-static", "-nostdlib", "-nostartfiles", "-ffreestanding",
                     "-fno-builtin", "-fno-stack-protector", "-O2", "-Wall", "-Wextra",
                     "-Werror", "-I", str(guest), "-I", str(source / "os"),
                     f"-DPERSIST_WRITE={write}", f"-DPERSIST_LBA={lba}",
                     "-T", str(guest / "link.ld"),
                     *map(str, required[:4]), str(source / "os/block.c"),
                     "-Wl,--build-id=none", "-o", str(elf)])
                images.append(elf)
            common = [args.run, "--terminal", "--max-steps", "1000000"]
            # Reader alone must reject the original sentinel disk (negative control).
            result = run(common + ["--image", str(images[1]), "--disk-image", str(disk)], 6)
            require(":FAIL:" in result.stdout, "reader negative control lacked assertion")
            require("the Guest reported failure code 2147483656 after " in result.stderr,
                    "reader negative control did not fail its byte comparison (0x80000008)")
            run(common + ["--image", str(images[0]), "--disk-image", str(disk)])
            expected = bytearray(initial)
            expected[lba*4096:(lba+1)*4096] = bytes(
                (i*17 + (i >> 8)*29 + lba*41) & 255 for i in range(4096))
            require(disk.read_bytes() == expected, "writer changed wrong bytes or lost data")
            for _ in range(2):
                run(common + ["--image", str(images[1]), "--disk-image", str(disk)])
                require(disk.read_bytes() == expected, "reopen/read modified disk")
            print(f"PASS: block {lba}: writer exited, fresh readers compared 4096 bytes")

        elf = str(images[1])
        if args.run_close_fault:
            # Same successful reader and disk, with only the disk's fclose
            # changed. A failure must override Guest PASS with Host exit 5.
            close_command = [args.run_close_fault, "--image", elf, "--disk-image", str(disk),
                             "--terminal", "--max-steps", "1000000"]
            close_env = dict(os.environ)
            close_env.pop("YAN_TEST_DISK_CLOSE_PATH", None)
            run(close_command, env=close_env)
            close_env["YAN_TEST_DISK_CLOSE_PATH"] = str(disk)
            result = run(close_command, 5, env=close_env)
            require(result.stderr == "TEST: disk fclose failure injected\n"
                    "yan_run: disk close failed\n", "close fault did not reach the final Host exit")
            require(disk.read_bytes() == expected, "closing a reader changed the image")
            print("PASS: Guest PASS plus disk close failure exits 5; image unchanged")

        for name, payload in (("empty", b""), ("unaligned", b"x" * 4097)):
            bad = case / name
            bad.write_bytes(payload)
            run([args.run, "--image", elf, "--disk-image", str(bad)], 5)
            require(bad.read_bytes() == payload, "invalid image was modified")
        absent = case / "absent.img"
        run([args.run, "--image", elf, "--disk-image", str(absent)], 5)
        require(not absent.exists(), "opening missing image created it")
        run([args.run, "--image", elf, "--disk-image", str(case)], 5)
        for switches in (["--disk", "8", "--disk-image", str(disk)],
                         ["--disk-image", str(disk), "--disk", "8"]):
            run([args.run, "--image", elf, *switches], 2)
        # Output aliases must not truncate an attached disk, including hardlinks.
        alias = case / "alias.img"
        alias.hardlink_to(disk)
        for path in (disk, alias):
            run([args.run, "--image", elf, "--disk-image", str(disk),
                 "--trace", str(path)], 5)
            run([args.run, "--image", elf, "--disk-image", str(disk),
                 "--signature", str(path), "0x80000000", "0x80000004"], 5)
            require(disk.read_bytes() == expected, "output alias destroyed disk")
    print("PASS: persistent block acceptance and invalid-image checks")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as error:
        print(f":FAIL: {error}", file=sys.stderr)
        sys.exit(1)
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"HARNESS-ERROR: {error}", file=sys.stderr)
        sys.exit(2)
