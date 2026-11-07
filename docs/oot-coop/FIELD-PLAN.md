# OoT co-op field plan

This revision is **transport + diagnostics + an offline application state machine**.
It cannot synchronize live saves. Steps 1–3 below can test this revision; steps 4–7
require the explicitly listed runtime adapter work. A successful connection is not
playable co-op and an `ack` is not a saved grant.

Use title `0400486024DEC000`, main build
`E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348` on both instances. Preserve untouched backups,
make two separate disposable copies of the same starting save, and use isolated emulator
user directories. Do not share an actively written user/save directory. Record emulator
commit, game build, mods/seed, policy identity and save generation for every run. Owner
launches/stops games; never automate a restart of their playing session.

The relay deployment is another lane's job. It needs reachable **TCP 43390**, protocol
v2 (4-byte big-endian length + UTF-8 JSON, payload at most 1 MiB), from both clients and
return traffic. No UDP, discovery or inbound game-client ports are needed. Use loopback
for two instances on one host, then Daniel and Anthony's site-to-site VPN with an
explicit private relay address. No public exposure, credentials or NAT traversal are
required by this prototype.

## 1. External lane transport gate

Outside the socket-restricted development sandbox, run:

```sh
python3 -I tools/oot/coop/test_relay.py
```

Expected: **34 tests pass** (21 socket-free, 13 TCP). The suite exercises split and
coalesced frames, malformed framing, title/seed/policy refusal, team isolation,
reconnect snapshots, OR deduplication, presence and cleanup. Do not substitute the
21 pure cases for this gate. Two `client_sim.py` processes can then exchange synthetic
flags using the README's commands; never inject a guessed synthetic hash into a game.
Pass: one accepted key per team, correct snapshot/replay, no duplicate event on resend.
Fail: unexpected re-broadcast, unsupported scalar accepted, peer mismatch accepted,
or a framing/timeout exception.

## 2. Two instances on one host: off and observation controls

For the off control, leave all `EDEN_NX2_OOT_COOP*` variables unset. Trace independently:

```sh
EDEN_NX2_GAMEDATA_TRACE=1
EDEN_NX2_ROMFS_TRACE=1
```

Set environment variables in the owner's existing launch method; these snippets are
settings, not instructions for this lane to launch the game. Use `Core_ARM:Info` and
`Loader:Info` logging (or `*:Info`). For instance A then B:

```sh
EDEN_NX2_OOT_COOP=127.0.0.1:43390
EDEN_NX2_OOT_COOP_ROOM=field-C2-local-1
EDEN_NX2_OOT_COOP_TEAM=together
EDEN_NX2_OOT_COOP_SEED=0
EDEN_NX2_OOT_COOP_POLICY=or-v2
EDEN_NX2_OOT_COOP_NAME=Daniel
EDEN_NX2_OOT_COOP_ALLOW_CLASSES=
EDEN_NX2_GAMEDATA_TRACE=1
EDEN_NX2_ROMFS_TRACE=1
```

On B change only `EDEN_NX2_OOT_COOP_NAME=Anthony`. Use the same private deny-list
file contents if `EDEN_NX2_OOT_COOP_DENY_LIST` is set. There is no automatic seed or
policy digest; vanilla is seed `0`. `EDEN_NX2_OOT_COOP_PLAYED_EVENTS` no longer enables
grants. No environment variable enables the production guest writer in this revision.

Expected current log substrings:

- `OoT co-op: connected; guest application disabled` after handshake + initial state.
- `OoT co-op: observation only; setter request is not a grant receipt` once a shared
  trace hook executes. No grants are emitted by these hooks.
- `OoT co-op: not applied: grant class disabled` on a received candidate with empty
  allow list. For a deliberate transport-only test using `...ALLOW_CLASSES=bit`,
  the same candidate instead reaches `OoT co-op: not applied: no site (guest writes disabled)`.
- `OoT co-op: handshake refused or mismatched` for deliberate seed/policy mismatch;
  a mismatching title/build is rejected earlier with `OoT co-op off: endpoint/title/build gate`.
- Queue failure diagnostics: `OoT co-op: pending queue full; reconnect for a fresh snapshot`,
  `OoT co-op: outgoing queue full; observation skipped`, or `OoT co-op: relay rejected a packet`.
  Any such line fails a normal smoke test.

Pass: co-op off is silent, trace still works, both matching clients connect, deliberate
mismatch is refused, no guest save state changes due to incoming traffic. Fail: an
incoming grant claims applied, or a mere setter request is sent as a successful grant.

## 3. Validate read-only trace evidence

