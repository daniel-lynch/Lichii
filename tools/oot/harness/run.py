#!/usr/bin/env python3
"""Unattended Ocarina of Time test run on Lichii: boot -> title -> load the newest save -> walk ->
rotate the camera through four headings, with MangoHud (mangoapp) fps, the PCIe sampler, the
emulator log and screenshots, then a clean shutdown and summary.json / summary.md.

    python3 -I tools/oot/harness/run.py [--binary PATH] [--duration 120] [--headless]
                                        [--refresh-profile] [--env EDEN_NX2_X=1 ...]
                                        [--gdb] [--svc-trace FIRST:LAST]

Never touches the live ~/.config/eden, ~/.local/share/eden or ~/.cache/eden: the emulator runs
with its working directory in the harness profile, whose user/ directory puts it in portable mode
(see portable.py). Output: ~/Downloads/OOT/harness-runs/<stamp>/. Exit 0 = full run, 2 = a step
failed (the run stops at the last reliable state; see run.json "failed_step" and the screenshots),
3 = refused to start (another instance, low memory).

--gdb turns the emulator's gdb stub on in the PROFILE's qt-config.ini only (never ~/.config/eden),
continues the guest at boot (the stub holds it until a client sends `c`) and, once in game, runs
the private runtime kit's rsp_probe.py (delta / pcs / count-frames) into the run folder; see
gdbkit.py. The stub listens on 0.0.0.0:<port> (all interfaces, debugger.cpp:146), with no
authentication, for as long as the run is up: anyone who can reach the port can read and write
guest memory. Firewall the port or keep such runs off untrusted networks.
Flash/flicker (on by default; --no-flash turns it off): the run starts the emulator's debug server
on 127.0.0.1:<--debug-port> (loopback only) with its dump dir in <run>/debug/, and once in game arms
the Vulkan flash monitor (flash_monitor): every present is downsampled to 64x36 on the GPU and read
back, and each frame is tested for a one-frame centre brightening of >= --flash-jump luma over both
neighbours ("flashes") and for 8x6-sample tiles whose mean or deviation spikes by >= --tile-jump
over both neighbours ("flicker" frames). The counters are read and reset at the start and end of
each measurement window (flash_stats), so the summary has a flashes and a flicker column per
window. The first detected flash arms one frame capture into <run>/debug/flash-monitor-*/first-flash/.

--svc-trace FIRST:LAST sets EDEN_NX2_SVC_TRACE for the run and parses the log into svc-trace.md
with the kit's svc_trace_parse.py (thread names from the stub when --gdb is on).
"""
import argparse
import datetime as dt
import json
import os
import re
import resource
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.dont_write_bytecode = True  # keep __pycache__ out of the worktree
sys.path.insert(0, str(HERE))  # -I drops the script dir; these are the harness's own modules

import analyze  # noqa: E402
import debugsrv  # noqa: E402
import gdbkit  # noqa: E402
import pad as padmod  # noqa: E402
import portable as prof  # noqa: E402
import states  # noqa: E402

HOME = Path.home()
DEF_BIN = HOME / "dev/Lichii/build-test/bin/lichii"
DEF_GAME = HOME / "Downloads/OOT/The Legend of Zelda Ocarina of TIme [04004860224DEC000].nup"
DEF_PROFILE = HOME / "Downloads/OOT/harness-profile"
DEF_OUT = HOME / "Downloads/OOT/harness-runs"
DEF_ENV = {"EDEN_NX2_DEFERRED_WRITE_LOG": "1"}
EMU_PATTERN = r"bin/lichii|lichii-linux|bin/eden"
CLEANUP_LINE = "Emulation cleanup completed"
LOG_TS = re.compile(rb"^\[\s*(\d+\.\d+)\]")


class Abort(Exception):
    """A scripted step failed; the run stops at the last reliable state."""


# ------------------------------------------------------------------ screenshot backends
class GamescopeShots:
    """gamescopectl screenshot against this run's gamescope socket (independent of the desktop
    compositor; captures the composited output). A debug-server backend (upstream PR #111,
    EDEN_NX2_DEBUG_SERVER) can be added as another class with the same capture() signature."""

    name = "gamescope"

    def __init__(self, socket_name):
        self.env = dict(os.environ, GAMESCOPE_WAYLAND_DISPLAY=socket_name)

    def capture(self, path, timeout=10):
        path = Path(path)
        try:
            subprocess.run(["gamescopectl", "screenshot", str(path)], env=self.env,
                           capture_output=True, timeout=timeout)
        except subprocess.TimeoutExpired:
            return None
        for _ in range(int(timeout / 0.1)):  # gamescope writes the file asynchronously
            if path.exists() and path.stat().st_size > 0:
                time.sleep(0.2)
                return path
            time.sleep(0.1)
        return None


SHOT_BACKENDS = {"gamescope": GamescopeShots}


# ------------------------------------------------------------------ helpers
def host_load():
    """1-minute load average and the number of compiler/JVM processes: a run with builds in
    flight is not comparable (2026-11-05: FAST_DRAW runs at 57-97 % system CPU load were invalid)."""
    try:
        load1 = float(open("/proc/loadavg").read().split()[0])
    except (OSError, ValueError):
        load1 = -1.0
    # Count only RUNNING build processes: a build parked with SIGSTOP (the emu-priority daemon pauses
    # compiles while an emulator runs) costs nothing and must not block a run.
    out = subprocess.run(["ps", "-eo", "stat=,comm="], capture_output=True, text=True).stdout
    names = {"ninja", "cc1plus", "cc1", "c++", "g++", "clang", "clang++", "ld", "ld.lld", "mold", "java", "lto1", "as"}
    builders = 0
    for line in out.splitlines():
        parts = line.split(None, 1)
        if len(parts) == 2 and parts[1].strip() in names and "T" not in parts[0]:
            builders += 1
    return load1, builders


