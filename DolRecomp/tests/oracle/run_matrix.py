#!/usr/bin/env python3
"""Run the oracle in bounded Dolphin processes and merge strict captures."""

import argparse
import json
import subprocess
import tempfile
from pathlib import Path

from verify_capture import parse_capture


def main() -> int:
    here = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser()
    parser.add_argument("--coverage", type=Path, default=here / "coverage.json")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument(
        "--cpu-core",
        type=int,
        default=0,
        help="Dolphin.Core.CPUCore value; 0 is Interpreter64 and is the oracle default.",
    )
    parser.add_argument(
        "--dolphin-default-cpu-core",
        action="store_true",
        help="Do not override Dolphin.Core.CPUCore; useful for optional JIT drift checks.",
    )
    parser.add_argument("--chunk-size", type=int, default=48)
    parser.add_argument("--timeout", type=int, default=120)
    args = parser.parse_args()

    coverage = json.loads(args.coverage.read_text(encoding="ascii"))
    ordinary_count = int(coverage["total_ordinary"])
    if ordinary_count <= 0 or args.chunk_size <= 0:
        raise SystemExit("invalid coverage count or chunk size")

    merged_cases = []
    merged_blocks = []
    with tempfile.TemporaryDirectory(prefix="dolrecomp-oracle-") as temp:
        temp_dir = Path(temp)
        for start in range(0, ordinary_count, args.chunk_size):
            end = min(start + args.chunk_size, ordinary_count)
            capture_path = temp_dir / f"capture-{start}-{end}.txt"
            cmd = [
                "python3",
                str(here / "run_dolphin_oracle.py"),
                "--timeout",
                str(args.timeout),
                "--case-start",
                str(start),
                "--case-end",
                str(end),
                "--out",
                str(capture_path),
            ]
            if args.dolphin_default_cpu_core:
                cmd.append("--dolphin-default-cpu-core")
            else:
                cmd.extend(("--cpu-core", str(args.cpu_core)))
            subprocess.run(cmd, cwd=here, check=True)

            parsed = parse_capture(capture_path)
            merged_cases.extend(case["name"] for case in parsed["cases"])
            lines = capture_path.read_text(encoding="ascii").splitlines()
            merged_blocks.extend(lines[1:-1])

    expected_count = ordinary_count + 1
    if len(merged_cases) != expected_count:
        raise SystemExit(
            f"merged {len(merged_cases)} cases, expected {expected_count}"
        )
    if len(set(merged_cases)) != len(merged_cases):
        raise SystemExit("merged capture contains duplicate case names")

    text = (
        f"ORACLE_BEGIN,version,2,cases,{expected_count},mem_size,256\n"
        + "\n".join(merged_blocks)
        + "\nORACLE_END\n"
    )
    args.out.write_text(text, encoding="ascii")
    args.out.with_suffix(args.out.suffix + ".raw").write_bytes(text.encode("ascii"))
    print(
        f"merged {expected_count} cases from bounded Dolphin runs into {args.out}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
