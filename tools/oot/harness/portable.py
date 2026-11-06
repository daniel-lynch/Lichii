"""Portable harness profile: a directory whose `user/` subdirectory makes the emulator run in
portable mode (path_util.cpp: GetCurrentDir()/user), so a harness run never touches the live
~/.config/eden, ~/.local/share/eden or ~/.cache/eden.

    <profile>/seed/config  pristine copies of qt-config.ini + custom/ (re-patched into user/ every run)
    <profile>/seed/nand    pristine copy of the NAND (system + save; restored every run)
    <profile>/user/        what the emulator sees: config/, nand/, cache/ (shader caches persist
                           between runs so they stay warm), keys/, log/, screenshots/, ...

Everything is copied, never linked. `refresh()` recopies from the live directories.
"""
import os
import re
import shutil
from pathlib import Path

HOME = Path.home()
LIVE_CONFIG = HOME / ".config/eden"
LIVE_DATA = HOME / ".local/share/eden"
LIVE_CACHE = HOME / ".cache/eden"
TITLE_INI = "0400486024DEC000.ini"
DATA_SKIP = {"log", "screenshots", "crash_dumps"}

# Per-game settings the harness pins (section, key, value).
GAME_PINS = [
    ("Renderer", "gpu_accuracy", "0"),               # GPU Mode: Fast
    ("Renderer", "use_reactive_flushing", "false"),
    ("Renderer", "use_asynchronous_shaders", "true"),
    ("Renderer", "preload_all_pipelines", "true"),
    ("Renderer", "evict_unused_pipelines", "false"),
]


def _copytree(src, dst):
    if dst.exists():
        shutil.rmtree(dst)
    shutil.copytree(src, dst, symlinks=False)


def refresh(profile: Path, log=print):
    """(Re)build the profile from the live directories. Read-only on the live side."""
    profile = Path(profile)
    user = profile / "user"
    seed = profile / "seed"
    (seed).mkdir(parents=True, exist_ok=True)
    user.mkdir(parents=True, exist_ok=True)
    seed_cfg = seed / "config"
    if seed_cfg.exists():
        shutil.rmtree(seed_cfg)
    (seed_cfg / "custom").mkdir(parents=True)
    shutil.copy2(LIVE_CONFIG / "qt-config.ini", seed_cfg / "qt-config.ini")
    shutil.copy2(LIVE_CONFIG / "custom" / TITLE_INI, seed_cfg / "custom" / TITLE_INI)
    for p in LIVE_DATA.iterdir():
        if p.name in DATA_SKIP:
            continue
        dst = (seed / "nand") if p.name == "nand" else (user / p.name)
        if p.is_dir():
            _copytree(p, dst)
        else:
            shutil.copy2(p, dst)
    cache = user / "cache"
    if (LIVE_CACHE / "shader").exists():
        _copytree(LIVE_CACHE / "shader", cache / "shader")
    for d in ("log", "screenshots"):
        (user / d).mkdir(exist_ok=True)
    log(f"profile refreshed from live dirs into {profile}")


def cold_copy(profile: Path, dest: Path, log=print, seed=()):
    """Copy the profile to `dest` without its shader caches, for a run that starts with an empty
    shader directory. The source profile is only read; the copy is left in the run folder.
    Each `seed` file is copied into that directory under its parent directory's name (the title
    id), so a run can start with one chosen cache file and nothing else."""
    profile, dest = Path(profile).resolve(), Path(dest).resolve()
    if dest.exists():
        shutil.rmtree(dest)
    shader = profile / "user/cache/shader"
    shutil.copytree(profile, dest, symlinks=False,
                    ignore=lambda d, names: [n for n in names if Path(d, n).resolve() == shader])
    (dest / "user/cache/shader").mkdir(parents=True, exist_ok=True)
    for f in seed:
        f = Path(f).resolve()
        target = dest / "user/cache/shader" / f.parent.name / f.name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(f, target)
        log(f"cold shader cache: seeded {target.relative_to(dest)}")
    log(f"cold shader cache: running from {dest} (empty user/cache/shader{', seeded' if seed else ''})")
    return dest


def exists(profile: Path):
    p = Path(profile)
    return (p / "seed/config/qt-config.ini").exists() and (p / "seed/nand").is_dir()