def gpu_mem_used_mib():
    """nvidia-smi memory.used of GPU 0 in MiB, or None when unavailable."""
    try:
        out = subprocess.run(["nvidia-smi", "--query-gpu=memory.used", "--format=csv,noheader,nounits", "-i", "0"],
                             capture_output=True, text=True, timeout=10).stdout.strip().splitlines()
        return int(out[0]) if out else None
    except (OSError, ValueError, subprocess.TimeoutExpired):
        return None


def emu_pids():
    out = subprocess.run(["pgrep", "-f", EMU_PATTERN], capture_output=True, text=True).stdout.split()
    # The harness's own ancestors (a `bash -c ... --binary .../bin/lichii` wrapper) match the
    # pattern too; exclude the whole ancestor chain, not just this pid.
    mine = set()
    pid = os.getpid()
    while pid > 1:
        mine.add(pid)
        try:
            with open(f"/proc/{pid}/stat") as f:
                pid = int(f.read().rsplit(")", 1)[1].split()[1])
        except (OSError, ValueError, IndexError):
            break
    # Only processes whose executable is an emulator: other harnesses waiting in --wait-idle carry
    # "--binary .../bin/lichii" in their command line too, and counting them made every waiting
    # harness wait for the others until the timeout.
    def is_emulator(pid):
        try:
            return re.search(EMU_PATTERN, os.readlink(f"/proc/{pid}/exe")) is not None
        except OSError:
            return False
    return [int(p) for p in out if int(p) not in mine and is_emulator(int(p))]


def descendants(pid):
    kids, frontier = [], [pid]
    while frontier:
        p = frontier.pop()
        try:
            for t in Path(f"/proc/{p}/task").iterdir():
                c = (t / "children").read_text().split()
                kids += map(int, c)
                frontier += map(int, c)
        except OSError:
            pass
    return kids


def mem_available_gib():
    for ln in open("/proc/meminfo"):
        if ln.startswith("MemAvailable:"):
            return int(ln.split()[1]) / 2**20
    return 0.0


def last_log_ts(path):
    """Timestamp of the last complete line of the emulator log (seconds since emulator start)."""
    try:
        with open(path, "rb") as f:
            f.seek(0, 2)
            size = f.tell()
            f.seek(max(0, size - 65536))
            lines = f.read().split(b"\n")[:-1]
    except OSError:
        return None
    for ln in reversed(lines):
        m = LOG_TS.match(ln)
        if m:
            return float(m.group(1))
    return None


def port_free(port):
    """Bind test only: connecting to a live gdb stub would pause that emulator's guest."""
    import socket
    with socket.socket() as s:
        try:
            s.bind(("0.0.0.0", port))
            return True
        except OSError:
            return False


def live_fingerprint():
    """mtimes of the live emulator files the harness must never write (checked after the run)."""
    fp = {}
    for root in (prof.LIVE_CONFIG, prof.LIVE_CACHE / "shader", prof.LIVE_DATA / "nand/user"):
        if root.exists():
            for p in root.rglob("*"):
                if p.is_file():
                    fp[str(p)] = p.stat().st_mtime_ns
    return fp


