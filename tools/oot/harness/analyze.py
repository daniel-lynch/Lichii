"""Summarise one harness run directory (stdlib only; safe to re-run):

    python3 -I tools/oot/harness/analyze.py ~/Downloads/OOT/harness-runs/<stamp>

Inputs (written by run.py): run.json (phase windows in wall-clock seconds and the log/MangoHud
clock offsets), mangohud.csv (MangoHud/mangoapp, 100 ms rows), eden_log.txt (per-frame
"NX2 deferred writes frame=..." lines), pcie.txt (nvidia-smi dmon -s tu -o DT).
Outputs: summary.json, summary.md.
"""
import csv
import datetime as dt
import json
import re
import statistics
import sys
from pathlib import Path

LOG_TS = re.compile(r"^\[\s*(\d+\.\d+)\]")
KV = re.compile(r"(?<![\w)])(\w+)=(\d+)")
SUM_KEYS = ["collected_writes", "collected_bytes", "pool_writes", "pool_bytes", "applied_ranges",
            "applied_bytes", "applied_pool_bytes", "native_dispatches", "native_marked_bytes",
            "fence_copies", "fence_copy_bytes", "fence_copy_pool_bytes", "sync_download_calls",
            "invalidations"]


def pct(xs, p):
    if not xs:
        return None
    xs = sorted(xs)
    k = (len(xs) - 1) * p / 100.0
    lo, hi = int(k), min(int(k) + 1, len(xs) - 1)
    return xs[lo] + (xs[hi] - xs[lo]) * (k - lo)


def r2(x, n=1):
    return None if x is None else round(x, n)


# ------------------------------------------------------------------ readers
def read_mangohud(path):
    rows = []
    if not path.exists():
        return rows
    with open(path, newline="") as f:
        lines = f.read().splitlines()
    try:
        hdr = next(i for i, l in enumerate(lines) if l.startswith("fps,"))
    except StopIteration:
        return rows
    for rec in csv.DictReader(lines[hdr:]):
        try:
            rows.append((float(rec["elapsed"]) / 1e9, float(rec["fps"]), float(rec["frametime"])))
        except (ValueError, KeyError, TypeError):
            pass
    return rows


def read_log(path):
    frames, presents = [], []
    if not path.exists():
        return frames, presents
    with open(path, errors="replace") as f:
        for ln in f:
            m = LOG_TS.match(ln)
            if not m:
                continue
            if "NX2 deferred writes frame=" in ln:
                kv = {k: int(v) for k, v in KV.findall(ln)}
                frames.append((float(m.group(1)), kv))
            elif "last 600 presents: frame" in ln:
                mm = re.search(r"frame (\d+) us \(max (\d+)", ln)
                if mm:
                    presents.append((float(m.group(1)), int(mm.group(1)), int(mm.group(2))))
    return frames, presents


def read_pcie(path):
    out = []
    if not path.exists():
        return out
    for ln in path.read_text(errors="replace").splitlines():
        if ln.startswith("#") or not ln.strip():
            continue
        p = ln.split()
        try:
            t = dt.datetime.strptime(p[0] + " " + p[1], "%Y%m%d %H:%M:%S").timestamp()
            # columns: date time gpu rxpci txpci sm mem ...
            out.append((t, float(p[3]), float(p[4]), float(p[5])))
        except (ValueError, IndexError):
            pass
    return out


# ------------------------------------------------------------------ window stats
def _in(t, wins):
    return any(a <= t < b for a, b in wins)


def mh_stats(rows, wins):
    w = [r for r in rows if _in(r[0], wins)]
    if not w:
        return {}
    fps = [r[1] for r in w]
    ft = [r[2] for r in w]
    return {"fps_median": r2(statistics.median(fps)), "fps_mean": r2(statistics.fmean(fps)),
            "fps_p5": r2(pct(fps, 5)), "fps_p95": r2(pct(fps, 95)),
            "frametime_p95_ms": r2(pct(ft, 95), 2), "frametime_p99_ms": r2(pct(ft, 99), 2),
            "samples": len(w)}


