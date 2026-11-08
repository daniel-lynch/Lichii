# OoT co-op C4 field plan

C4 adds chest Bool polling and manager-cell binding to the frame drain and
class/key-gated deferred setter injection. Bool64 remains for pickups, breakables and enemies. It is an **unverified field candidate**, with production grant policy empty
by default. The tests validate control flow and protocol behavior, not game/save
correctness. The harness must be run by the external lane when the game is idle.

Both instances must use title `0400486024DEC000`, main build
`E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348`. Preserve backups and use disposable saves.
Record emulator commit, game build, seed/mods and the reviewed key policy. Use c1 backing Bool `0xC8643846`, false on both copies, with the 206-key chest policy.
There are no scalar or pouch grants. The C3 failure and its resolution are recorded in
`~/Downloads/OOT/field/coop-C3-20261108-0550/{RESULT,CHEST-STATE}.md`.

## 1. External transport gate

Outside this socket-restricted lane:

```sh
python3 -I tools/oot/coop/test_relay.py
```

All 34 cases must pass (21 pure and 13 TCP). Local unit tests are not a substitute for
this gate. TCP 43390 loopback is sufficient for the one-host test.

## 2. One-host harness

Prepare a self-contained portable `user/` template containing `config/`, keys/firmware
and `nand/` with the disposable starting save. Absolute paths to saves outside that
NAND are not imported. The harness copies this entire template twice (no hardlinks or
symlinks), rewrites storage paths in the copied INIs and captures both emulator logs.
It refuses an existing run directory, a running `lichii` process, or
`~/Downloads/OOT/harness-runs/.aot-priority`. It checks again after copying and before
launches. It never stops a game; close both game windows normally to finish.

From the worktree root, set these three variables to the lane's actual local files:
`OOT_EMULATOR` (native Linux lichii executable), `OOT_GAME` (game file/directory),
`OOT_PROFILE` (portable user template). Use `tools/oot/coop/chest-bools.json` as the
reviewed typed key policy. Then run this exact command:

```sh
python3 -I tools/oot/coop/two_instances.py \
  --emulator "${OOT_EMULATOR:?set native lichii path}" \
  --game "${OOT_GAME:?set game path}" \
  --profile "${OOT_PROFILE:?set disposable portable user template}" \
  --keys tools/oot/coop/chest-bools.json \
  --allow bool --vram-profile \
  --run-dir "$HOME/Downloads/OOT/harness-runs/coop-C4-$(date +%Y%m%d-%H%M%S)"
```

The harness starts the loopback relay and two separate portable instances, sets
`EDEN_NX2_OOT_COOP=1`, `...ALLOW=bool`, `...KEYS=<copied file>`, matching room/team/seed
and a digest of the reviewed keys as policy ID. It enables both GameData and ROMFS
traces. Omit `--allow bool` for the empty-class control. `--seed` defaults to `0`.
`--vram-profile` sets resolution_setup=0 (Res1_4X), vram_usage_mode=0
(Conservative), and use_docked_mode=0 in both copies, including custom game INIs.
The VRAM gate/guard remain the field lane's responsibility; this preset has not been
measured in this lane. The harness was not run in the development lane.

Logs: `relay.log`, `A.console.log`, `B.console.log`, and each
`A/user/log/eden_log.txt`, `B/user/log/eden_log.txt`. `settings.json` records inputs.
Re-run the checker on closed-session logs if needed (use `--allow ""` for the control):

```sh
python3 -I tools/oot/coop/check_logs.py \
  "$RUN/A/user/log/eden_log.txt" "$RUN/B/user/log/eden_log.txt" --allow bool
```

The empty-allow control is judged solely by absence of grant activity. A missing B
log is reported as SKIP, not FAIL; this proves only no grants in the available logs,
not a successful two-instance launch. A missing B log still fails an enabled run.

## 3. Off/trace controls and the frame site

With co-op unset, no co-op connection/application lines should appear; trace can run
independently. With co-op enabled but `EDEN_NX2_OOT_COOP_ALLOW` empty, no grants may
change guest state. `ALLOW` accepts exact comma-separated `bool`/`bit` tokens and is
empty by default. `KEYS` also defaults empty. The `bool` adapter accepts only configured keys from the chest catalog; `bit` retains
its separate configured Bool64 keys. `ALLOW_CLASSES` from C2 no longer enables a class.

