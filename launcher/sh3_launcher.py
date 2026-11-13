"""Silent Hill 3 (native PC port) launcher.

Picks the user's own game files (disc image + PS2 BIOS), edits the port's
settings (bin/ps2native.ini, read by the game at start) and starts the game.

Layout of a release:
    Silent Hill 3.exe        this launcher
    bin/SLUS_206_22.exe      the natively recompiled game + PCSX2 core DLLs
    bin/ps2native.ini        settings (written here)
    bin/memcards/            memory card (saves)
    bin/logs/                log of the last run
"""

import datetime
import json
import os
import shutil
import subprocess
import sys
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

import sh3_lang
from sh3_lang import T

GAME_TITLE = "Silent Hill 3"
GAME_SERIAL = "SLUS-20622"
BOOT_FILE = "SLUS_206.22"
BOOT_PATH = r"cdrom0:\SLUS_206.22;1"
GAME_EXE = "SLUS_206_22.exe"
VERSION = "1.0"


# ------------------------------------------------------------------ paths
def launcher_dir():
    if getattr(sys, "frozen", False):
        return Path(sys.executable).resolve().parent
    return Path(__file__).resolve().parent


def assets_dir():
    base = Path(getattr(sys, "_MEIPASS", Path(__file__).resolve().parent))
    return base / "assets"


def find_bin_dir():
    """bin/ next to the launcher (release), else the development build."""
    here = launcher_dir()
    for cand in (here / "bin", here.parent / "build" / "games" / "sh3" / "bin"):
        if (cand / GAME_EXE).exists():
            return cand
    return here / "bin"


BIN = find_bin_dir()
INI = BIN / "ps2native.ini"
MEMCARDS = BIN / "memcards"
LOGS = BIN / "logs"
APPDATA = Path(os.environ.get("APPDATA", str(Path.home()))) / "sh3native-launcher"
LAUNCHER_SETTINGS = APPDATA / "settings.json"

NO_WINDOW = 0x08000000 if os.name == "nt" else 0


# ------------------------------------------------------------------ ini
class Ini:
    """key=value file shared with the game; unknown keys and order are kept."""

    def __init__(self, path):
        self.path = Path(path)
        self.lines = []
        self.values = {}
        if self.path.exists():
            for line in self.path.read_text(encoding="utf-8", errors="replace").splitlines():
                self.lines.append(line)
                if "=" in line and not line.lstrip().startswith(("#", ";")):
                    k, v = line.split("=", 1)
                    self.values[k.strip()] = v.strip()

    def get(self, key, default=""):
        return self.values.get(key, default)

    def set(self, key, value):
        self.values[key] = str(value)

    def save(self):
        out, seen = [], set()
        for line in self.lines:
            if "=" in line and not line.lstrip().startswith(("#", ";")):
                k = line.split("=", 1)[0].strip()
                if k in self.values:
                    out.append(f"{k}={self.values[k]}")
                    seen.add(k)
                    continue
            out.append(line)
        for k, v in self.values.items():
            if k not in seen:
                out.append(f"{k}={v}")
        self.path.parent.mkdir(parents=True, exist_ok=True)
        tmp = self.path.with_suffix(".tmp")
        tmp.write_text("\n".join(out) + "\n", encoding="utf-8")
        os.replace(tmp, self.path)
        self.lines = out


