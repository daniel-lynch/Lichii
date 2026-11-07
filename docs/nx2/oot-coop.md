# Ocarina of Time co-op: first draft

This is a protocol and emulator skeleton, **not playable co-op**. The relay and simulated
clients implement the wire protocol. The emulator can observe requests, connect, retry,
receive snapshots and drain a pending queue, but **cannot apply a network change to the
guest**. All new setter sites are zero and `GuestWritesVerified` is a compile-time false.
There is no switch that enables guest writes.

Scope: title `0400486024DEC000`, main build
`E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348`. The constants come from
[`OotGameDataTrace`](../../src/core/arm/dynarmic/oot_gamedata_trace.h).
Other titles/builds never create a co-op session. All hashes below are opaque integers;
there are no shipped name lists, actor tables, seeds, extracted binaries or game assets.
Examples use synthetic numbers.

## Model and differences from Anchor

The brief is sections 2 and 5 of the owner's private randomizer research. Reference code
read for the protocol model: Shipwright `soh/soh/Network/Network.cpp`,
`Anchor/{Anchor.h,Anchor.cpp,HookHandlers.cpp,Packets/*.cpp}`, and Anchor's
`server.go`, `client.go`, `room.go`, `team.go`. This is an independent implementation;
no Anchor implementation code is copied.

Retain one TCP connection per client, rooms containing teams, team snapshots plus a
bounded 512-message replay queue, heartbeats, and a queue drained from guest execution.
Anchor's incoming-packet guard motivates echo suppression. We intentionally make the
relay enforce progression conflicts and build/seed equality. It does not interpret
hashes or contain game-specific rules.

Unlike Anchor's client-supplied whole-save snapshot, this draft's server snapshot is
only the aggregate of **observed changes**. It is not a serialized save. TODO: verified
save baseline/export and reconciliation before promising existing-save synchronization.
Unlike Anchor's bounded queue that can discard history, this relay folds evicted events
into its checkpoint first, preserving pouch totals and old flags.

## Transport and handshake

UTF-8, one compact JSON object followed by LF per message; CRLF is accepted. Maximum
frame size is 1 MiB including LF, with at most 16 nested JSON containers. JSON strings escape embedded newlines. TCP reads may
split a frame or combine several; both implementations buffer through LF. NDJSON fits
the existing `nx2_debug_server` Boost.Asio transport and ordinary command-line tools,
and is easier to inspect than Anchor's NUL delimiter. This is not wire-compatible with
Anchor. Protocol version 1 rejects incompatible versions instead of guessing.

First packet:

```json
{"type":"hello","version":1,"build_id":"E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348","seed_hash":"0","room":"demo","team":"together","client_id":"session-a","name":"Sam"}
```

The first client fixes the room's build ID and seed hash. All clients in that room,
including other teams, must match exactly. Build IDs are 40 uppercase hex characters;
seed hashes are opaque strings of 1–128 hex characters, with `"0"` meaning vanilla.
Comparison is case-sensitive. TODO: choose a canonical digest of the randomizer's
actual seed/settings manifest and load it automatically; the draft uses explicit
`EDEN_NX2_OOT_COOP_SEED` configuration and cannot verify the installed mod.

Room, team and client IDs use 1–64 ASCII letters, digits, `_`, `.` or `-`. Names use
1–96 UTF-8 bytes with no control characters; use first names. The client generates a
new random ID per game session and retains it across TCP retries. A duplicate *live*
ID within a team is refused. An offline ID can reconnect with its existing sequence
history. An ID is an identifier, not an authenticated credential.

The server replies with `hello_ack`, echoing `version`, `build_id`, `seed_hash` and
`client_id`, then immediately sends `team_state`. The emulator checks all four fields
before accepting changes, and waits for that snapshot before sending its unacknowledged
outbox. Refusal is `{"type":"error","reason":"..."}` followed by disconnection.
No room/team reassignment is allowed on an established connection.

## Messages

All change packets carry `origin` and `seq`. `origin` must equal the connection's
handshake ID. Sequence numbers are strictly increasing **across all change kinds**, in
`1..2^53-1`. Hashes use unsigned32 except `actor` (unsigned64); JSON consumers must
preserve unsigned64 integers exactly. Generic `value` for int/enum is signed32.