def log_stats(frames, wins):
    n, span, dts = 0, 0.0, []
    sums = dict.fromkeys(SUM_KEYS, 0)
    for a, b in wins:
        w = [f for f in frames if a <= f[0] < b]
        if len(w) < 2:
            continue
        ts = [f[0] for f in w]
        dts += [(y - x) * 1000 for x, y in zip(ts, ts[1:])]
        n += len(w) - 1
        span += ts[-1] - ts[0]
        for f in w:
            for k in SUM_KEYS:
                sums[k] += f[1].get(k, 0)
    if not n:
        return {}
    return {"guest_frames": n, "guest_fps": r2(n / span if span > 0 else None),
            "guest_frame_ms_p95": r2(pct(dts, 95), 2), "guest_frame_ms_p99": r2(pct(dts, 99), 2),
            "native_dispatches": sums["native_dispatches"],
            "native_marked_KB": r2(sums["native_marked_bytes"] / 1024),
            "fence_copies": sums["fence_copies"],
            "fence_copy_MB": r2(sums["fence_copy_bytes"] / 2**20, 2),
            "pool_collected_writes": sums["pool_writes"],
            "pool_collected_KB": r2(sums["pool_bytes"] / 1024),
            "pool_applied_KB": r2(sums["applied_pool_bytes"] / 1024),
            "collected_KB": r2(sums["collected_bytes"] / 1024),
            "applied_KB": r2(sums["applied_bytes"] / 1024),
            "sync_download_calls": sums["sync_download_calls"],
            "per_frame": {"native_dispatches": r2(sums["native_dispatches"] / n),
                          "native_marked_KB": r2(sums["native_marked_bytes"] / 1024 / n),
                          "fence_copy_MB": r2(sums["fence_copy_bytes"] / 2**20 / n, 3),
                          "pool_collected_KB": r2(sums["pool_bytes"] / 1024 / n),
                          "pool_applied_KB": r2(sums["applied_pool_bytes"] / 1024 / n)}}


def pcie_stats(rows, wins):
    w = [r for r in rows if _in(r[0], wins)]
    if not w:
        return {}
    rx, tx, sm = [r[1] for r in w], [r[2] for r in w], [r[3] for r in w]
    return {"rx_MBps_median": r2(statistics.median(rx)), "rx_MBps_p95": r2(pct(rx, 95)),
            "tx_MBps_median": r2(statistics.median(tx)), "tx_MBps_p95": r2(pct(tx, 95)),
            "gpu_sm_pct_median": r2(statistics.median(sm))}


def window(run, mh, frames, pcie, wins):
    """wins: [(wall_start, wall_end), ...] -> stats over their union."""
    out = {"seconds": r2(sum(b - a for a, b in wins)), "visits": len(wins)}
    if run.get("mangohud_start_wall") is not None:
        o = run["mangohud_start_wall"]
        out["mangohud"] = mh_stats(mh, [(a - o, b - o) for a, b in wins])
    if run.get("log_wall_offset") is not None:
        o = run["log_wall_offset"]
        out["log"] = log_stats(frames, [(a - o, b - o) for a, b in wins])
    out["pcie"] = pcie_stats(pcie, wins)
    return out


def flash_sum(windows):
    """Flash-monitor window counters (flash_stats "window") summed over visits; None if never measured."""
    ws = [w for w in windows if w]
    if not ws:
        return None
    out = {k: sum(w.get(k) or 0 for w in ws)
           for k in ("frames", "judged", "duplicates", "gaps", "flashes", "flicker_frames", "flicker_tiles")}
    out["max_jump"] = r2(max(w.get("max_jump") or 0 for w in ws), 1)
    out["max_tile_jump"] = r2(max(w.get("max_tile_jump") or 0 for w in ws), 1)
    flashes = [w["last_flash_frame"] for w in ws if w.get("last_flash_frame") is not None]
    out["last_flash_frame"] = max(flashes) if flashes else None
    return out