# ------------------------------------------------------------------ the run
class Run:
    def __init__(self, a):
        self.a = a
        self.stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
        self.dir = Path(a.out_root) / self.stamp
        self.dir.mkdir(parents=True)
        self.t0 = time.time()
        self.steps, self.phases = [], []
        self.log_offset = None
        self.mh_start = None
        self.gs = self.sampler = self.pad = self.shots = None
        self.emu_pid = None
        self.user = None
        self.n_shot = 0
        self.dbg = None  # debugsrv.DebugClient while the flash monitor runs
        self.info = {"stamp": self.stamp, "binary": str(a.binary), "game": str(a.game),
                     "profile": str(a.profile), "headless": a.headless}
        self.kit = gdbkit.kit_tools(a.kit)

    # -- bookkeeping
    def say(self, msg):
        line = f"[{time.time() - self.t0:7.1f}s] {msg}"
        print(line, flush=True)
        with open(self.dir / "harness.log", "a") as f:
            f.write(line + "\n")

    def step(self, name, status, note=""):
        self.steps.append({"t": round(time.time() - self.t0, 1), "step": name, "status": status, "note": note})
        self.say(f"{name}: {status}" + (f" ({note})" if note else ""))

    @property
    def emu_log(self):
        return self.user / "log/eden_log.txt"

    def sync_clock(self):
        ts = last_log_ts(self.emu_log)
        if ts is not None:
            off = time.time() - ts
            self.log_offset = off if self.log_offset is None else min(self.log_offset, off)

    def shot(self, name):
        self.n_shot += 1
        p = self.dir / "screenshots" / f"{self.n_shot:03d}-{name}.png"
        p.parent.mkdir(exist_ok=True)
        return self.shots.capture(p)

    def look(self, keep=None):
        """Screenshot + classification. Polling shots go to screenshots/poll/ and are pruned."""
        self.sync_clock()
        tmp = self.dir / "screenshots/poll" / f"{int((time.time() - self.t0) * 10):06d}.png"
        tmp.parent.mkdir(parents=True, exist_ok=True)
        if not self.shots.capture(tmp):
            return "noshot", {}, None
        from PIL import Image
        state, f = states.classify(Image.open(tmp), self.refs)
        if keep:
            self.n_shot += 1
            dst = self.dir / "screenshots" / f"{self.n_shot:03d}-{keep}.png"
            shutil.copy2(tmp, dst)
            return state, f, dst
        return state, f, tmp

    def wait_state(self, want, timeout, every=2.0, on_tick=None):
        end = time.time() + timeout
        state, f, last = None, {}, None
        while time.time() < end:
            if self.emu_pid and not Path(f"/proc/{self.emu_pid}").exists():
                raise Abort("emulator exited")
            state, f, last = self.look()
            if state in want:
                return state, f, last
            if on_tick:
                on_tick(state)
            time.sleep(every)
        return None, f, last

    # -- launch
    def preflight(self):
        # One emulator at a time, enforced with a lock held for the whole run: polling emu_pids()
        # alone let every waiting harness launch at the same instant when the previous run
        # exited (2026-11-05 19:05: three emulators at once faulted the GPU, Xid 51 / reset required).
        self.lock_path = Path.home() / "Downloads/OOT/harness-runs/.emulator.lock"
        self.lock_path.parent.mkdir(parents=True, exist_ok=True)
        self.lock_fd = os.open(self.lock_path, os.O_RDWR | os.O_CREAT, 0o644)
        import fcntl
        deadline = time.time() + max(self.a.wait_idle, 0)
        while True:
            try:
                fcntl.flock(self.lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except OSError:
                if time.time() >= deadline:
                    self.say("refusing to start: another harness holds the emulator lock")
                    shutil.rmtree(self.dir, ignore_errors=True)
                    sys.exit(3)
                time.sleep(5)
        os.ftruncate(self.lock_fd, 0)
        os.write(self.lock_fd, f"{os.getpid()} {self.dir}\n".encode())
        # The previous run released the lock at process exit, but the driver can take seconds to
        # free that instance's VRAM; launching into that window dies at vkCreateDevice with
        # std::bad_alloc (2026-11-05 20:54, two runs in a row). Wait for the GPU to settle.
        settle_end = time.time() + 90
        while time.time() < settle_end:
            used = gpu_mem_used_mib()
            if not emu_pids() and (used is None or used < self.a.gpu_settle_mib):
                break
            time.sleep(2)
        else:
            self.say(f"GPU did not settle (used {gpu_mem_used_mib()} MiB); launching anyway")
        time.sleep(3)
        others = emu_pids()
        if others and self.a.wait_idle:
            self.say(f"another emulator instance is running ({others}); waiting up to {self.a.wait_idle}s")
            end = time.time() + self.a.wait_idle
            while others and time.time() < end:
                time.sleep(5)
                others = emu_pids()
        load1, builders = host_load()
        if self.a.wait_idle and (load1 > self.a.max_load or builders > 0):
            self.say(f"host busy (load {load1:.1f}, {builders} build procs); waiting up to {self.a.wait_idle}s")
            end = time.time() + self.a.wait_idle
            while (load1 > self.a.max_load or builders > 0) and time.time() < end:
                time.sleep(15)
                load1, builders = host_load()
        why = None
        mem = mem_available_gib()
        if others:
            why = f"another emulator instance is running: {others}"
        elif self.a.max_load >= 0 and (load1 > self.a.max_load or builders > 0):
            why = f"host busy: load {load1:.1f} > {self.a.max_load} or {builders} build procs (use --max-load -1 to override)"
        elif mem < self.a.min_mem_gib:
            why = f"MemAvailable {mem:.1f} GiB < {self.a.min_mem_gib} GiB"
        elif not Path(self.a.binary).exists():
            why = f"binary not found: {self.a.binary}"
        elif not self.a.no_flash and not debugsrv.port_free(self.a.debug_port):
            why = f"debug server port {self.a.debug_port} is already bound; pick --debug-port"
        elif self.a.gdb and not port_free(self.a.gdb_port):
            why = f"gdb port {self.a.gdb_port} is already bound (another stub?); pick --gdb-port"
        if why:
            self.say("refusing to start: " + why)
            shutil.rmtree(self.dir, ignore_errors=True)
            sys.exit(3)
        self.info["host_load_start"] = {"load1": load1, "build_procs": builders}

    def launch(self):
        a = self.a
        if a.refresh_profile or not prof.exists(a.profile):
            prof.refresh(a.profile, log=self.say)
        qt_extra = [("Debugging", "use_gdbstub", "true"), ("Debugging", "gdbstub_port", str(a.gdb_port))] \
            if a.gdb else [("Debugging", "use_gdbstub", "false")]
        self.user = prof.prepare_run(a.profile, padmod.sdl_guid(), keep_save=a.keep_save,
                                     extra_pins=a.pin, qt_extra=qt_extra, log=self.say)
        if self.emu_log.exists():
            self.emu_log.unlink()
        self.refs = states.load_refs()

        env = dict(os.environ)
        run_env = dict(DEF_ENV)
        run_env.update({k: v for k, v in os.environ.items() if k.startswith("EDEN_NX2_")})
        if not a.no_flash:
            run_env["EDEN_NX2_DEBUG_SERVER"] = f"127.0.0.1:{a.debug_port}"
            run_env["EDEN_NX2_DEBUG_DIR"] = str(self.dir / "debug")
        for kv in a.env:
            k, _, v = kv.partition("=")
            if v == "":
                run_env.pop(k, None)
            else:
                run_env[k] = v
        env.update(run_env)
        self.info["env"] = run_env
        env["MANGOHUD_CONFIG"] = (f"fps,frametime,no_display,control=mangohud,output_folder={self.dir},"
                                  f"log_interval={a.mangohud_interval}")
        env.pop("MANGOHUD", None)

        # The virtual pad must exist before SDL enumerates joysticks.
        self.pad = padmod.Pad()
        self.say(f"virtual pad {self.pad.devnode} guid {padmod.sdl_guid()}")
        time.sleep(1.0)

        self.sampler = subprocess.Popen(["nvidia-smi", "dmon", "-s", "tu", "-d", "1", "-o", "DT"],
                                        stdout=open(self.dir / "pcie.txt", "w"), stderr=subprocess.DEVNULL)
        gs = ["gamescope", "-W", str(a.width), "-H", str(a.height), "-w", str(a.width), "-h", str(a.height),
              "--force-windows-fullscreen"]
        if a.headless:
            gs += ["--backend", "headless"]
        if not a.no_mangohud:
            gs += ["--mangoapp"]
        inner = ["env", "QT_QPA_PLATFORM=xcb", "MANGOHUD=0", str(a.binary), "-f", "-g", str(a.game)]

        def child_setup():
            os.nice(0)
            if not a.keep_cores:  # crash cores of a 36 GB process fill the disk
                resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

        self.say("launch: " + " ".join(gs + ["--"] + inner))
        self.gs = subprocess.Popen(gs + ["--"] + inner, cwd=str(a.profile), env=env,
                                   stdout=open(self.dir / "gamescope.txt", "w"), stderr=subprocess.STDOUT,
                                   preexec_fn=child_setup, start_new_session=True)
        end = time.time() + 30

        def is_emu(pid):
            try:
                return Path(f"/proc/{pid}/exe").resolve() == Path(a.binary).resolve()
            except OSError:
                return False

        def ppid(pid):
            try:
                return int(Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[1])
            except (OSError, ValueError, IndexError):
                return -1

        while time.time() < end and not self.emu_pid:
            # the emulator itself, not a forked helper of it (whose parent is also the emulator)
            for k in sorted(descendants(self.gs.pid)):
                if is_emu(k) and not is_emu(ppid(k)):
                    self.emu_pid = k
                    break
            time.sleep(0.2)
        sock = None
        if self.emu_pid:  # gamescope tells its clients where it is; read it from the emulator's env
            for _ in range(50):
                try:
                    for kv in Path(f"/proc/{self.emu_pid}/environ").read_bytes().split(b"\0"):
                        if kv.startswith(b"GAMESCOPE_WAYLAND_DISPLAY="):
                            sock = kv.split(b"=", 1)[1].decode()
                except OSError:
                    pass
                if sock:
                    break
                time.sleep(0.2)
        if sock:
            self.shots = SHOT_BACKENDS[a.shots](sock)
        self.info["gamescope_socket"] = sock
        self.info["emu_pid"] = self.emu_pid
        self.step("launch", "ok" if self.emu_pid else "fail", f"pid {self.emu_pid}, socket {sock}")
        if not self.emu_pid:
            raise Abort("emulator process not found under gamescope" +
                        (" (gamescope exited; see gamescope.txt)" if self.gs.poll() is not None else ""))
        if not sock:
            raise Abort("could not read GAMESCOPE_WAYLAND_DISPLAY from the emulator's environment")
        if a.gdb:
            self.gdb_boot()

    # -- gdb stub (--gdb)
    def gdb_boot(self):
        """The stub holds the guest at boot until a client continues it: wait for the server line,
        then send `c` (through the kit's rsp_probe.py, or the kit-less fallback)."""
        a = self.a
        alive = lambda: Path(f"/proc/{self.emu_pid}").exists()
        m = gdbkit.wait_log(self.emu_log, gdbkit.SERVER_LINE, 90, alive)
        if not m:
            raise Abort("gdb stub: no 'Starting server on port' line in the log within 90 s")
        if int(m.group(1)) != a.gdb_port:
            self.say(f"gdb stub: log says port {m.group(1)}, expected {a.gdb_port}; using the log's")
            a.gdb_port = int(m.group(1))
        self.say(f"gdb stub listening on 0.0.0.0:{a.gdb_port} (all interfaces) for this run")
        if self.kit["rsp_probe"]:
            rc, secs = gdbkit.probe(a.kit, a.gdb_port, None, ["resume"], self.dir / "gdb-boot.txt", 90)
            err = None if rc == 0 else f"rsp_probe resume exit {rc} (gdb-boot.txt)"
        else:
            err = gdbkit.raw_continue(a.gdb_port)
        self.step("gdb-continue", "fail" if err else "ok",
                  err or ("rsp_probe resume" if self.kit["rsp_probe"] else
                          f"kit missing at {a.kit}: continued with the built-in client; measurements will be skipped"))
        if err:
            raise Abort("gdb stub: could not continue the guest: " + err)

    def gdb_experiments(self):
        """In game: threads, delta read, pc sampler, fork-join count; each into the run folder and
        as a phase (kind "gdb") so the summary has guest fps over it."""
        a = self.a
        g = self.info.setdefault("gdb", {"port": a.gdb_port, "kit": str(a.kit), "steps": {}})
        if not self.kit["rsp_probe"]:
            g["skipped"] = (f"rsp_probe.py not found in {a.kit} (the private runtime kit is not part of "
                            "this repository); the guest was continued, nothing was measured")
            self.step("gdb-experiments", "skipped", g["skipped"])
            return
        m = gdbkit.MAIN_LINE.search(self.emu_log.read_text(errors="replace"))
        if not m:
            g["skipped"] = "no 'Module main loaded at' line in the log; main base unknown"
            self.step("gdb-experiments", "skipped", g["skipped"])
            return
        base = g["main_base"] = m.group(1)
        d = self.dir
        plan = [
            ("threads", None, ["threads", "--out", str(d / "threads.txt")], "threads-stub.txt", 60),
            ("delta", "gdb-delta", ["delta", "--seconds", str(a.gdb_delta_seconds), "--out", str(d / "delta.csv")],
             "delta.txt", a.gdb_delta_seconds + 90),
            ("pcs", "gdb-pcs", ["pcs", "--seconds", str(a.gdb_pcs_seconds), "--top", "10", "--out", str(d / "pcs.csv")],
             "pcs.txt", a.gdb_pcs_seconds + 120),
            ("count", "gdb-count", ["count-frames", "--frames", str(a.gdb_frames), "--max-seconds", "300"],
             "count-frames.txt", 400),
        ]
        for key, phase, args, fname, timeout in plan:
            if not Path(f"/proc/{self.emu_pid}").exists():
                raise Abort(f"emulator exited before gdb {key}")
            self.sync_clock()
            start = time.time()
            rc, secs = gdbkit.probe(a.kit, a.gdb_port, base, args, d / fname, timeout)
            self.sync_clock()
            if phase:
                self.phases.append({"name": phase, "kind": "gdb", "start": start, "end": time.time()})
            g["steps"][key] = {"rc": rc, "seconds": secs, "file": fname}
            self.step(f"gdb-{key}", "ok" if rc == 0 else "fail", f"exit {rc}, {secs}s -> {fname}")
        # Whatever happened above, leave the guest running for the rest of the run.
        gdbkit.probe(a.kit, a.gdb_port, base, ["resume"], d / "gdb-resume.txt", 60)
        g["delta"] = gdbkit.summarize_delta(d / "delta.csv")
        g["pcs"] = gdbkit.summarize_pcs(d / "pcs.csv")
        g["count"] = gdbkit.summarize_count(d / "count-frames.txt")

    def check_input(self):
        """The SDL driver logs controller lines at boot; record what it saw."""
        txt = self.emu_log.read_text(errors="replace") if self.emu_log.exists() else ""
        lines = [l for l in txt.splitlines()[:400] if "input_common/drivers/sdl" in l or "Input <" in l]
        self.info["input_log_lines"] = lines[:10]

    # -- scripted boot
    def boot_to_game(self):
        a = self.a
        st, f, _ = self.wait_state({"title"}, a.boot_timeout)
        self.check_input()
        if st != "title":
            self.shot("FAIL-title")
            raise Abort(f"title screen not detected within {a.boot_timeout}s (last features {f})")
        time.sleep(1.5)  # title menu fades in; input before that is ignored
        self.shot("title")
        self.step("title", "ok", f"title score {f.get('title')}")

        for name, want, keep in (("file-select", {"load_list", "load_confirm"}, "file-select"),
                                 ("load-confirm", {"load_confirm"}, "load-confirm")):
            ok = None
            for attempt in range(3):
                self.pad.press("a")
                ok, f, _ = self.wait_state(want, 6, every=1.0)
                if ok:
                    break
                self.say(f"{name}: not reached after A (attempt {attempt + 1}, features {f})")
            if not ok:
                self.shot(f"FAIL-{name}")
                raise Abort(f"{name} not reached after 3 A presses (last features {f})")
            time.sleep(0.8)
            self.shot(keep)
            self.step(name, "ok", ok)

        pressed = [time.time()]
        self.pad.press("a")

        def nudge(state):  # still on the confirm dialog: the press was dropped, press again
            if state == "load_confirm" and time.time() - pressed[0] > 4:
                self.pad.press("a")
                pressed[0] = time.time()

        st, f, _ = self.wait_state({"ingame"}, a.load_timeout, on_tick=nudge)
        if st != "ingame":
            self.shot("FAIL-ingame")
            raise Abort(f"in-game HUD not detected within {a.load_timeout}s (last features {f})")
        time.sleep(a.settle)
        self.shot("in-game")
        self.step("in-game", "ok", f"hearts {f.get('hearts')}")

    # -- measurement
    def mangohud(self, on):
        if self.a.no_mangohud:
            return
        r = subprocess.run(["mangohudctl", "set", "log_session", "1" if on else "0"],
                           capture_output=True, text=True)
        if on:
            self.mh_start = time.time()
        self.step("mangohud-log-" + ("start" if on else "stop"), "ok" if r.returncode == 0 else "fail",
                  (r.stderr or r.stdout).strip()[:200])

    # -- flash / flicker monitor (debug server)
    def flash_start(self):
        a = self.a
        if a.no_flash:
            return
        info = self.info["flash"] = {"port": a.debug_port, "flash_jump": a.flash_jump, "tile_jump": a.tile_jump,
                                     "capture_first": not a.no_flash_capture}
        client = debugsrv.DebugClient(a.debug_port)
        err = None
        for _ in range(20):  # the server starts once the game has loaded; it is up by now, but be patient
            try:
                info["start"] = client.call("flash_monitor", {"enable": True, "flash_jump": a.flash_jump,
                                                              "tile_jump": a.tile_jump,
                                                              "capture_first": not a.no_flash_capture})
                self.dbg, err = client, None
                break
            except debugsrv.DebugError as e:
                err = str(e)
                if "unknown method" in err:
                    break
                time.sleep(1)
        info["error"] = err
        self.step("flash-monitor", "ok" if self.dbg else "fail",
                  f"127.0.0.1:{a.debug_port}, flash_jump {a.flash_jump}, tile_jump {a.tile_jump}" if self.dbg else err)

    def flash_read(self, reset=True):
        if not self.dbg:
            return None
        try:
            return self.dbg.call("flash_stats", {"reset": reset})
        except debugsrv.DebugError as e:
            self.say(f"flash_stats failed: {e}")
            return None

    def flash_stop(self):
        """Final totals; waits (bounded) for the first-flash capture to finish writing."""
        if not self.dbg:
            return
        info = self.info["flash"]
        end = time.time() + self.a.flash_capture_wait
        final = self.flash_read(reset=False)
        while (final and (final.get("first_flash_capture") or {}).get("complete") is False
               and time.time() < end and Path(f"/proc/{self.emu_pid}").exists()):
            time.sleep(1)
            final = self.flash_read(reset=False) or final
        try:
            final = self.dbg.call("flash_monitor", {"enable": False}) or final
        except debugsrv.DebugError as e:
            self.say(f"flash_monitor disable failed: {e}")
        info["final"] = final
        self.dbg.close()
        self.dbg = None
        t = (final or {}).get("total", {})
        cap = (final or {}).get("first_flash_capture")
        self.step("flash-monitor-stop", "ok",
                  f"{t.get('flashes')} flashes, {t.get('flicker_frames')} flicker frames over {t.get('frames')} probes; "
                  f"cpu {(final or {}).get('cpu_mean_ms')} ms gpu {(final or {}).get('gpu_mean_ms')} ms per frame"
                  + (f"; first flash dump {cap.get('directory')} complete={cap.get('complete')}" if cap else ""))

    def phase(self, name, seconds, kind="phase", action=None, **extra):
        self.flash_read(reset=True)  # discard what happened before the window (turns, screenshots)
        start = time.time()
        if action:
            action()
        end_at = start + seconds
        while time.time() < end_at:
            if not Path(f"/proc/{self.emu_pid}").exists():
                raise Abort(f"emulator exited during {name}")
            self.sync_clock()
            time.sleep(min(1.0, max(0.0, end_at - time.time())))
        ph = {"name": name, "kind": kind, "start": start, "end": time.time()}
        fl = self.flash_read(reset=True)
        if fl:
            ph["flash"] = fl.get("window")
        ph.update(extra)
        self.phases.append(ph)
        return ph

    def turn(self):
        """~90 degrees of camera yaw: short right-stick pulses (calibrated with --probe-steps:
        24 x 0.2 s pulses = one full turn on the Kokiri Forest spawn)."""
        for _ in range(self.a.turn_pulses):
            self.pad.hold_stick("r", self.a.rot_dir, 0, self.a.pulse)
            time.sleep(self.a.pulse_gap)

    def measure(self):
        a = self.a
        first = len(self.phases)  # phases before this (e.g. --gdb's) are not part of "in-game"
        self.mangohud(True)
        t_end = time.time() + a.duration
        self.phase("idle-spawn", min(a.heading_seconds, a.duration / 4))
        self.shot("spawn")
        walk = self.phase("walk", a.walk, action=lambda: self.pad.hold_stick("l", 0, -1, a.walk))
        self.shot("after-walk")
        self.step("walk", "ok", f"{walk['end'] - walk['start']:.1f}s forward")
        from PIL import Image
        box = (0, 120, 1920, 900)
        cycle, start_view = 0, None
        per_cycle = 4 * (a.heading_seconds + a.turn_pulses * (a.pulse + a.pulse_gap) + 1.5)
        while cycle == 0 or time.time() + per_cycle <= t_end:
            for q in range(4):
                time.sleep(0.5)  # camera settles
                shot = self.shot(f"heading{q}-c{cycle}")
                if q == 0 and shot:
                    v = states.thumb(Image.open(shot), box)
                    if start_view is not None:  # did four turns come back to the same view?
                        self.step(f"rotation-closure-c{cycle}", "ok", f"MAD vs cycle-0 q0 view {states.mad(v, start_view):.1f}")
                    else:
                        start_view = v
                self.phase(f"q{q}", a.heading_seconds, kind="heading", cycle=cycle,
                           screenshot=str(shot.relative_to(self.dir)) if shot else None)
                self.turn()
            self.step(f"heading-cycle-{cycle}", "ok")
            cycle += 1
        rest = t_end - time.time()
        if rest > 2:
            self.phase("tail", rest)
        self.mangohud(False)
        self.phases.insert(0, {"name": "in-game", "kind": "phase",
                               "start": self.phases[first]["start"], "end": self.phases[-1]["end"]})
        self.flash_stop()

    def rotation_probe(self):
        """Calibration: turn the camera in small right-stick steps with a screenshot after each, so
        --turn-pulses/--pulse can be set to what turns 90 degrees (compare the shots to step 0)."""
        from PIL import Image
        box = (0, 120, 1920, 900)
        p0 = self.shot("probe-000")
        ref = states.thumb(Image.open(p0), box) if p0 else None
        scores = []
        for i in range(1, self.a.probe_steps + 1):
            self.pad.hold_stick("r", self.a.rot_dir, 0, self.a.probe_step)
            time.sleep(0.6)
            p = self.shot(f"probe-{i:03d}")
            d = round(states.mad(states.thumb(Image.open(p), box), ref), 1) if (p and ref) else None
            scores.append((round(i * self.a.probe_step, 2), d))
        self.info["rotation_probe"] = scores
        self.step("rotation-probe", "ok", " ".join(f"{t}s:{d}" for t, d in scores))

    # -- shutdown
    def shutdown(self):
        res = {"signal": None, "exit_seconds": None, "cleanup_line": False, "forced": False}
        if self.emu_pid and Path(f"/proc/{self.emu_pid}").exists():
            t = time.time()
            os.kill(self.emu_pid, signal.SIGTERM)  # SIGINT is _exit(1) in this frontend
            res["signal"] = "SIGTERM"
            while Path(f"/proc/{self.emu_pid}").exists() and time.time() - t < self.a.shutdown_timeout:
                time.sleep(0.25)
            if Path(f"/proc/{self.emu_pid}").exists():
                os.kill(self.emu_pid, signal.SIGKILL)
                res["forced"] = True
                time.sleep(1)
            res["exit_seconds"] = round(time.time() - t, 1)
        if self.gs:
            try:
                self.gs.wait(15)
            except subprocess.TimeoutExpired:
                os.killpg(self.gs.pid, signal.SIGTERM)
                try:
                    self.gs.wait(10)
                except subprocess.TimeoutExpired:
                    os.killpg(self.gs.pid, signal.SIGKILL)
        if self.sampler:
            self.sampler.terminate()
            try:
                self.sampler.wait(5)
            except subprocess.TimeoutExpired:
                self.sampler.kill()
        if self.pad:
            self.pad.close()
            self.pad = None
        if self.user and self.emu_log.exists():
            shutil.copy2(self.emu_log, self.dir / "eden_log.txt")
            res["cleanup_line"] = CLEANUP_LINE in (self.dir / "eden_log.txt").read_text(errors="replace")
        if self.emu_pid:  # did it crash on the way out? (systemd-coredump records it even with cores off)
            time.sleep(1)
            try:
                cd = subprocess.run(["coredumpctl", "--json=short", "--no-pager", "list", str(self.emu_pid)],
                                    capture_output=True, text=True, timeout=20)
                hits = json.loads(cd.stdout) if cd.returncode == 0 and cd.stdout.strip() else []
                if hits:
                    res["crash_signal"] = signal.Signals(hits[-1]["sig"]).name
            except (subprocess.TimeoutExpired, ValueError, KeyError, OSError):
                pass
        csvs = sorted(p for p in self.dir.glob("*.csv") if not p.name.endswith("_summary.csv"))
        if csvs:
            csvs[-1].rename(self.dir / "mangohud.csv")
            for s in self.dir.glob("*_summary.csv"):
                s.rename(self.dir / "mangohud_summary.csv")
        poll = self.dir / "screenshots/poll"
        if poll.exists() and not self.a.keep_poll_shots:
            shutil.rmtree(poll)
        clean = res["cleanup_line"] and not res["forced"] and "crash_signal" not in res
        self.step("shutdown", "clean" if clean else "unclean",
                  json.dumps(res))
        return res

    def write_run_json(self, result, failed=None, shutdown=None):
        self.info.update({"result": result, "failed_step": failed, "shutdown": shutdown, "steps": self.steps,
                          "phases": self.phases, "log_wall_offset": self.log_offset,
                          "mangohud_start_wall": self.mh_start, "launch_wall": self.t0,
                          "args": {k: (str(v) if isinstance(v, Path) else v) for k, v in vars(self.a).items()}})
        (self.dir / "run.json").write_text(json.dumps(self.info, indent=2))


def build_parser():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--binary", type=Path, default=DEF_BIN)
    ap.add_argument("--game", type=Path, default=DEF_GAME)
    ap.add_argument("--profile", type=Path, default=DEF_PROFILE, help="harness profile dir (portable user/)")
    ap.add_argument("--out-root", type=Path, default=DEF_OUT)
    ap.add_argument("--duration", type=float, default=120, help="in-game measurement seconds")
    ap.add_argument("--heading-seconds", type=float, default=10, help="fps sample per heading")
    ap.add_argument("--walk", type=float, default=3, help="seconds of left stick forward")
    ap.add_argument("--turn-pulses", type=int, default=6, help="right-stick pulses per 90-degree turn")
    ap.add_argument("--pulse", type=float, default=0.2, help="seconds per right-stick pulse")
    ap.add_argument("--pulse-gap", type=float, default=0.6, help="seconds between pulses (the camera coasts briefly; shorter gaps over-turn)")
    ap.add_argument("--rot-dir", type=float, default=1.0, help="right stick x deflection (+1 right, -1 left)")
    ap.add_argument("--probe-steps", type=int, default=0, help="calibration: rotate in N small steps instead of measuring")
    ap.add_argument("--probe-step", type=float, default=0.2, help="calibration: seconds per step")
    ap.add_argument("--headless", action="store_true", help="gamescope headless backend (no window)")
    ap.add_argument("--refresh-profile", action="store_true", help="recopy config/save/caches from the live dirs")
    ap.add_argument("--keep-save", action="store_true", help="do not restore the seed save before the run")
    ap.add_argument("--env", action="append", default=[], metavar="K=V",
                    help="emulator env (repeatable; K= removes a default). EDEN_NX2_* from the caller's env pass through")
    ap.add_argument("--pin", action="append", default=[], metavar="KEY=VAL",
                    help="extra per-game [Renderer] setting to pin (repeatable)")
    ap.add_argument("--no-mangohud", action="store_true")
    ap.add_argument("--mangohud-interval", type=int, default=100, help="ms per MangoHud CSV row")
    ap.add_argument("--shots", choices=sorted(SHOT_BACKENDS), default="gamescope")
    ap.add_argument("--width", type=int, default=1920)
    ap.add_argument("--height", type=int, default=1080)
    ap.add_argument("--boot-timeout", type=float, default=180)
    ap.add_argument("--load-timeout", type=float, default=90)
    ap.add_argument("--settle", type=float, default=4, help="seconds after the HUD appears before measuring")
    ap.add_argument("--shutdown-timeout", type=float, default=60)
    ap.add_argument("--wait-idle", type=float, default=0, help="wait up to N s for another instance to exit")
    ap.add_argument("--max-load", type=float, default=10.0, help="refuse (or, with --wait-idle, wait) while the 1-minute load average exceeds this or any compiler/JVM runs; -1 disables")
    ap.add_argument("--gpu-settle-mib", type=int, default=4096, help="after taking the run lock, wait (up to 90 s) until no emulator runs and GPU memory used is below this, so the previous instance's VRAM is released before vkCreateDevice")
    ap.add_argument("--min-mem-gib", type=float, default=40)
    ap.add_argument("--keep-cores", action="store_true", help="allow core dumps of the emulator")
    ap.add_argument("--keep-poll-shots", action="store_true")
    f = ap.add_argument_group("flash / flicker monitor (debug server; on by default)")
    f.add_argument("--no-flash", action="store_true", help="do not start the debug server or the flash monitor")
    f.add_argument("--debug-port", type=int, default=6545, help="debug server port (127.0.0.1 only)")
    f.add_argument("--flash-jump", type=float, default=6.0,
                   help="flash: centre mean luma (0-255) above BOTH neighbouring frames by at least this")
    f.add_argument("--tile-jump", type=float, default=12.0,
                   help="flicker: a tile's mean or deviation spikes over BOTH neighbours by at least this")
    f.add_argument("--no-flash-capture", action="store_true", help="do not dump a frame on the first flash")
    f.add_argument("--flash-capture-wait", type=float, default=60,
                   help="seconds to wait at the end for the first-flash dump to finish writing")
    g = ap.add_argument_group("gdb stub / SVC trace (opt-in; the measurement scripts are a private kit outside this repo)")
    g.add_argument("--gdb", action="store_true",
                   help="enable the gdb stub in the profile's qt-config.ini, continue the guest at boot, and run "
                        "delta / pcs / count-frames in game. The stub binds 0.0.0.0 (all interfaces), "
                        "unauthenticated, while the run is up")
    g.add_argument("--gdb-port", type=int, default=6543)
    g.add_argument("--kit", type=Path, default=gdbkit.DEF_KIT,
                   help="private runtime kit dir with rsp_probe.py / svc_trace_parse.py (missing -> the guest is "
                        "still continued, the measurements are skipped with a message)")
    g.add_argument("--gdb-delta-seconds", type=float, default=20)
    g.add_argument("--gdb-pcs-seconds", type=float, default=60)
    g.add_argument("--gdb-frames", type=int, default=60, help="frames for count-frames")
    g.add_argument("--svc-trace", metavar="FIRST:LAST",
                   help="EDEN_NX2_SVC_TRACE=FIRST:LAST (present-counter window) for the run, then "
                        "svc_trace_parse.py on the log -> svc-trace.md")
    return ap


def main():
    ap = build_parser()
    a = ap.parse_args()
    if a.svc_trace:
        if not re.fullmatch(r"\d+:\d+", a.svc_trace):
            ap.error("--svc-trace wants FIRST:LAST present numbers, e.g. 4800:4870")
        a.env.append(f"EDEN_NX2_SVC_TRACE={a.svc_trace}")
    a.kit = a.kit.expanduser()
    a.binary, a.game, a.profile = a.binary.resolve(), a.game.resolve(), a.profile.resolve()
    os.nice(5)

    run = Run(a)
    run.preflight()
    fp_before = live_fingerprint()
    result, failed, sd = "ok", None, None
    old_int = signal.getsignal(signal.SIGINT)

    def interrupted(*_):
        raise Abort("interrupted")

    signal.signal(signal.SIGINT, interrupted)
    signal.signal(signal.SIGTERM, interrupted)
    try:
        run.launch()
        run.boot_to_game()
        if a.gdb:
            run.gdb_experiments()
        if a.probe_steps:
            run.rotation_probe()
        else:
            run.flash_start()
            run.measure()
    except Abort as e:
        result, failed = "failed", str(e)
        run.step("abort", "fail", str(e))
        if run.shots and run.emu_pid and Path(f"/proc/{run.emu_pid}").exists():
            run.shot("last-state")
        if run.mh_start and not a.no_mangohud:
            run.mangohud(False)
        if run.dbg and run.emu_pid and Path(f"/proc/{run.emu_pid}").exists():
            run.flash_stop()
    finally:
        signal.signal(signal.SIGINT, signal.SIG_IGN)
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        sd = run.shutdown()
        signal.signal(signal.SIGINT, old_int)
        changed = [p for p, m in live_fingerprint().items() if fp_before.get(p) != m]
        run.info["live_files_changed"] = changed[:20]
        if changed:
            run.say(f"WARNING: live emulator files changed during the run (another instance?): {changed[:5]}")
        if a.svc_trace and (run.dir / "eden_log.txt").exists():
            rc = gdbkit.svc_trace(a.kit, run.dir / "eden_log.txt", run.dir / "threads.txt", run.dir / "svc-trace.md")
            run.info["svc_trace"] = {"window": a.svc_trace, "parse_exit": rc,
                                     "threads": (run.dir / "threads.txt").exists()}
            run.say(f"svc-trace.md written (parser exit {rc}"
                    + ("" if (run.dir / "threads.txt").exists() else "; no thread names: run with --gdb for them") + ")")
        run.write_run_json(result, failed, sd)
    s = analyze.analyze(run.dir)
    print((run.dir / "summary.md").read_text())
    print(f"run dir: {run.dir}")
    return 0 if result == "ok" else 2


if __name__ == "__main__":
    sys.exit(main())