def load_launcher_settings():
    try:
        return json.loads(LAUNCHER_SETTINGS.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def save_launcher_settings(data):
    try:
        APPDATA.mkdir(parents=True, exist_ok=True)
        LAUNCHER_SETTINGS.write_text(json.dumps(data, indent=2), encoding="utf-8")
    except OSError:
        pass


# ------------------------------------------------------------------ file checks
def check_disc(path):
    """(ok, message).  ISO images are opened and their boot file checked."""
    p = Path(path)
    if not p.is_file():
        return False, T("file_not_found")
    ext = p.suffix.lower()
    if ext in (".chd", ".cso", ".zso", ".gz"):
        return True, T("compressed", ext=ext)
    try:
        with open(p, "rb") as f:
            f.seek(16 * 2048 + 1)
            if f.read(5) != b"CD001":
                return False, T("not_ps2")
    except OSError as e:
        return False, str(e)
    try:
        sys.path.insert(0, str(launcher_dir().parent / "recompiler"))
        from ps2recomp.iso import Iso
        iso = Iso(str(p))
        boot = iso.boot_path()
        iso.close()
    except Exception as e:  # noqa: BLE001 - any parse error means "not this game"
        return False, T("cnf_fail", e=e)
    if BOOT_FILE.upper() not in boot.upper():
        return False, T("wrong_game", title=GAME_TITLE, serial=GAME_SERIAL, boot=boot)
    size_gb = p.stat().st_size / (1 << 30)
    return True, T("disc_found", title=GAME_TITLE, serial=GAME_SERIAL, size=f"{size_gb:.2f}")


def check_bios(path):
    p = Path(path)
    if p.is_dir():
        for e in sorted(p.iterdir()):
            if e.is_file() and 512 * 1024 <= e.stat().st_size <= 8 * 1024 * 1024:
                ok, msg = check_bios(e)
                if ok:
                    return ok, f"{e.name}: {msg}"
        return False, T("no_bios_in_dir")
    if not p.is_file():
        return False, T("file_not_found")
    size = p.stat().st_size
    if not 512 * 1024 <= size <= 8 * 1024 * 1024:
        return False, T("bios_size")
    with open(p, "rb") as f:
        data = f.read(64 * 1024)
    pos = data.find(b"RESET\0")
    if pos < 0:
        return False, T("bios_sig")
    version = ""
    # ROMDIR: 16-byte entries {name[10], ext[2], size[4]}; ROMVER holds e.g. 0200AC20040614.
    off, entry = 0, pos
    while entry + 16 <= len(data) and data[entry] != 0:
        name = data[entry:entry + 10].rstrip(b"\0")
        esize = int.from_bytes(data[entry + 12:entry + 16], "little")
        if name == b"ROMVER":
            with open(p, "rb") as f:
                f.seek(off)
                version = f.read(14).decode("ascii", "replace")
            break
        off += (esize + 15) & ~15
        entry += 16
    if len(version) >= 14:
        ver = f"v{int(version[0:2])}.{version[2:4]}"
        region = T(f"reg_{version[4]}") if version[4] in "AJECH" else version[4]
        date = f"{version[12:14]}.{version[10:12]}.{version[6:10]}"
        return True, f"PS2 BIOS {ver} ({region}, {date})"
    return True, "PS2 BIOS"


# ------------------------------------------------------------------ keyboard names
# Tk keysym -> key name understood by the game (host/_stubs.inc).
_KEYSYMS = {
    "Up": "Up", "Down": "Down", "Left": "Left", "Right": "Right", "Return": "Return",
    "Escape": "Escape", "BackSpace": "Backspace", "Tab": "Tab", "space": "Space",
    "Shift_L": "Shift", "Shift_R": "Shift", "Control_L": "Control", "Control_R": "Control",
    "Alt_L": "Alt", "Alt_R": "Alt", "Insert": "Insert", "Delete": "Delete", "Home": "Home",
    "End": "End", "Prior": "PageUp", "Next": "PageDown", "plus": "Plus", "equal": "Plus",
    "minus": "Minus", "comma": "Comma", "period": "Period", "semicolon": "Semicolon",
    "slash": "Slash", "backslash": "Backslash", "apostrophe": "Quote",
    "bracketleft": "BracketLeft", "bracketright": "BracketRight", "Caps_Lock": "CapsLock",
    "KP_Add": "NumpadPlus", "KP_Subtract": "NumpadMinus",
}
for _i in range(10):
    _KEYSYMS[f"KP_{_i}"] = f"Numpad{_i}"
for _i in range(1, 13):
    _KEYSYMS[f"F{_i}"] = f"F{_i}"
_KP_NAV = {"KP_Insert": 0, "KP_End": 1, "KP_Down": 2, "KP_Next": 3, "KP_Left": 4,
           "KP_Begin": 5, "KP_Right": 6, "KP_Home": 7, "KP_Up": 8, "KP_Prior": 9}


def key_name(event):
    ks = event.keysym
    if ks in _KEYSYMS:
        return _KEYSYMS[ks]
    if ks in _KP_NAV:
        return f"Numpad{_KP_NAV[ks]}"
    if len(ks) == 1 and ks.isalnum():
        return ks.upper()
    return None


# (button id, label, default key); labels in the current language.
_PAD = [
    ("Cross", "btn_cross", "S"), ("Circle", "btn_circle", "D"),
    ("Square", "btn_square", "A"), ("Triangle", "btn_triangle", "W"),
    ("Up", "dpad:dir_up", "Up"), ("Down", "dpad:dir_down", "Down"),
    ("Left", "dpad:dir_left", "Left"), ("Right", "dpad:dir_right", "Right"),
    ("LUp", "lstick:dir_up", "I"), ("LDown", "lstick:dir_down", "K"),
    ("LLeft", "lstick:dir_left", "J"), ("LRight", "lstick:dir_right", "L"),
    ("RUp", "rstick:dir_up", "Numpad8"), ("RDown", "rstick:dir_down", "Numpad2"),
    ("RLeft", "rstick:dir_left", "Numpad4"), ("RRight", "rstick:dir_right", "Numpad6"),
    ("L1", "=L1", "1"), ("R1", "=R1", "3"), ("L2", "=L2", "Q"), ("R2", "=R2", "E"),
    ("L3", "=L3", "Z"), ("R3", "=R3", "C"),
    ("Start", "=Start", "Return"), ("Select", "=Select", "Backspace"),
]


def pad_buttons():
    out = []
    for btn, label, default in _PAD:
        if label.startswith("="):
            text = label[1:]
        elif ":" in label:
            group, direction = label.split(":")
            text = f"{T(group)} {T(direction)}"
        else:
            text = T(label)
        out.append((btn, text, default))
    return out


def renderers():
    return [("auto", T("auto")), ("dx12", "DirectX 12"), ("dx11", "DirectX 11"),
            ("vulkan", "Vulkan"), ("opengl", "OpenGL")]


def resolutions():
    return [("1", T("res_native")), ("2", "720p (2×)"), ("3", "1080p (3×)"),
            ("4", "1440p (4×)"), ("5", "1800p (5×)"), ("6", "4K (6×)"), ("8", "5K+ (8×)")]


def aniso():
    return [("0", T("off_c")), ("2", "2×"), ("4", "4×"), ("8", "8×"), ("16", "16×")]

DEFAULTS = {
    "renderer": "auto", "upscale": "3", "widescreen": "1", "fullscreen": "0",
    "anisotropy": "16", "fxaa": "0", "sharpen": "0", "vsync": "0", "gpu": "high",
    "fast_cdvd": "1", "fps60": "0", "camera_behind": "0", "camera_l3": "0",
    "controller": "1", "vibration": "1",
}

# ------------------------------------------------------------------ theme
BG = "#121212"
PANEL = "#1b1b1b"
SIDEBAR = "#0c0c0c"
FG = "#e6e2dc"
MUTED = "#8f8a84"
ACCENT = "#a3261d"
ACCENT_HI = "#c4362b"
OK = "#5fae5f"
BAD = "#d0533f"
_FAMILY = ["Segoe UI"]


def font(size=10, bold=False):
    """UI font for the current language (CJK languages need their own fonts)."""
    if bold and _FAMILY[0] == "Segoe UI":
        return ("Segoe UI Semibold", size)
    return (_FAMILY[0], size, "bold") if bold else (_FAMILY[0], size)


def set_language(code):
    sh3_lang.set_language(code)
    _FAMILY[0] = sh3_lang.FONTS.get(sh3_lang.language(), "Segoe UI")


class Launcher(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title(f"{GAME_TITLE} - Native PC Port")
        dpi = max(1.0, float(self.tk.call("tk", "scaling")) / (96 / 72))
        self.geometry(f"{int(1000 * dpi)}x{int(680 * dpi)}")
        self.minsize(int(860 * dpi), int(560 * dpi))
        self.configure(bg=BG)
        try:
            self.iconbitmap(default=str(assets_dir() / "icon.ico"))
        except tk.TclError:
            pass
        self.ini = Ini(INI)
        for k, v in DEFAULTS.items():
            if k not in self.ini.values:
                self.ini.set(k, v)
        self.lset = load_launcher_settings()
        set_language(self.lset.get("language") or sh3_lang.system_language())
        self.game_proc = None
        self.vars = {}
        self._style()
        self._build()
        self.protocol("WM_DELETE_WINDOW", self.on_close)
        if not (self.disc_ok() and self.bios_ok()):
            self.show("files")
        else:
            self.show("play")

    def change_language(self, code):
        """Rebuild the whole UI in another language, staying on the same page."""
        if code == sh3_lang.language():
            return
        self._lset("language", code)
        set_language(code)
        page = getattr(self, "current_key", "play")
        for w in self.winfo_children():
            w.destroy()
        self.vars = {}
        self._style()
        self._build()
        self.show(page)

    # ---------------------------------------------------------------- style
    def _style(self):
        s = ttk.Style(self)
        s.theme_use("clam")
        s.configure(".", background=BG, foreground=FG, font=font(), fieldbackground=PANEL,
                    bordercolor="#2c2c2c", lightcolor=PANEL, darkcolor=PANEL)
        s.configure("TFrame", background=BG)
        s.configure("Panel.TFrame", background=PANEL)
        s.configure("TLabel", background=BG, foreground=FG)
        s.configure("Panel.TLabel", background=PANEL)
        s.configure("Muted.TLabel", foreground=MUTED)
        s.configure("PanelMuted.TLabel", background=PANEL, foreground=MUTED)
        s.configure("H1.TLabel", font=font(20, True))
        s.configure("H2.TLabel", font=font(12, True), background=PANEL)
        s.configure("TCheckbutton", background=PANEL, foreground=FG)
        s.map("TCheckbutton", background=[("active", PANEL)], indicatorcolor=[("selected", ACCENT)])
        s.configure("TRadiobutton", background=PANEL, foreground=FG)
        s.map("TRadiobutton", background=[("active", PANEL)], indicatorcolor=[("selected", ACCENT)])
        s.configure("TButton", background="#2a2a2a", foreground=FG, padding=(12, 6), borderwidth=0)
        s.map("TButton", background=[("active", "#383838")])
        s.configure("Accent.TButton", background=ACCENT, foreground="white",
                    font=font(14, True), padding=(40, 12))
        s.map("Accent.TButton", background=[("active", ACCENT_HI), ("disabled", "#4a2522")])
        s.configure("TCombobox", fieldbackground=PANEL, background="#2a2a2a", foreground=FG,
                    arrowcolor=FG, padding=4)
        s.map("TCombobox", fieldbackground=[("readonly", PANEL)], foreground=[("readonly", FG)])
        self.option_add("*TCombobox*Listbox.background", PANEL)
        self.option_add("*TCombobox*Listbox.foreground", FG)
        self.option_add("*TCombobox*Listbox.selectBackground", ACCENT)
        s.configure("Horizontal.TScale", background=PANEL, troughcolor="#2a2a2a")
        s.configure("Treeview", background=PANEL, fieldbackground=PANEL, foreground=FG,
                    rowheight=26, borderwidth=0)
        s.configure("Treeview.Heading", background="#222", foreground=MUTED)
        s.map("Treeview", background=[("selected", ACCENT)])

    # ---------------------------------------------------------------- layout
    def _build(self):
        side = tk.Frame(self, bg=SIDEBAR, width=210)
        side.pack(side="left", fill="y")
        side.pack_propagate(False)
        tk.Label(side, text="SILENT HILL 3", bg=SIDEBAR, fg=FG,
                 font=("Segoe UI Semibold", 16)).pack(anchor="w", padx=20, pady=(24, 0))
        tk.Label(side, text="Native PC Port", bg=SIDEBAR, fg=MUTED,
                 font=font(9)).pack(anchor="w", padx=20, pady=(0, 20))
        self.nav = {}
        pages = [(k, T("nav_" + k)) for k in
                 ("play", "files", "graphics", "game", "controls", "saves", "about")]
        for key, label in pages:
            b = tk.Label(side, text="   " + label, bg=SIDEBAR, fg=FG, anchor="w",
                         font=font(11), padx=8, pady=9, cursor="hand2")
            b.pack(fill="x")
            b.bind("<Button-1>", lambda _e, k=key: self.show(k))
            self.nav[key] = b
        tk.Label(side, text=f"v{VERSION}", bg=SIDEBAR, fg="#555").pack(side="bottom", anchor="w",
                                                                       padx=20, pady=(4, 12))
        names = [n for _c, n in sh3_lang.LANGUAGES]
        lang_var = tk.StringVar(value=dict(sh3_lang.LANGUAGES)[sh3_lang.language()])
        cb = ttk.Combobox(side, textvariable=lang_var, values=names, state="readonly", width=16)
        cb.pack(side="bottom", anchor="w", padx=20)
        cb.bind("<<ComboboxSelected>>", lambda _e: self.change_language(
            {n: c for c, n in sh3_lang.LANGUAGES}[lang_var.get()]))
        tk.Label(side, text="🌐  " + T("language"), bg=SIDEBAR, fg=MUTED,
                 font=font(9)).pack(side="bottom", anchor="w", padx=20, pady=(0, 2))
        self.body = ttk.Frame(self)
        self.body.pack(side="left", fill="both", expand=True)
        self.pages = {
            "play": self._page_play(), "files": self._page_files(),
            "graphics": self._page_graphics(), "game": self._page_game(),
            "controls": self._page_controls(), "saves": self._page_saves(),
            "about": self._page_about(),
        }

    def show(self, key):
        for k, page in self.pages.items():
            page.outer.pack_forget()
            self.nav[k].configure(bg=SIDEBAR, fg=FG)
        self.nav[key].configure(bg="#1f1f1f", fg="white")
        if key == "play":
            self.refresh_play()
        if key == "saves":
            self.refresh_saves()
        self.pages[key].outer.pack(fill="both", expand=True)
        self.current = self.pages[key]
        self.current_key = key
        self.current.canvas.yview_moveto(0)

    def page(self, title, subtitle="", hero=False):
        """A vertically scrollable page; returns the content frame."""
        outer = ttk.Frame(self.body)
        canvas = tk.Canvas(outer, bg=BG, highlightthickness=0, bd=0)
        sb = ttk.Scrollbar(outer, orient="vertical", command=canvas.yview)
        canvas.configure(yscrollcommand=sb.set)
        sb.pack(side="right", fill="y")
        canvas.pack(side="left", fill="both", expand=True)
        f = ttk.Frame(canvas, padding=(28, 24))
        win = canvas.create_window(0, 0, window=f, anchor="nw")
        f.bind("<Configure>", lambda _e: canvas.configure(scrollregion=canvas.bbox("all")))
        canvas.bind("<Configure>", lambda e: canvas.itemconfigure(win, width=e.width))
        f.outer, f.canvas = outer, canvas
        if not hasattr(self, "_wheel"):
            self._wheel = True
            self.bind_all("<MouseWheel>", lambda e: self.current.canvas.yview_scroll(
                int(-e.delta / 120), "units") if self.current.canvas.bbox("all")
                and self.current.canvas.bbox("all")[3] > self.current.canvas.winfo_height() else None)
        if hero:
            self.banner(f, "background.jpg", 340, title, subtitle, focus=(0.5, 0.35), dim=0.1,
                        title_size=36)
        else:
            self.banner(f, "header.jpg", 120, title, subtitle, focus=(0.5, 0.5), dim=0.45)
        return f

    def banner(self, parent, image, height, title, subtitle="", focus=(0.5, 0.5), dim=0.5,
               title_size=22):
        """Header with a cover-scaled picture fading into the background,
        title and subtitle drawn on top."""
        c = tk.Canvas(parent, height=height, bg=BG, highlightthickness=0, bd=0)
        c.pack(fill="x", pady=(0, 14))
        state = {"size": None}

        def render(_e=None):
            w, h = c.winfo_width(), c.winfo_height()
            if w < 10 or state["size"] == (w, h):
                return
            state["size"] = (w, h)
            c.delete("all")
            photo = self.cover_image(image, w, h, focus, dim)
            if photo is not None:
                c.create_image(0, 0, image=photo, anchor="nw")
                state["photo"] = photo
            # Subtitle first (it may wrap), then the title right above it.
            ty = h - 12
            if subtitle:
                sub = c.create_text(18, h - 10, text=subtitle, fill="#d8d2ca", font=font(), anchor="sw",
                                    width=w - 36)
                x0, y0, x1, y1 = c.bbox(sub)
                ty = y0 - 2
            tfont = font(title_size, True)
            c.create_text(19, ty + 1, text=title, fill="black", font=tfont, anchor="sw")
            c.create_text(18, ty, text=title, fill="white", font=tfont, anchor="sw")
        c.bind("<Configure>", render)
        return c

    def cover_image(self, name, w, h, focus=(0.5, 0.5), dim=0.5):
        """Picture scaled to cover w x h, darkened and faded to BG at the bottom."""
        try:
            from PIL import Image, ImageTk
        except ImportError:
            return None
        cache = self.__dict__.setdefault("_img_cache", {})
        if name not in cache:
            try:
                cache[name] = Image.open(assets_dir() / name).convert("RGB")
            except OSError:
                cache[name] = None
        src = cache[name]
        if src is None:
            return None
        sw, sh = src.size
        scale = max(w / sw, h / sh)
        rw, rh = int(sw * scale + 0.5), int(sh * scale + 0.5)
        img = src.resize((rw, rh), Image.LANCZOS)
        left = int((rw - w) * focus[0])
        top = int((rh - h) * focus[1])
        img = img.crop((left, top, left + w, top + h))
        # Darken, then fade the lower part into the page colour.
        bg = Image.new("RGB", (w, h), BG)
        img = Image.blend(bg, img, 1.0 - dim)
        mask = Image.linear_gradient("L").resize((w, h))   # 0 top -> 255 bottom
        mask = mask.point(lambda v: max(0, (v - 110)) * 255 // 145)
        img = Image.composite(bg, img, mask)
        return ImageTk.PhotoImage(img)

    def card(self, parent, title=None):
        c = ttk.Frame(parent, style="Panel.TFrame", padding=16)
        c.pack(fill="x", pady=(0, 12))
        if title:
            ttk.Label(c, text=title, style="H2.TLabel").pack(anchor="w", pady=(0, 8))
        return c

    def row(self, parent, label, hint=""):
        r = ttk.Frame(parent, style="Panel.TFrame")
        r.pack(fill="x", pady=4)
        left = ttk.Frame(r, style="Panel.TFrame")
        left.pack(side="left", fill="x", expand=True)
        ttk.Label(left, text=label, style="Panel.TLabel").pack(anchor="w")
        if hint:
            ttk.Label(left, text=hint, style="PanelMuted.TLabel", wraplength=440,
                      justify="left").pack(anchor="w")
        return r

    # ---- setting widgets bound to ini keys
    def combo(self, parent, key, label, choices, hint=""):
        r = self.row(parent, label, hint)
        names = [n for _v, n in choices]
        cur = self.ini.get(key, DEFAULTS.get(key, ""))
        var = tk.StringVar(value=dict(choices).get(cur, names[0]))
        cb = ttk.Combobox(r, textvariable=var, values=names, state="readonly", width=24)
        cb.pack(side="right")

        def changed(_e=None):
            value = {n: v for v, n in choices}[var.get()]
            self.ini.set(key, value)
            self.save_ini()
        cb.bind("<<ComboboxSelected>>", changed)
        self.vars[key] = var

    def check(self, parent, key, label, hint="", on="1", off="0"):
        r = self.row(parent, label, hint)
        var = tk.BooleanVar(value=self.ini.get(key, DEFAULTS.get(key, off)) == on)

        def changed():
            self.ini.set(key, on if var.get() else off)
            self.save_ini()
        ttk.Checkbutton(r, variable=var, command=changed).pack(side="right")
        self.vars[key] = var

    def slider(self, parent, key, label, lo, hi, hint=""):
        r = self.row(parent, label, hint)
        var = tk.IntVar(value=int(self.ini.get(key, DEFAULTS.get(key, "0")) or 0))
        val = ttk.Label(r, text=str(var.get()), style="Panel.TLabel", width=4)
        val.pack(side="right")

        def changed(_v=None):
            v = int(float(var.get()))
            var.set(v)
            val.configure(text=str(v))
            self.ini.set(key, v)
            self.save_ini()
        ttk.Scale(r, from_=lo, to=hi, variable=var, command=changed, length=200).pack(side="right", padx=8)

    def save_ini(self):
        try:
            self.ini.save()
        except OSError as e:
            messagebox.showerror(GAME_TITLE, T("save_fail", e=e))

    # ---------------------------------------------------------------- play
    def _page_play(self):
        f = self.page("SILENT HILL 3", T("play_sub"), hero=True)
        c = self.card(f, T("status"))
        self.status_lbl = {}
        for key, label in (("disc", T("disc")), ("bios", T("bios")), ("exe", T("port"))):
            r = ttk.Frame(c, style="Panel.TFrame")
            r.pack(fill="x", pady=3)
            ttk.Label(r, text=label, style="Panel.TLabel", width=16).pack(side="left")
            lbl = tk.Label(r, text="", bg=PANEL, fg=FG, font=font(), anchor="w")
            lbl.pack(side="left", fill="x", expand=True)
            self.status_lbl[key] = lbl
        c2 = self.card(f, T("summary"))
        self.summary = ttk.Label(c2, text="", style="PanelMuted.TLabel", justify="left")
        self.summary.pack(anchor="w")
        bottom = ttk.Frame(f)
        bottom.pack(fill="x", pady=(10, 0))
        self.play_btn = ttk.Button(bottom, text=T("play"), style="Accent.TButton", command=self.play)
        self.play_btn.pack(side="left")
        self.play_state = ttk.Label(bottom, text="", style="Muted.TLabel")
        self.play_state.pack(side="left", padx=16)
        self.lvar_backup = tk.BooleanVar(value=self.lset.get("auto_backup", True))
        self.lvar_minimize = tk.BooleanVar(value=self.lset.get("minimize", True))
        opts = ttk.Frame(f)
        opts.pack(fill="x", pady=(14, 0))
        for var, text, key in ((self.lvar_backup, T("backup_before"), "auto_backup"),
                               (self.lvar_minimize, T("minimize"), "minimize")):
            cb = tk.Checkbutton(opts, text=text, variable=var, bg=BG, fg=FG, selectcolor=PANEL,
                                activebackground=BG, activeforeground=FG, font=font(),
                                command=lambda k=key, v=var: self._lset(k, v.get()))
            cb.pack(anchor="w")
        return f

    def _lset(self, key, value):
        self.lset[key] = value
        save_launcher_settings(self.lset)

    def disc_ok(self):
        p = self.ini.get("iso")
        return bool(p) and Path(p).is_file()

    def bios_ok(self):
        p = self.ini.get("bios")
        return bool(p) and Path(p).exists() and check_bios(p)[0]

    def refresh_play(self):
        def put(key, ok, text):
            self.status_lbl[key].configure(text=("✔  " if ok else "✖  ") + text, fg=OK if ok else BAD)
        iso = self.ini.get("iso")
        put("disc", self.disc_ok(), Path(iso).name if self.disc_ok() else T("not_selected"))
        bios = self.ini.get("bios")
        put("bios", self.bios_ok(), check_bios(bios)[1] if self.bios_ok() else T("not_selected"))
        exe = BIN / GAME_EXE
        put("exe", exe.exists(), str(exe) if exe.exists() else T("exe_missing", exe=GAME_EXE, dir=BIN))
        g = self.ini.get
        res = dict(resolutions()).get(g("upscale"), "?")
        rend = dict(renderers()).get(g("renderer"), T("auto"))
        aspect = T("ws169") if g("widescreen") == "1" else "4:3"
        mode = T("fullscreen") if g("fullscreen") == "1" else T("windowed")
        gpu = T("gpu_high_s") if g("gpu") != "integrated" else T("gpu_int_s")
        lines = [
            f"{T('sum_res')}: {res}    {T('sum_aspect')}: {aspect}    {mode}",
            f"{T('sum_renderer')}: {rend}    GPU: {gpu}",
            f"{T('sum_fps')}: {'60 FPS' if g('fps60') == '1' else T('fps30')}"
            f"    {T('sum_fast')}: {T('on') if g('fast_cdvd') == '1' else T('off')}",
        ]
        self.summary.configure(text="\n".join(lines))
        ready = self.disc_ok() and self.bios_ok() and exe.exists()
        running = self.game_proc is not None and self.game_proc.poll() is None
        self.play_btn.state(["!disabled"] if ready and not running else ["disabled"])
        if running:
            self.play_state.configure(text=T("running"))
        elif not ready:
            self.play_state.configure(text=T("need_files"))
        else:
            self.play_state.configure(text=T("ready"))

    def play(self):
        if self.game_proc is not None and self.game_proc.poll() is None:
            return
        self.ini.set("boot", BOOT_PATH)
        self.save_ini()
        if self.lvar_backup.get():
            self.backup_saves(quiet=True)
        LOGS.mkdir(parents=True, exist_ok=True)
        log_path = LOGS / "last_run.log"
        try:
            log = open(log_path, "w", encoding="utf-8", errors="replace")
            self.game_proc = subprocess.Popen([str(BIN / GAME_EXE)], cwd=str(BIN), stdout=log,
                                              stderr=subprocess.STDOUT, creationflags=NO_WINDOW)
            log.close()
        except OSError as e:
            messagebox.showerror(GAME_TITLE, T("start_fail", e=e))
            return
        self.refresh_play()
        if self.lvar_minimize.get():
            self.iconify()
        self.after(1000, self.watch_game)

    def watch_game(self):
        if self.game_proc is None:
            return
        code = self.game_proc.poll()
        if code is None:
            self.after(1000, self.watch_game)
            return
        self.game_proc = None
        self.deiconify()
        self.refresh_play()
        if code not in (0, 1):
            if messagebox.askyesno(GAME_TITLE, T("crashed", code=code)):
                os.startfile(LOGS / "last_run.log")

    # ---------------------------------------------------------------- files
    def _page_files(self):
        f = self.page(T("nav_files"), T("files_sub"))
        c = self.card(f, T("disc_card"))
        self.disc_var = tk.StringVar(value=self.ini.get("iso"))
        self.disc_msg = tk.Label(c, text="", bg=PANEL, fg=MUTED, font=font(), anchor="w")
        self._path_row(c, self.disc_var, self.pick_disc)
        self.disc_msg.pack(fill="x", pady=(6, 0))
        c = self.card(f, T("bios_card"))
        self.bios_var = tk.StringVar(value=self.ini.get("bios"))
        self.bios_msg = tk.Label(c, text="", bg=PANEL, fg=MUTED, font=font(), anchor="w")
        self._path_row(c, self.bios_var, self.pick_bios, folder_cmd=self.pick_bios_dir)
        self.bios_msg.pack(fill="x", pady=(6, 0))
        self._validate_files()
        return f

    def _path_row(self, parent, var, cmd, folder_cmd=None):
        r = ttk.Frame(parent, style="Panel.TFrame")
        r.pack(fill="x")
        e = tk.Entry(r, textvariable=var, bg="#111", fg=FG, insertbackground=FG, relief="flat",
                     font=font(), readonlybackground="#111", state="readonly")
        e.pack(side="left", fill="x", expand=True, ipady=6)
        if folder_cmd:
            ttk.Button(r, text=T("folder_btn"), command=folder_cmd).pack(side="right", padx=(6, 0))
        ttk.Button(r, text=T("choose_btn"), command=cmd).pack(side="right", padx=(6, 0))

    def _validate_files(self):
        iso = self.ini.get("iso")
        if iso:
            ok, msg = check_disc(iso)
            self.disc_msg.configure(text=("✔  " if ok else "✖  ") + msg, fg=OK if ok else BAD)
        else:
            self.disc_msg.configure(text=T("not_chosen"), fg=MUTED)
        bios = self.ini.get("bios")
        if bios:
            ok, msg = check_bios(bios)
            self.bios_msg.configure(text=("✔  " if ok else "✖  ") + msg, fg=OK if ok else BAD)
        else:
            self.bios_msg.configure(text=T("not_chosen"), fg=MUTED)

    def pick_disc(self):
        path = filedialog.askopenfilename(
            title=T("pick_disc_title"),
            filetypes=[(T("disc_filetype"), "*.iso *.chd *.cso *.zso *.bin"), (T("all_files"), "*.*")])
        if not path:
            return
        ok, msg = check_disc(path)
        if not ok:
            messagebox.showerror(GAME_TITLE, msg)
            return
        path = str(Path(path))
        self.disc_var.set(path)
        self.ini.set("iso", path)
        self.ini.set("boot", BOOT_PATH)
        self.save_ini()
        self._validate_files()

    def _set_bios(self, path):
        ok, msg = check_bios(path)
        if not ok:
            messagebox.showerror(GAME_TITLE, msg)
            return
        path = str(Path(path))
        self.bios_var.set(path)
        self.ini.set("bios", path)
        self.save_ini()
        self._validate_files()

    def pick_bios(self):
        path = filedialog.askopenfilename(title=T("pick_bios_title"),
                                          filetypes=[("PS2 BIOS", "*.bin *.rom *.rom0"),
                                                     (T("all_files"), "*.*")])
        if path:
            self._set_bios(path)

    def pick_bios_dir(self):
        path = filedialog.askdirectory(title=T("pick_bios_dir_title"))
        if path:
            self._set_bios(path)

    # ---------------------------------------------------------------- graphics
    def _page_graphics(self):
        f = self.page(T("nav_graphics"), T("gfx_sub"))
        c = self.card(f, T("card_display"))
        self.combo(c, "upscale", T("res_label"), resolutions(), T("res_hint"))
        self.combo(c, "widescreen", T("aspect_label"), [("1", T("ws169")), ("0", T("ar43_orig"))],
                   T("aspect_hint"))
        self.combo(c, "fullscreen", T("screen_label"), [("0", T("windowed")), ("1", T("fullscreen_bl"))],
                   T("screen_hint"))
        self.check(c, "vsync", T("vsync_label"), T("vsync_hint"))
        c = self.card(f, T("card_quality"))
        self.combo(c, "anisotropy", T("aniso_label"), aniso(), T("aniso_hint"))
        self.check(c, "fxaa", T("fxaa_label"))
        self.slider(c, "sharpen", T("sharpen_label"), 0, 100, T("sharpen_hint"))
        c = self.card(f, T("card_hw"))
        self.combo(c, "renderer", T("api_label"), renderers(), T("api_hint"))
        self.combo(c, "gpu", T("gpu_label"), [("high", T("gpu_high")), ("integrated", T("gpu_int"))],
                   T("gpu_hint"))
        return f

    # ---------------------------------------------------------------- game
    def _page_game(self):
        f = self.page(T("nav_game"), T("game_sub"))
        c = self.card(f, T("card_perf"))
        self.check(c, "fps60", "60 FPS", T("fps60_hint"))
        self.check(c, "fast_cdvd", T("fast_label"), T("fast_hint"))
        c = self.card(f, T("card_camera"))
        self.check(c, "camera_behind", T("cam_behind"), T("cam_behind_hint"))
        self.check(c, "camera_l3", T("cam_l3"), T("cam_l3_hint"))
        return f

    # ---------------------------------------------------------------- controls
    def _page_controls(self):
        f = self.page(T("nav_controls"), T("ctrl_sub"))
        c = self.card(f, T("card_pad"))
        self.combo(c, "controller", T("pad_label"), [("1", T("auto")), ("0", T("keyboard_only"))])
        self.check(c, "vibration", T("vibration"))
        c = self.card(f, T("card_kb"))
        ttk.Label(c, text=T("kb_hint"),
                  style="PanelMuted.TLabel").pack(anchor="w", pady=(0, 6))
        frame = ttk.Frame(c, style="Panel.TFrame")
        frame.pack(fill="both", expand=True)
        self.keys = ttk.Treeview(frame, columns=("button", "key"), show="headings", height=9)
        self.keys.heading("button", text=T("col_ps2"))
        self.keys.heading("key", text=T("col_key"))
        self.keys.column("button", width=260)
        self.keys.column("key", width=200)
        sb = ttk.Scrollbar(frame, orient="vertical", command=self.keys.yview)
        self.keys.configure(yscrollcommand=sb.set)
        self.keys.pack(side="left", fill="both", expand=True)
        sb.pack(side="right", fill="y")
        for btn, label, default in pad_buttons():
            self.keys.insert("", "end", iid=btn, values=(label, self.ini.get(f"key_{btn}", default)))
        self.keys.bind("<Double-1>", self.rebind)
        bar = ttk.Frame(c, style="Panel.TFrame")
        bar.pack(fill="x", pady=(8, 0))
        ttk.Button(bar, text=T("reset"), command=self.reset_keys).pack(side="right")
        return f

    def rebind(self, _event=None):
        sel = self.keys.selection()
        if not sel:
            return
        btn = sel[0]
        label = dict((b, l) for b, l, _d in pad_buttons())[btn]
        dlg = tk.Toplevel(self)
        dlg.title(T("bind_title"))
        dlg.configure(bg=PANEL)
        dlg.geometry("420x140")
        dlg.transient(self)
        dlg.grab_set()
        tk.Label(dlg, text=f"{label}\n\n{T('bind_prompt')}", bg=PANEL, fg=FG,
                 font=font()).pack(expand=True)

        def on_key(event):
            if event.keysym == "Escape":
                dlg.destroy()
                return
            name = key_name(event)
            if not name:
                return
            if name == "F11":
                messagebox.showwarning(GAME_TITLE, T("f11_reserved"), parent=dlg)
                return
            self.keys.set(btn, "key", name)
            self.ini.set(f"key_{btn}", name)
            self.save_ini()
            dlg.destroy()
        dlg.bind("<Key>", on_key)
        dlg.focus_force()

    def reset_keys(self):
        for btn, _label, default in pad_buttons():
            self.keys.set(btn, "key", default)
            self.ini.set(f"key_{btn}", default)
        self.save_ini()

    # ---------------------------------------------------------------- saves
    def _page_saves(self):
        f = self.page(T("nav_saves"), T("saves_sub"))
        c = self.card(f, T("card_mc"))
        self.mc_info = ttk.Label(c, text="", style="PanelMuted.TLabel", justify="left")
        self.mc_info.pack(anchor="w")
        bar = ttk.Frame(c, style="Panel.TFrame")
        bar.pack(fill="x", pady=(10, 0))
        ttk.Button(bar, text=T("backup_now"), command=self.backup_saves).pack(side="left")
        ttk.Button(bar, text=T("restore"), command=self.restore_saves).pack(side="left", padx=6)
        ttk.Button(bar, text=T("open_folder"), command=lambda: self.open_dir(MEMCARDS)).pack(side="left")
        return f

    def refresh_saves(self):
        mc = MEMCARDS / "Mcd001.ps2"
        backups = sorted((MEMCARDS / "backups").glob("*.ps2")) if (MEMCARDS / "backups").exists() else []
        if mc.exists():
            t = datetime.datetime.fromtimestamp(mc.stat().st_mtime).strftime("%d.%m.%Y %H:%M")
            text = T("mc_info", path=mc, time=t, n=len(backups))
        else:
            text = T("no_mc")
        self.mc_info.configure(text=text)

    def backup_saves(self, quiet=False):
        mc = MEMCARDS / "Mcd001.ps2"
        if not mc.exists():
            if not quiet:
                messagebox.showinfo(GAME_TITLE, T("nothing_backup"))
            return
        dst = MEMCARDS / "backups"
        dst.mkdir(parents=True, exist_ok=True)
        stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
        shutil.copy2(mc, dst / f"Mcd001_{stamp}.ps2")
        old = sorted(dst.glob("Mcd001_*.ps2"))
        for o in old[:-20]:  # keep the 20 most recent
            o.unlink()
        if not quiet:
            self.refresh_saves()
            messagebox.showinfo(GAME_TITLE, T("backed_up"))

    def restore_saves(self):
        if self.game_proc is not None and self.game_proc.poll() is None:
            messagebox.showwarning(GAME_TITLE, T("close_game_first"))
            return
        path = filedialog.askopenfilename(title=T("pick_backup"), initialdir=str(MEMCARDS / "backups"),
                                          filetypes=[(T("mc_filetype"), "*.ps2")])
        if not path:
            return
        if not messagebox.askyesno(GAME_TITLE, T("restore_confirm")):
            return
        self.backup_saves(quiet=True)
        MEMCARDS.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, MEMCARDS / "Mcd001.ps2")
        self.refresh_saves()
        messagebox.showinfo(GAME_TITLE, T("restored"))

    def open_dir(self, path):
        Path(path).mkdir(parents=True, exist_ok=True)
        os.startfile(path)

    # ---------------------------------------------------------------- about
    def _page_about(self):
        f = self.page(T("nav_about"))
        c = self.card(f)
        text = T("about_text")
        ttk.Label(c, text=text, style="Panel.TLabel", wraplength=640, justify="left").pack(anchor="w")
        bar = ttk.Frame(f)
        bar.pack(fill="x")
        ttk.Button(bar, text=T("game_folder"), command=lambda: self.open_dir(BIN)).pack(side="left")
        ttk.Button(bar, text=T("last_log"), command=self.open_log).pack(side="left", padx=6)
        return f

    def open_log(self):
        log = LOGS / "last_run.log"
        if log.exists():
            os.startfile(log)
        else:
            messagebox.showinfo(GAME_TITLE, T("no_log"))

    def on_close(self):
        if self.game_proc is not None and self.game_proc.poll() is None:
            if not messagebox.askyesno(GAME_TITLE, T("close_confirm")):
                return
        self.destroy()


def single_instance():
    """Bring an already open launcher to the front instead of opening twice."""
    if os.name != "nt":
        return True
    import ctypes
    k32 = ctypes.windll.kernel32
    k32.CreateMutexW(None, False, "Local\\sh3native-launcher")
    if k32.GetLastError() == 183:  # ERROR_ALREADY_EXISTS
        hwnd = ctypes.windll.user32.FindWindowW(None, f"{GAME_TITLE} - Native PC Port")
        if hwnd:
            ctypes.windll.user32.ShowWindow(hwnd, 9)
            ctypes.windll.user32.SetForegroundWindow(hwnd)
        return False
    return True


def main():
    if os.name == "nt":
        try:
            import ctypes
            ctypes.windll.shcore.SetProcessDpiAwareness(1)
        except (OSError, AttributeError):
            pass
    if not single_instance():
        return
    Launcher().mainloop()


if __name__ == "__main__":
    main()