Confirm `gamedata trace on`, `gamedata frame`, and `gamedata actor-flag-set` /
`struct-bool-set` when opening c1 on the exact build. Chests must not depend on `bit-*` calls. Existing step-set, step-apply and played-event diagnostics must remain
correct. The manager is bound from the cell at each frame; the observer reads committed
flat Bool bytes at frame boundaries. A setter request by itself must never emit a grant.

The owner must establish that main+0x11E7A50 is a safe loaded-save boundary on the
selected guest thread and that the manager/table/queue remain valid. Validate absence
of a transition or active event at injection. C4 detects changed object/table identity,
unreadable state and observed flag rollbacks; this is not a complete save lifecycle
oracle, particularly for same-address reloads. Do not enable a real-save policy based
only on the unit tests or on two unchanged pointers.

## 4. First field pass

Wait until **both** logs show `relay connected`, `baseline exported` and
`baseline exchanged`. Confirm the chosen key is false on B. Open c1 normally on A.
The logs must show, for the same class/key:

1. A: `observed class=bool key=00000000c8643846 value=1 path=observer` exactly once.
2. B: `started ... path=setter echo=1-bool-00000000c8643846` and then `returned` with that ID.
3. B: `applied ... path=setter` only after the authoritative byte becomes true.
4. B: `suppressed ... path=observer` with the same ID, and no `observed` for that key.
   Suppression can precede the applied line during the same drain.

The checker requires a necessary setter/data write, one applied receipt, positive
suppression evidence and no echo or runtime failure diagnostics. An `already-true`
receipt, relay ack, setter return, or silent log does not count as this field pass.
No queue overflow, quarantine, allocation failure, mismatch or rejected-packet line is
acceptable. Verify full context continuation and no repeated frame instruction,
wrong-thread return or return-stack failure. A setter that stalls before another frame
cannot trip the frame-clock watchdog; use the live debugger to diagnose it, never
rewind arbitrary code that may hold locks.

Check B's flat Bool `0xC8643846` becomes 1. Change B's scene and return to inspect c1;
a loaded chest may need a scene reload since the grant skips actor-side notifications.
**No item is granted. B's wallet must remain unchanged.** Items are separate `IsGet.*`
Bools and `PouchData.*.StockNum` Ints; the pouch grant entry is unknown.

Finally save B, close both games so the harness checker runs on clean logs, then
relaunch B alone with co-op unset and verify the intended key persisted and unrelated
flags stayed unchanged. Do not reload inside the harness session: a title-screen or
earlier-save reload can read the key false again, which the observer treats as a
rollback and quarantines, failing the checker after an otherwise good pass. (A reload that
moves or clears the manager cell logs `baseline re-bound` instead; only a same-address
reload reads as a rollback.) The checker cannot prove persistence, event safety or actor
notification behavior. If an actor needs reload to reflect the flag, record that as a
separate notification limitation. Repeat B→A with another reviewed key.

## 5. Baselines, duplicate delivery and resets

Repeat with different pre-existing reviewed true flags on A/B and confirm both readers
converge to their union after both uploads and snapshot barriers. Duplicate and
simultaneous flags must cause at most one necessary write. Test a short disconnect
without restarting the relay, then a separate round with a fresh relay and restarted
emulators so their baselines are exported again. Mid-session relay-state loss is not
a supported recovery claim: a new process must re-export its actual save baseline.

Exercise a scene transition and disposable save rollback. A changed binding/read
failure/observed true→false must stop injection and retain receipts. Same-address save
reloads need explicit investigation. Do not clear receipts while a setter can still
return. No automatic retry of a timed-out call/commit is allowed.

## 6. Two machines

Only after the local field and persistence gates pass, repeat over the private VPN with
an explicit relay endpoint, matching game/build/seed/policy and independent disposable
profiles. No public relay, real-time actors, movement/combat synchronization, teleport,
quest progression, additive rewards or inventory synchronization is claimed.

Keep save dumps and game-derived traces private. Hand back settings/build IDs, the
relevant log lines and permitted before/after flag evidence.
