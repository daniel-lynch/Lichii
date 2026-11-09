#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run ProtocolTests without sockets; run the full suite in the external test lane."""

import asyncio
import importlib.util
import math
from pathlib import Path
import json
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest import mock

WALLET = 0x0BADCAFE  # synthetic wallet key (the real one is game-derived and private)

sys.dont_write_bytecode = True


def load_sibling(name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(name + ".py"))
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


relay = load_sibling("relay")
client_sim = load_sibling("client_sim")
fake_peer = load_sibling("fake_peer")


def framed(payload):
    return len(payload).to_bytes(4, "big") + payload


def grant(seq=1, *, origin="a", key=1, kind="bool_set", value=True):
    return {"type": kind, "origin": origin, "seq": seq,
            "hash" if kind == "bool_set" else "actor": key, "value": value}


def pose(**changes):
    return dict({"type": "pose", "t": 1234, "banc": "Field_1", "child": True, "loading": False,
                 "pos": [-46.09, 118.46, 15.39], "yaw": 108.6, "anim": "walk", "anim_seq": 3,
                 "rate": 1.0, "phase": 0.25}, **changes)


BAD_POSES = (
    {"banc": "bad banc"}, {"banc": "a-b"}, {"banc": "x" * 64}, {"banc": 7},
    {"pos": [float("nan"), 0, 0]}, {"pos": [0, float("inf"), 0]}, {"yaw": float("-inf")},
    {"pos": [100000.5, 0, 0]}, {"pos": [0, 0, -100001]}, {"yaw": 400}, {"yaw": -360.1},
    {"pos": [1, 2]}, {"pos": [1, 2, 3, 4]}, {"pos": (1, 2, 3)}, {"pos": [True, 0, 0]},
    {"yaw": False}, {"yaw": "1"}, {"t": -1}, {"t": 1 << 32}, {"t": 1.0}, {"child": 1},
    {"loading": None}, {"extra": 1}, {"origin": "a"}, {"seq": 1},
    {"anim": "jump"}, {"anim": None}, {"anim_seq": -1}, {"anim_seq": 1 << 32}, {"rate": -0.1},
    {"rate": 11}, {"phase": 1.0}, {"phase": -0.01}, {"phase": float("nan")},
)


def materialize(snapshot):
    state = {relay.state_key(p): p for p in snapshot["changes"]}
    previous = snapshot["revision"]
    for packet in snapshot["replay"]:
        assert packet["rev"] == previous + 1
        previous = packet["rev"]
        relay.fold(state, packet)
    return state


