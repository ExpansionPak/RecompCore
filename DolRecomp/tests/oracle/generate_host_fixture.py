#!/usr/bin/env python3
"""Generate a C fixture from a strict Interpreter oracle capture."""

import argparse
from pathlib import Path

from verify_capture import parse_capture, validate_seed_semantics


def c_u32(values) -> str:
    return ", ".join(f"0x{value:08X}u" for value in values)


def c_u64(values) -> str:
    return ", ".join(f"0x{value:016X}ull" for value in values)


def c_u8(hex_image: str) -> str:
    return ", ".join(f"0x{hex_image[i:i + 2]}u" for i in range(0, len(hex_image), 2))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("capture", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    capture = parse_capture(args.capture)
    validate_seed_semantics(capture)
    cases = [case for case in capture["cases"] if case["name"] != "msr_fp_clear"]

    lines = ["/* Generated from the strict Interpreter capture. Do not edit. */"]
    lines.append("static const HostOracleCase host_oracle_cases[] = {")
    for index, case in enumerate(cases):
        inp = case["input"]
        address = 0x81000000 + index * 0x100
        lines.extend(
            [
                "    {",
                f'        .name = "{case["name"]}",',
                f"        .raw = {{{c_u32(case['raw'])}}},",
                f"        .raw_count = {len(case['raw'])}u,",
                f"        .address = 0x{address:08X}u,",
                f"        .guest_mem_base = 0x{case['guest_mem_base']:08X}u,",
                f"        .oracle_mem_base = 0x{case['oracle_mem_base']:08X}u,",
                f"        .in_gpr = {{{c_u32(inp['in_gpr'])}}},",
                f"        .in_fpr = {{{c_u64(inp['in_fpr'])}}},",
                f"        .in_ps1 = {{{c_u64(inp['in_ps1'])}}},",
                f"        .in_cr = 0x{inp['cr']:08X}u,",
                f"        .in_xer = 0x{inp['xer']:08X}u,",
                f"        .in_lr = 0x{inp['lr']:08X}u,",
                f"        .in_ctr = 0x{inp['ctr']:08X}u,",
                f"        .in_msr = 0x{inp['msr']:08X}u,",
                f"        .in_fpscr = 0x{inp['fpscr'] & 0xFFFFFFFF:08X}u,",
                f"        .in_srr0 = 0x{inp['srr0']:08X}u,",
                f"        .in_srr1 = 0x{inp['srr1']:08X}u,",
                f"        .in_hid2 = 0x{inp['hid2']:08X}u,",
                f"        .in_sr = {{{c_u32(inp['in_sr'])}}},",
                f"        .in_gqr = {{{c_u32(inp['in_gqr'])}}},",
                f"        .in_mem = {{{c_u8(inp['memory'])}}},",
                f"        .out_gpr = {{{c_u32(case['gpr'])}}},",
                f"        .out_fpr = {{{c_u64(case['fpr'])}}},",
                f"        .out_ps1 = {{{c_u64(case['ps1'])}}},",
                f"        .out_cr = 0x{case['cr']:08X}u,",
                f"        .out_xer = 0x{case['xer']:08X}u,",
                f"        .out_lr = 0x{case['lr']:08X}u,",
                f"        .out_ctr = 0x{case['ctr']:08X}u,",
                f"        .out_msr = 0x{case['msr']:08X}u,",
                f"        .out_fpscr = 0x{case['fpscr_arch']:08X}u,",
                f"        .out_srr0 = 0x{case['srr0']:08X}u,",
                f"        .out_srr1 = 0x{case['srr1']:08X}u,",
                f"        .out_dar = 0x{case['dar']:08X}u,",
                f"        .out_dsisr = 0x{case['dsisr']:08X}u,",
                f"        .out_ear = 0x{case['ear']:08X}u,",
                f"        .out_hid2 = 0x{case['hid2']:08X}u,",
                f"        .out_sr = {{{c_u32(case['sr'])}}},",
                f"        .out_gqr = {{{c_u32(case['gqr'])}}},",
                f"        .out_exception = {case['exception']}u,",
                f"        .out_mem = {{{c_u8(case['memory'])}}},",
                "    },",
            ]
        )
    lines.append("};")
    lines.append(
        "static const unsigned host_oracle_case_count = "
        "(unsigned)(sizeof(host_oracle_cases) / sizeof(host_oracle_cases[0]));"
    )
    args.output.write_text("\n".join(lines) + "\n", encoding="ascii")
    print(f"generated {len(cases)} host differential cases into {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
