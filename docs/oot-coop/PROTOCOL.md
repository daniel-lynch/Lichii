# OoT co-op protocol v3

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

| Anchor packet or lifecycle | v3 mapping and status |
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
| `PLAYER_UPDATE` | `pose` (v3): position, facing, scene (`banc`), age and a loading bit only, best effort, forwarded to the team, never state. Joints, equipment and animation are dropped. Like `Ship/Packets/PlayerUpdate.cpp` it is drawn only for same-scene peers. |
| Dummy player creation/update/destruction | No packet. With class `pose` allowed, each same-scene peer is a "statue" ghost: a Link-shaped puppet actor created through the game's own actor manager and moved only by the actor's own set-transform call (no joints, animation, collision or equipment). It is parked below the map, not destroyed, when the peer leaves the scene or disconnects. `Ship/Anchor.cpp:199` refreshes remote actors; `Ship/DummyPlayer.cpp:115` gates updates on scene, online and save-loaded state. |
| `PLAYER_SFX`, `OCARINA_SFX`, `DAMAGE_PLAYER`, `REQUEST_TELEPORT`, `TELEPORT_TO` | Drop audio, PvP and teleportation. |
| `HEARTBEAT` | `heartbeat`/`heartbeat_ack`, same liveness purpose, new schema. |
| `DISABLE_ANCHOR`, disconnect, unload | No remote disable command. Socket closure broadcasts `client_state` with `online:false`. Local session teardown cancels transport and pending applications; ghost actors are parked below the map. Anchor's `Disable()` clears clients and refreshes actors (`Ship/Anchor.cpp:23`). |

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
for delimiters. Protocol v1 newline clients are intentionally incompatible; v2 clients are
refused at `hello` with `protocol version mismatch` (v3 only adds `pose`).

Every payload is a typed object. Duplicate keys, non-finite numbers, invalid UTF-8,
more than 16 nested containers, unknown packet types and unknown grant/hello/pose fields
are rejected. JSON integer decoders must preserve unsigned 64-bit actor keys exactly;
JavaScript `Number` is unsuitable. Sequence and revision numbers are limited to 2^53−1.

The first payload must be:

