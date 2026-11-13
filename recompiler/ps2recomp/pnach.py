"""PCSX2 .pnach patches for natively recompiled programs.

Writes that hit code are applied to the executable *before* recompiling
(the recompiled code cannot be patched at run time).  Everything else
(data writes, conditional codes) becomes a runtime table applied every
vblank, like PCSX2's continuous patches.
"""

import struct
from dataclasses import dataclass


@dataclass
class Patch:
    addr: int
    value: int
    size: int                 # 1, 2 or 4 bytes
    cond_addr: int = 0        # 16-bit equality condition (pnach "D" code)
    cond_value: int = 0
    has_cond: bool = False
    option: int = -1          # >= 0: only while runtime option N is on


@dataclass
class PnachSection:
    name: str
    patches: list
    settings: dict


def parse_pnach(path):
    """Returns {section name: PnachSection}.  Lines before the first [section]
    go to the "" section."""
    sections = {}
    cur = PnachSection("", [], {})
    sections[""] = cur
    pending_cond = None
    with open(path, encoding="utf-8", errors="replace") as f:
        for raw in f:
            line = raw.split("//")[0].strip()
            if not line:
                continue
            if line.startswith("[") and line.endswith("]"):
                cur = PnachSection(line[1:-1].strip(), [], {})
                sections[cur.name] = cur
                pending_cond = None
                continue
            if "=" not in line:
                continue
            key, val = (x.strip() for x in line.split("=", 1))
            if key != "patch":
                cur.settings[key] = val
                continue
            parts = [p.strip() for p in val.split(",")]
            if len(parts) < 5 or parts[1].upper() != "EE":
                continue
            addr, kind, value = int(parts[2], 16), parts[3].lower(), int(parts[4], 16)
            if kind == "extended":
                cmd = addr >> 28
                a = addr & 0x0FFFFFFF
                if cmd == 0xD:                       # if (u16 at a == value) next line
                    pending_cond = (a, value & 0xFFFF)
                    continue
                size = {0: 1, 1: 2, 2: 4}.get(cmd)
                if size is None:
                    print(f"pnach: unsupported extended code {addr:08X} in [{cur.name}]")
                    pending_cond = None
                    continue
                p = Patch(a, value, size)
            else:
                size = {"byte": 1, "short": 2, "word": 4}.get(kind)
                if size is None:
                    print(f"pnach: unsupported patch type {kind} in [{cur.name}]")
                    continue
                p = Patch(addr, value, size)
            if pending_cond:
                p.cond_addr, p.cond_value = pending_cond
                p.has_cond = True
                pending_cond = None
            cur.patches.append(p)
    return sections


def select(sections, names):
    """Patches and settings of the requested sections (case-insensitive)."""
    wanted = {n.strip().lower() for n in names if n.strip()}
    out_p, out_s, used = [], {}, []
    for name, sec in sections.items():
        if name == "" or name.lower() in wanted:
            out_p += sec.patches
            out_s.update(sec.settings)
            if name:
                used.append(name)
    missing = wanted - {u.lower() for u in used}
    return out_p, out_s, used, sorted(missing)


def make_optional(elf, patches, option, code_ranges):
    """Turn a pnach section into a run-time option.  Data patches are tagged
    with the option; code patches are only supported in the "conditional
    branch -> nop" form (the branch is compiled with an extra condition).
    Returns (runtime patches, {branch addr: option}) or raises ValueError."""
    from .r5900 import decode, K_BRANCH
    runtime, toggles = [], {}
    for p in patches:
        in_code = any(lo <= p.addr < hi for lo, hi in code_ranges)
        if not in_code:
            runtime.append(Patch(p.addr, p.value, p.size, p.cond_addr, p.cond_value, p.has_cond, option))
            continue
        word = elf.read32(p.addr & ~3)
        ins = decode(p.addr, word) if word is not None else None
        if (p.size != 4 or p.value != 0 or p.has_cond or ins is None or ins.kind != K_BRANCH
                or ins.likely or ins.uncond or ins.fields.get("link")):
            raise ValueError(f"code patch at {p.addr:08X} cannot be switched at run time "
                             f"(only 'conditional branch -> nop' patches can)")
        toggles[p.addr] = option
    return runtime, toggles


def apply_to_elf(raw, elf, patches, code_ranges):
    """Apply unconditional patches inside code to the ELF bytes.  Returns
    (patched bytes, remaining runtime patches, number applied)."""
    data = bytearray(raw)
    runtime, applied = [], 0
    for p in patches:
        in_code = any(lo <= p.addr < hi for lo, hi in code_ranges)
        if p.has_cond or not in_code:
            runtime.append(p)
            continue
        seg = elf.segment_for(p.addr)
        off = p.addr - seg.vaddr if seg else None
        if seg is None or off + p.size > seg.filesz:
            runtime.append(p)
            continue
        fmt = {1: "<B", 2: "<H", 4: "<I"}[p.size]
        struct.pack_into(fmt, data, seg.offset + off, p.value & ((1 << (8 * p.size)) - 1))
        applied += 1
    return bytes(data), runtime, applied
