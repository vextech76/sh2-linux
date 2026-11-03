#!/usr/bin/env python3
"""Differential test: LANTERN's R5900 decoder vs GNU objdump (-m mips:5900).

Generates synthetic instruction words that sweep every opcode/function field, disassembles
them with both tools, and reports mnemonic disagreements. Needs binutils-mips64-linux-gnu.

usage: r5900_oracle_diff.py LANTERN_DISASM [--show N] [--random N | --file RAW]
"""
import random
import re
import subprocess
import sys
import tempfile

OBJDUMP = "mips64-linux-gnu-objdump"
VU_SUFFIX = re.compile(r"^(v[a-z0-9]+)\.[xyzw]*$")


def gen_words():
    rnd = random.Random(5900)
    words = []
    for op in range(64):
        for rs in range(32):
            for funct in range(64):
                w = (op << 26) | (rs << 21) | (rnd.getrandbits(16) << 6 & 0x1FFFC0) | funct
                words.append(w)
    # Sub-op fields that live in rt (REGIMM, COPz branches) and sa (MMI0-3, PMFHL).
    for rt in range(32):
        words.append((1 << 26) | (rnd.getrandbits(5) << 21) | (rt << 16) | rnd.getrandbits(16))
        for cop in (16, 17, 18):
            words.append((cop << 26) | (8 << 21) | (rt << 16) | rnd.getrandbits(16))
    for funct in (8, 9, 40, 41, 48, 49):
        for sa in range(32):
            words.append((28 << 26) | (rnd.getrandbits(15) << 11) | (sa << 6) | funct)
    # VU0 special2: 11-bit opcode across bits 10..6 and 1..0.
    for idx in range(128):
        hi, lo = idx >> 2, idx & 3
        words.append((18 << 26) | (1 << 25) | (rnd.getrandbits(14) << 11) | (hi << 6) | 0x3C | lo)
    return words


ALIASES = {"neg": "sub", "negu": "subu", "dneg": "dsub", "dnegu": "dsubu", "trunc.w.s": "cvt.w.s",
           "mfps": "mfc0", "mfpc": "mfc0", "mtps": "mtc0", "mtpc": "mtc0", "mfbpc": "mfc0", "mtbpc": "mtc0",
           "mfiab": "mfc0", "mtiab": "mtc0", "mfiabm": "mfc0", "mtiabm": "mtc0", "mfdab": "mfc0", "mtdab": "mtc0",
           "mfdabm": "mfc0", "mtdabm": "mtc0", "mfdvb": "mfc0", "mtdvb": "mtc0", "mfdvbm": "mfc0", "mtdvbm": "mtc0"}
# Where GNU's table is wrong for the R5900 (verified against the R5900 manual and real PS2 code):
# sqrt.s uses "fd, ft" on the R5900; GNU rejects valid vsqrt with ftf/fsf = 0.
GNU_FALSE_INVALID = {"sqrt.s", "vsqrt"}
# GNU decodes these although the R5900 lacks them.
GNU_FALSE_VALID = ("jalx", "c0", "c1", "c2", "ceil.l.s", "floor.l.s", "cvt.l.s", "cvt.d.l", "cfc0", "ctc0")


def norm(m):
    m = ALIASES.get(m.strip(), m.strip())
    if m in ("sync.l", "sync"):
        return "sync"
    m = VU_SUFFIX.sub(r"\1", m)
    for p in ("qmfc2", "qmtc2", "cfc2", "ctc2"):
        if m.startswith(p + "."):
            return p
    return m


def parse(text):
    out = {}
    for line in text.splitlines():
        parts = line.split("\t")
        if len(parts) < 3 or not parts[0].strip().endswith(":"):
            continue
        try:
            addr = int(parts[0].strip()[:-1], 16)
        except ValueError:
            continue
        mn = parts[2].strip().split(" ")[0] if len(parts) > 2 else ""
        out[addr] = mn
    return out


def main():
    lantern = sys.argv[1]
    show = int(sys.argv[sys.argv.index("--show") + 1]) if "--show" in sys.argv else 40
    words = gen_words()
    if "--random" in sys.argv:
        n = int(sys.argv[sys.argv.index("--random") + 1])
        rnd = random.Random(1999)
        words = [rnd.getrandbits(32) for _ in range(n)]
    if "--file" in sys.argv:  # raw little-endian words, e.g. a code segment dumped locally
        data = open(sys.argv[sys.argv.index("--file") + 1], "rb").read()
        words = [int.from_bytes(data[i:i + 4], "little") for i in range(0, len(data) - 3, 4)]
    with tempfile.NamedTemporaryFile(suffix=".bin") as f:
        f.write(b"".join(w.to_bytes(4, "little") for w in words))
        f.flush()
        gnu = subprocess.run([OBJDUMP, "-D", "-b", "binary", "-m", "mips:5900", "-EL", "-z", "-M",
                              "no-aliases,gpr-names=32", f.name], capture_output=True, text=True, check=True).stdout
        ours = subprocess.run([lantern, "raw", f.name], capture_output=True, text=True, check=True).stdout
    g, o = parse(gnu), parse(ours)
    mismatches = {}
    for i, w in enumerate(words):
        a = i * 4
        gm = g.get(a, "")
        om = o.get(a, "")
        # GNU quirks: jalx (MIPS16/microMIPS, absent on R5900) and c0/c1/c2 (generic
        # "unknown coprocessor op" fallbacks) are treated as invalid.
        # The R5900 FPU is single-precision only: no .d / .l formats.
        fpu64 = bool(re.search(r"\.(d|l)(\.|$)", gm))
        g_invalid = gm in ("", ".word") or gm in GNU_FALSE_VALID or fpu64 or gm.startswith("0x")
        if gm == "dli":  # alias objdump prints even with no-aliases for ori rt,zero,imm
            gm = "ori"
        o_invalid = om == "(invalid)"
        if g_invalid and o_invalid:
            continue
        if g_invalid and om in GNU_FALSE_INVALID:
            continue
        if o_invalid and gm in GNU_FALSE_INVALID:  # same layout difference, other direction
            continue
        if norm(gm) != norm(om):
            key = (gm if not g_invalid else "<invalid>", om if not o_invalid else "<invalid>")
            mismatches.setdefault(key, []).append(w)
    total = sum(len(v) for v in mismatches.values())
    print(f"words: {len(words)}  mismatching words: {total}  distinct pairs: {len(mismatches)}")
    for (gm, om), ws in sorted(mismatches.items(), key=lambda kv: -len(kv[1]))[:show]:
        print(f"  gnu={gm:<14} lantern={om:<14} x{len(ws):<6} e.g. {ws[0]:08x}")
    return 0 if total == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
