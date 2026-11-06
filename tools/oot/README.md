# Ocarina of Time analysis tools

Stdlib-only Python and Ghidra headless scripts used to analyse the Switch 2 Ocarina of Time
container, the emulator's frame dumps, and the game executable. Run Python with `python3 -I`
and pass paths as arguments; treat extracted game data as untrusted and keep it out of this repo.

- `container/`: NUP container parser (`nup_tool.py` info / tree / extract), game `.zs` with
  dictionaries (`unzs.py`), `sarc.py`, the BNSH/NVN shader-container parser (`bnsh.py`), an LZ4
  NSO decompressor (`nso.py`), and the needle/match/scan scripts that located the emulator's
  dumped shaders in the romfs (`build_index.py` writes `shader-index.json`: dumped program key →
  romfs path, offset, size, stage). `NOTES.md` has every command.
- `framedump/`: readers for `EDEN_NX2_FRAME_DUMP` output — per-pass and per-draw summaries, buffer
  show/compare/stats, compute-output-to-draw matching, HDR previews.
- `ghidra/`: `nso2elf.py` (NSO → ELF for the LZ4 modules; `main` uses the Switch 2 ZBIC
  compression, which the Adubbz Switch loader extension decodes), the headless scripts
  (`OotAnchors`, `NvnTable` — recovers and labels the runtime-resolved NVN function-pointer table,
  `NvnTypes`, `OotProbe`), and `NOTES.md` with the exact `analyzeHeadless` invocations and caps.
  The Ghidra project itself and any findings quoting game code are not published.
- `harness/`: unattended boot-to-game runs with MangoHud fps, guest fps, PCIe and screenshots per
  camera heading, and per-window `flashes` / `flicker` counts from the emulator's flash monitor
  (debug server `flash_monitor` / `flash_stats`, docs/nx2/debug-mcp.md); `harness/README.md`.
- `run-logged.sh`: the play-session launcher (shader dump, MangoHud frame-time CSV, PCIe sampler,
  per-game GPU-mode pin, opt-in frame capture and reject-hash workaround).
