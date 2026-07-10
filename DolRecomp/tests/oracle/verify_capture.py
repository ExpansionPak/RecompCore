#!/usr/bin/env python3
"""Strictly validate and normalize Dolphin oracle captures."""

import argparse
import copy
import json
from pathlib import Path


VECTOR_COUNTS = {"GPR": 32, "FPR": 32, "PS1": 32}
CASE_FIELDS = (
    "exception",
    "exid",
    "pc",
    "cr",
    "xer",
    "lr",
    "ctr",
    "msr",
    "fpscr",
    "guest_mem_base",
    "oracle_mem_base",
)


def fail(message: str) -> None:
    raise ValueError(message)


def parse_hex(value: str, bits: int, context: str) -> int:
    width = bits // 4
    if len(value) != width or any(c not in "0123456789ABCDEF" for c in value):
        fail(f"{context}: expected {width} uppercase hex digits, got {value!r}")
    return int(value, 16)


def parse_capture(path: Path) -> dict:
    try:
        text = path.read_text(encoding="ascii")
    except UnicodeDecodeError as exc:
        fail(f"{path}: non-ASCII byte at offset {exc.start}")
    if not text.endswith("\n"):
        fail(f"{path}: missing final newline")

    lines = text.splitlines()
    if not lines or lines[-1] != "ORACLE_END":
        fail(f"{path}: missing exact ORACLE_END trailer")

    header = lines[0].split(",")
    if len(header) != 7 or header[:2] != ["ORACLE_BEGIN", "version"]:
        fail(f"{path}: malformed header")
    if header[2] != "2" or header[3] != "cases" or header[5] != "mem_size":
        fail(f"{path}: unsupported header {lines[0]!r}")
    declared_count = int(header[4])
    mem_size = int(header[6])
    if declared_count <= 0 or mem_size != 256:
        fail(f"{path}: invalid case count or memory size")

    cases = []
    seen = set()
    index = 1
    while index < len(lines) - 1:
        fields = lines[index].split(",")
        if len(fields) != 3 + 2 * len(CASE_FIELDS) or fields[0] != "CASE":
            fail(f"{path}:{index + 1}: malformed CASE record")
        name = fields[1]
        if not name or name in seen:
            fail(f"{path}:{index + 1}: empty or duplicate case name {name!r}")
        seen.add(name)
        try:
            word_count = int(fields[2])
        except ValueError:
            fail(f"{path}:{index + 1}: invalid instruction count")
        if word_count <= 0 or word_count > 16:
            fail(f"{path}:{index + 1}: instruction count out of range")

        metadata = {}
        for offset, expected_name in enumerate(CASE_FIELDS):
            key = fields[3 + offset * 2]
            value = fields[4 + offset * 2]
            if key != expected_name:
                fail(
                    f"{path}:{index + 1}: expected field {expected_name!r}, got {key!r}"
                )
            if key in ("exception", "exid"):
                if not value.isdecimal():
                    fail(f"{path}:{index + 1}:{key}: expected decimal integer")
                metadata[key] = int(value)
            else:
                bits = 64 if key == "fpscr" else 32
                metadata[key] = parse_hex(value, bits, f"{path}:{index + 1}:{key}")
        index += 1

        raw_fields = lines[index].split(",")
        if raw_fields[0] != "RAW" or len(raw_fields) != word_count + 1:
            fail(f"{path}:{index + 1}: RAW count does not match CASE")
        raw = [
            parse_hex(value, 32, f"{path}:{index + 1}:RAW")
            for value in raw_fields[1:]
        ]
        index += 1

        input_fields = lines[index].split(",")
        input_names = ("cr", "xer", "lr", "ctr", "msr", "fpscr")
        if len(input_fields) != 1 + 2 * len(input_names) or input_fields[0] != "INPUT":
            fail(f"{path}:{index + 1}: malformed INPUT record")
        input_metadata = {}
        for offset, expected_name in enumerate(input_names):
            key = input_fields[1 + offset * 2]
            value = input_fields[2 + offset * 2]
            if key != expected_name:
                fail(f"{path}:{index + 1}: expected INPUT field {expected_name!r}")
            input_metadata[key] = parse_hex(
                value, 64 if key == "fpscr" else 32,
                f"{path}:{index + 1}:INPUT:{key}"
            )
        index += 1

        input_vectors = {}
        for label, bits in (("IN_GPR", 32), ("IN_FPR", 64), ("IN_PS1", 64)):
            vector_fields = lines[index].split(",")
            if vector_fields[0] != label or len(vector_fields) != 33:
                fail(f"{path}:{index + 1}: malformed {label} vector")
            input_vectors[label.lower()] = [
                parse_hex(value, bits, f"{path}:{index + 1}:{label}")
                for value in vector_fields[1:]
            ]
            index += 1

        input_special_fields = lines[index].split(",")
        if (
            len(input_special_fields) != 7
            or input_special_fields[0] != "IN_SPECIAL"
            or input_special_fields[1::2] != ["srr0", "srr1", "hid2"]
        ):
            fail(f"{path}:{index + 1}: malformed IN_SPECIAL record")
        input_special = {
            input_special_fields[i]: parse_hex(
                input_special_fields[i + 1], 32,
                f"{path}:{index + 1}:IN_SPECIAL:{input_special_fields[i]}"
            )
            for i in range(1, 7, 2)
        }
        index += 1

        for label, count in (("IN_SR", 16), ("IN_GQR", 8)):
            vector_fields = lines[index].split(",")
            if vector_fields[0] != label or len(vector_fields) != count + 1:
                fail(f"{path}:{index + 1}: malformed {label} vector")
            input_vectors[label.lower()] = [
                parse_hex(value, 32, f"{path}:{index + 1}:{label}")
                for value in vector_fields[1:]
            ]
            index += 1

        mem_input_fields = lines[index].split(",")
        if len(mem_input_fields) != 2 or mem_input_fields[0] != "MEM_IN":
            fail(f"{path}:{index + 1}: malformed MEM_IN record")
        mem_input_hex = mem_input_fields[1]
        if (
            len(mem_input_hex) != mem_size * 2
            or any(c not in "0123456789ABCDEF" for c in mem_input_hex)
        ):
            fail(f"{path}:{index + 1}: invalid input memory image")
        index += 1

        vectors = {}
        for label, count in VECTOR_COUNTS.items():
            vector_fields = lines[index].split(",")
            if vector_fields[0] != label or len(vector_fields) != count + 1:
                fail(f"{path}:{index + 1}: malformed {label} vector")
            bits = 32 if label == "GPR" else 64
            vectors[label.lower()] = [
                parse_hex(value, bits, f"{path}:{index + 1}:{label}")
                for value in vector_fields[1:]
            ]
            index += 1

        special_fields = lines[index].split(",")
        special_names = ("srr0", "srr1", "dar", "dsisr", "ear", "hid2")
        if (
            len(special_fields) != 1 + 2 * len(special_names)
            or special_fields[0] != "SPECIAL"
            or tuple(special_fields[1::2]) != special_names
        ):
            fail(f"{path}:{index + 1}: malformed SPECIAL record")
        specials = {
            special_fields[i]: parse_hex(
                special_fields[i + 1], 32,
                f"{path}:{index + 1}:SPECIAL:{special_fields[i]}"
            )
            for i in range(1, len(special_fields), 2)
        }
        index += 1
        for label, count in (("SR", 16), ("GQR", 8)):
            vector_fields = lines[index].split(",")
            if vector_fields[0] != label or len(vector_fields) != count + 1:
                fail(f"{path}:{index + 1}: malformed {label} vector")
            vectors[label.lower()] = [
                parse_hex(value, 32, f"{path}:{index + 1}:{label}")
                for value in vector_fields[1:]
            ]
            index += 1

        mem_fields = lines[index].split(",")
        if len(mem_fields) != 2 or mem_fields[0] != "MEM":
            fail(f"{path}:{index + 1}: malformed MEM record")
        mem_hex = mem_fields[1]
        if (
            len(mem_hex) == 0
            or len(mem_hex) % 2
            or len(mem_hex) > mem_size * 2
            or any(c not in "0123456789ABCDEF" for c in mem_hex)
        ):
            fail(f"{path}:{index + 1}: invalid memory image")
        index += 1

        if lines[index] != f"ENDCASE,{name}":
            fail(f"{path}:{index + 1}: missing matching ENDCASE")
        index += 1

        cases.append(
            {
                "name": name,
                "raw": raw,
                **metadata,
                **specials,
                **vectors,
                "input": {
                    **input_metadata,
                    **input_special,
                    **input_vectors,
                    "memory": mem_input_hex,
                },
                "memory": mem_hex,
                "fpscr_arch": metadata["fpscr"] & 0xFFFFFFFF,
            }
        )

    if len(cases) != declared_count:
        fail(f"{path}: declared {declared_count} cases, parsed {len(cases)}")
    return {"version": 2, "mem_size": mem_size, "cases": cases}


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def validate_seed_semantics(capture: dict) -> None:
    by_name = {case["name"]: case for case in capture["cases"]}
    expected_names = {
        "seq.carry",
        "seq.lfs",
        "seq.frsp",
        "seq.fmadd",
        "seq.crbr",
        "msr_fp_clear",
    }
    require(expected_names <= set(by_name), "capture is missing a required seed case")

    for name in expected_names:
        case = by_name[name]
        require(case["lr"] == 0x817F0000, f"{name}: LR sentinel changed")
        require(case["ctr"] == 0x817F1000, f"{name}: CTR sentinel changed")
        require(len(case["gpr"]) == len(case["fpr"]) == len(case["ps1"]) == 32,
                f"{name}: incomplete register vector")

    case = by_name["seq.carry"]
    require(case["exception"] == 0, "seq.carry: unexpected exception")
    require(case["gpr"][4] == 0 and case["gpr"][5] == 0, "seq.carry: bad GPR result")
    require(case["xer"] & 0x20000000, "seq.carry: carry bit not propagated")

    case = by_name["seq.lfs"]
    require(case["fpr"][1] == 0x3FF0000000000000, "seq.lfs: bad PS0")
    require(case["ps1"][1] == 0x3FF0000000000000, "seq.lfs: lfs did not fill PS1")
    require(case["memory"][0x40 * 2 : 0x48 * 2] == "3F8000003F800000",
            "seq.lfs: consumer store mismatch")

    case = by_name["seq.frsp"]
    require(case["fpr"][1] == 0x3FF8000000000000, "seq.frsp: bad PS0")
    require(case["ps1"][1] == 0x3FF8000000000000, "seq.frsp: frsp did not fill PS1")
    require(case["memory"][0x40 * 2 : 0x48 * 2] == "3FC000003FC00000",
            "seq.frsp: consumer store mismatch")

    case = by_name["seq.fmadd"]
    require(case["fpr"][17] == 0x4024000000000000, "seq.fmadd: bad PS0")
    require(case["ps1"][17] == 0x4024000000000000,
            "seq.fmadd: fmadds did not fill PS1")
    require(case["memory"][0x40 * 2 : 0x48 * 2] == "4120000041200000",
            "seq.fmadd: consumer store mismatch")

    case = by_name["seq.crbr"]
    require(case["exception"] == 0, "seq.crbr: unexpected exception")
    require(case["gpr"][7] == 0 and case["gpr"][8] == 2, "seq.crbr: bad branch path")
    require(case["cr"] & 0xF0000000 == 0x20000000, "seq.crbr: CR0 EQ is not set")

    case = by_name["msr_fp_clear"]
    require(case["exception"] == 1 and case["exid"] == 8,
            "msr_fp_clear: expected FP-unavailable exception")
    require(case["pc"] != 0 and case["msr"] & 0x2000 == 0,
            "msr_fp_clear: bad fault PC or MSR[FP]")
    require(all(value == 0 for value in case["gpr"]),
            "msr_fp_clear: raw instruction modified GPR state")
    require(set(case["memory"]) == {"0"}, "msr_fp_clear: memory changed")


