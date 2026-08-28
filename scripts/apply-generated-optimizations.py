#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path
import sys

MARKER = "ANIMANIACS_IDLE_SPIN_V12"
SPIN_LOOPS = (
    (2148714392, 13, -30356, True, True),
    (2148792380, 13, -23240, False, True),
    (2149073296, 13, -29408, True, False),
    (2149073420, 13, -29408, True, False),
    (2149074084, 13, -29408, True, False),
    (2149074216, 13, -29408, True, False),
    (2149074304, 13, -29408, True, False),
    (2149074684, 13, -29408, True, False),
    (2149104232, 13, -29304, True, True),
    (2149118000, 31, 124, True, True),
    (2149244180, 13, -21372, True, True),
)


def find_function_span(text: str, signature: str) -> tuple[int, int]:
    start = text.find(signature)
    if start < 0:
        raise RuntimeError(f"{signature!r} not found")
    brace = text.find("{", start)
    if brace < 0:
        raise RuntimeError(f"{signature}: opening brace not found")

    depth = 0
    in_string = False
    in_char = False
    escaped = False
    for index in range(brace, len(text)):
        ch = text[index]
        if escaped:
            escaped = False
        elif ch == "\\" and (in_string or in_char):
            escaped = True
        elif ch == '"' and not in_char:
            in_string = not in_string
        elif ch == "'" and not in_string:
            in_char = not in_char
        elif not in_string and not in_char:
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    return start, index + 1
    raise RuntimeError(f"{signature}: closing brace not found")


def replacement(address: int, base_gpr: int, displacement: int,
                signed_cmp: bool, wait_while_zero: bool) -> str:
    next_pc = address + 12

    if signed_cmp:
        cr_calc = (
            "    const s32 signed_value = (s32)value;\n"
            "    const u32 compare_bits =\n"
            "        signed_value < 0 ? 0x8u : (signed_value > 0 ? 0x4u : 0x2u);"
        )
    else:
        cr_calc = "    const u32 compare_bits = value != 0u ? 0x4u : 0x2u;"

    exit_test = "value != 0u" if wait_while_zero else "value == 0u"

    return (
        f"static void loop_{address:08X}(CPUState* ctx) {{\n"
        f"    /* {MARKER}\n"
        "     * Exact one-poll version of an invariant-address GANE7U busy wait.\n"
        "     * When the wait is still active, PC stays at the loop head so the\n"
        "     * StaticRecomp chassis can CoreTiming::Idle() the rest of the slice.\n"
        "     */\n"
        f"    const u32 ea = ctx->gpr[{base_gpr}] + (u32)(s32)({displacement});\n\n"
        f"    ctx->pc = 0x{address:08X}u;\n"
        "    ctx->downcount -= 3;\n\n"
        "    const u32 value = mem_read32(ctx, ea);\n"
        "    ctx->gpr[0] = value;\n\n"
        "    const u32 cr_low = ctx->cr & 0x0FFFFFFFu;\n"
        "    const u32 so = (ctx->xer >> 31) & 1u;\n"
        f"{cr_calc}\n"
        "    ctx->cr = cr_low | ((compare_bits | so) << 28);\n\n"
        f"    if ({exit_test})\n"
        f"        ctx->pc = 0x{next_pc:08X}u;\n"
        "}\n"
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "artifact",
        type=Path,
        help="moderngekko-port artifact directory containing dolrecomp-output/generated",
    )
    args = parser.parse_args()

    chunks = args.artifact.resolve() / "dolrecomp-output" / "generated" / "chunks"
    if not chunks.is_dir():
        print(f"error: generated chunk directory is missing: {chunks}", file=sys.stderr)
        return 1

    files = sorted(chunks.glob("chunk_*_text1_*.c"))
    if not files:
        print(f"error: no generated text1 chunks in {chunks}", file=sys.stderr)
        return 1

    contents: dict[Path, str] = {}
    owners: dict[int, list[Path]] = {}

    for path in files:
        try:
            text = path.read_text()
        except UnicodeDecodeError:
            continue
        contents[path] = text
        for address, *_ in SPIN_LOOPS:
            signature = f"static void loop_{address:08X}(CPUState* ctx)"
            if signature in text:
                owners.setdefault(address, []).append(path)

    changed_files: set[Path] = set()
    patched = 0

    for address, base_gpr, displacement, signed_cmp, wait_while_zero in SPIN_LOOPS:
        hits = owners.get(address, [])
        if len(hits) == 0:
            print(f"[generated-opt] warning: loop_{address:08X} not emitted as helper")
            continue
        if len(hits) != 1:
            print(f"error: loop_{address:08X} emitted {len(hits)} times", file=sys.stderr)
            return 1

        path = hits[0]
        text = contents[path]
        signature = f"static void loop_{address:08X}(CPUState* ctx)"
        start, end = find_function_span(text, signature)
        old = text[start:end]

        if MARKER in old:
            continue

        text = text[:start] + replacement(
            address, base_gpr, displacement, signed_cmp, wait_while_zero
        ) + text[end:]
        contents[path] = text
        changed_files.add(path)
        patched += 1

    for path in sorted(changed_files):
        path.write_text(contents[path])
        print(f"[generated-opt] idle-spin optimized {path}")

    print(f"[generated-opt] idle-spin helpers patched: {patched}/{len(SPIN_LOOPS)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
