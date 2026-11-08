# OoT co-op C3 field plan

C3 wires frame draining, Bool64 observation/baseline export and class/key-gated setter
injection. It is an **unverified field candidate**, with production grant policy empty
by default. The tests validate control flow and protocol behavior, not game/save
correctness. The harness must be run by the external lane when the game is idle.

Both instances must use title `0400486024DEC000`, main build
`E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348`. Preserve backups and use disposable saves.
Record emulator commit, game build, seed/mods and the reviewed key policy. Use one
permanent Bool64 key initially, false on both copies, and no scalar or pouch grants.

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

From the worktree root, set these four variables to the lane's actual local files:
`OOT_EMULATOR` (native Linux lichii executable), `OOT_GAME` (game file/directory),
`OOT_PROFILE` (portable user template), and `OOT_KEYS` (private reviewed JSON array of
nonzero numeric u64 keys). Then run this exact command:

```sh
python3 -I tools/oot/coop/two_instances.py \
  --emulator "${OOT_EMULATOR:?set native lichii path}" \
  --game "${OOT_GAME:?set game path}" \
  --profile "${OOT_PROFILE:?set disposable portable user template}" \
  --keys "${OOT_KEYS:?set reviewed key JSON path}" \
  --allow bit \
  --run-dir "$HOME/Downloads/OOT/harness-runs/coop-C3-$(date +%Y%m%d-%H%M%S)"
```

The harness starts the loopback relay and two separate portable instances, sets
`EDEN_NX2_OOT_COOP=1`, `...ALLOW=bit`, `...KEYS=<copied file>`, matching room/team/seed
and a digest of the reviewed keys as policy ID. It enables both GameData and ROMFS
traces. Omit `--allow bit` for the empty-class control. `--seed` defaults to `0`.
The harness was not run in the development lane.

Logs: `relay.log`, `A.console.log`, `B.console.log`, and each
`A/user/log/eden_log.txt`, `B/user/log/eden_log.txt`. `settings.json` records inputs.
Re-run the checker on closed-session logs if needed:

```sh
python3 -I tools/oot/coop/check_logs.py \
  "$RUN/A/user/log/eden_log.txt" "$RUN/B/user/log/eden_log.txt"
```

## 3. Off/trace controls and the frame site

With co-op unset, no co-op connection/application lines should appear; trace can run
independently. With co-op enabled but `EDEN_NX2_OOT_COOP_ALLOW` empty, no grants may
change guest state. `ALLOW` accepts exact comma-separated `bool`/`bit` tokens and is
empty by default. `KEYS` also defaults empty. Only reviewed `bit` keys have a C3
production adapter. `ALLOW_CLASSES` from C2 no longer enables a class.

Confirm `gamedata trace on`, `gamedata frame`, and `gamedata bit-read`/`bit-set` on the
exact build. Existing step-set, step-apply and played-event diagnostics must remain
correct. The new reader/setter hooks only learn the manager; the observer reads
committed bytes at frame boundaries. A setter request by itself must never emit a grant.

The owner must establish that main+0x11E7A50 is a safe loaded-save boundary on the
selected guest thread and that the manager/table/queue remain valid. Validate absence
of a transition or active event at injection. C3 detects changed object/table identity,
unreadable state and observed flag rollbacks; this is not a complete save lifecycle
oracle, particularly for same-address reloads. Do not enable a real-save policy based
only on the unit tests or on two unchanged pointers.

## 4. First field pass

Wait until **both** logs show `relay connected`, `baseline exported` and
`baseline exchanged`. Confirm the chosen key is false on B. Collect it normally on A.
The logs must show, for the same class/key:

1. A: `observed class=bit key=<key> value=1 path=observer` exactly once.
2. B: `started ... path=setter echo=1-bit-<key>` and then `returned` with that ID.
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

Finally save B, close both games so the harness checker runs on clean logs, then
relaunch B alone with co-op unset and verify the intended key persisted and unrelated
flags stayed unchanged. Do not reload inside the harness session: a title-screen or
earlier-save reload can read the key false again, which the observer treats as a
rollback and quarantines, failing the checker after an otherwise good pass. The checker cannot prove persistence, event safety or actor
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