Perform one ordinary forward quest transition and one event on disposable save A;
observe B without repeating the action. With `EDEN_NX2_GAMEDATA_TRACE=1`, expect
`gamedata trace on: main build ...`, `gamedata step-set ...`,
`gamedata step-apply ... forwards` (or `same`/`backwards` if that is what the game requests),
and `gamedata played-event name=... hash=...`. StepApply is an entry trace, so independently
read the record after the update and confirm the actual resulting index/hash. For a
natural rollback, verify the trace reports `backwards`; never force rollback to test it.
`EDEN_NX2_ROMFS_TRACE=1` reports `romfs gamedata`, `romfs banc` and relevant
`romfs evfl`/`romfs timeline` loads; caching means a file need not log every action.
Trace logs are bounded per session; absence after the cap is not evidence of no mutation.

Pass: exact-build original instruction checks and observed registers match HOOKS.md,
trace-only and trace+co-op produce the same natural progression. Fail: missing valid
fields, unexplained `?`, wrong manager/record, or trace Resume executes twice.
There is no trace-installed frame callback in this base. Do not treat StepSet,
StepApply or PlayedEvent as a safe once-per-frame drain.

## 4. Runtime adapter acceptance gate — future field build

Implement the production adapter described in HOOKS.md before attempting this step.
Keep key policy empty until a reviewed permanent check is chosen from the private data.
Set `EDEN_NX2_OOT_COOP_ALLOW_CLASSES=bit`, a shared policy identity for the reviewed
key list, and retain both traces. A class setting alone must still fail closed until
all code verification gates and the private key allow list are satisfied.

The owner should use the live debugger to validate `main+0x11E7A50` on the main guest
thread after save load. Establish that no save transition/event is active, manager
and Bool64 table remain valid, the key exists and is false, queues have headroom, and
private stack/sentinel memory belong to this process. First exercise a no-mutation
sentinel round trip: every GPR, SIMD register, SP/PC/flags/FP state/TLS and exclusive
monitor policy must agree after restore; the original instruction executes once.

The adapter must add logs for start, sentinel return, authoritative commit, timeout,
quarantine and reset with opaque key, guest thread, generation and sequence identifiers.
**Those application logs do not exist in this revision**; the offline engine returns
`ApplyEvent::{Started,Returned,Committed,Recovered,Quarantined}` for that future wiring.
Do not report those event names as observed runtime log lines today.

Pass: complete restoration and continued gameplay, no borrowed-stack corruption and no
writes during a scene transition. Fail: wrong-thread sentinel, generation change,
unmatched SP, callback reentry, or a restore that only covers callee-saved registers.
On timeout do not rewind arbitrary running code or replay the grant. Quarantine; let
owner recover the disposable session. A setter could still hold a lock or have queued
its write before failing.

## 5. One allowlisted check, local instances — future field build

A collects the chosen permanent check. After the successful local mutation, A emits
one `bit_set`. B must re-read its value, invoke `main+0x3761720` only if false, restore
on its sentinel, then wait for the authoritative byte to become true. Persist/reload
B's disposable save and verify it remains true. Repeat B→A with another reviewed key.

Pass: one accepted grant, at most one necessary call, true only for the intended key,
no echo from B even if the setter's delayed consumer runs after sentinel return, no
unrelated flags changed. Existing actors may need scene reload; a stale chest/wall
requires actor-notification research, not blind rewrites. GameData trace should continue
logging natural transitions/events, but its three hooks **do not log generic Bool64
writes**, so absence of a trace line is expected and cannot validate this step alone.

## 6. Reconciliation and failures — future field build

Start with A and B having different pre-existing permanent checks, export both complete
allowlisted baselines, then use the protocol's FIFO state-request barriers. Pass only
after both save readers confirm the union. Restart/reconnect and deliberately resend
the same event. Collect the same physical check simultaneously on both machines: one
OR key survives; no extra item/count may be created. Additive items and small keys are
unsupported and must be rejected, not counted as successful tests.

Disconnect B, collect on A, reconnect; then repeat past 512 events using simulator-only
synthetic keys. Restart the memory-only relay only as a separate disposable round;
baselines must rebuild its state. Save rollback/new save/scene transition while a grant
is pending must invalidate the active generation and stop injection. An injected fault
or missing sentinel must quarantine without automatic retry. The current prototype has
no baseline extractor or durable receipts; these gates cannot pass on this revision.

## 7. Daniel + Anthony over VPN

Repeat steps 2–6 with the two separate machines. Change only the relay endpoint to the
private VPN-reachable address and use a fresh room (e.g. `field-C2-vpn-1`). Keep build,
seed, reviewed key policy, disposable save lineage and settings identical. Exercise
latency and a short disconnect, compare both logs and post-reload save differences.
Pass: the same convergence, echo and recovery criteria as localhost; no real-time player
movement, dummy actors, combat, sounds or teleportation is expected. Report those as
outside this protocol, not as missing successful flag synchronization.

Keep save dumps and game-derived traces private. Hand back build/settings, relevant
log lines and the specific permitted before/after flag evidence; do not post upstream.
