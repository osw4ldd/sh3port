"""ps2native - graphical front end.

Select a PS2 executable (e.g. SLUS_123.45 or a homebrew .elf), recompile it
to a native Windows program and run it.
"""

import contextlib
import io
import os
import queue
import subprocess
import sys
import threading
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "recompiler"))

from ps2recomp.__main__ import main as recomp_main  # noqa: E402
from ps2recomp.paths import project_root  # noqa: E402

ROOT = project_root()
RENDERERS = ["auto", "vulkan", "dx12", "dx11", "opengl", "software"]


class QueueWriter(io.TextIOBase):
    def __init__(self, q):
        self.q = q

    def write(self, s):
        if s:
            self.q.put(s)
        return len(s)


class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("ps2native - PS2 → native x86-64")
        self.geometry("820x560")
        self.minsize(700, 460)
        self.log_q = queue.Queue()
        self.worker = None
        self.last_exe = None

        self.elf = tk.StringVar()
        self.out = tk.StringVar()
        self.name = tk.StringVar()
        self.backend = tk.StringVar(value="pcsx2")
        self.renderer = tk.StringVar(value="auto")
        self.cdrom = tk.StringVar()
        self.entries = tk.StringVar()

        pad = {"padx": 6, "pady": 3}
        frm = ttk.Frame(self)
        frm.pack(fill="x", padx=8, pady=8)
        frm.columnconfigure(1, weight=1)

        def row(r, label, var, browse=None):
            ttk.Label(frm, text=label).grid(row=r, column=0, sticky="w", **pad)
            ttk.Entry(frm, textvariable=var).grid(row=r, column=1, sticky="ew", **pad)
            if browse:
                ttk.Button(frm, text="Seç…", command=browse).grid(row=r, column=2, **pad)

        row(0, "PS2 programı (ELF):", self.elf, self.pick_elf)
        row(1, "Çıktı klasörü:", self.out, self.pick_out)
        row(2, "Program adı:", self.name)
        row(3, "Disk klasörü (cdrom0:):", self.cdrom, self.pick_cdrom)
        row(4, "Ek giriş adresleri:", self.entries)
        ttk.Label(frm, text="(boşlukla ayrılmış hex, örn. 0x1A2B30)", foreground="gray").grid(
            row=5, column=1, sticky="w", padx=6)

        opts = ttk.Frame(frm)
        opts.grid(row=6, column=0, columnspan=3, sticky="w", **pad)
        ttk.Label(opts, text="Arka uç:").pack(side="left")
        ttk.Radiobutton(opts, text="PCSX2 (VU1 + Vulkan/DirectX)", variable=self.backend,
                        value="pcsx2").pack(side="left", padx=4)
        ttk.Radiobutton(opts, text="Yazılımsal (VU1 yok)", variable=self.backend,
                        value="soft").pack(side="left", padx=4)
        ttk.Label(opts, text="   Çizici:").pack(side="left")
        ttk.Combobox(opts, textvariable=self.renderer, values=RENDERERS, width=10,
                     state="readonly").pack(side="left", padx=4)

        btns = ttk.Frame(self)
        btns.pack(fill="x", padx=8)
        self.btn_build = ttk.Button(btns, text="Dönüştür ve derle", command=self.start_build)
        self.btn_build.pack(side="left", padx=4)
        self.btn_run = ttk.Button(btns, text="Çalıştır", command=self.run_exe, state="disabled")
        self.btn_run.pack(side="left", padx=4)
        ttk.Button(btns, text="Çıktı klasörünü aç", command=self.open_out).pack(side="left", padx=4)
        self.progress = ttk.Progressbar(btns, mode="indeterminate", length=160)
        self.progress.pack(side="right", padx=4)

        logf = ttk.Frame(self)
        logf.pack(fill="both", expand=True, padx=8, pady=8)
        self.log = tk.Text(logf, wrap="word", height=18, font=("Consolas", 9))
        sb = ttk.Scrollbar(logf, command=self.log.yview)
        self.log.configure(yscrollcommand=sb.set)
        self.log.pack(side="left", fill="both", expand=True)
        sb.pack(side="right", fill="y")
        self.say("ps2native hazır. Bir PS2 ELF dosyası seçip \"Dönüştür ve derle\"ye basın.\n"
                 "İlk PCSX2 derlemesi uzun sürebilir; sonrakiler yalnızca oyun kodunu derler.\n")
        self.after(100, self.drain_log)

    # ------------------------------------------------------------ helpers
    def say(self, text):
        self.log.insert("end", text)
        self.log.see("end")

    def drain_log(self):
        try:
            while True:
                self.say(self.log_q.get_nowait())
        except queue.Empty:
            pass
        self.after(100, self.drain_log)

    def pick_elf(self):
        p = filedialog.askopenfilename(title="PS2 programı seç",
                                       filetypes=[("PS2 ELF", "*.elf *.ELF SLUS_* SLES_* SCUS_* SCES_* SLPS_* SLPM_*"),
                                                  ("Tüm dosyalar", "*.*")])
        if p:
            self.elf.set(p)
            stem = Path(p).name.replace(".", "_")
            if not self.name.get():
                self.name.set(stem)
            if not self.out.get():
                self.out.set(str(ROOT / "build" / "games" / stem))

    def pick_out(self):
        p = filedialog.askdirectory(title="Çıktı klasörü")
        if p:
            self.out.set(p)

    def pick_cdrom(self):
        p = filedialog.askdirectory(title="Diskin içeriği (açılmış ISO) klasörü")
        if p:
            self.cdrom.set(p)

    def open_out(self):
        out = self.out.get()
        if out and Path(out).exists():
            os.startfile(out)

    # ------------------------------------------------------------ actions
    def start_build(self):
        if self.worker and self.worker.is_alive():
            return
        elf, out = self.elf.get().strip(), self.out.get().strip()
        if not elf or not Path(elf).is_file():
            messagebox.showerror("ps2native", "Geçerli bir PS2 ELF dosyası seçin.")
            return
        if not out:
            messagebox.showerror("ps2native", "Bir çıktı klasörü seçin.")
            return
        args = [elf, "-o", out, "--backend", self.backend.get()]
        if self.name.get().strip():
            args += ["--name", self.name.get().strip()]
        for e in self.entries.get().split():
            args += ["--entry", e]
        self.btn_build.configure(state="disabled")
        self.btn_run.configure(state="disabled")
        self.progress.start(12)
        self.say(f"\n> ps2recomp {' '.join(args)}\n")
        self.worker = threading.Thread(target=self.build_thread, args=(args,), daemon=True)
        self.worker.start()

    def build_thread(self, args):
        w = QueueWriter(self.log_q)
        try:
            with contextlib.redirect_stdout(w), contextlib.redirect_stderr(w):
                rc = recomp_main(args)
        except Exception as e:  # report, never crash the UI
            self.log_q.put(f"hata: {e}\n")
            rc = 1
        self.after(0, self.build_done, rc)

    def find_exe(self):
        out = Path(self.out.get())
        cands = list((out / "bin").glob("*.exe")) + list(out.glob("*.exe"))
        return max(cands, key=lambda p: p.stat().st_mtime) if cands else None

    def build_done(self, rc):
        self.progress.stop()
        self.btn_build.configure(state="normal")
        if rc == 0:
            self.last_exe = self.find_exe()
            self.say(f"\nTamam: {self.last_exe}\n")
            self.btn_run.configure(state="normal")
        else:
            self.say("\nDerleme başarısız (ayrıntılar yukarıda).\n")

    def run_exe(self):
        exe = self.last_exe or self.find_exe()
        if not exe:
            return
        cmd = [str(exe), "--renderer", self.renderer.get()]
        if self.cdrom.get().strip():
            cmd += ["--cdrom", self.cdrom.get().strip()]
        self.say(f"\n> {' '.join(cmd)}\n")
        subprocess.Popen(cmd, cwd=str(exe.parent))


def main():
    App().mainloop()


if __name__ == "__main__":
    main()
