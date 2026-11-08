# OoT co-op protocol v2

This document supersedes the wire and conflict sections of `docs/nx2/oot-coop.md`.
The relay and simulator implement this protocol. Emulator guest application is still
unverified and disabled by default; C4 has opt-in chest Bool and actor Bool64 frame
adapters. Chest flags mark locations checked; they grant no items. Bool64 covers pickups, breakables and enemies.
A transport acknowledgement never means that a save changed.

## Anchor source comparison

This is an independent protocol inspired by the locally inspected Anchor implementation,
not an Anchor-compatible client. Source references below use `Ship` for
`~/dev/research/shipwright/soh/soh/Network/Anchor/` and `Relay` for
`~/dev/research/anchor/`. The research summary is
`~/Downloads/OOT/randomizer/RESEARCH.md`, section 2. No Anchor game tables or names are
imported. The C4 chest adapter ships only the reviewed member/backing-Bool hashes from this game's schema (see HOOKS.md).

Anchor frames JSON with NUL (`Relay/server.go:328`, `Relay/client.go:42`), limits packets
to 8 MiB, and retains 512 queued team events (`Relay/team.go:10`). It forwards requests
for a whole save to an online teammate or serves its cached save and event queue
(`Relay/client.go`, `REQUEST_TEAM_STATE`/`UPDATE_TEAM_STATE` branches). Its client queues
network input and drains on `OnGameFrameUpdate` (`Ship/HookHandlers.cpp:101`). Its direct
in-process access to the original game's save/actors is unavailable in this emulator.

No Anchor packet is adopted **as-is on the wire**. The table distinguishes reused
semantics from omitted capabilities:

| Anchor packet or lifecycle | v2 mapping and status |
|---|---|
| `HANDSHAKE` | `hello`/`hello_ack`: retain room/team/client identity, require exact build, seed and policy identity. Server no longer trusts client-supplied routing targets. |
| `SET_FLAG` | `bool_set` or `bit_set`, depending on the verified storage key namespace. Retain monotonic true flag semantics; no hardcoded game deny-list. `Ship/Packets/SetFlag.cpp`. |
| `UNSET_FLAG` | Drop. False observations are acknowledged no-ops; no bit clearing. |
| `GIVE_ITEM` | Drop. `Ship/Packets/GiveItem.cpp` invokes item grant routines with side effects, including non-idempotent rewards. A proven permanent ownership bit can eventually use `bool_set`; that does not imply an inventory item was granted. |
| `UPDATE_DUNGEON_ITEMS`, `UPDATE_BEANS_COUNT` | Drop counts and inventory replacement. Maps/ownership bits can enter a future verified flag policy. Key consumption and consumables are not OR operations. |
| `SET_CHECK_STATUS` | No separate tracker packet. A verified permanent check-completion bit is an ordinary flag; source check identity must be established before item rewards are supported. `Ship/Packets/SetCheckStatus.cpp` updates the tracker, not an atomic grant ledger. |
| `ENTRANCE_DISCOVERED`, `GAME_COMPLETE` | No separate messages. Only a verified permanent flag may be shared by ordinary grants. |
| `REQUEST_TEAM_STATE`, `UPDATE_TEAM_STATE` | `team_state_request`/`team_state`: authoritative relay union of flags plus checkpoint/replay, never a whole guest save. Clients cannot replace relay state. |
| `UPDATE_CLIENT_STATE`, `ALL_CLIENT_STATE` | `client_state` to other team members and cached state on join. Retain online/name/save-loaded/scene/room presence concepts. Presence has no guest-write authority. |
| `UPDATE_ROOM_STATE` | Drop mutable owner settings. Room build, seed and policy are fixed by first join. No mid-session team migration. |
| `PLAYER_UPDATE` | Drop position, joints, equipment and animation. `Ship/Packets/PlayerUpdate.cpp` targets same-scene peers; v2 is save progress only. |
| Dummy player creation/update/destruction | No packet or guest actor injection. `Ship/Anchor.cpp:199` refreshes remote actors; `Ship/DummyPlayer.cpp:115` gates updates on scene, online and save-loaded state. All of this needs a separate verified actor integration. |
| `PLAYER_SFX`, `OCARINA_SFX`, `DAMAGE_PLAYER`, `REQUEST_TELEPORT`, `TELEPORT_TO` | Drop audio, PvP and teleportation. |
| `HEARTBEAT` | `heartbeat`/`heartbeat_ack`, same liveness purpose, new schema. |
| `DISABLE_ANCHOR`, disconnect, unload | No remote disable command. Socket closure broadcasts `client_state` with `online:false`. Local session teardown cancels transport and pending applications; no remote actors exist to remove. Anchor's `Disable()` clears clients and refreshes actors (`Ship/Anchor.cpp:23`). |

## Framing and handshake