| Type | Payload beyond `type`, `origin`, `seq` | Meaning |
|---|---|---|
| `bool_set` | `hash`, `value` (JSON bool) | Set a GameData boolean. Only true propagates. |
| `int_set` | `hash`, `value` | Generic GameData int; provisional relay-arrival order. |
| `enum_set` | `hash`, `value` | Generic GameData enum; provisional relay-arrival order. |
| `quest_step` | `challenge`, `step`, `index` (0–255) | Quest transition; step hashes are never compared for ordering. |
| `bit_set` | `actor`, `value` (JSON bool) | Bool64bitKey entry keyed by a 64-bit actor hash; only true propagates. |
| `pouch_add` | `hash`, `count` (1..2^31-1) | Add items keyed by actor-name hash; no removal in this draft. |

For example:

```json
{"type":"quest_step","origin":"session-a","seq":1,"challenge":123,"step":456,"index":3}
{"type":"bit_set","origin":"session-a","seq":2,"actor":18446744073709551615,"value":true}
{"type":"pouch_add","origin":"session-a","seq":3,"hash":789,"count":2}
```

Accepted events receive a server `rev` (1..2^53-1). They go only to other clients of
the same team, never the sender or another team/room. Clients cannot select another
target. The sender receives `{"type":"ack","seq":3}` even if a valid event was a
no-op, denied, backward, false, or an already-seen retransmission. An ACK means
**relay processing**, not successful application to any guest.

Other messages:

| Type | Fields / direction |
|---|---|
| `team_state_request` | Client→server, no other fields; request the current checkpoint plus replay. |
| `team_state` | Server→client: `revision`, `changes`, `seen`, `replay`; detailed below. Client-authored snapshots are rejected. |
| `client_state` | Client→server: `state` object, ≤4 KiB including envelope. Server→teammates adds `origin`. Cached while connected; sent on join/update/disconnect. |
| `heartbeat` | Either direction, no other fields. Receiver replies with `heartbeat_ack`. |
| `heartbeat_ack` | Liveness response; never replied to, avoiding ping-pong. |
| `hello_ack`, `ack`, `error` | Control messages described above. |

The C++ client announces `state: {"apply_enabled":false}`. TODO: discover save-loaded,
scene/age and safe-to-apply state before publishing them; do not infer them from a
network connection. Player locations and puppet actors are out of this first draft.

## Conflict rules and replay

* **Bools: monotonic OR. Never send or apply a false.** False packets are acknowledged
  and consumed as no-ops by the relay. A change must be an edge, not a periodic resend.
* **Quest steps: forward only by index.** The game's setter can move a quest backward.
  The relay retains the highest index; an equal/lower index is ignored, even if its hash
  differs. Before a future guest call, compare against the *actual local quest index*
  again. Network shadow state cannot establish what a pre-existing local save contains.
  TODO: confirm every observed quest table's ordering, including special/substeps;
  never order by hash or guessed step names.
* **Bool64bitKey: OR per actor hash.** No clear/kill reversal.
* **Pouch adds: additive, deduplicated by `(origin, seq)`.** Two different clients'
  additions are distinct even if the actor hash matches. A sender high-water mark is
  sufficient because its changes arrive in strictly increasing order over one TCP
  stream. A late lower sequence is ignored, not treated as a reordered delta. Send
  retained unacknowledged events in order after reconnect. Totals are bounded by
  `2^53-1`; overflow is an error, not wraparound.
* **Ints/enums:** last accepted arrival at the relay wins, independently per typed hash.
  This is a provisional transport policy, not a safe general progression merge. Quests
  must use `quest_step`. TODO: classify which non-quest scalar values may actually sync.
* **Small-key counts: TODO, deliberately unsupported by guest hooks.** Need verified
  per-dungeon identity/count layout, key-acquisition and key-use hooks, and a conflict
  decision for simultaneous use. Anchor syncs both count updates and key use. Merely
  adding grants or passing counts through generic scalar LWW is insufficient.