def analyze(rundir):
    rundir = Path(rundir)
    run = json.loads((rundir / "run.json").read_text())
    mh = read_mangohud(rundir / "mangohud.csv")
    frames, presents = read_log(rundir / "eden_log.txt")
    pcie = read_pcie(rundir / "pcie.txt")
    summary = {"stamp": run.get("stamp"), "binary": run.get("binary"), "env": run.get("env", {}),
               "result": run.get("result"), "failed_step": run.get("failed_step"),
               "shutdown": run.get("shutdown"), "steps": run.get("steps", []),
               "phases": {}, "headings": [], "heading_visits": [],
               "gdb": run.get("gdb"), "svc_trace": run.get("svc_trace"), "flash": run.get("flash")}
    groups = {}
    for ph in run.get("phases", []):
        if ph.get("kind") == "heading":
            groups.setdefault(ph["name"], []).append(ph)
        else:
            st = summary["phases"][ph["name"]] = window(run, mh, frames, pcie, [(ph["start"], ph["end"])])
            st["flash"] = flash_sum([ph.get("flash")])
    for name in sorted(groups):
        st = window(run, mh, frames, pcie, [(p["start"], p["end"]) for p in groups[name]])
        st["name"] = name
        st["screenshots"] = [p.get("screenshot") for p in groups[name]]
        st["flash"] = flash_sum([p.get("flash") for p in groups[name]])
        summary["headings"].append(st)
        for p in groups[name]:
            v = window(run, mh, frames, pcie, [(p["start"], p["end"])])
            v["name"] = f"{name}/c{p.get('cycle', 0)}"
            v["flash"] = flash_sum([p.get("flash")])
            summary["heading_visits"].append(v)
    total = _g(run, "flash", "final", "total")
    if total and "in-game" in summary["phases"]:
        # in-game = everything since flash_monitor started, camera turns included
        summary["phases"]["in-game"]["flash"] = flash_sum([total])
    if presents:
        summary["vi2_present_windows"] = [{"log_t": t, "frame_us": a, "max_us": b} for t, a, b in presents[-20:]]
    (rundir / "summary.json").write_text(json.dumps(summary, indent=2))
    # host contention: MangoHud's system-wide CPU load (the emulator alone is ~27-30 % on a 24-thread
    # box; builds in flight push it far higher and invalidate the fps numbers) + the preflight record
    hl = run.get("host_load_start") or {}
    host = {"load1_start": hl.get("load1"), "build_procs_start": hl.get("build_procs")}
    try:
        mrow = next(iter(csv.DictReader(open(rundir / "mangohud_summary.csv"))))
        host["cpu_load_avg"] = float(mrow.get("CPU Load", "nan"))
        host["cpu_load_peak"] = float(mrow.get("Peak CPU Load", "nan"))
    except (OSError, StopIteration, ValueError):
        pass
    host["tainted"] = bool((host.get("cpu_load_avg") or 0) > 40 or (host.get("build_procs_start") or 0) > 0)
    summary["host"] = host
    (rundir / "summary.md").write_text(render_md(summary))
    return summary


def _g(d, *ks):
    for k in ks:
        if not isinstance(d, dict):
            return None
        d = d.get(k)
    return d


def _fmt(v):
    return "-" if v is None else str(v)