Use one TCP stream per client, default port **43390**, over a trusted LAN or the
site-to-site VPN. No UDP, discovery, TLS or authentication is implemented. Relay
provisioning is a separate task. The relay defaults to loopback; LAN binding is explicit.

Each frame is a **4-byte unsigned big-endian payload length**, followed by exactly that
many UTF-8 JSON bytes. Length is **1..1,048,576 inclusive**, excluding the prefix.
There is no NUL or newline terminator. Receivers assemble split headers/payloads and
process concatenated frames in order. A zero/oversized length is rejected before body
allocation. EOF inside a frame is malformed; EOF between frames is normal disconnect.
JSON is chosen over msgpack to keep the relay stdlib-only and manual inspection easy.
Length prefixing supplies bounded allocation and unambiguous framing without scanning
for delimiters. Protocol v1 newline clients are intentionally incompatible.

Every payload is a typed object. Duplicate keys, non-finite numbers, invalid UTF-8,
more than 16 nested containers, unknown packet types and unknown grant/hello fields
are rejected. JSON integer decoders must preserve unsigned 64-bit actor keys exactly;
JavaScript `Number` is unsuitable. Sequence and revision numbers are limited to 2^53−1.

The first payload must be:

```json
{"type":"hello","version":2,"build_id":"<build-id>","seed_hash":"0","room":"field-round-1","team":"together","client_id":"session-a","name":"Daniel","policy_id":"or-v2"}
```

`build_id` is 40 uppercase hex characters. `seed_hash` is 1..128 hex characters,
compared exactly (including case); `0` denotes vanilla. Room, team, client and policy
IDs are 1..64 ASCII letters, digits, `.`, `_` or `-`. Name is 1..96 UTF-8 bytes with
no ASCII control characters. Every team in a room uses the same build/seed/policy.
A duplicate connected client ID in a team is refused. A changed policy needs a new room.

`policy_id` identifies the reviewed allow/deny configuration shared by testers; use a
shared policy name or digest. Default `or-v2` is only the conservative prototype policy,
not proof that two private configuration files match. The relay checks the supplied
identity, not their content. The C4 one-host harness derives a digest of its typed
bool/bit key policy; manually configured clients must still supply a consistent policy identity.

Success yields `hello_ack` with `version`, `build_id`, `seed_hash`, `policy_id`, `client_id`,
then `team_state`, then each connected peer's cached `client_state`. Failure yields
`{"type":"error","reason":"..."}` and disconnect. Reasons include `build id mismatch`,
`seed hash mismatch`, `policy id mismatch` and `protocol version mismatch`.

## Grants, receipts and presence

Supported client grant schemas, using synthetic keys:

```json
{"type":"bool_set","origin":"session-a","seq":1,"hash":123,"value":true}
{"type":"bit_set","origin":"session-a","seq":2,"actor":18446744073709551615,"value":true}
```

`hash` is an unsigned 32-bit opaque GameData key. `actor` is an unsigned 64-bit opaque
Bool64bitKey, not a mask; `value` is a JSON boolean. The two namespaces are distinct.
`seq` is positive, increases across both classes and must match the authenticated-by-
connection `origin` (there is no cryptographic authentication). Relay grant acceptance
is an OR: each `(type,key)` enters team state only once. False, duplicate and denied
observations do not change state. Every well-formed observation advances that sender's
high-water mark, even if ignored. Retries at or below it receive an ack but no event.
Gaps are permitted, so clients must replay unacknowledged packets in ascending order.

Each grant receives `{"type":"ack","seq":1}`; a newly accepted grant is sent to other
team members with its monotonic team `rev`. The sender gets no echo. Ack means relay
processing only, not durable storage or successful guest application. A receiver reads
its actual save before applying, keeps an application receipt/echo guard across deferred
guest execution, and records success only after verified completion. Protocol retries
must not bypass the local class/key allow list or build/lifecycle guards.

Wallet deltas (class `rupee`, C6) are an additive event, not state:

```json
{"type":"rupee_add","origin":"session-a","seq":3,"hash":<hash>,"delta":5}
```

`hash` must be the wallet key `PlayerStatus.CurrentRupee` (<hash>); `delta` is a non-zero
integer in [-999, 999]. The relay forwards and acks it like a grant and keeps it in the replay
window (so revisions stay contiguous), but never folds it into team state; clients ignore
`rupee_add` in a `team_state` snapshot or replay, so a reconnect never re-applies one (and a
late joiner does not receive earlier deltas). A receiver with `rupee` in
`EDEN_NX2_OOT_COOP_ALLOW` applies it by calling the game's wallet entry from the frame drain;
the game's own pouch update clamps it. The local wallet is not observed or re-sent in this pass.

`quest_step`, `int_set`, `enum_set`, `pouch_add`, `pouch_total` are **rejected in v2**.
A quest index maximum is not an OR over flags and an observed setter request is not
proof of successful progress. The existing trace can still observe it locally. Pouch
stock, health, ammo, small-key balances, progressive tiers and spending are not inferred
from ownership bits and cannot be synchronized by scalar arrival order.

