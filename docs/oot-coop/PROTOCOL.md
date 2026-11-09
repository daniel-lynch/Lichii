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
| `PLAYER_UPDATE` | `pose` (v3): position, facing, scene (`banc`), age, loading and animation hints, plus optional negotiated equipment; best effort, forwarded to the team, never state. Joints are dropped. Like `Ship/Packets/PlayerUpdate.cpp` it is drawn only for same-scene peers. |
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
refused at `hello` with `protocol version mismatch`. Versions 3 and 4 share this framing;
v4 adds negotiated animation IDs.

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

Animation clients use `version:4` and add `"animation":{"av":1,"table_sha256":"<64-lowercase-hex>"}`
to hello. SHA256 covers the exact bytes of the private `coop-anim.json` sidecar beside the
key file. The relay echoes the descriptor in `hello_ack`, with the requested protocol
version. For v4 recipients it includes each peer's negotiated descriptor in
`client_state.state.animation`; that descriptor comes from hello and cannot be replaced
by later client-state writes. Version 3 recipients receive ordinary presence without it.
A sender emits animation IDs only while a peer advertises the identical descriptor;
a receiver checks the origin's descriptor before interpreting an ID. Disconnect clears
that peer's negotiation. Missing or mismatched tables retain legacy animation hints.
A client sends the v4 hello only when its private key file turns an animation sender or
receiver on and the sidecar loads; with both off it keeps the v3 hello (and so still joins a
relay that predates v4). Deploy the v4 relay before enabling either.

Equipment-capable clients add the optional boolean `ghost_equipment_v2:true` to `hello`.
The relay echoes the flag in `hello_ack` when supplied. Send equipment only after a true
acknowledgement; omission or false disables it. Legacy clients retain the original handshake. The flag is independent of
the protocol version: a v3 or v4 hello may carry it.

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

## Same-chest interaction guard

`item_give` with a chest source carries its ordinary chest Bool as `origin_key` and, from a
guarded client, three transaction fields, all or none:

| Field | Range | Meaning |
|---|---|---|
| `chest_transaction` | 1..2^53-1 | The sender's open attempt; every outer give of that open shares it |
| `chest_part` | 0..63 | Outer give index within the transaction (bag, then its ammo, ...) |
| `chest_final` | boolean | True on the last part |