class ProtocolTests(unittest.TestCase):
    def memory_server(self):
        self.now = 0.0
        clock = mock.patch.object(relay.asyncio, "get_running_loop",
                                  return_value=SimpleNamespace(time=lambda: self.now))
        clock.start()
        self.addCleanup(clock.stop)
        return relay.Relay()

    @staticmethod
    def stalled(server, client_id, equipment=None, **kwargs):
        """A real relay Client whose write loop never runs: its outbox only fills."""
        closed = []
        peer = relay.Client(None, SimpleNamespace(close=lambda: closed.append(True)))
        peer.closed = closed
        hello = client_sim.ClientSim(client_id, **kwargs).hello
        if equipment is not None:
            hello["ghost_equipment_v2"] = equipment
        server.join(peer, hello)
        return peer

    @staticmethod
    def queued(peer):
        return [relay.decode(entry[0]) for entry in peer.outbox]

    @staticmethod
    def drain(peer):
        """What the write loop does, without a socket."""
        sent = []
        while peer.outbox:
            entry = peer.outbox.popleft()
            if entry[1] is not None:
                peer.pending.pop(entry[1], None)
            peer.queued_bytes -= len(entry[0])
            sent.append(relay.decode(entry[0]))
        return sent

    @staticmethod
    def peer(server, client_id, **kwargs):
        messages = []
        peer = SimpleNamespace(send=messages.append, messages=messages)
        server.join(peer, client_sim.ClientSim(client_id, **kwargs).hello)
        return peer

    def test_wire_v2_exact_prefix_and_uint64(self):
        packet = grant(key=(1 << 64) - 1, kind="bit_set")
        encoded = relay.encode(packet)
        self.assertEqual(int.from_bytes(encoded[:4], "big"), len(encoded) - 4)
        self.assertEqual(encoded[4:5], b"{")
        self.assertEqual(relay.decode(encoded), packet)
        self.assertEqual(client_sim.wire.encode(packet), encoded)

    def test_frame_lengths(self):
        valid = relay.encode({"type": "heartbeat"})
        for data in (b"", b"\x00", b"\x00\x00\x00\x00", valid[:-1], valid + b"x",
                     (relay.MAX_FRAME + 1).to_bytes(4, "big"), b'{"type":"heartbeat"}\n'):
            with self.subTest(data=data[:40]), self.assertRaises(relay.ProtocolError):
                relay.decode(data)
        with self.assertRaises(relay.ProtocolError):
            relay.encode({"type": "state", "value": "x" * relay.MAX_FRAME})
        # A valid payload of exactly MAX_FRAME bytes is accepted.
        base = relay.encode({"type": "state", "value": ""})
        exact = relay.encode({"type": "state", "value": "x" * (relay.MAX_FRAME - len(base) + 4)})
        self.assertEqual(len(exact), relay.MAX_FRAME + 4)
        self.assertEqual(relay.decode(exact)["type"], "state")

    def test_invalid_json(self):
        for payload in (b'{"type":"heartbeat","type":"heartbeat"}',
                        b'{"type":"state","value":NaN}', b'[]', b'{"type":7}', b'\xff',
                        b'{"type":"state","value":{"x":1,"x":2}}'):
            with self.subTest(payload=payload), self.assertRaises(relay.ProtocolError):
                relay.decode(framed(payload))

    def test_nesting_and_escaped_strings(self):
        for depth in (15, 16):
            payload = b'{"type":"state","value":' + b'[' * depth + b'0' + b']' * depth + b'}'
            if depth == 15:
                relay.decode(framed(payload))
            else:
                with self.assertRaises(relay.ProtocolError):
                    relay.decode(framed(payload))
        packet = {"type": "state", "value": '["' * 30}
        self.assertEqual(relay.decode(relay.encode(packet)), packet)

    def test_read_frame_without_event_loop_or_socket(self):
        class Reader:
            def __init__(self, data):
                self.data = data
            async def readexactly(self, size):
                data, self.data = self.data[:size], self.data[size:]
                if len(data) != size:
                    raise asyncio.IncompleteReadError(data, size)
                return data
        packet = relay.encode({"type": "heartbeat"})
        reader = Reader(packet * 2)
        for _ in range(2):
            with self.assertRaises(StopIteration) as done:
                relay.read_frame(reader).send(None)
            self.assertEqual(done.exception.value, packet)
        for data, error in ((b"", EOFError), (b"x", relay.ProtocolError),
                            (packet[:-1], relay.ProtocolError),
                            ((relay.MAX_FRAME + 1).to_bytes(4, "big"), relay.ProtocolError)):
            with self.assertRaises(error):
                relay.read_frame(Reader(data)).send(None)

    def test_handshake_validation(self):
        packet = client_sim.ClientSim("a").hello
        relay.validate_hello(packet)
        for changes in ({"version": True}, {"version": 1}, {"version": 2}, {"build_id": "F" * 39},
                        {"seed_hash": 0}, {"room": "bad room"}, {"name": "\n"},
                        {"policy_id": "bad policy"}):
            with self.subTest(changes=changes), self.assertRaises(relay.ProtocolError):
                relay.validate_hello(dict(packet, **changes))

    def test_change_validation(self):
        for changes in ({"origin": "b"}, {"seq": 0}, {"hash": True}, {"value": 1},
                        {"seq": relay.MAX_SEQ + 1}, {"targetTeamId": "other"}):
            with self.subTest(changes=changes), self.assertRaises(relay.ProtocolError):
                relay.validate_change(dict(grant(), **changes), "a")
        with self.assertRaises(relay.ProtocolError):
            relay.validate_change(grant(kind="bit_set", key=1 << 64), "a")

    def test_bool_bit_or_and_duplicate_check_across_players(self):
        team = relay.Team()
        for kind in ("bool_set", "bit_set"):
            self.assertIsNone(team.accept(grant(1, origin=kind, kind=kind, value=False), set()))
            self.assertIsNotNone(team.accept(grant(2, origin=kind, kind=kind), set()))
            self.assertIsNone(team.accept(grant(3, origin=kind, kind=kind, value=False), set()))
            self.assertIsNone(team.accept(grant(1, origin=kind + "-other", kind=kind), set()))
        self.assertEqual(team.revision, 2)
        self.assertEqual(len(team.current), 2)

    def test_unsafe_grants_rejected_without_state_mutation(self):
        team = relay.Team()
        for kind in ("quest_step", "int_set", "enum_set", "pouch_add", "pouch_total"):
            with self.subTest(kind=kind), self.assertRaises(relay.ProtocolError):
                team.accept(dict(grant(), type=kind), set())
        self.assertEqual(team.current, {})
        self.assertEqual(team.seen, {})

    def test_rupee_deltas_are_events_not_state(self):
        def rupee(seq, delta=5, **extra):
            return dict({"type": "rupee_add", "origin": "b", "seq": seq, "hash": WALLET,
                         "delta": delta}, **extra)
        for bad in (rupee(1, 0), rupee(1, 1000), rupee(1, -1000), rupee(1, True), rupee(1, 2.5),
                    rupee(1, hash=0), rupee(1, value=True)):
            with self.subTest(bad=bad), self.assertRaises(relay.ProtocolError):
                relay.validate_change(bad, "b")
        team = relay.Team()
        team.accept(grant(1), set())
        self.assertEqual(team.accept(rupee(1), set())["rev"], 2)
        self.assertEqual(team.accept(rupee(2, -999), set())["rev"], 3)
        self.assertIsNone(team.accept(rupee(2), set()))  # duplicate sequence: no second +5
        self.assertIsNone(team.accept(rupee(3), {WALLET}))
        self.assertEqual(list(team.current), [("bool_set", 1)])
        snapshot = team.snapshot()
        self.assertEqual([p["delta"] for p in snapshot["replay"] if p["type"] == "rupee_add"], [5, -999])
        self.assertEqual(materialize(snapshot), team.current)
        for seq in range(4, 604):
            team.accept(rupee(seq), set())
        snapshot = team.snapshot()
        self.assertEqual(snapshot["changes"], [{"type": "bool_set", "hash": 1, "value": True}])
        self.assertEqual(len(snapshot["replay"]), 512)
        self.assertEqual(materialize(snapshot), team.current)

    def test_key_gains_are_events_not_state(self):
        def key(seq, delta=1, **extra):
            return dict({"type": "key_add", "origin": "b", "seq": seq, "hash": 0x1234, "delta": delta}, **extra)
        for bad in (key(1, 0), key(1, -1), key(1, 33), key(1, True), key(1, hash=0), key(1, value=True)):
            with self.subTest(bad=bad), self.assertRaises(relay.ProtocolError):
                relay.validate_change(bad, "b")
        team = relay.Team()
        self.assertEqual(team.accept(key(1), set())["rev"], 1)
        self.assertIsNone(team.accept(key(1), set()))  # duplicate: not a second key
        self.assertEqual(team.accept(key(2, 2), set())["rev"], 2)
        self.assertEqual(team.current, {})
        self.assertEqual([p["delta"] for p in team.snapshot()["replay"]], [1, 2])

    def test_item_give_may_carry_its_chest(self):
        give = {"type": "item_give", "origin": "b", "seq": 1, "name": "Item_X", "category": 6, "count": 1,
                "get": 7, "flags": [0, 0, 1, 0]}
        relay.validate_change(dict(give, origin_key=0xC1), "b")
        for bad in (0, -1, 1 << 32, "c1", True):
            with self.subTest(bad=bad), self.assertRaises(relay.ProtocolError):
                relay.validate_change(dict(give, origin_key=bad), "b")
        with self.assertRaises(relay.ProtocolError):
            relay.validate_change(dict({"type": "key_add", "origin": "b", "seq": 1, "hash": 5, "delta": 1},
                                       origin_key=1), "b")

    def test_noop_highwater_and_deny(self):
        team = relay.Team()
        self.assertIsNone(team.accept(grant(3, value=False), set()))
        self.assertIsNone(team.accept(grant(2), set()))
        self.assertIsNone(team.accept(grant(4), {1}))
        self.assertEqual(team.seen, {"a": 4})
        self.assertEqual(team.revision, 0)

    def test_checkpoint_compaction_over_512(self):
        team = relay.Team()
        for seq in range(1, 601):
            team.accept(grant(seq, key=seq), set())
        snapshot = team.snapshot()
        self.assertEqual(snapshot["revision"], 88)
        self.assertEqual(len(snapshot["changes"]), 88)
        self.assertEqual(len(snapshot["replay"]), 512)
        self.assertEqual(materialize(snapshot), team.current)
        self.assertEqual(snapshot["seen"], {"a": 600})

    def test_state_capacity_is_transactional(self):
        team = relay.Team()
        for number in range(relay.MAX_KEYS):
            team.accept(grant(number + 1, key=number), set())
        before = relay.encode(team.snapshot())
        with self.assertRaisesRegex(relay.ProtocolError, "capacity"):
            team.accept(grant(relay.MAX_KEYS + 1, key=relay.MAX_KEYS), set())
        self.assertEqual(relay.encode(team.snapshot()), before)
        self.assertLess(len(before), relay.MAX_FRAME)

    def test_revision_overflow_is_transactional(self):
        team = relay.Team(revision=relay.MAX_SEQ)
        with self.assertRaisesRegex(relay.ProtocolError, "exhausted"):
            team.accept(grant(), set())
        self.assertEqual(team.current, {})
        self.assertEqual(team.seen, {})

    def test_dispatcher_team_room_scope_and_origin(self):
        server = self.memory_server()
        a, b = self.peer(server, "a"), self.peer(server, "b")
        others = [self.peer(server, "c", team="other"), self.peer(server, "d", room="other")]
        for peer in [a, b, *others]:
            peer.messages.clear()
        server.receive(a, grant())
        self.assertEqual(a.messages, [{"type": "ack", "seq": 1}])
        self.assertEqual(b.messages, [dict(grant(), rev=1)])
        self.assertTrue(all(not peer.messages for peer in others))
        with self.assertRaisesRegex(relay.ProtocolError, "origin mismatch"):
            server.receive(b, grant())

    def test_dispatcher_presence_heartbeat_and_snapshot(self):
        server = self.memory_server()
        a, b = self.peer(server, "a"), self.peer(server, "b")
        presence = {"name": "Player", "online": True, "save_loaded": True, "scene": 1, "room": 2}
        server.receive(a, {"type": "client_state", "state": presence})
        self.assertEqual(b.messages[-1]["state"], presence)
        server.receive(a, {"type": "heartbeat"})
        self.assertEqual(a.messages[-1], {"type": "heartbeat_ack"})
        server.receive(b, {"type": "team_state_request"})
        self.assertEqual(materialize(b.messages[-1]), {})

    def test_preexisting_save_union_and_completion_barrier(self):
        server = self.memory_server()
        a, b = self.peer(server, "a"), self.peer(server, "b")
        for peer, keys in ((a, (1, 2)), (b, (2, 3))):
            for seq, key in enumerate(keys, 1):
                server.receive(peer, grant(seq, origin=peer.client_id, key=key))
            server.receive(peer, {"type": "team_state_request"})
            self.assertEqual(peer.messages[-1]["seen"][peer.client_id], 2)
        server.receive(a, {"type": "team_state_request"})
        expected = {("bool_set", n) for n in (1, 2, 3)}
        self.assertEqual(set(materialize(a.messages[-1])), expected)
        self.assertEqual(set(materialize(b.messages[-1])), expected)
        self.assertEqual(a.team.revision, 3)

    def test_handshake_build_seed_policy_and_duplicate_refusals(self):
        server = self.memory_server()
        self.peer(server, "a")
        for kwargs, error in (({"build_id": "A" * 40}, "build id mismatch"),
                              ({"seed_hash": "1234"}, "seed hash mismatch"),
                              ({"policy_id": "other"}, "policy id mismatch"),
                              ({}, "already connected")):
            with self.subTest(kwargs=kwargs), self.assertRaisesRegex(relay.ProtocolError, error):
                self.peer(server, "a", **kwargs)

    def test_reconnect_dedup(self):
        server = self.memory_server()
        a = self.peer(server, "a")
        server.receive(a, grant())
        del a.team.clients["a"]
        a = self.peer(server, "a")
        self.assertEqual(a.messages[1]["seen"], {"a": 1})
        server.receive(a, grant())
        self.assertEqual(a.team.revision, 1)

    def test_unsupported_dispatcher_packets(self):
        server = self.memory_server()
        a = self.peer(server, "a")
        for kind in ("pouch_add", "quest_step", "int_set", "enum_set", "PLAYER_UPDATE", "DISABLE_ANCHOR"):
            with self.subTest(kind=kind), self.assertRaises(relay.ProtocolError):
                server.receive(a, {"type": kind})

    def test_join_fits_outbox_at_max_clients(self):
        server = self.memory_server()
        for index in range(relay.MAX_CLIENTS - 1):
            self.peer(server, str(index))
        last = self.peer(server, "last")
        self.assertEqual(len(last.messages), relay.MAX_CLIENTS + 1)
        self.assertLessEqual(sum(len(relay.encode(p)) for p in last.messages), relay.MAX_OUTBOX_BYTES)

    def test_slow_client_disconnect(self):
        self.memory_server()
        closed = []
        client = relay.Client(None, SimpleNamespace(close=lambda: closed.append(True)))
        for _ in range(relay.MAX_OUTBOX):
            client.send({"type": "heartbeat"})
        client.send({"type": "heartbeat"})
        self.assertTrue(client.failed)
        self.assertEqual(closed, [True])

    def test_v2_hello_refused(self):
        hello = dict(client_sim.ClientSim("a").hello, version=2)
        with self.assertRaisesRegex(relay.ProtocolError, "protocol version mismatch"):
            relay.validate_hello(hello)
        self.assertEqual(relay.VERSION, 3)

    def test_pose_validation(self):
        relay.validate_pose(pose())
        relay.validate_pose(pose(banc="", t=0, pos=[100000, -100000, 0], yaw=-360, loading=True))
        relay.validate_pose(pose(banc="x" * 63, t=(1 << 32) - 1, yaw=360))
        for changes in BAD_POSES:
            with self.subTest(changes=changes), self.assertRaises(relay.ProtocolError):
                relay.validate_pose(dict(pose(), **changes))
        for missing in relay.POSE_FIELDS[1:]:
            packet = pose()
            del packet[missing]
            with self.subTest(missing=missing), self.assertRaises(relay.ProtocolError):
                relay.validate_pose(packet)
        with self.assertRaisesRegex(relay.ProtocolError, "too large"):
            relay.validate_pose(pose(), size=relay.MAX_POSE_FRAME + 1)
        relay.validate_pose(pose(), size=relay.MAX_POSE_FRAME)

    def test_equipment_hello_capability(self):
        hello = client_sim.ClientSim("a").hello
        for flag in (True, False):
            relay.validate_hello(dict(hello, ghost_equipment_v2=flag))
        for flag in (0, 1, "true", None, []):
            with self.subTest(flag=flag), self.assertRaises(relay.ProtocolError):
                relay.validate_hello(dict(hello, ghost_equipment_v2=flag))
        server = self.memory_server()
        enabled = self.stalled(server, "enabled", equipment=True)
        disabled = self.stalled(server, "disabled", equipment=False)
        legacy = self.stalled(server, "legacy")
        self.assertIs(self.queued(enabled)[0]["ghost_equipment_v2"], True)
        self.assertIs(self.queued(disabled)[0]["ghost_equipment_v2"], False)
        self.assertNotIn("ghost_equipment_v2", self.queued(legacy)[0])

    def test_equipment_codec_fields_exhaustive(self):
        base = 0x10000000
        fields = ((0, 3, {0, 1, 2, 3, 7}), (3, 3, {0, 1, 2, 3, 7}),
                  (6, 2, set(range(4))), (8, 2, set(range(4))),
                  (10, 2, set(range(4))), (12, 2, set(range(4))),
                  (14, 4, set(range(11)) | {15}), (18, 2, {0, 1, 3}), (20, 1, {0, 1}))
        for shift, width, allowed in fields:
            for value in range(1 << width):
                packet = pose(eq32=base | (value << shift), eqseq=65535,
                              child=not (shift == 18 and value == 1))
                with self.subTest(shift=shift, value=value):
                    if value in allowed:
                        relay.validate_pose(packet)
                    else:
                        with self.assertRaises(relay.ProtocolError):
                            relay.validate_pose(packet)
        # Transitional ID/state combinations are valid snapshots, settled by the receiver.
        relay.validate_pose(pose(eq32=base | (1 << 6), eqseq=0))
        relay.validate_pose(pose(eq32=base | 1, eqseq=0))

    def test_equipment_malformed_and_age_validation(self):
        for word in (-1, 1 << 32, True, 0, 0x10200000, 0x10080000, 0x10000004,
                     0x1002C000, 0x20000000, 1.0, "268435456", None):
            with self.subTest(word=word), self.assertRaises(relay.ProtocolError):
                relay.validate_pose(pose(eq32=word, eqseq=0))
        for bit in range(21, 28):
            with self.subTest(bit=bit), self.assertRaises(relay.ProtocolError):
                relay.validate_pose(pose(eq32=0x10000000 | (1 << bit), eqseq=0))
        for seq in (-1, 65536, True, 1.0, "1", None):
            with self.subTest(seq=seq), self.assertRaises(relay.ProtocolError):
                relay.validate_pose(pose(eq32=0x10000000, eqseq=seq))
        for extra in ({"eq32": 0x10000000}, {"eqseq": 0}):
            with self.subTest(extra=extra), self.assertRaises(relay.ProtocolError):
                relay.validate_pose(pose(**extra))
        for child in (False, True):
            relay.validate_pose(pose(child=child, eq32=0x100C0000, eqseq=0))
            with self.assertRaises(relay.ProtocolError):
                relay.validate_pose(pose(child=child, eq32=0x10000000 | (int(child) << 18), eqseq=0))

    def test_equipment_pose_has_a_bounded_extended_budget(self):
        packet = pose(banc="x" * 63, t=(1 << 32) - 1, pos=[-100000, -100000, -100000],
                      eq32=0x10000000, eqseq=65535)
        self.assertGreater(len(relay.encode(packet)) - 4, relay.MAX_POSE_FRAME)
        relay.validate_pose(packet)
        relay.validate_pose(packet, size=relay.MAX_EQUIPMENT_POSE_FRAME)
        with self.assertRaisesRegex(relay.ProtocolError, "too large"):
            relay.validate_pose(packet, size=relay.MAX_EQUIPMENT_POSE_FRAME + 1)
        with self.assertRaisesRegex(relay.ProtocolError, "too large"):
            relay.validate_pose(pose(), size=relay.MAX_POSE_FRAME + 1)

    def test_equipment_negotiation_forwarding_and_coalescing(self):
        server = self.memory_server()
        a = self.stalled(server, "a", equipment=True)
        b = self.stalled(server, "b", equipment=True)
        legacy = self.stalled(server, "legacy")
        for peer in (a, b, legacy):
            self.drain(peer)
        server.receive(a, pose(eq32=0x10000001, eqseq=65535))
        server.receive(a, pose(t=1235, eq32=0x10000002, eqseq=0))
        self.assertEqual(len(b.outbox), 1)
        self.assertEqual((self.queued(b)[0]["eq32"], self.queued(b)[0]["eqseq"]), (0x10000002, 0))
        self.assertNotIn("eq32", self.queued(legacy)[0])
        self.assertNotIn("eqseq", self.queued(legacy)[0])
        self.assertEqual(a.team.snapshot()["changes"], [])
        self.assertEqual(a.team.snapshot()["replay"], [])
        with self.assertRaisesRegex(relay.ProtocolError, "not negotiated"):
            server.receive(legacy, pose(eq32=0x10000000, eqseq=0))

    def test_equipment_fake_peer_commands_without_sockets(self):
        peer = fake_peer.FakePeer()
        peer.client.ack = {"ghost_equipment_v2": True}
        peer.client.send = mock.AsyncMock()
        for options, expected in (("eq32=0x10000081", 1), ("eq32=0x10100081", 1),
                                  ("eq32=0x100fffff", 1), ("eq32=0x10000081", 1),
                                  ("eq32=0x10000002", 1),
                                  ("eq32=0x10000082 eqseq=65535", 65535),
                                  ("eq32=0x10000083", 0)):
            with self.assertRaises(StopIteration) as done:
                peer.command("pose Field_1 1 0 0 0 0 " + options).send(None)
            self.assertEqual(done.exception.value["sent"]["eqseq"], expected)
        self.assertEqual(peer.client.send.await_count, 7)
        for options in ("eqseq=1", "eq32=0", "eq32=0x10000000 eqseq=65536",
                        "eq32=0x10000000 eq32=0x10000000", "unknown=1"):
            with self.subTest(options=options), self.assertRaises(ValueError):
                peer.command("pose Field_1 1 0 0 0 0 " + options).send(None)
        peer.client.ack = {}
        with self.assertRaisesRegex(ValueError, "not negotiated"):
            peer.command("pose Field_1 1 0 0 0 0 eq32=0x10000000").send(None)

    def test_fake_peer_poseequip_sets_what_streams_carry(self):
        peer = fake_peer.FakePeer()
        with self.assertRaises(StopIteration) as done:
            peer.command("poseequip 0x10040292 child=0").send(None)
        self.assertEqual(done.exception.value, {"stream_eq32": 0x10040292, "stream_child": False})
        self.assertEqual((peer.stream_eq32, peer.stream_child), (0x10040292, False))
        for bad in ("poseequip 0x10000289 child=0", "poseequip", "poseequip 0x10000289 child=2"):
            with self.subTest(bad=bad), self.assertRaises((ValueError, relay.ProtocolError)):
                peer.command(bad).send(None)
        with self.assertRaises(StopIteration) as done:
            peer.command("poseequip off child=1").send(None)
        self.assertEqual((peer.stream_eq32, peer.stream_child), (None, True))

    def test_pose_forwarded_to_team_only_with_origin_and_no_ack(self):
        server = self.memory_server()
        a, b, c = (self.stalled(server, name) for name in ("a", "b", "c"))
        others = [self.stalled(server, "d", team="other"), self.stalled(server, "e", room="other")]
        for peer in (a, b, c, *others):
            self.drain(peer)
        server.receive(a, pose())
        expected = {"type": "pose", "origin": "a", **{k: v for k, v in pose().items() if k != "type"}}
        self.assertEqual(self.queued(b), [expected])
        self.assertEqual(self.queued(c), [expected])
        self.assertEqual(self.queued(a), [])  # no ack, no echo
        self.assertTrue(all(not peer.outbox for peer in others))
        self.assertEqual((a.team.revision, a.team.seen, a.team.current, list(a.team.replay)),
                         (0, {"a": 0, "b": 0, "c": 0}, {}, []))

    def test_malformed_pose_is_a_protocol_error_without_forwarding(self):
        server = self.memory_server()
        a, b = self.stalled(server, "a"), self.stalled(server, "b")
        self.drain(b)
        for changes in BAD_POSES:
            with self.subTest(changes=changes), self.assertRaises(relay.ProtocolError):
                server.receive(a, dict(pose(), **changes))
        self.assertEqual(self.queued(b), [])

    def test_pose_latest_wins_in_a_stalled_outbox(self):
        server = self.memory_server()
        a, b, c = (self.stalled(server, name) for name in ("a", "b", "c"))
        self.drain(c)
        server.receive(a, pose(t=1))
        server.receive(b, pose(t=10))
        server.receive(a, grant(1))
        server.receive(a, pose(t=2))
        server.receive(a, pose(t=3, pos=[1, 2, 3]))
        got = self.queued(c)
        self.assertEqual([(p["type"], p.get("origin"), p.get("t")) for p in got],
                         [("pose", "a", 3), ("pose", "b", 10), ("bool_set", "a", None)])
        self.assertEqual(got[0]["pos"], [1, 2, 3])
        self.assertEqual(c.poses_coalesced, 2)
        self.assertEqual(c.queued_bytes, sum(len(entry[0]) for entry in c.outbox))
        self.drain(c)
        server.receive(a, pose(t=4))  # sent poses are not replaced: a new frame is queued
        self.assertEqual([p["t"] for p in self.queued(c)], [4])

    def test_stalled_peer_drops_poses_and_keeps_its_grant_session(self):
        server = self.memory_server()
        a, b = self.stalled(server, "a"), self.stalled(server, "b")
        self.drain(b)
        for seq in range(1, relay.MAX_POSE_QUEUE + 2):  # 65 queued grant events
            server.receive(a, grant(seq, key=seq))
            self.drain(a)  # a reads its acks; only b is stalled
        self.assertEqual(len(b.outbox), relay.MAX_POSE_QUEUE + 1)
        for t in range(5):
            server.receive(a, pose(t=t))
        self.assertEqual(len(b.outbox), relay.MAX_POSE_QUEUE + 1)
        self.assertEqual(b.poses_dropped, 5)
        self.assertFalse(b.failed)
        self.assertEqual(b.closed, [])
        # A queued pose can never push a client over the grant limit: it is evicted first.
        self.drain(b)
        server.receive(a, pose(t=9))
        for seq in range(100, 100 + relay.MAX_OUTBOX):
            server.receive(a, grant(seq, key=seq))
            self.drain(a)
        self.assertFalse(b.failed)
        self.assertEqual(len(b.outbox), relay.MAX_OUTBOX)
        self.assertTrue(all(p["type"] == "bool_set" for p in self.queued(b)))
        self.assertEqual(b.poses_dropped, 6)
        # The grant session still works once the peer drains.
        sent = self.drain(b)
        self.assertEqual(sent[-1]["seq"], 100 + relay.MAX_OUTBOX - 1)
        self.drain(a)
        server.receive(b, grant(1, origin="b", key=0xB0B0))
        self.assertEqual(self.queued(b), [{"type": "ack", "seq": 1}])
        self.assertEqual(self.queued(a), [dict(grant(1, origin="b", key=0xB0B0), rev=a.team.revision)])
        self.drain(a)
        server.receive(a, grant(500, key=500))
        self.assertEqual(self.queued(b)[-1]["seq"], 500)
        server.receive(a, pose(t=11))
        self.assertEqual(self.queued(b)[-1]["t"], 11)
        # The genuine slow-client rule still holds for grants once no pose is left to evict.
        self.drain(b)
        for seq in range(600, 600 + relay.MAX_OUTBOX + 1):
            server.receive(a, grant(seq, key=seq))
            self.drain(a)
        self.assertTrue(b.failed)

    def test_pose_never_in_snapshot_replay_or_join(self):
        server = self.memory_server()
        a, b = self.stalled(server, "a"), self.stalled(server, "b")
        server.receive(a, grant(1))
        server.receive(a, pose())
        late = self.stalled(server, "late")
        self.assertFalse(any(p["type"] == "pose" for p in self.queued(late)))
        snapshot = a.team.snapshot()
        self.assertEqual([p["type"] for p in snapshot["replay"]], ["bool_set"])
        self.assertNotIn("pose", json.dumps(snapshot))
        self.assertEqual(materialize(snapshot), {("bool_set", 1): {"type": "bool_set", "hash": 1, "value": True}})

    def test_pose_rate_cap_drops_without_disconnect(self):
        server = self.memory_server()
        a, b = self.stalled(server, "a"), self.stalled(server, "b")
        self.drain(b)
        for t in range(40):
            self.now = t * 0.001
            server.receive(a, pose(t=t))
        self.assertEqual(a.poses_rate_dropped, 40 - relay.POSE_RATE)
        self.assertEqual(b.poses_coalesced + len(b.outbox), relay.POSE_RATE)
        self.assertEqual(self.queued(b)[0]["t"], relay.POSE_RATE - 1)
        self.assertFalse(a.failed or b.failed)
        self.now = 0.5  # still inside the window of the first accepted pose
        server.receive(a, pose(t=50))
        self.assertEqual(a.poses_rate_dropped, 40 - relay.POSE_RATE + 1)
        self.now = 1.0  # the first accepted pose (t=0) has left the window
        server.receive(a, pose(t=51))
        self.assertEqual(self.queued(b)[0]["t"], 51)


class RelayTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.server = relay.Relay(heartbeat_interval=60, idle_timeout=120)
        self.port = await self.server.start(port=0)
        self.clients = []

    async def asyncTearDown(self):
        for client in self.clients:
            await client.close()
        await self.server.close()

    async def connect(self, client_id, **kwargs):
        client = client_sim.ClientSim(client_id, **kwargs)
        self.clients.append(client)
        return await client.connect("127.0.0.1", self.port)

    async def change(self, client, **values):
        packet = await client.change(values.pop("kind", "bool_set"), **values)
        self.assertEqual(await client.receive("ack"), {"type": "ack", "seq": packet["seq"]})
        return packet

    async def snapshot(self, client):
        await client.send({"type": "team_state_request"})
        return await client.receive("team_state")

    async def test_team_and_room_routing_without_echo(self):
        a, b = await self.connect("a"), await self.connect("b")
        others = [await self.connect("c", team="other"), await self.connect("d", room="other")]
        await self.change(a, hash=1, value=True)
        self.assertEqual((await b.receive("bool_set"))["hash"], 1)
        for client in [a, *others]:
            with self.assertRaises(TimeoutError):
                await client.receive("bool_set", timeout=0.03)
        for client in others:
            self.assertEqual(materialize(await self.snapshot(client)), {})

    async def test_bool_bit_or_duplicate_check(self):
        a, b = await self.connect("a"), await self.connect("b")
        for kind, key in (("bool_set", {"hash": 1}), ("bit_set", {"actor": (1 << 64) - 1})):
            for client, value in ((a, False), (a, True), (b, False), (b, True)):
                await self.change(client, kind=kind, **key, value=value)
        snapshot = await self.snapshot(a)
        self.assertEqual(len(snapshot["replay"]), 2)
        self.assertEqual(snapshot["seen"], {"a": 4, "b": 4})

    async def test_checkpoint_over_512(self):
        a = await self.connect("a")
        for index in range(520):
            await self.change(a, hash=index, value=True)
        b = await self.connect("b")
        self.assertEqual(b.snapshot["revision"], 8)
        self.assertEqual(len(b.snapshot["replay"]), 512)
        self.assertEqual(len(materialize(b.snapshot)), 520)

    async def test_reconnect(self):
        a = await self.connect("a")
        packet = await self.change(a, hash=1, value=True)
        await a.close()
        for _ in range(100):
            if not self.server.rooms["default"].teams["default"].clients:
                break
            await asyncio.sleep(0.001)
        await a.connect("127.0.0.1", self.port)
        await a.send(packet)
        await a.receive("ack")
        self.assertEqual(len((await self.snapshot(a))["replay"]), 1)
        self.assertEqual(a.seq, 1)

    async def test_handshake_refusals(self):
        await self.connect("a")
        for kwargs, reason in (({"build_id": "A" * 40}, "build id mismatch"),
                               ({"seed_hash": "1234"}, "seed hash mismatch"),
                               ({"policy_id": "other"}, "policy id mismatch"),
                               ({}, "already connected")):
            with self.assertRaisesRegex(ValueError, reason):
                await self.connect("a", **kwargs)

    async def test_presence_and_teardown(self):
        a, b = await self.connect("a"), await self.connect("b")
        await a.send({"type": "client_state", "state": {"scene": 1, "room": 2, "online": True}})
        while "scene" not in (await b.receive("client_state"))["state"]:
            pass
        await a.close()
        while True:
            packet = await b.receive("client_state")
            if packet["origin"] == "a" and packet["state"].get("online") is False:
                break
        self.assertEqual(packet["state"], {"online": False})

    async def test_heartbeat_idle_and_room_expiry(self):
        await self.server.close()
        self.server = relay.Relay(heartbeat_interval=0.02, idle_timeout=0.12,
                                  room_ttl=0.04, cleanup_interval=0.005)
        self.port = await self.server.start(port=0)
        a = await self.connect("a")
        b = await self.connect("b", room="idle", auto_heartbeat=False)
        await a.receive("heartbeat")
        with self.assertRaises(EOFError):
            while True:
                await b.receive(timeout=1)
        for _ in range(100):
            if "idle" not in self.server.rooms:
                break
            await asyncio.sleep(0.005)
        self.assertNotIn("idle", self.server.rooms)
        self.assertIn("default", self.server.rooms)

    async def test_preexisting_union(self):
        a, b = await self.connect("a"), await self.connect("b")
        for client, keys in ((a, (1, 2)), (b, (2, 3))):
            for key in keys:
                await self.change(client, hash=key, value=True)
            await self.snapshot(client)
        self.assertEqual(set(materialize(await self.snapshot(a))), {("bool_set", n) for n in (1, 2, 3)})

    async def test_unsupported_grants(self):
        for index, kind in enumerate(("pouch_add", "quest_step", "int_set", "enum_set")):
            client = await self.connect(str(index))
            await client.send({"type": kind})
            self.assertEqual((await client.receive("error"))["reason"], "unsupported message type")
        self.assertEqual(self.server.rooms["default"].teams["default"].current, {})

    async def test_invalid_and_spoofed_changes(self):
        for index, changes in enumerate(({"hash": True}, {"value": 1}, {"origin": "victim"},
                                         {"targetTeamId": "other"}, {"seq": relay.MAX_SEQ + 1})):
            client = await self.connect(f"bad{index}")
            await client.send(dict(grant(origin=f"bad{index}"), **changes))
            self.assertEqual((await client.receive("error"))["type"], "error")
        self.assertEqual(self.server.rooms["default"].teams["default"].current, {})

    async def test_bad_json_and_length(self):
        for data in (framed(b'{bad}'), framed(b'{"type":"hello","type":"hello"}'),
                     framed(b'{"type":"state","value":NaN}'), b'\x00\x00\x00\x00',
                     (relay.MAX_FRAME + 1).to_bytes(4, "big")):
            reader, writer = await asyncio.open_connection("127.0.0.1", self.port)
            try:
                writer.write(data)
                await writer.drain()
                response = relay.decode(await asyncio.wait_for(relay.read_frame(reader), 2))
                self.assertEqual(response["type"], "error")
            finally:
                writer.close()
                await writer.wait_closed()

    async def test_fragmented_and_coalesced_frames(self):
        reader, writer = await asyncio.open_connection("127.0.0.1", self.port)
        try:
            hello = relay.encode(client_sim.ClientSim("a").hello)
            for byte in hello:
                writer.write(bytes([byte]))
                await writer.drain()
            self.assertEqual(relay.decode(await relay.read_frame(reader))["type"], "hello_ack")
            self.assertEqual(relay.decode(await relay.read_frame(reader))["type"], "team_state")
            writer.write(relay.encode(grant()) + relay.encode({"type": "team_state_request"}))
            await writer.drain()
            self.assertEqual(relay.decode(await relay.read_frame(reader))["type"], "ack")
            snapshot = relay.decode(await relay.read_frame(reader))
            self.assertEqual(len(materialize(snapshot)), 1)
        finally:
            writer.close()
            await writer.wait_closed()

    async def test_runtime_deny_list(self):
        self.server.deny_list = frozenset({7})
        a = await self.connect("a")
        for kind, key in (("bool_set", {"hash": 7}), ("bit_set", {"actor": 7})):
            await self.change(a, kind=kind, **key, value=True)
        self.assertEqual(materialize(await self.snapshot(a)), {})

    async def test_v2_client_refused(self):
        client = client_sim.ClientSim("old")
        client.hello["version"] = 2
        self.clients.append(client)
        with self.assertRaisesRegex(ValueError, "protocol version mismatch"):
            await client.connect("127.0.0.1", self.port)

    async def test_pose_routing_origin_and_no_ack(self):
        a, b = await self.connect("a"), await self.connect("b")
        other = await self.connect("c", team="other")
        sent = await a.pose("Field_1", True, [1.5, -2, 3], -90.25, t=77)
        got = await b.receive_pose(since=0)
        self.assertEqual(got, dict(sent, origin="a"))
        self.assertEqual(list(got)[:2], ["type", "origin"])
        with self.assertRaises(TimeoutError):
            await a.receive("ack", timeout=0.05)
        self.assertEqual((a.pose_count, other.pose_count), (0, 0))
        self.assertEqual(materialize(await self.snapshot(b)), {})
        # The session is still a normal grant session afterwards.
        await self.change(a, hash=5, value=True)
        self.assertEqual((await b.receive("bool_set"))["hash"], 5)

    async def test_malformed_pose_errors_but_well_formed_flood_does_not(self):
        bad = await self.connect("bad")
        await bad.send(pose(banc="no spaces"))
        self.assertEqual((await bad.receive("error"))["reason"], "invalid pose banc")
        a, b = await self.connect("a"), await self.connect("b")
        a.writer.write(b"".join(relay.encode(pose(t=t)) for t in range(40)))
        await a.writer.drain()
        await a.send({"type": "heartbeat"})
        self.assertEqual(await a.receive("heartbeat_ack"), {"type": "heartbeat_ack"})
        server_a = next(c for c in self.server.clients if c.client_id == "a")
        self.assertEqual(server_a.poses_rate_dropped, 40 - relay.POSE_RATE)
        await b.send({"type": "heartbeat"})
        await b.receive("heartbeat_ack")
        self.assertLessEqual(b.pose_count, relay.POSE_RATE)
        self.assertGreater(b.pose_count, 0)
        self.assertEqual(b.poses[-1]["t"], relay.POSE_RATE - 1)
        # An oversized (whitespace-padded) pose frame is rejected on its wire size.
        big = await self.connect("big")
        big.writer.write(framed(json.dumps(pose(), indent=40).encode()))
        await big.writer.drain()
        self.assertEqual((await big.receive("error"))["reason"], "pose too large")