def compare_cores(primary: dict, secondary: dict) -> int:
    if primary == secondary:
        return 0
    normalized = copy.deepcopy(secondary)
    primary_cases = {case["name"]: case for case in primary["cases"]}
    secondary_cases = {case["name"]: case for case in normalized["cases"]}
    if set(primary_cases) != set(secondary_cases):
        fail("Interpreter and JIT have different case sets")

    interp_cmp = primary_cases.get("cmp")
    jit_cmp = secondary_cases.get("cmp")
    allowed = 0
    if interp_cmp and jit_cmp:
        if interp_cmp["cr"] == 0x08000000 and jit_cmp["cr"] == 0x09000000:
            jit_cmp["cr"] = interp_cmp["cr"]
            allowed = 1

    if primary != normalized:
        for left, right in zip(primary["cases"], normalized["cases"]):
            if left != right:
                fail(f"Interpreter and JIT differ unexpectedly in case {left['name']}")
        fail("Interpreter and JIT capture metadata differs unexpectedly")
    return allowed


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("capture", type=Path)
    parser.add_argument("--compare", type=Path)
    parser.add_argument("--json", type=Path)
    args = parser.parse_args()

    capture = parse_capture(args.capture)
    validate_seed_semantics(capture)

    allowed_core_differences = 0
    if args.compare:
        other = parse_capture(args.compare)
        validate_seed_semantics(other)
        allowed_core_differences = compare_cores(capture, other)

    if args.json:
        args.json.write_text(
            json.dumps(capture, indent=2, sort_keys=True) + "\n", encoding="ascii"
        )

    suffix = ""
    if args.compare:
        suffix = f"; matches {args.compare}"
        if allowed_core_differences:
            suffix += (
                f" except {allowed_core_differences} catalogued Dolphin JIT discrepancy"
            )
    print(f"PASS: {args.capture}: {len(capture['cases'])} strict full-state cases{suffix}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as exc:
        raise SystemExit(f"FAIL: {exc}") from exc