Transient flags stay local. Load a **user-supplied JSON array of numeric hashes** through
`EDEN_NX2_OOT_COOP_DENY_LIST` and relay `--deny-list`. It covers hashes for bool/int/enum,
quest challenge, actor bits and pouch actor names. No exclusions or game names are
compiled in. Missing/invalid configured files fail closed. With no configured file the
list is empty; this is suitable for transport experiments, not an assertion that every
flag is safe. TODO: privately identify transient mechanisms (water-level switches,
elevators, timers and alternate-mode state), and supply matching lists on all peers.
TODO: negotiate a policy digest before enabling writes so differing exclusions cannot
silently diverge. Existing trace names can still come from the user's names file; names
are not transmitted as game-data identifiers.

Each team maintains current aggregate state, an older checkpoint, a deque of at most
512 accepted events, and current per-sender high-water marks. When the deque overflows,
fold its oldest entry into the checkpoint *before* removal. Denied/no-op changes advance
sender high-water marks but do not consume a revision or replay slot.

A snapshot is one atomic event-loop operation:

```json
{"type":"team_state","revision":12,"changes":[{"type":"bool_set","hash":123,"value":true},{"type":"pouch_total","hash":789,"count":4}],"seen":{"session-a":20},"replay":[{"type":"pouch_add","origin":"session-a","seq":20,"hash":789,"count":2,"rev":13}]}
```

Here the reconstructed pouch total is **6**, not 4, 2 or 10. `changes` contains canonical
state entries without sender/sequence metadata. `pouch_total` is snapshot-only, absolute
accumulated grants; it is **not a delta and not current inventory**. `revision` names the
checkpoint boundary. Replay revisions are contiguous from `revision+1`. `seen` is
current, including the replay and ignored requests: reconstruct checkpoint + replay
first, then adopt the high-water marks. Using `seen` to discard that replay loses data.
The C++ receiver reconstructs a fresh protocol state and queues its canonical values;
repeated totals merge by maximum, never by addition. A queued checkpoint marker installs
the snapshot high-water marks after those values. Live pouch deltas use sender dedup.

The relay cannot reconstruct changes that preceded the first connection. TODO: baseline
ownership, export/import, applied-grant receipts tied to a save, and safe treatment of
consumable inventory. On a future client, install a snapshot and replay atomically
relative to live incoming events, with save-relative receipts preventing a second grant.
Do not inject `pouch_total` into a pouch-add function.

## Relay lifecycle and limits

Run [`relay.py`](../../tools/oot/coop/relay.py) with an explicit bind address; default is
`127.0.0.1`, port `43390`. The server sends heartbeats every 30 seconds, disconnects
clients without incoming traffic for 90 seconds, and removes empty rooms after 300
seconds. Outgoing traffic does not refresh incoming liveness. Tests use short intervals.
Room deletion deletes teams, checkpoints, queues and dedup history together.

The relay is stdlib-only `asyncio`, with one reader and writer task per client and one
maintenance task. There is no await between accepting a change and taking its snapshot
state. Each client has a bounded outbox; a slow receiver is disconnected without
stalling teammates. State and identities are bounded, with explicit errors instead of
silent eviction. Exact limits and simulator commands are in
[the relay README](../../tools/oot/coop/README.md).

State is in memory only. TODO: durable epochs/checkpoints and client save receipts
before safely recovering additive state across relay restart, room expiry, emulator
restart or save rollback. A generated session ID prevents accidental sequence reuse;
it cannot prevent the same saved item from being rediscovered under a new identity.
Different players collecting the same world check concurrently can still generate two
distinct pouch additions. TODO: verify acquisition ordering and, if needed, introduce
an atomic check/grant identity rather than pretending sender dedup solves that race.

LAN-only security: no authentication, TLS, authorization or trust in claimed build/seed.
Anyone who can reach the port can join a known room and submit changes. Bind address
is explicit; `--bind 0.0.0.0` deliberately exposes all IPv4 interfaces. Do not publish
this service to the Internet. TODO: authentication/encryption/rate limits if scope ever
extends beyond a trusted LAN. Limits are robustness measures, not a security boundary.

## Emulator observation and session lifetime

