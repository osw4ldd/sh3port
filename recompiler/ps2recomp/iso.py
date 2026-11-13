"""Minimal ISO9660 reader for PS2 discs (read-only).

Finds files by path, reads SYSTEM.CNF and extracts the boot executable.
"""

import re
import struct

SECTOR = 2048


class IsoError(Exception):
    pass


class Iso:
    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        pvd = self._read(16, 1)
        if pvd[1:6] != b"CD001":
            raise IsoError(f"{path}: not an ISO9660 image (2048-byte sectors)")
        self.volume_id = pvd[40:72].decode("ascii", "replace").strip()
        root = pvd[156:156 + 34]
        self.root = self._parse_record(root)

    def close(self):
        self.f.close()

    def _read(self, lba, count):
        self.f.seek(lba * SECTOR)
        return self.f.read(count * SECTOR)

    @staticmethod
    def _parse_record(rec):
        length = rec[0]
        lba = struct.unpack_from("<I", rec, 2)[0]
        size = struct.unpack_from("<I", rec, 10)[0]
        flags = rec[25]
        name_len = rec[32]
        name = rec[33:33 + name_len].decode("ascii", "replace")
        return {"len": length, "lba": lba, "size": size, "dir": bool(flags & 2), "name": name}

    def listdir(self, rec):
        data = self._read(rec["lba"], (rec["size"] + SECTOR - 1) // SECTOR)
        out, off = [], 0
        while off < len(data):
            n = data[off]
            if n == 0:
                off = (off // SECTOR + 1) * SECTOR  # records never cross sectors
                continue
            r = self._parse_record(data[off:off + n])
            if r["name"] not in ("\x00", "\x01"):
                out.append(r)
            off += n
        return out

    def find(self, path):
        """Case-insensitive lookup; accepts cdrom0:\\DIR\\FILE.EXT;1 style paths."""
        p = re.sub(r"^[a-z0-9]+:", "", path, flags=re.I).replace("\\", "/").strip("/")
        p = p.split(";")[0]
        rec = self.root
        for part in [x for x in p.split("/") if x]:
            for r in self.listdir(rec):
                if r["name"].split(";")[0].upper() == part.upper():
                    rec = r
                    break
            else:
                raise IsoError(f"{path}: not found on disc")
        return rec

    def read_file(self, path):
        rec = self.find(path)
        return self._read(rec["lba"], (rec["size"] + SECTOR - 1) // SECTOR)[:rec["size"]]

    def system_cnf(self):
        text = self.read_file("SYSTEM.CNF").decode("ascii", "replace")
        cnf = {}
        for line in text.splitlines():
            if "=" in line:
                k, v = line.split("=", 1)
                cnf[k.strip().upper()] = v.strip()
        return cnf

    def boot_path(self):
        cnf = self.system_cnf()
        boot = cnf.get("BOOT2")
        if not boot:
            raise IsoError("SYSTEM.CNF has no BOOT2 entry (PS1 disc?)")
        return boot