class FakePeerTests(unittest.IsolatedAsyncioTestCase):
    """The single-emulator fake peer: in-process relay plus a scripted peer (TCP loopback)."""

    async def asyncSetUp(self):
        self.peer = fake_peer.FakePeer(port=0, room="r", team="t", policy="p", wallet=WALLET)
        self.port = await self.peer.start()
        self.emu = client_sim.ClientSim("emu", room="r", team="t", policy_id="p")
        self.emu.hello["ghost_equipment_v2"] = True
        await self.emu.connect("127.0.0.1", self.port)

    async def asyncTearDown(self):
        await self.emu.close()
        await self.peer.close()

    async def test_records_the_emulator_broadcast(self):
        await self.emu.change("bool_set", hash=0xC1, value=True)
        packet = (await self.peer.command("expect c1 2"))["packet"]
        self.assertEqual((packet["hash"], packet["origin"], packet["rev"]), (0xC1, "emu", 1))
        self.assertEqual((await self.peer.command("expect c2 0.05")), {"timeout": True})

    async def test_burst_arrives_in_order_and_duplicates_add_nothing(self):
        result = await self.peer.command("grant a1 a2 a3 a4")
        self.assertEqual(result["sent"], [1, 2, 3, 4])
        got = [(await self.emu.receive("bool_set"))["hash"] for _ in range(4)]
        self.assertEqual(got, [0xA1, 0xA2, 0xA3, 0xA4])
        await self.peer.command("grant a2")
        with self.assertRaises(TimeoutError):
            await self.emu.receive("bool_set", timeout=0.05)
        state = (await self.peer.command("state"))["state"]
        self.assertEqual(len(materialize(state)), 4)

    async def test_rupee_burst_reaches_the_emulator_and_never_becomes_state(self):
        result = await self.peer.command("rupee 5 -2")
        self.assertEqual(result["sent"], [1, 2])
        got = [(await self.emu.receive("rupee_add"))["delta"] for _ in range(2)]
        self.assertEqual(got, [5, -2])
        state = (await self.peer.command("state"))["state"]
        self.assertEqual(state["changes"], [])
        self.assertEqual(materialize(state), {})
        with self.assertRaises(ValueError):
            await self.peer.command("rupee")

    async def test_snapshot_replays_grants_made_before_the_emulator_joined(self):
        late = client_sim.ClientSim("late", room="r", team="t", policy_id="p")
        await self.peer.command("grant b1 b2")
        await late.connect("127.0.0.1", self.port)
        try:
            self.assertEqual({p["hash"] for p in late.snapshot["replay"]}, {0xB1, 0xB2})
        finally:
            await late.close()

    async def test_pose_commands_both_ways(self):
        with tempfile.TemporaryDirectory() as directory:
            self.peer.record = Path(directory) / "received.jsonl"
            for t in range(16):
                await self.emu.pose("Field_1", t % 2 == 0, [t, 0, 0], 0, t=t + 1)
                await asyncio.sleep(0.04)  # under the relay's 30/s cap
            for _ in range(100):
                if self.peer.pose_count >= 16:
                    break
                await asyncio.sleep(0.01)
            result = await self.peer.command("poses 3")
            self.assertEqual(result["count"], 16)
            self.assertEqual([p["t"] for p in result["poses"]], [14, 15, 16])
            self.assertTrue(all(p["origin"] == "emu" for p in result["poses"]))
            lines = [json.loads(line) for line in self.peer.record.read_text().splitlines()]
            self.assertEqual([line["pose_n"] for line in lines if line["type"] == "pose"], [1, 16])
            self.peer.record = None
        sent = (await self.peer.command("pose - 0 1 2 3 45.5 1"))["sent"]
        got = await self.emu.receive_pose(since=0)
        self.assertEqual((got["banc"], got["child"], got["loading"], got["pos"], got["yaw"]),
                         ("", False, True, [1.0, 2.0, 3.0], 45.5))
        self.assertEqual(got["origin"], self.peer.client.hello["client_id"])
        self.assertEqual(sent["t"], got["t"])
        with self.assertRaises(ValueError):
            await self.peer.command("pose Field_1 1 2 3")
        with self.assertRaises(ValueError):  # refused locally, so the peer's session survives
            await self.peer.command("pose bad-banc 1 0 0 0 0")
        self.assertIn("state", await self.peer.command("state"))

    async def test_equipment_pose_command_and_sequence(self):
        count = 0
        for options, expected in (("eq32=0x10000081", 1), ("eq32=0x10100081", 1),
                                  ("eq32=0x100fffff", 1), ("eq32=0x10000081", 1),
                                  ("eq32=0x10000002", 1),
                                  ("eq32=0x10000082 eqseq=65535", 65535),
                                  ("eq32=0x10000083", 0)):
            sent = (await self.peer.command("pose Field_1 1 0 0 0 0 " + options))["sent"]
            got = await self.emu.receive_pose(since=count)
            count += 1
            self.assertEqual(sent["eqseq"], expected)
            self.assertEqual((got["eq32"], got["eqseq"]), (sent["eq32"], expected))
        for options in ("eqseq=1", "eq32=0", "eq32=0x10000000 eqseq=65536",
                        "eq32=0x10000000 eq32=0x10000000", "unknown=1"):
            with self.subTest(options=options), self.assertRaises(ValueError):
                await self.peer.command("pose Field_1 1 0 0 0 0 " + options)

    async def test_posecircle_stream_and_stop(self):
        result = await self.peer.command("posecircle Field_1 10 5 -20 100 4 20 0")
        self.assertIn("started", result)
        first = await self.emu.receive_pose(since=0)
        self.assertEqual((first["pos"], first["yaw"], first["child"], first["loading"]),
                         ([110.0, 5.0, -20.0], 0.0, True, False))  # at +X moving toward +Z
        for _ in range(200):
            if self.emu.pose_count >= 6:
                break
            await asyncio.sleep(0.01)
        stopped = await self.peer.command("posestop")
        self.assertTrue(stopped["stopped"])
        self.assertGreaterEqual(stopped["sent"], 6)
        poses = list(self.emu.poses)
        self.assertEqual([b["t"] - a["t"] for a, b in zip(poses, poses[1:])], [3] * (len(poses) - 1))
        for packet in poses:
            x, _, z = packet["pos"]
            self.assertAlmostEqual(math.hypot(x - 10, z + 20), 100, places=2)
            # Counter-clockwise in X/Z: travel is (-(z - cz), x - cx); yaw = atan2(dx, dz).
            self.assertAlmostEqual(packet["yaw"], math.degrees(math.atan2(-(z + 20), x - 10)), places=1)
        self.assertLess(poses[1]["yaw"], 0)  # past +X, heading toward -X as well as +Z
        count = self.emu.pose_count
        await asyncio.sleep(0.15)
        self.assertLessEqual(self.emu.pose_count - count, 1)  # at most one in flight
        self.assertEqual((await self.peer.command("posestop"))["stopped"], False)
        timed = await self.peer.command("posecircle - 0 0 0 1 1 30 0.1")
        self.assertEqual(timed["started"]["banc"], "")
        for _ in range(100):
            if self.peer.stream.done():
                break
            await asyncio.sleep(0.01)
        self.assertEqual((await self.peer.command("posestop"))["sent"], 3)
        with self.assertRaises(ValueError):
            await self.peer.command("posecircle Field_1 0 0 0 1 0 15 0")


if __name__ == "__main__":
    unittest.main(verbosity=2)
