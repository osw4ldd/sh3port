"""Minimal ELF32 little-endian loader for PS2 EE executables."""

import struct
from dataclasses import dataclass, field

PT_LOAD = 1
PF_X = 1
SHT_SYMTAB = 2
STT_FUNC = 2
STT_OBJECT = 1


class ElfError(Exception):
    pass


@dataclass
class Segment:
    vaddr: int
    memsz: int
    filesz: int
    offset: int
    flags: int
    data: bytes

    @property
    def executable(self):
        return bool(self.flags & PF_X)

    def contains(self, addr):
        return self.vaddr <= addr < self.vaddr + self.memsz


@dataclass
class Section:
    name: str
    addr: int
    size: int
    flags: int
    type: int

    @property
    def executable(self):
        return bool(self.flags & 4) and self.addr != 0


@dataclass
class Symbol:
    name: str
    value: int
    size: int
    type: int


@dataclass
class Elf:
    path: str
    entry: int
    segments: list
    sections: list = field(default_factory=list)
    symbols: list = field(default_factory=list)
    raw: bytes = b""

    def segment_for(self, addr):
        for s in self.segments:
            if s.contains(addr):
                return s
        return None

    def read32(self, addr):
        s = self.segment_for(addr)
        if s is None:
            return None
        off = addr - s.vaddr
        if off + 4 > s.filesz:
            return 0  # bss
        return struct.unpack_from("<I", s.data, off)[0]

    def code_ranges(self):
        """Address ranges that contain instructions.

        Prefer executable sections (precise) and fall back to executable
        segments when the section table is missing or stripped."""
        ranges = [(s.addr, s.addr + s.size) for s in self.sections
                  if s.executable and s.size and s.type != 8]  # skip NOBITS
        if not ranges:
            ranges = [(s.vaddr, s.vaddr + s.filesz) for s in self.segments if s.executable]
        return sorted(ranges)

    def function_symbols(self):
        return [s for s in self.symbols if s.type == STT_FUNC and s.value]


def load_elf(path) -> Elf:
    with open(path, "rb") as f:
        raw = f.read()
    if raw[:4] != b"\x7fELF":
        raise ElfError(f"{path}: not an ELF file")
    if raw[4] != 1 or raw[5] != 1:
        raise ElfError(f"{path}: expected ELF32 little-endian (PS2 EE)")
    (e_type, e_machine, _ver, e_entry, e_phoff, e_shoff, _flags, _ehsize,
     e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx) = struct.unpack_from(
        "<HHIIIIIHHHHHH", raw, 16)
    if e_machine != 8:
        raise ElfError(f"{path}: e_machine={e_machine}, expected MIPS (8)")

    segments = []
    for i in range(e_phnum):
        (p_type, p_offset, p_vaddr, _paddr, p_filesz, p_memsz, p_flags,
         _align) = struct.unpack_from("<IIIIIIII", raw, e_phoff + i * e_phentsize)
        if p_type != PT_LOAD or p_memsz == 0:
            continue
        segments.append(Segment(p_vaddr, p_memsz, p_filesz, p_offset, p_flags,
                                raw[p_offset:p_offset + p_filesz]))

    elf = Elf(path, e_entry, segments, raw=raw)

    # Section headers are optional (retail games often strip them).
    if e_shoff and e_shnum and e_shoff + e_shnum * e_shentsize <= len(raw):
        shdrs = []
        for i in range(e_shnum):
            shdrs.append(struct.unpack_from("<IIIIIIIIII", raw, e_shoff + i * e_shentsize))
        strtab = b""
        if e_shstrndx < len(shdrs):
            sh = shdrs[e_shstrndx]
            strtab = raw[sh[4]:sh[4] + sh[5]]

        def cstr(tab, off):
            end = tab.find(b"\0", off)
            return tab[off:end if end >= 0 else len(tab)].decode("latin-1")

        for sh in shdrs:
            name_off, sh_type, sh_flags, sh_addr, sh_offset, sh_size = sh[:6]
            elf.sections.append(Section(cstr(strtab, name_off), sh_addr, sh_size, sh_flags, sh_type))
        for sh in shdrs:
            if sh[1] != SHT_SYMTAB:
                continue
            link = sh[6]
            if link >= len(shdrs):
                continue
            st = shdrs[link]
            symstr = raw[st[4]:st[4] + st[5]]
            entsize = sh[9] or 16
            for off in range(sh[4], sh[4] + sh[5], entsize):
                st_name, st_value, st_size, st_info, _other, _shndx = struct.unpack_from(
                    "<IIIBBH", raw, off)
                if not st_name:
                    continue
                elf.symbols.append(Symbol(cstr(symstr, st_name), st_value, st_size, st_info & 15))
    return elf
