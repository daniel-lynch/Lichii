The first-draft co-op design, relay, simulator and emulator skeleton are in the working tree. **Guest application remains disabled; this is not playable co-op.**

Files added:

- `docs/nx2/oot-coop.md`: protocol, conflict rules, lifecycle, hook design, application strategies and explicit TODOs.
- `tools/oot/coop/relay.py`, `client_sim.py`, `test_relay.py`, `README.md`: stdlib relay, simulated client, tests and LAN instructions.
- `src/core/arm/dynarmic/oot_coop_sync.h` and `.cpp`: codecs, conflict state, gating, deny-list, echo guard, asynchronous client, reconnect, pending queue and disabled writers.
- `src/tests/core/oot_coop_sync.cpp`: 12 Catch2 test cases.
- `CODEX-REPORT.md`: this report, left untracked.

Files changed:

- `arm_dynarmic_64.cpp/.h`: share the existing trace breakpoints with co-op and resume the substituted instruction once.
- `src/core/loader/nso.cpp` and `src/core/core.cpp`: build-gated registration and transport cleanup at session boundaries/shutdown.
- `src/core/CMakeLists.txt` and `src/tests/CMakeLists.txt`: register the new sources.
- `dist/nx2_switches.txt`: commented-out co-op settings and disabled-application explanation.

The relay implements team routing, build/seed refusal, monotonic flags and quest indices, additive pouch deduplication, checkpoint snapshots plus 512-event replay, presence, heartbeats, idle cleanup and bounded queues. Snapshot pouch totals are absolute; sender high-water marks are installed after reconstruction. The emulator observes resolved forward StepSet requests and optional PlayedEvent flags, with the existing trace still usable independently or simultaneously.

Validation:

| Command/check | Result |
|---|---|
| `python3 -I tools/oot/coop/test_relay.py` | **28 tests: 13 passed, 15 setup errors, 0 assertion failures, 0 skipped.** |
| `python3 -I tools/oot/coop/test_relay.py ProtocolTests` | **13 passed.** Covers pure rules plus dispatcher routing, refusals, snapshots and queue limits. |
| `git diff --check` | Passed. |
| C++ compilation and Catch2 | **Not run. No builds or compiler invocations were performed.** |

The full relay suite does **not** pass in this environment: the sandbox rejects loopback socket creation with `PermissionError: [Errno 1] Operation not permitted`, so all 15 real TCP tests fail before their assertions. Detailed output is in `.tmp/oot-coop-relay-tests.log`. The TCP tests remain intact; socket-free checks do not substitute for network validation. C++ received static review only and compilation remains unverified.

Remaining stubs and their activation requirements:

| Stub | Required before enabling |
|---|---|
| GameData bool/int/enum/Bool64bitKey and pouch send hooks | Verified per-build offsets, original instructions, argument/return conventions, manager lifetime, key namespace and successful-mutation detection. Every new site remains zero. |
| Callable quest setter | Verify the actual callable entry and ABI. The existing StepSet observation is an interior instruction, and observing a request does not prove acceptance. |
| Guest-call injection | Verified safe guest thread/hook, full CPU context save/restore, x0–x7 arguments, valid sentinel LR, scheduling/fault recovery and suppression lasting until the guest returns. Implement the stub and deliberately change the compile-time verification gate only after validation. |
| Direct storage writes | Verified typed storage/pouch layouts, key lookup, lifetime, synchronization, notifications, dirty flags and cache behavior. Implement the writer; addresses alone do not enable it. |
| Safe application and echo bookkeeping | Read actual guest values before applying; identify save-loaded/transition states; release the queue lock before guest execution; persist application receipts and update local observations only after success. The current scope covers synchronous calls only. |
| Snapshot/save reconciliation | Define the save baseline, durable relay epochs, grant receipts and restart/rollback policy. Never pass snapshot `pouch_total` to an additive setter. |
| Small-key counts | Establish dungeon identity, count layout, acquisition/use hooks and simultaneous-use conflict rules. Generic scalar arrival order is insufficient. |
| Seed/transient policy | Load a verified seed digest automatically and agree on policy identity. Supply private runtime deny-list files; no game-specific exclusions are compiled in. |

Both writer methods return an unapplied status. Zero sites and `GuestWritesVerified=false` independently block access; the methods contain no guest-write or execution-redirection implementation. Incoming candidates log `not applied: no site` once per session.

Principal risks are unverified hook layouts and setter intents; incomplete snapshots of pre-existing saves; duplicate grants when two players collect the same check; lost observations after a long offline period fills the outbox; and divergence from unsuitable scalar rules or mismatched deny lists. PlayedEvent key semantics still need runtime verification and remain opt-in. The relay is memory-only and unauthenticated, suitable only for a trusted LAN with an explicit bind address.

Next steps, in order:

1. Run the unchanged full relay suite where loopback TCP is permitted, then check two simulated LAN clients.
2. When builds are permitted, compile the C++ and run the 12 new Catch2 cases plus existing trace tests.
3. Verify read-only StepSet/PlayedEvent observations, successful-transition detection and a safe guest-thread drain site.
4. Locate and validate setter/pouch entries and ABIs; settle save baselines, grant receipts, transient policy and small-key semantics.
5. Implement one guest-call path with sentinel restoration, recovery and persistent echo suppression; validate with disposable saves before expanding coverage.

All writes stayed inside this worktree. No emulator, game, harness or build was launched. No commits, pushes or upstream changes were made.