The relay validates the shape only (a chest source, all three fields); it does not arbitrate.
A chest `item_give` without the fields (a legacy sender, the fake peer's `itemk`) is a
one-part transaction.

A private `chest` profile section enables the traps: the treasure box's accepted-open
interaction (Begin), its opening event (Bind), the give entry (Gate, before its first
instruction) and the give's return. The local chest-struct setter or the observed end of the
event completes the open. Ownership is per receiver: the first source this client sees wins,
either its own Begin or a peer's live chest Bool / tagged give. A peer-owned or already paid
chest returns success from the local give without running it; a peer copy of a chest this
client owns is dropped, at receipt and again at issue time. Two clients that start the same
chest before either hears of the other each keep their own copy (+1 each, never +2): this is
not a global winner election. An originless baseline Bool is history, never a payable owner.
Receipts (per chest: source, transaction, applied parts) live for the session.

Limits: unknown attribution (no attempt, conflicting actor/event) leaves the give on the
ordinary untagged path and is logged; an open no event ever confirmed is released after 1800
frames when nothing was paid; a save change turns the guard off for the session (ordinary
gives, logged); dungeon Bool/Int rewards bypass the give entry and are not covered;
`chest.enforce=false` observes and logs without skipping or dropping.

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
eviction. Flags do not consume benefit receipts. Chest sources are not relay receipts: the
clients' chest records select one opener and retain benefit receipts. The receipt limit is
4096; overflow rejects the event before changing team state.
This storage is in memory, not durable across relay restart or room expiration.

The sender captures a reviewed key at the pickup-component entry on the same guest thread,
reserves it before the outer give, and records the benefit after the verified return reports
success. A losing local pickup returns success without running the give. A pickup without a
linked reviewed key (a dynamic drop, a gated family, or `bit` not allowed) keeps the C13
untagged give path. A receiver whose replayed give returns w0=0 keeps the receipt and sets
the flags (the game's own clamp, e.g. a full wallet); it never retries. Later Bool64 writes
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
A committed flag stops the next spawn but does not remove an already-loaded collectible (field
probe). With `cleanup="death"` and a private `cleanup` object (the placement-record walk: root
cell, block groups, record/actor/link/death-component offsets, all `0x????` placeholders here),
the receiver of a live peer flag for a currency (or, with hearts, heart) key walks the game's
placement records for that key once the flag has committed, checks the attached actor with pure
reads (back link, lifecycle range, destroy marker, lifetime link, no delete/death pending) and
issues the game's own death request for it (`pickup.death_wrapper`, reason from the profile) from
the frame drain, one call per frame, never when a grant or ghost call runs; it re-walks 60
frames later to log the outcome. `EDEN_NX2_OOT_COOP_WORLD_CLEANUP=walk` logs the walk without
calling. Tokens are never cleaned this way (their AI waits for its own death signal).

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
  forwarded as a delta with its own origin/sequence. Sharing the chest Bool does not prevent a
  simultaneous native small-key grant: dungeon Bool/Int grants bypass the item entry guard and
  still need separate race coverage. Cost: a player whose door was
  opened by the peer (if door flags are ever shared) keeps an extra key; a late joiner gets no
  earlier gains (the same gap as item gives).
- Map, compass and boss key are Bools (`bool_set`, OR). The give path never carries them (the key
  file's `give.never`), so nothing is counted twice.
- Per-player Bools (`deny.bool`) are dropped by the client's protocol state and never applied.

## Poses (class `pose`, v3 and v4)

A pose is a peer's live position, sent so the others can draw a ghost of it. It is **not a
grant**: no `seq`, no ack, no `rev`, no replay window, never folded into team state, never in a
`team_state` snapshot and never sent on join. A client sends these required fields:

```json
{"type":"pose","t":123456,"banc":"<banc>","child":true,"loading":false,"pos":[-46.09,118.46,15.39],"yaw":108.6,"anim":"walk","anim_seq":3,"rate":1.0,"phase":0.25}
```

`anim`, `anim_seq`, `rate` and `phase` are an animation hint for a later animated receiver; the v1
statue receiver ignores them. The sender infers `anim` (`idle`, `walk` or `run`) from its speed,
because the game's command table shares a locomotion command for walking and running. `anim_seq`
(unsigned 32-bit) changes only when the category changes, so a receiver restarts an animation
then and never per packet. `rate` (0..10) is the speed relative to the category's nominal speed;
`phase` (0 <= phase < 1) is the cycle position (distance per stride, or time while idle).

Negotiated equipment adds the optional pair `eq32` and `eqseq`; both must be present or absent.
`eq32` is an unsigned 32-bit JSON integer and `eqseq` an unsigned 16-bit JSON integer (booleans
and floats are rejected). The relay forwards the pair only to receivers that negotiated
`ghost_equipment_v2`; it strips both fields for legacy receivers. Unnegotiated senders cannot
send either field. Equipment fields count toward the same 512-byte pose payload limit as the
animation fields; the existing cadence, coalescing and rate limits apply.

| `eq32` bits | Mod-defined value |
|---|---|
| 0–2, 3–5 | Sword/shield identity: 0 none, 1–3 allowlisted IDs, 7 unknown; 4–6 invalid |
| 6–7, 8–9 | Actual sword/shield state: 0 absent, 1 drawn, 2 sheathed, 3 unknown |
| 10–11, 12–13 | Tunic/boots: 0–2 allowlisted IDs, 3 unknown |
| 14–17 | Mask: 0 default head, 1–10 allowlisted IDs, 15 unknown; 11–14 invalid |
| 18–19 | Age: 0 child, 1 adult, 3 unknown; 2 invalid |
| 20 | Guard-front boolean |
| 21–27 | Reserved, must be zero |
| 28–31 | Equipment schema version, exactly 1 (independent of protocol version) |

A known equipment age must agree with outer `child`. Unknown age cannot create or re-age a
ghost. Unknown fields retain the last confirmed desired value only within the current peer
freshness, scene epoch and actor generation. None plus a nonzero state, or an equipped identity
plus state zero, is an unsettled snapshot: accept its encoding, but defer native changes until
it settles. IDs index the private allowlist sidecar; names, guest pointers and native addresses
never travel in these fields. Runtime entries such as `main+0x????` come from the private profile.

`eqseq` changes only when a known ID, actual state or age changes; unknown observations and the
guard bit do not increment it; it wraps at 65536 and is not an action count. Receivers apply
equipment snapshots in arrival order (one ordered stream, every packet a full snapshot) and do not
reject by `eqseq`, since a restarted client restarts its sequence; outer pose `t`, freshness and
scene identity still reject stale poses. Send the latest full snapshot at the existing cadence and
coalesce before issuing any guest calls. Boots identities travel on the wire, but the receiver
defers them: no guarded native boots path exists yet.

The kit controller's `peer pose <banc> 1 <x> <y> <z> <yaw> eq32=<word> [eqseq=<u16>]`
accepts decimal or `0x` equipment words. Omitted `eqseq` tracks known changes and wraps at 65536;
an explicit value supports stale/wrap probes. Use the private probe report for actual pack IDs.

`t` is the sender's frame counter (unsigned 32-bit, wraps). `banc` is the sender's current scene
name, 1..63 of `[A-Za-z0-9_]`, or `""` when there is no save, the title screen is up or the scene
cannot be read. `child` and `loading` are JSON booleans. `pos` is exactly three finite numbers
(integer or float, never a boolean), each with magnitude at most 100000. `yaw` is a finite number
of degrees in [-360, 360]. The JSON payload is at most 512 bytes, including the relay-added
`origin` on forwarded packets and any negotiated animation and equipment fields. `origin` and `seq`
are not accepted from the client and unknown fields are rejected. A malformed pose is a protocol
error like any other malformed packet (an `error` and disconnect); a well-formed one never
causes a disconnect unless adding the required origin exceeds the payload limit.

V4 poses retain every legacy field and optionally add all five animation fields together:

| Field | Encoding |
|---|---|
| `av` | Integer 1; other revisions rejected |
| `aid` | Unsigned 32-bit integer; 0 selects legacy approximation; other values require an age-matching table entry |
| `af` | Finite resource-frame number in `[0, entry.frames]`, or null when phase correspondence is unknown |
| `ar` | Finite requested rate in `[0,4]`; 0 freezes |
| `aseq` | Unsigned 32-bit transition generation, including restarts of the same ID |

The relay validates numeric shape, finite values, rate, and a generic frame ceiling of
1,000,000; the client validates ID membership, age and the entry's actual frame bound.
Booleans and fractional IDs/generations are rejected. Frame and rate serialization must
retain float32 precision. Clip strings never travel on the wire. The relay forwards these
five fields only to peers whose hello descriptor matches the sender's; other peers receive
the legacy fields alone. An unnegotiated sender cannot submit animation fields.

Animation selection is discrete at the selected pose timestamp. Transition ordering uses
`0 < uint32(new-old) < 0x80000000`, reset on a new session. Newer packets coalesce while a
native call is outstanding; completion only commits that call's own generation. New
transitions play frozen, set the frame, set the rate, then apply the anchor transform.
Rate changes within a transition do not replay it. Verified in-place one-shots hold their
last frame until another sender transition; they never automatically select idle.

The probe peer accepts `--anim-table <private-sidecar>` and commands such as
`pose <banc> 1 0 0 0 0 aid=<id> af=0 ar=0`. Omit `aseq` for frozen frame steps: each command
advances the generation and reapplies the chosen frame. Reuse `aseq=<generation>` for a
rate-only update. Same-transition frame correction is not enabled.

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
Slow clients disconnect; capacity failures do not silently evict state. Poses: 512-byte
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

## Ledger, catch-up and the mod gate (C16)

These are additive extensions to v3/v4; existing live grants, typed origins, chest
multipart fields, animation/equipment presence and wallet baselines retain their
wire formats. Catch-up never carries save bytes. A network ack still means relay
acceptance only, not application or a durable guest save.

### Handshake and compatibility

Hello may add `"mod_hash":"<64 lowercase hex>"`. Any other representation is a
protocol error. After build/seed/policy checks, the first hashing client fixes the
room's hash for its lifetime. A legacy-created room initially has no hash. A
hashing client with a different hash receives
`{"type":"error","reason":"mod mismatch (room <first 12 hex>, client <first 12 hex>)"}`
and disconnects. The relay prints:

```text
OoT co-op relay: handshake refused: mod mismatch room=<room> team=<team> client=<client_id> room_hash=<12> client_hash=<12>
```

A hello without `mod_hash` is admitted unchecked, even into a hashed room. It gets
exactly the c15 hello_ack, with no added fields. Each such join prints:

```text
OoT co-op relay: client <id> joined room <room> without a mod hash (legacy client, unchecked)
```

Only a hello containing `mod_hash` gets these additional hello_ack fields:

```json
{"ledger":{"v":1,"epoch":"<room epoch>","floor":0},"mod_hash":"<room hash>"}
```

The relay creates a random 16-character lowercase hex epoch per room. Persistence
preserves it; deletion/reset followed by recreation makes a new one. The client
accepts capability revision 1, an epoch of 1..64 `[A-Za-z0-9_.-]` characters, and a
floor in 0..2^53−1. Absence means no catch-up capability for that connection.

| Client | Relay | Result |
|---|---|---|
| C16 hashing client | C16, same kit | Mod check; catch-up when locally enabled and save bound |
| C16 hashing client | C16, different kit | Refused; client stops reconnecting until restart |
| C16 hashing client | deployed c15 | First hello refused with exactly `missing or unknown fields`; client removes the hash for this process and reconnects with normal backoff |
| c15 v3/v4 client | C16 | Unchecked legacy join; unchanged ack, live events and state; no unsolicited catch-up |
| C16 client with hash sending disabled/failed | C16 | Unchecked legacy hello; no capability echo and no automatic catch-up |
| c15 client | c15 | Unchanged |

The fail-soft client logs once:

```text
OoT co-op: relay predates the ledger (c16): no mod check, no catch-up; playing on
```

A hash mismatch instead logs and latches co-op off:

```text
OoT co-op: handshake refused: mod mismatch (ours <12 hex>; <relay reason>); co-op stays off until restart with the room's kit
```

### Ledger and paging

Each team retains at most 4096 accepted additive broadcast event dictionaries,
including `rev`: `item_give`, `rupee_add`, and `key_add`. False/denied writes and
sender sequence retries are not ledger entries. Flags remain in team_state.
Evicting the oldest entry advances `ledger_floor` to its revision; the initial
floor is zero. Live forwarding is unchanged.

For a chest origin (including legacy `origin_key`), the first accepted give fixes
`(origin, chest_transaction or null)` as that chest's ledger owner. All of that
owner's multipart gives remain eligible. Gives from a different owner/transaction
are still broadcast live but are omitted from catch-up. The owner table stops
adding keys at MAX_KEYS; untracked keys still ledger normally. Bit/token item
origins retain the existing team benefit-receipt uniqueness rule.

Any joined client may request exactly:

```json
{"type":"catchup_request","after":0}
```

`after` is an integer in 0..2^53−1 (not a boolean). The response is exactly:

```json
{"type":"catchup","epoch":"<epoch>","floor":0,"through":123,"after":0,"grants":[],"more":false}
```

`through` is the team's revision when this page is built. `grants` contains all
senders' ledger events with `after < rev <= through`, sorted by revision, at most
256 per page. It includes the requester's own grants; per-save client IDs decide
which to skip. The relay shrinks pages to fit the 1 MiB payload limit. `more` is
true iff eligible ledger events remain beyond the page's last grant. Repeat with
`after` equal to that last revision while `more` is true. An empty page never has
`more:true`. A page is assembled synchronously; different pages can have different
`through` values as the room advances. Requests change no state and never dirty the
state file. There is no frozen multi-page snapshot or acknowledgment of page use.

The client starts at zero on each connection after the ordinary baseline exchange
and save bind. It allows one transaction per connection and at most 16384 grants
per transaction. Each page must match the epoch and requested cursor, have valid
u53 floor/through/revisions, strictly increasing positive grant revisions, and
contain only decodable additive events. Invalid pages abandon the transaction
with a diagnostic, without deliberately disconnecting. Local `catchup` metadata
is never encoded as a grant field.

Ordering is flags first (the preceding team_state drained in FIFO order), then
catch-up gives in revision order through the existing one-call-per-frame apply
queue, then live gives. Additive live packets received while awaiting initial
catch-up or paging are held and released after the final page; flags continue to
enter the FIFO. Queue limits still apply. A reconnect starts another transaction
and retains this process's working save receipts.

Once per second while connected, the client looks for `<user dir>/coop-resync`.
It deletes the trigger, logs `OoT co-op: resync requested (coop-resync)`, requests
team_state, and starts catch-up after that state arrives if capable, enabled and
bound. A request during paging schedules another transaction after the current
one; otherwise a diagnostic explains unavailable catch-up.

### Exact mod-hash input

Compute SHA256, lowercase hex, of UTF-8 text with lines joined by `\n` and a final
`\n`. The canonical lines are exactly:

```text
oot-coop-mod-hash 1
keys <size> <sha256>
sidecar coop-ux.json <size> <sha256>
sidecar coop-anim.json <size> <sha256>
sidecar coop-equip.json <size> <sha256>
sidecar coop-ledger.json <size> <sha256>
mod <relpath> <size> <sha256>
```

For a missing sidecar its entire line is `sidecar <name> absent`. The four names
always appear in that order, beside the exact key file loaded by the client,
independently of feature switches. File sizes are decimal byte counts; file
hashes cover exact bytes, without JSON normalization. Each regular file under
`<load dir>/<program id as 16 uppercase hex>/<any mod dir>/romfs/**` or
`.../exefs/**` contributes one `mod` line. `relpath` is the UTF-8 slash-separated
path from the mod directory, such as `romfs/a/b.bin`. The mod directory's name is
excluded. Retain duplicate records and sort bytewise by relpath, then file hash.
Exclude `cheats/` and all other top-level trees. Follow symlinks; unreadable files,
broken links in the traversed trees, and directory cycles fail hashing. Newlines
in relative paths are rejected because they cannot be represented unambiguously
in this line format.

The hash is computed once at session start, independently of the ledger enable
rule. `EDEN_NX2_OOT_COOP_MOD_HASH=0` disables sending it. A failure logs and sends
no hash. Success logs:

```text
OoT co-op: mod hash <first 12 hex> (keys + 4 sidecars + <n> mod files, <MiB> MiB)
```

Operators can use `python3 -I tools/oot/coop/mod_hash.py --keys FILE --load DIR
[--text]`. DIR can be the title's 16-hex load directory, or a load root containing
one title. With multiple titles, use `--program-id HEX` to select one; it never
hashes unrelated titles. `--text` prints the canonical text instead of its digest.
This does not include local FPS cheats, folder names, or enabled/disabled mod
selection; every file in the specified mod trees contributes.

### Save binding and receipt sidecars

The private configuration `coop-ledger.json`, beside the key file, is strictly
`{"version":1,"save_file":"<ECMAScript regex>"}`. Unknown fields are rejected.
The entire filesystem virtual path must match, with exactly one capture group
naming the slot. Slot names must be safe identifiers (no slash, `.` or `..`).
There are no game save names or offsets in the implementation. Catch-up requires
this config, valid private pose offsets, the item class, and
`EDEN_NX2_OOT_COOP_LEDGER` not equal to `0`. Startup logs either:

```text
OoT co-op: ledger on (save pattern loaded)
OoT co-op: ledger off: <reason>
```

Successful read/write opens and renames notify the session. The first 64 matched
operations log `OoT co-op: ledger save file op=<read|write|rename> slot=<slot>
path=<virtual> host=<host>`. After 60 consecutive drain frames with nonempty local
pose banc and no loading flag, bind the last read-open slot. No read means a new
save, with an empty working set and slot unknown until its first write. Read-write
opens before bind are logged but do not write receipts. Another slot read after
bind disables catch-up until restart; one save per process remains the contract.

Before binding, the frame drain holds the pending FIFO. The game can retain its
manager across title and load: applying team flags before loading would lose those
flags, and the observed true-to-false transition would quarantine the drain.
After bind, first observe the loaded local flag baseline before queueing incoming
flags, so local coverage cannot be inferred from team flags applied by co-op.

The receipt sidecar lives at `<matched host file's parent>/coop-ledger.json`, so
it travels with the slot directory. If the host path is empty or relative, or its
parent directory does not exist, use `<user dir>/oot-coop-ledger/<slot>.json`.
A rename has no host path and uses that fallback. Back up/copy the applicable
receipt file together with the slot. The receipt document (distinct from the
private regex config despite the same basename) is:

```json
{"format":1,"slot":"slot_a","rooms":{"room/team":{"epoch":"0011223344556677","through":123,"used_ms":1000,"applied":[["sender",42]]}}}
```

Keep at most eight room/team entries, evicting least recently used, and at most
8192 IDs per entry, evicting oldest inserted. A different epoch resets the selected
entry's receipt state. `through` is the maximum page watermark seen, not proof that
all grants succeeded. A matched write/rename after bind snapshots the working IDs
under their mutex and posts an atomic sidecar replacement to the co-op I/O thread.
It merges only the current room/team entry into the destination slot's document.
File fsync is best effort. Corrupt/unreadable documents are treated as absent for
reading, logged, and left untouched until a save-write; replacement first preserves
a `.bad` copy. If that copy fails, the write fails rather than destroying evidence.

Book admission uses exact `(sender, seq)` IDs, not quantities or sender high-water
marks, for catch-up. It checks already-applied, then already-queued, then local
source coverage (chest Bool, or every bit/token source flag), otherwise queues the
give. Live admission additionally uses the existing sender high-water rule. Own
outgoing additive IDs count as applied. Item/rupee IDs become applied on sentinel
return regardless of w0; key IDs are recorded after both queued key adds commit.
Catch-up chest gives bypass historical C13 chest claims; bit/token gives retain
normal world reservation/application. Policy or full-queue drops are not applied
receipts. Per-save ID bounds and source coverage are deliberately separate.

The client emits bounded per-grant decisions and these transaction receipts:

```text
OoT co-op: ledger save bound slot=<slot|new> applied=<n> sidecar=<path|none>
OoT co-op: ledger sidecar written slot=<slot> applied=<n> path=<path>
OoT co-op: catchup grant sender=<8> seq=<n> class=<item|rupee|key> name=<name|-> decision=<queued|already-applied|covered|policy>
OoT co-op: catchup received epoch=<e> floor=<f> through=<t> grants=<N> queued=<Q> already-applied=<a> covered=<c> in-flight=<q>
OoT co-op: catchup: <N> grants, <M> applied, <K> already had
OoT co-op: catchup warning: the relay ledger window starts after rev <floor>; older gives cannot be replayed
```

`K = already-applied + covered + in-flight`. If `M < Q`, the final line appends the
number of queued grants not applied. The receive line means intake completed;
the final line waits for the queued gives' returns or known drops. It is not a
save-commit receipt. A save-open notification and asynchronously written sidecar
are **not atomic with a guest save commit**; a crash or overlapping save/give can
leave the two out of sync. This design offers bounded per-save dedupe in the normal
workflow, not a proof of crash-safe exactly-once delivery.

### Relay persistence and lifetime

The library default is memory-only. CLI defaults to `relay-state.json` beside
relay.py; `--state-file PATH` overrides it and `--no-state` disables persistence.
The format is `{"format":1,"rooms":{...}}`: room build/seed/policy, epoch, mod hash,
last-active wall time; team revision, folded current flags, sender high-waters,
benefit receipts, ledger/floor and chest owners. Clients, sockets, presence, poses
and wallets are not persisted. Load makes current flags a checkpoint at the saved
revision with empty replay, preserving legacy team_state decoding.

Dirty changes write the whole document at most once per second and on close,
using a same-directory mode-0600 temporary file, flush/fsync, atomic replace and
directory fsync. Missing state starts empty; corrupt, unreadable, inconsistent or
larger-than-64-MiB state refuses startup. Runtime write errors log once per distinct
error and retry while continuing in memory. Acknowledged grants can therefore be
lost on a crash before persistence or during write failures.

Memory-only empty rooms keep the 300-second TTL. Persistent empty rooms expire
after `--room-ttl-days` (default 30) since last activity. At room capacity the least
recently active empty room may be evicted; only an entirely connected set refuses
a new room. Capacity eviction is logged with the idle timestamp. Room deletion
loses that campaign's ledger; backup and room naming remain operator responsibilities.
`--reset-room NAME` is an offline, synchronous command: load, remove only that room,
atomically write, print, exit without opening a server or event loop. Missing room
is an error. Startup reports the state path or `memory only` and loaded room count.