```json
{"type":"hello","version":3,"build_id":"<build-id>","seed_hash":"0","room":"field-round-1","team":"together","client_id":"session-a","name":"Daniel","policy_id":"or-v2"}
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

`hash` is the wallet key `PlayerStatus.CurrentRupee` (non-zero; the receiver admits only the
wallet key of its private profile, the relay any non-zero key); `delta` is a non-zero
integer in [-999, 999]. The relay forwards and acks it like a grant and keeps it in the replay
window (so revisions stay contiguous), but never folds it into team state; clients ignore
`rupee_add` in a `team_state` snapshot or replay, so a reconnect never re-applies one (and a
late joiner does not receive earlier deltas). A receiver with `rupee` in
`EDEN_NX2_OOT_COOP_ALLOW` applies it by calling the game's wallet entry from the frame drain;
the game's own pouch update clamps it. C14 observes reviewed signed gameplay calls as described below.

Item gives (class `item`, C7) are an event too: the game's own give-to-pouch call, as the opener's
game made it, replayed on the receiver:

```json
{"type":"item_give","origin":"session-a","seq":4,"name":"<actor name>","category":6,"count":1,"get":7,"flags":[0,0,1,0]}
```

`name` is the item's actor name (1-63 of `[A-Za-z0-9_]`), `category` and `get` the call's pouch
category and get type (0-15), `count` 1-999, `flags` the call's three flag registers and its
stack byte (0-255 each). The opener forwards a call only when its return address is one of the
profile's `item_callers` (a chest's event-flow give); every other give is logged as `seen` and
not sent, and the receiver's own replay (it returns to the co-op sentinel) is never re-sent. Like
`rupee_add` it is deduplicated by origin/sequence, kept in the replay window and never folded into
team state or applied from a snapshot, so a duplicate delivery never gives twice and two
different gives of the same item both add. The chest's opened Bool travels separately as an
ordinary `bool_set`. The receiver replays it from the frame drain only when no event is running and
the profile's guard pointers are live; until then it waits at the head of the queue.

`quest_step`, `int_set`, `enum_set`, `pouch_add`, `pouch_total` are **rejected** (since v2).
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

## Same-chest race (C13)

`item_give` may carry `"origin_key": <chest Bool>` (1..2^32-1). Receivers apply one chest's gives
from one opener only (themselves, or the first claimant); see HOOKS.md `chest_members`. Old relays
reject the field: update the relay before running a C13 client with `chest_members`.

## Collectible transactions and wallet baselines (C14)

`origin` remains the sender ID. `item_give` and `bit_set` additionally accept a strict
source union. All examples below use synthetic identities:

| `origin_kind` | Required fields | Meaning |
|---|---|---|
| omitted or `none` | none | Unlinked event or ordinary flag |
| `chest` | `origin_key` (nonzero u32) | Ordinary Bool chest source; old `origin_key`-only packets remain valid |
| `bit` | `origin_key64` (nonzero u64) | Reviewed collectible instance |
| `token` | `origin_key64`, `origin_partner` (distinct nonzero u64) | Reviewed source and collection pair |

Mixed fields, strings/floats in place of integers, zero and overflowing keys are rejected.
`bit_set` accepts only bit/token sources and its `actor` must match one of that source's
keys. Pair semantics and collectible families come from the receiver's private profile;
the relay knows no game-specific pairing rule. Old relays reject these fields: deploy
the revised relay and use a matching policy ID on both clients before enabling them.

The relay deduplicates all events by sender/sequence. It also keeps a team-lifetime
receipt for each bit/token benefit, independent of sequence or sender and of replay-window
eviction. Flags do not consume benefit receipts. Chest sources retain a winning sender
instead, preserving several legitimate top-level gives from one chest opener. The combined
source receipt/owner limit is 4096; overflow rejects the event before changing team state.
This storage is in memory, not durable across relay restart or room expiration.

The sender captures a reviewed key at the pickup-component entry on the same guest thread,
reserves it before the outer give, and records the benefit after the verified return reports
success. A losing local pickup returns success without running the give. Later Bool64 writes
carry that source. The receiver reserves the source before replay, records success after the
native return, and queues its source flags through the existing deferred flag setter. It
never increments a separate token counter. Pending/applied receipts survive transport
reconnect in the same loaded process; source flags in a baseline remain state, not receipts.
Typed collectible replay is accepted on a same-process reconnect, but first-connection
historical item replay remains disabled without save-integrated receipts. Receipts are
not sidecars, and a save-manager move quarantines world application. Restart/catch-up
exactly-once behavior and resettable drop generations are not implemented.

Simultaneous local winners each retain their own single benefit and reject the incoming
copy; this requires equivalent pickup content in the shared build/seed. It is not a global
pre-grant authority or correction of different speculative payloads. Probe with two clients
before treating this as a verified simultaneous-pickup guarantee.

Private-profile defaults are `world.hearts=false`, `token.enabled=false`,
`world.cleanup="flag_only"`, and `world.wallet_reconcile="off"`. The flag policy admits
reviewed currency and token families; healing/permanent-health families require the heart
switch as well as the existing item policy. Token benefit linkage requires its probe gate.
There is no proved arbitrary resident-actor walk: even `cleanup="death"` logs a refusal and
uses flags only. Loaded visual/collision disappearance still needs a live probe.

Wallet observation accepts signed deltas from the existing reviewed callers plus the private
shop return address. Sequence/initialization and unlisted callers remain excluded. Calls on
the applier's guest thread while applying a remote give or delta are suppressed. A forwarded
outer give remains the only economic event for its nested/inline credit; independent shop
spends and direct rewards use `rupee_add`, replayed with reason 7. The separate inline capacity
reduction path is not observed by this hook; this is not complete wallet-writer coverage.

`hello` and `team_state_request` optionally carry
`"world_wallet":{"balance":20,"capacity":100}`. The same object may appear in
`client_state.state`; `team_state.world_wallet` is a map of connected sender IDs to those
observations. Require integer `0 <= balance <= capacity <= INT32_MAX`, with no extra fields.
The frame lane waits for zero pouch pending currency and two equal committed samples before
export. Baselines are observations, not live authoritative relay balances. On first peer
baseline, log `OoT co-op: wallet baseline ours=.. peer=..`. With `target_current`, only the
higher client ID adjusts to the lower ID's observation, once per process, through an
echo-suppressed signed delta. Unequal capacities and corrections outside the wire limit are
refused. Default `off` only reports divergence. Concurrent transactions near capacity,
inline clipping and durable baseline revisions still require a later ordered-wallet design.

## Small keys, dungeon items and per-player flags (C13)

- `key_add {origin, seq, hash, delta}`: a dungeon's small-key **gain** (`delta` 1..32), `hash` = that
  dungeon's total-keys-got Int key (private key file). An event like `rupee_add`/`item_give`:
  deduplicated by origin/sequence, kept in the relay's replay window, never folded into team state,
  never in a snapshot. Class token `dungeon`.
- **Rule: gains are shared, spending is per-player.** Anchor shares the absolute count as last
  writer wins, on pickup and on use. That converges only if door unlocks are shared too, and it
  loses a key when two players pick up (or one picks up while the other spends) within a round trip.
  Here the door state of a dungeon is a per-actor flag that is not in the reviewed list, so a shared
  spend would leave the other player's door locked with one key fewer: a soft lock. A shared gain is
  safe either way: a chest's key is taken once by the team (its chest Bool is shared), so the other
  player must be credited, and a surplus key can never block progress. Each gain is exact under
  concurrency because it is a delta with its own origin/sequence. Cost: a player whose door was
  opened by the peer (if door flags are ever shared) keeps an extra key; a late joiner gets no
  earlier gains (the same gap as item gives).
- Map, compass and boss key are Bools (`bool_set`, OR). The give path never carries them (the key
  file's `give.never`), so nothing is counted twice.
- Per-player Bools (`deny.bool`) are dropped by the client's protocol state and never applied.

## Poses (class `pose`, v3)

A pose is a peer's live position, sent so the others can draw a ghost of it. It is **not a
grant**: no `seq`, no ack, no `rev`, no replay window, never folded into team state, never in a
`team_state` snapshot and never sent on join. A client sends exactly these fields:

```json
{"type":"pose","t":123456,"banc":"<banc>","child":true,"loading":false,"pos":[-46.09,118.46,15.39],"yaw":108.6,"anim":"walk","anim_seq":3,"rate":1.0,"phase":0.25}
```

`anim`, `anim_seq`, `rate` and `phase` are an animation hint for a later animated receiver; the v1
statue receiver ignores them. The sender infers `anim` (`idle`, `walk` or `run`) from its speed,
because the game's own command table names both walking and running `Move`. `anim_seq`
(unsigned 32-bit) changes only when the category changes, so a receiver restarts an animation
then and never per packet. `rate` (0..10) is the speed relative to the category's nominal speed;
`phase` (0 <= phase < 1) is the cycle position (distance per stride, or time while idle).

`t` is the sender's frame counter (unsigned 32-bit, wraps). `banc` is the sender's current scene
name, 1..63 of `[A-Za-z0-9_]`, or `""` when there is no save, the title screen is up or the scene
cannot be read. `child` and `loading` are JSON booleans. `pos` is exactly three finite numbers
(integer or float, never a boolean), each with magnitude at most 100000. `yaw` is a finite number
of degrees in [-360, 360]. The framed payload, as sent, is at most 256 bytes. `origin` and `seq`
are not accepted from the client and unknown fields are rejected. A malformed pose is a protocol
error like any other malformed packet (an `error` and disconnect); a well-formed one never
causes a disconnect.

The relay forwards `{"type":"pose","origin":"<client-id>",...same fields}` to the other members
of the sender's team only; `origin` comes from the connection. Delivery is best effort, latest
wins:

- A pose from an origin still queued unsent in a receiver's outbox is replaced in place.
- A receiver whose outbox already holds more than 64 frames gets no new pose (dropped and
  counted). When a grant, ack or snapshot needs room, queued poses are evicted first, so poses
  can never push a client into the slow-client disconnect, which still applies to everything else.
- A sender over 30 poses in a sliding one-second window has the excess dropped silently.

Client side: when `pose` is in `EDEN_NX2_OOT_COOP_ALLOW` the emulator sends its own pose at about
15 Hz while it moves, with a 1 Hz keepalive while it stands still. Each peer whose latest pose is
in the receiver's own scene is drawn as a "statue" ghost: a Link-shaped puppet actor created
through the game's own actor manager and moved with the actor's own set-transform call from the
frame drain. The ghost is parked below the map when the peer leaves the scene or disconnects
(`client_state` with `online:false`). Grants keep priority in the drain's one guest call per
frame; a pose only ever moves a ghost and never changes a save.

Reconnects: a dropped or refused relay connection is logged with its reason (`relay connection
lost (<why>; N peer(s) dropped); reconnecting in S s (attempt N)`), retried after 1, 2, 4 .. 30 s
(reset once a connection is ready), and a successful one logs `relay reconnected (#N)`. Every new
connection repeats the handshake, the `team_state` baseline and the unacknowledged outgoing
changes, exactly like the first one. Peers' ghosts park while the relay is gone and return with
their next pose. Peer presence is logged: `peer <id> online (first pose ...)` and `peer <id>
offline (relay)`. The relay refuses a client id that it still holds open, so after a half-open
drop the reconnect succeeds once the relay's idle timeout (90 s) has closed the old connection.

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
Slow clients disconnect; capacity failures do not silently evict state. Poses: 256-byte
payload, no new pose to a receiver holding more than 64 queued frames, one queued pose per
origin per receiver, 30 poses/s per sender; pose excess is dropped and never disconnects. Heartbeat is
30 seconds; receive idle timeout is 90 seconds. A private deny-list JSON array of numeric
keys is loaded at relay startup with `--deny-list`; guest allow lists remain independent.

Socket-free suite, run in this sandbox:

```sh
python3 -I tools/oot/coop/test_relay.py ProtocolTests
```

**External lane must run the exact full command** (52 tests: 30 pure, 22 real TCP, of which
6 drive the single-emulator fake peer):

```sh
python3 -I tools/oot/coop/test_relay.py
```

TCP cases cover routing, concurrent check identity, snapshots after compaction, reconnect,
refusal, presence teardown, heartbeat/expiry, pre-existing baseline union, unsafe grant
rejection, malformed JSON/length, split/coalesced frames, deny-list enforcement, v2 refusal,
pose routing without ack, malformed and oversized poses, the pose rate cap, and the fake peer's
pose commands. Pose coalescing and the stalled-receiver drop are pure tests.
A pure-test pass is not evidence that these real-socket tests passed.