[`oot_coop_sync.h/.cpp`](../../src/core/arm/dynarmic/oot_coop_sync.h) separates pure wire
codecs, protocol conflict state, endpoint/build gating, deny-list parsing and echo
suppression from Boost.Asio I/O. The `nso.cpp` main-load hook registers a process-owned
session only with a configured `EDEN_NX2_OOT_COOP=<host:port>` and matching title/build.
`BeginSession` and shutdown stop/join the old transport and close its socket, even if
JIT callbacks still hold a shared session. No network thread accesses guest memory.

The client asynchronously resolves/connects on its own thread, validates the handshake,
waits for the automatic snapshot, sends bounded outgoing changes, and parses incoming
changes into a mutex-protected pending queue. Network failures retry after two seconds;
handshake/connect timeout is ten seconds; active inbound timeout is 90 seconds.
Sequence allocation and observed-value edge detection are under the same lock. Up to
4096 unacknowledged events survive connection retries in memory and are resent in order.
A full outbox logs and drops further observations; a full pending queue disconnects
and retries for a fresh snapshot. TODO: save-state rescan/backpressure recovery for
outbox overflow; the draft cannot promise lossless long offline play.

The existing original-instruction-checked BRK substitutions remain the only hook sites:

| Existing site | Current draft use |
|---|---|
| StepSet, `main+0x2CF54A4` | Read challenge/step hashes from x1/x2; find requested/current indices through the same bounded manager/challenge layouts already used by the trace. Enqueue only an identifiable forward request. |
| StepApply, `main+0x2CF1830` | Preserve tracing; opportunity to drain pending network observations. |
| PlayedEvent, `main+0x2BDC4E0` | With `_PLAYED_EVENTS=1`, read the full bounded, NUL-terminated event string and hash `HavePlayedEvent.` plus the name into a true boolean observation. |

Co-op and trace can run separately or together. At a breakpoint, co-op observes and
drains, trace logs if enabled, and the original instruction is emulated **exactly once**.
No guest code bytes are patched. StepSet's substituted instruction still sets w8 to 1;
the other two substitutions retain the trace's no-op handling of `pacibsp`.

TODO: verify register layouts and table indices at runtime. StepSet is a setter *request*,
not a successful-write receipt: add return/post-commit observation or correlate with
StepApply before treating rejected requests as real progress. TODO: verify whether
PlayedEvent uses the full dotted name or a namespace/subkey in actual GameData storage;
keep its observation opt-in until confirmed. Skipped/malformed reads produce no guessed
hash or index. Hash names are not a substitute for verified indices.

New `Site` entries all have `offset=0` and expected instruction zero (disabled):

| Needed site | TODO: discovery and verification |
|---|---|
| GameData bool setter | Follow existing played-event and expression-evaluation call paths to the typed setter; verify manager, key/namespace, bool encoding, success result and side effects. |
| GameData int setter | Follow typed scalar writes and identify signedness, notifications and manager lifetime. |
| GameData enum setter | Distinguish ordinary enum storage from quest transitions and verify accepted values. |
| Bool64bitKey setter | Trace a privately observed actor-bit update, verify full 64-bit key and relevant namespace/table. |
| Pouch add | Follow the pouch-add event executor to the actual mutation; verify actor-name/hash conversion, count, capacity and successful-add result. |
| Callable quest entry | The observed StepSet site is an interior instruction at +4, not an approved call target. Verify the entry, ABI, manager and rejection behavior. |

Use the existing read-only logs/debug memory tools and private disassembly to identify
candidates, then measure before/after state in a later explicitly authorized run. TODO:
record exact per-build entry offsets, original instruction signatures, argument and
return conventions. Do not transplant addresses from another build or ship extracted
tables. No new addresses are guessed in this draft.

## Applying incoming changes: two disabled strategies

`Session::Drain` runs only from the guest breakpoint callback, at most 64 pending
changes per hit, serialized by the session mutex. It merges a **protocol shadow**, checks
against locally observed progress and calls `GuestWriter`. A shadow merge is not a
successful guest apply. The writer returns `NotAppliedNoSite`; one log per session says
`not applied: no site (guest writes disabled)`. It does not mark a save modified.
Snapshot pouch totals have no callable setter and remain unapplied.