Client presence is `{"type":"client_state","state":{...}}`. The relay forwards it as
`{"type":"client_state","origin":"session-a","state":{...}}`, caches it for joins,
and limits the framed packet to 4096 bytes. Conventionally the optional state fields
are `name`, `online`, `save_loaded`, `scene`, `room`, `ready`; absent means unknown.
The object is advisory and extensible; the relay does not validate scene/room semantics
or derive them from the guest. Guest scene/save-loaded discovery remains unimplemented.
A disconnect sends `state:{"online":false}`. Presence is scoped to the team, not the
entire room. `heartbeat` and `heartbeat_ack` have only `type`.

## Pre-existing saves and restart reconciliation

The desired baseline is the **union of verified permanent true flags**, never one
player's whole save replacing another. The wire already represents this without a
special baseline packet. C4 exports the configured reviewed Bool/Bool64 key set at
the frame boundary and requests a FIFO snapshot after upload acknowledgements. The following still requires field
validation before claiming save synchronization:

1. Use backed-up disposable saves, a new room name and identical build/seed/policy.
   Pause progression while each client enumerates the reviewed keys from its loaded save.
2. Export each true allowlisted bool/actor key as an ordinary grant. Do not export false,
   counts, quest indices or generic `IsGet` keys whose side effects are unknown. Feed in
   bounded batches and await acknowledgements so a large baseline cannot fill an outbox.
3. After its last ack, each client sends `{"type":"team_state_request"}`. FIFO processing
   gives a barrier for **that client's prior uploads only**. Wait for both participants
   to finish baseline upload, then request a fresh snapshot on both and reconcile its
   union against current guest values before declaring them ready.
4. A baseline is not atomic and the relay cannot know whether a client exported all
   keys. `ready` is advisory, not a distributed barrier. Do not resume gameplay merely
   because one snapshot arrived. Missing extraction/completion evidence is a failed test.

Snapshot fields are `type:"team_state"`, `revision` (checkpoint revision), `changes`
(canonical bool/bit entries without origin/seq/rev), `replay` (newest 512 accepted events)
and `seen` (current sender high-water marks). Fold checkpoint, then replay in revision
order, **then** adopt `seen`; filtering replay by `seen` first would lose the snapshot.
The relay compacts every evicted event into the checkpoint, so the 512-entry limit does
not drop old flags. Snapshot application is OR and cannot roll back a local true flag.

The relay is memory-only. Restart or 300 seconds with an empty room discards state and
sender history. A new game session uses a new client identity. After a relay restart,
re-export actual save baselines and reconcile again. OR flags are idempotent even if
old grants reappear; a stale queued grant can nevertheless restore a deliberately
rolled-back flag. Starting a fresh room and session is required for an intentional
rollback/new save. No durable epoch, complete-save detector or on-disk receipt database
is claimed. Automatic reconnect alone cannot rebuild missed pre-existing progress.

## Both players collect the same check

If the check is proven to be a shared permanent flag, both observations name the same
`(class,key)`. Only the first creates a revision; both receive acks and the team keeps
one true value. This also handles concurrent baselines and retransmission by a fresh
client identity. `(origin,seq)` alone would not solve the cross-player duplicate.

An item grant without its **source check identity** cannot distinguish two legitimate
copies from two players collecting one check. Therefore v2 sends no additive grant.
Both local games may award their own local item; the network never grants an additional
copy. A future item protocol requires a stable `(seed,source-check)` ID, a canonical
reward mapping, durable team check receipts, per-save applied receipts and an atomic
claim/commit or verified idempotent entitlement operation. It must cover crashes between
item mutation and receipt, save rollback, progressive items and consumed keys. Until
then there is no "exactly once" consumable synchronization claim.

## Bounds and validation commands

Limits: 128 connections, 32 rooms, 16 teams/room, 256 retained sender IDs/team, 2048
flag keys/team, 512 replay entries, 256 queued frames/client and 2 MiB queued bytes/client.
Slow clients disconnect; capacity failures do not silently evict state. Heartbeat is
30 seconds; receive idle timeout is 90 seconds. A private deny-list JSON array of numeric
keys is loaded at relay startup with `--deny-list`; guest allow lists remain independent.

Socket-free suite, run in this sandbox:

```sh
python3 -I tools/oot/coop/test_relay.py ProtocolTests
```

**External lane must run the exact full command** (34 tests: 21 pure, 13 real TCP):

```sh
python3 -I tools/oot/coop/test_relay.py
```

TCP cases cover routing, concurrent check identity, snapshots after compaction, reconnect,
refusal, presence teardown, heartbeat/expiry, pre-existing baseline union, unsafe grant
rejection, malformed JSON/length, split/coalesced frames and deny-list enforcement.
A pure-test pass is not evidence that these real-socket tests passed.