def render_md(s):
    L = [f"# OOT harness run {s['stamp']}", "",
         f"- result: **{s['result']}**" + (f" (failed at `{s['failed_step']}`)" if s.get("failed_step") else ""),
         f"- binary: `{s['binary']}`",
         f"- env: " + (", ".join(f"`{k}={v}`" for k, v in sorted(s["env"].items())) or "-"),
         f"- shutdown: {s.get('shutdown')}",
         "- host: " + (lambda h: (f"system CPU load avg {h.get('cpu_load_avg')} % / peak {h.get('cpu_load_peak')} %, "
                                 f"load1 {h.get('load1_start')} with {h.get('build_procs_start')} build procs at start"
                                 + (" — **TAINTED by host contention; do not compare fps**" if h.get('tainted') else "")))(s.get("host") or {}),
         ""]
    cols = ["window", "s", "fps med", "fps p5", "fps p95", "ft p95 ms", "ft p99 ms", "guest fps",
            "disp/f", "marked KB/f", "fence MB/f", "pool coll KB/f", "pool appl KB/f", "rx MB/s p95", "sm %",
            "flashes", "flicker"]
    L += ["| " + " | ".join(cols) + " |", "|" + "---|" * len(cols)]

    def row(name, st):
        vals = [name, st.get("seconds"), _g(st, "mangohud", "fps_median"), _g(st, "mangohud", "fps_p5"),
                _g(st, "mangohud", "fps_p95"), _g(st, "mangohud", "frametime_p95_ms"),
                _g(st, "mangohud", "frametime_p99_ms"), _g(st, "log", "guest_fps"),
                _g(st, "log", "per_frame", "native_dispatches"), _g(st, "log", "per_frame", "native_marked_KB"),
                _g(st, "log", "per_frame", "fence_copy_MB"), _g(st, "log", "per_frame", "pool_collected_KB"),
                _g(st, "log", "per_frame", "pool_applied_KB"), _g(st, "pcie", "rx_MBps_p95"),
                _g(st, "pcie", "gpu_sm_pct_median"), _g(st, "flash", "flashes"), _g(st, "flash", "flicker_frames")]
        L.append("| " + " | ".join(_fmt(v) for v in vals) + " |")

    for name, st in s["phases"].items():
        row(name, st)
    for h in s["headings"]:
        row(f"heading {h['name']} (x{h['visits']})", h)
    if len(s.get("heading_visits", [])) > len(s["headings"]):
        for v in s["heading_visits"]:
            row("  visit " + v["name"], v)
    L += ["", "headings: q0 = the view after the walk, q1..q3 = successive ~90-degree right-stick turns; "
          "screenshots/heading<q>-c<cycle>.png show each view.",
          "fps = MangoHud (host presents, 100 ms rows); guest fps = emulator log frame lines; /f = per guest "
          "frame (totals in summary.json).", ""]
    if s.get("flash"):
        L += render_flash(s["flash"])
    if s.get("steps"):
        L += ["## Steps", ""] + [f"- {st['t']:7.1f}s {st['step']}: {st['status']}" + (f" — {st['note']}" if st.get("note") else "")
                                  for st in s["steps"]]
    if s.get("gdb"):
        sys.path.insert(0, str(Path(__file__).resolve().parent))  # -I drops the script dir
        import gdbkit
        L += gdbkit.render_md(s["gdb"], s["phases"])
    if s.get("svc_trace"):
        t = s["svc_trace"]
        L += ["", "## SVC trace (`--svc-trace`)", "",
              f"- window presents {t.get('window')}; `svc-trace.md` (parser exit {t.get('parse_exit')}"
              + (", thread names from the stub)" if t.get("threads") else ", no thread names: add --gdb)")]
    return "\n".join(L) + "\n"


def render_flash(f):
    fin = f.get("final") or {}
    t = fin.get("total") or {}
    L = ["## Flash / flicker monitor", ""]
    if not fin:
        return L + [f"- not measured: {f.get('error') or 'no final flash_stats'}", ""]
    L += [f"- flashes = frames whose centre-half mean luma (0-255) is >= {fin.get('flash_jump')} above BOTH "
          f"neighbouring frames; flicker = non-flash frames where at least one of the 8x6 tiles' mean or "
          f"deviation spikes >= {fin.get('tile_jump')} over both neighbours (a one-frame spike or dip). "
          "Probe: every present downsampled to 64x36 (nearest) on the GPU; exact repeats of the previous "
          "present are skipped, so a guest frame shown twice still counts as one frame.",
          f"- in-game row = everything since the monitor started, camera turns included; heading/phase rows = "
          f"the still windows only.",
          f"- totals: {t.get('flashes')} flashes, {t.get('flicker_frames')} flicker frames "
          f"({t.get('flicker_tiles')} tile spikes) over {t.get('frames')} probed presents "
          f"({t.get('judged')} judged, {t.get('duplicates')} repeats, {t.get('gaps')} gaps, "
          f"{fin.get('dropped_samples')} dropped); max centre jump {r2(t.get('max_jump'))}, "
          f"max tile jump {r2(t.get('max_tile_jump'))}, last flash at probe frame {t.get('last_flash_frame')}",
          f"- cost per present: CPU {r2(fin.get('cpu_mean_ms'), 3)} ms mean / {r2(fin.get('cpu_max_ms'), 3)} max, "
          f"GPU blit+copy {r2(fin.get('gpu_mean_ms'), 3)} ms mean / {r2(fin.get('gpu_max_ms'), 3)} max"]
    cap = fin.get("first_flash_capture")
    if cap:
        L.append(f"- first-flash frame dump: `{cap.get('directory')}` (flash at probe frame {cap.get('trigger_frame')}, "
                 f"captured frame {cap.get('capture_frame')}, complete={cap.get('complete')}"
                 + (f", error {cap['error']}" if cap.get("error") else "") + "); the dump stalls the window it lands in")
    elif f.get("capture_first"):
        L.append("- first-flash frame dump: none (no flash detected)")
    return L + [""]


if __name__ == "__main__":
    s = analyze(sys.argv[1])
    print(Path(sys.argv[1], "summary.md").read_text())