TODO: identify a frequent, safe hook on the actual game/update thread, identify that
guest thread across scheduling/migration, and check save-loaded/scene-transition state.
These three hooks can run during load and are not a verified once-per-frame safe point.
A bounded queue can drain slowly or not at all while no hook executes. TODO: reread
current guest bool/step/bit values immediately before application; observed state cannot
protect against loads, resets, unobserved writes or stale setter intents. TODO: release
the queue lock before any future guest execution and serialize a resumable apply job;
the current lock is safe only because both writers return synchronously without writes.

**A. Guest-call injection (preferred).** `GuestWriter::InjectCall` is a declared and
compiled stub. The intended operation is:

1. At the verified safe site, save the entire interrupted CPU context: all GPRs, SP,
   PC, status flags, SIMD/FP state and any emulator execution state required for a
   transparent resume. TODO: identify the exact Dynarmic/kernel context API and ownership.
2. Resolve the live manager/object, marshal verified arguments into x0..x7, and provide
   ABI-aligned stack/scratch storage when necessary. TODO: setter ABI, object lifetime,
   stack use, pointer-authentication behavior and scheduling/exclusive-monitor handling.
3. Set LR to a verified sentinel breakpoint and jump to the game's setter entry.
   TODO: choose a noncolliding valid sentinel mapping/substitution, recognize it before
   ordinary breakpoints, and ensure it cannot be supplied by an untrusted packet.
4. Keep a per-guest-thread incoming-application token active through nested setters and
   across guest scheduling until the sentinel returns. Restore the complete context,
   resume after exactly one original hook instruction, and record success only after
   verifying the setter's result. TODO: cancellation/fault/timeout recovery and rollback.

This is preferred because the game's own setter can perform cache updates, notifications,
save dirty tracking, validation and item side effects. Those behaviors would be skipped
by raw writes. It still requires verified timing and ABI; calling a correct address at
an unsafe point is not sufficient.

**B. Direct storage write (fallback).** `GuestWriter::WriteStorage` is also a compiled
stub. TODO: typed GameData table and pouch layouts, key lookup, allocation/lifetime,
value size/alignment, guest memory validity, locking, dirty flags, observers and cache
invalidation. Use only where all side effects are understood; merely seeing a changed
byte in a memory dump does not establish a safe setter replacement.

TODO: after a verified successful apply, update the local observation shadow and durable
application receipt together; network receipt alone must never advance either.

Both strategies are blocked by zero sites and then by `GuestWritesVerified=false`.
Filling an address alone still returns `NotAppliedUnverified`; neither strategy contains
actual write/redirect code. Enabling requires implementing the strategy, verified sites,
thread/safe-point gating, current-state reads, successful-call receipts and tests, then
a deliberate reviewed change to the compile-time gate. There is no runtime opt-in.

Echo suppression uses an exception-safe, nestable `IncomingScope` checked before every
send. It is sufficient for a synchronous apply and unit-testable without I/O. TODO:
a guest-call injection spans emulator callbacks, so replace/extend the current host
thread-local scope with the persistent guest-thread token above before implementing A.
Do not clear suppression when merely scheduling a guest call. Deferred item rewards
need causal receipts as well; immediate hook suppression does not cover later callbacks.

## Validation and next work

Run `python3 -I tools/oot/coop/test_relay.py` for real TCP routing, replay, conflicts,
refusals and liveness tests, plus pure state/protocol checks. Tests start an in-process
relay on `127.0.0.1` with an ephemeral port; they never launch a game. In a sandbox that
forbids loopback sockets, pure tests can run as
`python3 -I tools/oot/coop/test_relay.py ProtocolTests`; the full suite intentionally
reports setup errors rather than fake passes. See the root report for this run's results.

Catch2 source covers codecs, bounds, OR rules, quest ordering, pouch dedup beyond 512
events, deny lists, scalar conflicts, echo suppression, gating and disabled application.
**C++ was not compiled and Catch2 was not run:** the owner forbids builds while their
game is running. TODO: compile and run these tests later when builds are permitted.

Order of work: validate real TCP tests in an allowed environment; compile/review C++
when permitted; verify read-only observations and safe-thread identity; discover typed
setters/ABIs; settle save baselines, receipts, policy digest and small-key rules; implement
one verified guest-call path with recovery; test two disposable saves before extending
to more setters. Direct writes and player puppets come later, if needed.