# ---------------------------------------------------------------- ini editing
def _set(text, section, key, value, pin_default=True):
    """Set `key=value` in [section]; also force `key\\default=false` so the value is honoured
    (a `key\\default=true` line makes the emulator ignore the value)."""
    lines = text.split("\n")
    out, in_sec, done, done_def, sec_seen = [], False, False, not pin_default, False
    kv = re.compile(r"^" + re.escape(key) + r"=")
    kd = re.compile(r"^" + re.escape(key) + r"\\default=")
    for ln in lines:
        if ln.startswith("["):
            if in_sec and not (done and done_def):
                if not done_def:
                    out.append(f"{key}\\default=false")
                if not done:
                    out.append(f"{key}={value}")
                done = done_def = True
            in_sec = ln.strip() == f"[{section}]"
            sec_seen = sec_seen or in_sec
        elif in_sec and kv.match(ln):
            ln, done = f"{key}={value}", True
        elif in_sec and kd.match(ln) and pin_default:
            ln, done_def = f"{key}\\default=false", True
        out.append(ln)
    if not sec_seen:
        out += ["", f"[{section}]"]
        in_sec = True
    if in_sec and not (done and done_def):
        if not done_def:
            out.append(f"{key}\\default=false")
        if not done:
            out.append(f"{key}={value}")
    return "\n".join(out)


def player0_mapping(guid):
    q = lambda s: '"' + s + '"'
    b = lambda n: q(f"engine:sdl,guid:{guid},port:0,button:{n}")
    hat = lambda d: q(f"engine:sdl,guid:{guid},hat:0,port:0,direction:{d}")
    trig = lambda a: q(f"engine:sdl,guid:{guid},threshold:0.500000,port:0,axis:{a},invert:+")
    stick = lambda x, y: q(f"engine:sdl,guid:{guid},axis_x:{x},offset_x:-0.000000,invert_y:+,"
                           f"deadzone:0.150000,port:0,axis_y:{y},offset_y:0.000000,invert_x:+")
    return {
        "button_a": b(1), "button_b": b(0), "button_x": b(3), "button_y": b(2),
        "button_lstick": b(9), "button_rstick": b(10), "button_l": b(4), "button_r": b(5),
        "button_zl": trig(2), "button_zr": trig(5), "button_plus": b(7), "button_minus": b(6),
        "button_dleft": hat("left"), "button_dup": hat("up"), "button_dright": hat("right"),
        "button_ddown": hat("down"), "button_slleft": b(4), "button_srleft": b(5),
        "button_home": b(8), "button_screenshot": "[empty]", "button_slright": b(4),
        "button_srright": b(5), "lstick": stick(0, 1), "rstick": stick(3, 4),
        "motionleft": "[empty]", "motionright": "[empty]",
    }


def prepare_run(profile: Path, guid, keep_save=False, extra_pins=(), qt_extra=(), log=print):
    """Copy the seed config/save into user/, then patch the copies for an unattended run.
    qt_extra: more (section, key, value) for the profile's qt-config.ini (e.g. the gdb stub)."""
    profile = Path(profile).resolve()
    user = profile / "user"
    cfg = user / "config"
    if cfg.exists():
        shutil.rmtree(cfg)
    shutil.copytree(profile / "seed/config", cfg)
    if not keep_save or not (user / "nand").exists():
        _copytree(profile / "seed/nand", user / "nand")
    for d in ("log", "screenshots", "cache"):
        (user / d).mkdir(exist_ok=True)

    qt = cfg / "qt-config.ini"
    s = qt.read_text()
    # Every absolute path that points at the live data/config/cache dirs -> the profile.
    s = s.replace(str(LIVE_DATA), str(user)).replace(str(LIVE_CONFIG), str(cfg)) \
         .replace(str(LIVE_CACHE), str(user / "cache"))
    s = _set(s, "Controls", "player_0_type", "0")
    s = _set(s, "Controls", "player_0_connected", "true")
    for k, v in player0_mapping(guid).items():
        s = _set(s, "Controls", f"player_0_{k}", v)
    s = _set(s, "UI", "confirmStop", "2")                 # ConfirmStop::Ask_Never: no close dialog
    s = _set(s, "UI", "check_for_updates", "false")       # no network on start
    s = _set(s, "UI", "pauseWhenInBackground", "false")
    s = _set(s, "UI", "muteWhenInBackground", "false")
    s = _set(s, "UI", "Screenshots\\enable_screenshot_save_as", "false")  # hotkey saves silently
    s = _set(s, "UI", "Screenshots\\screenshot_path", str(user / "screenshots"))
    s = _set(s, "Miscellaneous", "log_filter", '"*:Info"')
    for sec, k, v in qt_extra:
        s = _set(s, sec, k, v)
    qt.write_text(s)

    gi = cfg / "custom" / TITLE_INI
    g = gi.read_text()
    for sec, k, v in list(GAME_PINS) + [("Renderer",) + tuple(p.split("=", 1)) for p in extra_pins]:
        g = _set(g, sec, k + "\\use_global", "false", pin_default=False)
        g = _set(g, sec, k, v)
    gi.write_text(g)
    leaks = [str(p) for p in (LIVE_DATA, LIVE_CONFIG, LIVE_CACHE) if str(p) in s or str(p) in g]
    if leaks:
        raise RuntimeError(f"profile config still references live dirs: {leaks}")
    log(f"profile prepared: {cfg} (save {'kept' if keep_save else 'restored from seed'})")
    return user
