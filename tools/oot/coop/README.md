# OoT co-op transport prototype

The relay and simulator implement [protocol v2](../../../docs/oot-coop/PROTOCOL.md):
length-prefixed JSON over TCP, OR-only bool/actor flags, room/team routing and bounded
checkpoint/replay. **Guest writes and save extraction remain unverified; this is not
playable co-op.** See the [field plan](../../../docs/oot-coop/FIELD-PLAN.md) for emulator tests.

Requires Python 3.11+, stdlib only. Run from the worktree root. Relay deployment is a
separate lane; these commands are for its operator, not commands run in the sandbox.

```sh
python3 -I tools/oot/coop/relay.py --bind 0.0.0.0 --port 43390
```

Default bind is `127.0.0.1`. Permit TCP 43390 only from the intended LAN/VPN clients;
the prototype has no authentication or encryption. Start the receiver first, then the
sender; replace `192.168.1.10` with the relay host. Keys below are synthetic.

```sh
python3 -I tools/oot/coop/client_sim.py --host 192.168.1.10 --room demo-v2 --team together --policy or-v2 --name Anthony --duration 60
python3 -I tools/oot/coop/client_sim.py --host 192.168.1.10 --room demo-v2 --team together --policy or-v2 --name Daniel --send '{"type":"bool_set","hash":123,"value":true}' --send '{"type":"bit_set","actor":456,"value":true}'
```

Both print `hello_ack` and `team_state`. The sender gets `ack`, the receiver gets each
accepted flag with `rev`. Repeating the same flag from either sender adds no revision.
`client_state` can carry advisory scene/room presence; no dummy player is created.
Scalar, quest and additive pouch packets are rejected. v1 newline clients are incompatible.

The external lane must run the **full** suite, including real loopback TCP:

```sh
python3 -I tools/oot/coop/test_relay.py
```

There are 52 tests: 30 socket-free and 22 TCP (6 of them drive the fake peer). In this sandbox only run:

```sh
python3 -I tools/oot/coop/test_relay.py ProtocolTests
```

Tests deliberately do not skip networking failures. No socket was opened by the pure
suite; it drives the framed reader with a fake in-memory reader without an event loop.

Matching build, seed, room/team and policy IDs are required. Each emulator session gets
its own client identity, retained across retries. The simulator's `--client-id` is for
controlled reconnect tests, not sharing an identity between simultaneous clients.
`--deny-list path/to/private.json` accepts a JSON array of opaque numeric keys, e.g. `[]`.
Do not commit game-derived key tables. Policy IDs identify a reviewed shared policy;
the relay does not calculate a digest of each emulator's local configuration.

A relay ack is not a guest-write receipt. Before real synchronization, both disposable
saves must export their verified allowlisted true flags and finish baseline reconciliation.
C3 exports the explicitly reviewed Bool64 key set at the frame hook; its game/save
semantics remain unverified in the field. Relay restart loses memory; reconnecting cannot
recover flags never observed or exported. See the protocol's baseline and duplicate-check
sections before testing pre-existing saves or adding inventory support.

C3 adds `two_instances.py` (owner-run Linux relay + two portable/disposable copies),
`check_logs.py` (positive applied/suppressed receipt checking) and socket-free
`test_harness.py`. See the field plan for the exact command and required environment.
Do not run the harness during a playing session; it refuses `.aot-priority` and any
existing `lichii` process. Class and key allow lists ship empty.

## Single-emulator tests: `fake_peer.py`

One real emulator is enough to exercise both directions: `fake_peer.py` runs the relay **in
its own process** on a loopback port and joins it as a scripted second player. No separate
relay service and no second emulator are needed.

```sh
python3 -I tools/oot/coop/fake_peer.py --ctl /path/to/ctl --port 43391 \
    --room R --team together --policy flags-... --seed 0 \
    --build-id "$MAIN_BUILD_ID" --wallet "$WALLET_KEY"
```

`--build-id` (the running main build) and `--wallet` (for `rupee DELTA...`) come from your private
profile; the defaults are placeholders, and the relay refuses an emulator whose build differs from
the first client's.

Start the emulator with `EDEN_NX2_OOT_COOP=1 EDEN_NX2_OOT_COOP_RELAY=127.0.0.1:43391` and the
same room/team/policy/seed. Then append commands to `ctl/cmd.txt`; each gets one JSON line in
`ctl/out.jsonl`, and every packet the peer receives is logged to `ctl/received.jsonl`:

- `grant HEX...` sends bool grants in one socket write (a batch);
- `expect HEX SECS` waits for the emulator to broadcast that flag;
- `received`, `state`, `mark TEXT`, `quit`.

`FakePeerTests` in `test_relay.py` covers it over real loopback TCP.

## Mod hash

`python3 -I tools/oot/coop/mod_hash.py --keys FILE --load DIR [--text]`
computes the version 2 kit fingerprint. Use repeated `--disabled NAME` or
`--config qt-config.ini` to match the selected title's disabled add-ons.
`--program-id HEX` selects a title under a shared load root. Version 2 includes
case-insensitive romfs/exefs/romfslite trees of enabled folders, with disk spelling
preserved. Version 1 (`--hash-version 1`) reproduces the C16 exact-case romfs/exefs
format and ignores exclusions/config. See [the canonical format](../../../docs/oot-coop/PROTOCOL.md#exact-mod-hash-input).
Socket-free synthetic tests: `python3 -I -m unittest discover -s tools/oot/coop -p test_mod_hash.py`.
