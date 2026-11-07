# OoT co-op transport prototype

This draft connects emulators and shares observed changes with a LAN relay. **Remote game
writes are disabled. It does not yet synchronize saves or provide playable co-op.** The
relay and simulator can be tested without running a game. The complete protocol and
remaining guest integration work are in [the design](../../../docs/nx2/oot-coop.md).

Requires Python 3.11 or later; no third-party packages. All commands below run from the
worktree root. The protocol follows Anchor's room/team, snapshot and replay model, with
an independently written implementation and newline-delimited JSON.

## Relay and simulator checks

Start one relay on a trusted LAN host:

```sh
python3 -I tools/oot/coop/relay.py --bind 0.0.0.0 --port 43390
```

The default bind address is `127.0.0.1`. The LAN address above is explicit because there
is no authentication, encryption or access control. Do not expose this port to the
Internet. Allow TCP port 43390 only from the intended LAN clients.

In two terminals, substitute the relay's LAN IP for `192.168.1.10`. Start the receiver
first; all example hashes are synthetic opaque values:

```sh
python3 -I tools/oot/coop/client_sim.py --host 192.168.1.10 --room demo --team together --name Sam --duration 60
python3 -I tools/oot/coop/client_sim.py --host 192.168.1.10 --room demo --team together --name Alex --send '{"type":"bool_set","hash":123,"value":true}' --send '{"type":"quest_step","challenge":456,"step":789,"index":2}'
```

Both print `hello_ack` and an initial `team_state`. The sender prints an `ack`; the
receiver prints accepted changes with their relay revision. A late receiver gets the
checkpoint and replay. The simulator answers `heartbeat` with `heartbeat_ack`.

Run the tests (real loopback TCP, ephemeral ports, no emulator):

```sh
python3 -I tools/oot/coop/test_relay.py
```

The suite contains 13 socket-free protocol/state/dispatcher tests and 15 asyncio networking tests. If an
execution sandbox forbids loopback sockets, the networking tests fail at setup; they
are deliberately not skipped or replaced with simulated TCP. Pure tests can still run:

```sh
python3 -I tools/oot/coop/test_relay.py ProtocolTests
```

## Two emulators on the LAN

Use one emulator per machine with the same main build
`E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348`, the same randomizer seed (or vanilla `0`),
and matching room/team strings. Configure each emulator's existing launcher environment:

```sh
EDEN_NX2_OOT_COOP=192.168.1.10:43390
EDEN_NX2_OOT_COOP_ROOM=demo
EDEN_NX2_OOT_COOP_TEAM=together
EDEN_NX2_OOT_COOP_NAME=Sam
EDEN_NX2_OOT_COOP_SEED=0
```

Set a different first name on the second machine. Client identity is generated per game
session and retained during connection retries; there is no emulator client-id override.
The default room and team are `default`. Seed hashes are opaque hex strings compared
exactly, including case. Every peer in a room must match the build and seed, even when
using different teams. A second live connection with the same client ID in one team
is refused. Different teams never exchange changes or client state.

Optional `EDEN_NX2_OOT_COOP_PLAYED_EVENTS=1` enables outgoing played-event observations.
Keep it off until the implications of sharing these flags have been checked. Existing
trace switches can run alongside co-op. Nothing starts until both the co-op endpoint
is configured and the main build gate matches. Incoming changes currently reach only
the protocol shadow state and disabled apply interface; they do not change the game.

Transient flags must be identified privately and excluded through a user-supplied JSON
array of numeric hashes, for example an initially empty file containing `[]`. Do not
commit game-derived names or hash tables. Set `EDEN_NX2_OOT_COOP_DENY_LIST` to that file
on each emulator; pass `--deny-list /path/to/your-file.json` to the relay. The relay
loads it once at startup and excludes bool/int/enum hashes, quest challenge hashes,
64-bit actor hashes and pouch actor-name hashes. Invalid files cause startup failure.
All hashes travel as JSON integers; use tools that preserve 64-bit integers exactly.

## Relay semantics and limits

Accepted changes reach only other members of the sender's team. Bool and actor bits
only become true; quest indices only increase. Ints/enums use relay arrival order.
Pouch adds accumulate and each `(origin, seq)` is processed at most once while the
team exists. Sequence numbers increase across all change kinds, including changes
ignored by the conflict rules or deny-list. Retransmissions at or below the sender's
highwater are acknowledged without being replayed. Sequence gaps are permitted, so
an older out-of-order packet arriving after a newer one is ignored. Keep unacknowledged
changes in order when reconnecting.

The relay stores a checkpoint plus the newest 512 accepted events. Older events are
folded into that checkpoint, including cumulative `pouch_total` entries, before removal.
`pouch_total` is an absolute total of observed adds, not a delta or a real inventory
count. A snapshot's `revision` is the checkpoint revision; its `seen` map is current,
after replay and ignored changes. Reconstruct the checkpoint plus replay in revision
order before adopting `seen`; using `seen` to filter that same replay would lose updates.
Every join gets this snapshot automatically; `team_state_request` requests it again.
The relay does not accept client-authored snapshots.

The server sends heartbeats every 30 seconds, disconnects connections without incoming
traffic for 90 seconds, and removes empty rooms after 300 seconds. Room state is only
in memory. Restarting the relay or expiring a room loses the checkpoint, dedup history
and all observations. Reusing an identity requires preserving sequence history; rebooting
an emulator creates a fresh identity. Durable reconnect and save baselines remain TODO
before enabling guest writes, especially for consumables and small keys.

Limits are 1 MiB per JSON line including newline and 16 nested JSON containers; 128 connections; 32 rooms;
16 teams per room; 256 retained sender identities and 2048 distinct state keys per team;
512 replay entries; and 256 queued packets or 2 MiB queued bytes per connection. Slow
receivers disconnect rather than blocking other clients. Client-state objects are at
most 4 KiB and cached only while their client is connected. IDs use 1–64 ASCII letters,
digits, underscores, hyphens or periods; display names use 1–96 UTF-8 bytes. Deltas use
positive signed32 counts, totals and sequence/revision numbers are bounded by 2^53−1,
and actor hashes retain their full unsigned64 range. Capacity or malformed-message
errors disconnect the sender and never silently evict synchronization history.

The prototype has no rate limiting or durable storage. TODO: establish safe baseline
and reconciliation rules, small-key consumption semantics, verified setter addresses
and ABIs, a safe game-thread drain hook, and persistent application receipts before
turning guest application on. Raw int/enum arrival order is a provisional policy and
must not be used for small-key counts without those decisions.
