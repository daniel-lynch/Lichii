#!/usr/bin/env python3
"""Owner-run Linux harness. Copies a portable profile, then launches relay + A/B."""
import argparse
import configparser
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

PRIORITY = Path.home() / "Downloads/OOT/harness-runs/.aot-priority"


def refuse_busy(owned=()):
    result = subprocess.run(["pgrep", "-x", "lichii"], capture_output=True, text=True)
    if result.returncode not in (0, 1):
        raise RuntimeError("Cannot establish whether lichii is running")
    running = {int(pid) for pid in result.stdout.split()} if result.returncode == 0 else set()
    if result.returncode == 0 and not running:
        raise RuntimeError("pgrep returned no process IDs; refusing to launch")
    if PRIORITY.exists() or running.difference(owned):
        raise RuntimeError("Refusing to start: .aot-priority exists or lichii is running")


def prepare_profile(source, destination):
    if any(path.is_symlink() for path in source.rglob("*")):
        raise ValueError("Profile must be a self-contained disposable copy without symlinks")
    shutil.copytree(source, destination)
    # Absolute storage paths in copied INIs must not reach the owner's original save.
    for ini in (destination / "config").rglob("*.ini"):
        config = configparser.ConfigParser(interpolation=None, strict=False, delimiters=("=",))
        config.optionxform = str
        config.read(ini)
        if not config.has_section("Data%20Storage"):
            config.add_section("Data%20Storage")
# Qt encodes spaces in group names; handle existing plain INI profiles too.
        for section in ("Data%20Storage", "Data Storage", "DataStorage"):
            if not config.has_section(section):
                continue
            for key, directory in (("nand_directory", "nand"), ("save_directory", "nand"),
                                   ("sdmc_directory", "sdmc"), ("load_directory", "load"),
                                   ("dump_directory", "dump"), ("tas_directory", "tas")):
                config[section][key] = str(destination / directory)
                config[section][key + "\\default"] = "false"
        if not config.has_section("Miscellaneous"):
            config.add_section("Miscellaneous")
        config["Miscellaneous"]["log_filter"] = "*:Info"
        config["Miscellaneous"]["log_filter\\default"] = "false"
        with ini.open("w") as stream:
            config.write(stream, space_around_delimiters=False)
    log_dir = destination / "log"
    if log_dir.exists():
        shutil.rmtree(log_dir)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", type=Path, required=True, help="native Linux lichii binary")
    parser.add_argument("--game", type=Path, required=True)
    parser.add_argument("--profile", type=Path, required=True,
                        help="self-contained portable user/ template with disposable saves in nand/")
    parser.add_argument("--keys", type=Path, required=True, help="reviewed numeric Bool64 JSON array")
    parser.add_argument("--run-dir", type=Path, required=True, help="new directory, never reused")
    parser.add_argument("--allow", default="", choices=("", "bit"))
    parser.add_argument("--seed", default="0")
    parser.add_argument("--port", type=int, default=43390)
    args = parser.parse_args()
    if sys.platform != "linux":
        parser.error("This harness uses Linux cwd/user portable profiles")
    refuse_busy()
    emulator, game, profile, keys_path, run = (
        value.expanduser().resolve() for value in
        (args.emulator, args.game, args.profile, args.keys, args.run_dir))
    if not emulator.is_file() or not os.access(emulator, os.X_OK) or not game.exists():
        parser.error("Missing executable or game")
    if not (profile / "nand").is_dir() or not (profile / "config").is_dir():
        parser.error("Profile must contain config/ and nand/ with the disposable save")
    if run == profile or profile in run.parents:
        parser.error("Run directory must be outside the source profile")
    keys = json.loads(keys_path.read_text())
    if not isinstance(keys, list) or not 1 <= len(keys) <= 4096 or any(
            type(key) is not int or not 0 < key < (1 << 64) for key in keys):
        parser.error("Keys must be 1..4096 nonzero u64 integers")
    if not 1 <= args.port <= 65535:
        parser.error("Invalid port")
    policy = "bit-" + hashlib.sha256(json.dumps(sorted(set(keys))).encode()).hexdigest()[:32]
    run.mkdir(parents=True, exist_ok=False)
    (run / "keys.json").write_text(json.dumps(keys) + "\n")
    for name in ("A", "B"):
        prepare_profile(profile, run / name / "user")
    (run / "settings.json").write_text(json.dumps({
        "emulator": str(emulator), "game": str(game), "seed": args.seed,
        "policy": policy, "allow": args.allow, "port": args.port,
        "profiles": [str(run / name / "user") for name in ("A", "B")],
    }, indent=2) + "\n")
    refuse_busy()  # Copying a profile can take time; never rely on the first check alone.
    relay_log = (run / "relay.log").open("w")
    relay = subprocess.Popen([sys.executable, "-I", "-u", str(Path(__file__).with_name("relay.py")),
                              "--bind", "127.0.0.1", "--port", str(args.port)],
                             stdout=relay_log, stderr=subprocess.STDOUT)
    children = []
    streams = []
    try:
        # Read readiness from the relay's own log; do not probe by opening another connection.
        for _ in range(100):
            if relay.poll() is not None:
                raise RuntimeError("Relay failed; see relay.log")
            if "relay listening" in (run / "relay.log").read_text():
                break
            time.sleep(0.1)
        else:
            raise RuntimeError("Relay readiness timed out")
        refuse_busy()
        for name in ("A", "B"):
            refuse_busy(child.pid for child in children)
            env = {key: value for key, value in os.environ.items()
                   if not key.startswith("EDEN_NX2_OOT_COOP")}
            env.update({"EDEN_NX2_OOT_COOP": "1",
                        "EDEN_NX2_OOT_COOP_RELAY": f"127.0.0.1:{args.port}",
                        "EDEN_NX2_OOT_COOP_ALLOW": args.allow,
                        "EDEN_NX2_OOT_COOP_KEYS": str(run / "keys.json"),
                        "EDEN_NX2_OOT_COOP_POLICY": policy,
                        "EDEN_NX2_OOT_COOP_SEED": args.seed,
                        "EDEN_NX2_OOT_COOP_ROOM": "field-C3-" + str(os.getpid()),
                        "EDEN_NX2_OOT_COOP_TEAM": "together",
                        "EDEN_NX2_OOT_COOP_NAME": name,
                        "EDEN_NX2_GAMEDATA_TRACE": "1",
                        "EDEN_NX2_ROMFS_TRACE": "1"})
            stream = (run / f"{name}.console.log").open("w")
            streams.append(stream)
            children.append(subprocess.Popen([str(emulator), "-g", str(game)], cwd=run / name,
                                             env=env, stdout=stream, stderr=subprocess.STDOUT,
                                             start_new_session=True))
        print(f"Running A and B in {run}. Close both games normally to finish.", flush=True)
        for child in children:
            child.wait()
    finally:
        # Never stop a game automatically, including on Ctrl-C or a partial launch failure.
        if relay.poll() is None:
            relay.terminate()
            relay.wait(timeout=10)
        relay_log.close()
        for stream in streams:
            stream.close()
    return subprocess.call([sys.executable, "-I", str(Path(__file__).with_name("check_logs.py")),
                            str(run / "A/user/log/eden_log.txt"),
                            str(run / "B/user/log/eden_log.txt")])


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValueError, RuntimeError, OSError) as error:
        raise SystemExit(str(error)) from error
