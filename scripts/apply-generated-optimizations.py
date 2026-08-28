#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path
import sys

MARKER = "ANIMANIACS_PERF_OPT_8013F83C_V3"
FUNC_SIGNATURE = "static void loop_8013F83C(CPUState* ctx)"

REPLACEMENT = r'''static void loop_8013F83C(CPUState* ctx) {
    /* ANIMANIACS_PERF_OPT_8013F83C_V3
     *
     * GANE7U hot polling loop.
     *
     * V2 stopped rewriting unchanged r0/CR0/PC state. V3 also hoists the MEM1
     * translation and keeps downcount in a host register while the word is
     * zero. The relaxed atomic load prevents the compiler from caching a word
     * which can be changed by another emulation thread.
     *
     * The portable mem_read32 path remains below for unusual addresses/hosts.
     */
    const u32 ea = ctx->gpr[13] + (u32)(s32)(-23240);
    const u32 cr_low = ctx->cr & 0x0FFFFFFFu;
    const u32 so = (ctx->xer >> 31) & 1u;
    const u32 cr_eq = cr_low | ((0x2u | so) << 28);
    const u32 cr_gt = cr_low | ((0x4u | so) << 28);

#if defined(__GNUC__) || defined(__clang__)
    const u32 masked_ea = ea & ~0x40000000u;
    const u32 offset = masked_ea - GC_RAM_BASE;

    if (ctx->ram != 0 && (ea & 3u) == 0u && offset <= GC_MAIN_RAM_SIZE - 4u) {
        const u32* const poll_word =
            (const u32*)(const void*)(ctx->ram + offset);
        s64 downcount = ctx->downcount;

        ctx->pc = 0x8013F83Cu;

        downcount -= 3;
        u32 raw = __atomic_load_n(poll_word, __ATOMIC_RELAXED);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        u32 value = raw;
#else
        u32 value = __builtin_bswap32(raw);
#endif
        ctx->gpr[0] = value;

        if (value != 0u) {
            ctx->downcount = downcount;
            ctx->cr = cr_gt;
            ctx->pc = 0x8013F848u;
            return;
        }

        ctx->cr = cr_eq;

        if (downcount <= -(s64)DOLRECOMP_C_LOOP_CYCLE_BUDGET) {
            ctx->downcount = downcount;
            return;
        }

        for (;;) {
            downcount -= 3;
            raw = __atomic_load_n(poll_word, __ATOMIC_RELAXED);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
            value = raw;
#else
            value = __builtin_bswap32(raw);
#endif

            if (value != 0u) {
                ctx->downcount = downcount;
                ctx->gpr[0] = value;
                ctx->cr = cr_gt;
                ctx->pc = 0x8013F848u;
                return;
            }

            if (downcount <= -(s64)DOLRECOMP_C_LOOP_CYCLE_BUDGET) {
                ctx->downcount = downcount;
                return;
            }
        }
    }
#endif

    /* Exact portable/MMIO fallback: V2 behavior. */
    ctx->pc = 0x8013F83Cu;

    ctx->downcount -= 3;
    u32 value = mem_read32(ctx, ea);
    ctx->gpr[0] = value;

    if (value != 0u) {
        ctx->cr = cr_gt;
        ctx->pc = 0x8013F848u;
        return;
    }

    ctx->cr = cr_eq;

    if (ctx->downcount <= -(s64)DOLRECOMP_C_LOOP_CYCLE_BUDGET)
        return;

    for (;;) {
        ctx->downcount -= 3;
        value = mem_read32(ctx, ea);

        if (value != 0u) {
            ctx->gpr[0] = value;
            ctx->cr = cr_gt;
            ctx->pc = 0x8013F848u;
            return;
        }

        if (ctx->downcount <= -(s64)DOLRECOMP_C_LOOP_CYCLE_BUDGET)
            return;
    }
}'''


def find_function_span(text: str) -> tuple[int, int]:
    start = text.find(FUNC_SIGNATURE)
    if start < 0:
        raise RuntimeError(f"{FUNC_SIGNATURE!r} not found")

    brace = text.find("{", start)
    if brace < 0:
        raise RuntimeError("function opening brace not found")

    depth = 0
    in_string = False
    in_char = False
    escaped = False
    i = brace
    while i < len(text):
        ch = text[i]

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
                    return start, i + 1
        i += 1

    raise RuntimeError("function closing brace not found")


def patch_file(path: Path) -> bool:
    text = path.read_text()
    if MARKER in text:
        print(f"[generated-opt] already optimized: {path}")
        return False

    start, end = find_function_span(text)
    old = text[start:end]

    required = (
        "0x8013F83C",
        "0x8013F848",
        "-23240",
        "mem_read32",
        "DOLRECOMP_C_LOOP_CYCLE_BUDGET",
    )
    missing = [token for token in required if token not in old]
    if missing:
        raise RuntimeError(
            f"refusing to patch unexpected loop body; missing: {', '.join(missing)}"
        )

    path.write_text(text[:start] + REPLACEMENT + text[end:])
    print(f"[generated-opt] optimized {path}")
    return True


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "artifact",
        type=Path,
        help="moderngekko-port artifact directory containing dolrecomp-output/generated",
    )
    args = parser.parse_args()

    artifact = args.artifact.resolve()
    generated = artifact / "dolrecomp-output" / "generated"
    chunks = generated / "chunks"

    if not chunks.is_dir():
        print(f"error: generated chunk directory is missing: {chunks}", file=sys.stderr)
        return 1

    # Locate by function signature, not by a chunk filename. This also makes
    # the optimization survive future code-generator chunk-boundary changes.
    candidates = []
    for candidate in sorted(chunks.glob("chunk_*_text1_*.c")):
        try:
            candidate_text = candidate.read_text()
        except UnicodeDecodeError:
            continue
        if FUNC_SIGNATURE in candidate_text:
            candidates.append(candidate)

    if len(candidates) != 1:
        print(
            f"error: expected exactly one generated {FUNC_SIGNATURE}, "
            f"found {len(candidates)} in {chunks}",
            file=sys.stderr,
        )
        for candidate in candidates:
            print(f"  {candidate}", file=sys.stderr)
        return 1

    try:
        changed = patch_file(candidates[0])
    except RuntimeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    print("[generated-opt] result=" + ("changed" if changed else "unchanged"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
