#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run directly with python3 -I tools/oot/coop/test_relay.py."""

import asyncio
import importlib.util
import json
from pathlib import Path
import sys
import unittest
from types import SimpleNamespace
from unittest import mock

sys.dont_write_bytecode = True


def load_sibling(name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(name + ".py"))
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


relay = load_sibling("relay")
client_sim = load_sibling("client_sim")


class ProtocolTests(unittest.TestCase):
    def test_encode_decode_uint64_and_boundaries(self):
        packet = {"type": "bit_set", "origin": "a", "seq": relay.MAX_SEQ,
                  "actor": (1 << 64) - 1, "value": True}
        self.assertEqual(relay.decode(relay.encode(packet)), packet)
        relay.validate_change(packet, "a")
        with self.assertRaises(relay.ProtocolError):
            relay.encode({"type": "client_state", "state": "x" * relay.MAX_FRAME})

    def test_validation_rejects_duplicate_nonfinite_and_wrong_types(self):
        for data in (b'{"type":"heartbeat","type":"heartbeat"}\n',
                     b'{"type":"client_state","state":NaN}\n', b'[]\n',
                     b'{"type":"heartbeat"}', b'{"type":7}\n'):
            with self.subTest(data=data), self.assertRaises(relay.ProtocolError):
                relay.decode(data)
        for depth in (15, 16):
            data = b'{"type":"client_state","state":' + b'[' * depth + b'0' + b']' * depth + b'}\n'
            if depth == 15:
                self.assertEqual(relay.decode(data)["type"], "client_state")
            else:
                with self.assertRaises(relay.ProtocolError):
                    relay.decode(data)
        # Brackets and escaped quotes inside strings are not nesting.
        escaped = {"type": "client_state", "state": {"text": '["' * 30}}
        self.assertEqual(relay.decode(relay.encode(escaped)), escaped)
        base = {"type": "pouch_add", "origin": "a", "seq": 1, "hash": 5, "count": 1}
        for changes in ({"origin": "b"}, {"seq": 0}, {"hash": True}, {"count": 0},
                        {"count": 1 << 31}, {"seq": relay.MAX_SEQ + 1}, {"target": "other"}):
            with self.subTest(changes=changes), self.assertRaises(relay.ProtocolError):
                relay.validate_change(dict(base, **changes), "a")

    def test_handshake_validation(self):
        packet = client_sim.ClientSim("a").hello
        relay.validate_hello(packet)
        for changes in ({"version": True}, {"version": 2}, {"build_id": "F" * 39},
                        {"seed_hash": 0}, {"room": "bad room"}, {"name": "\n"}):
            with self.subTest(changes=changes), self.assertRaises(relay.ProtocolError):
                relay.validate_hello(dict(packet, **changes))

    def test_monotonic_bool_bit_and_quest(self):
        team = relay.Team()
        seq = 0
        def apply(kind, **values):
            nonlocal seq
            seq += 1
            return team.accept({"type": kind, "origin": "a", "seq": seq, **values}, set())
        for kind, values in (("bool_set", {"hash": 1}), ("bit_set", {"actor": (1 << 64) - 1})):
            self.assertIsNone(apply(kind, **values, value=False))
            self.assertIsNotNone(apply(kind, **values, value=True))
            self.assertIsNone(apply(kind, **values, value=False))
            self.assertIsNone(apply(kind, **values, value=True))
        self.assertIsNotNone(apply("quest_step", challenge=1, step=100, index=5))
        self.assertIsNone(apply("quest_step", challenge=1, step=999, index=4))
        self.assertIsNone(apply("quest_step", challenge=1, step=999, index=5))
        self.assertIsNotNone(apply("quest_step", challenge=1, step=2, index=6))
        self.assertEqual(team.current[("quest_step", 1)]["step"], 2)
        self.assertEqual(team.seen["a"], seq)

    def test_arrival_order_and_deny_list(self):
        team = relay.Team()
        for kind in ("int_set", "enum_set"):
            for seq, value in enumerate((10, -5, -5), start=1):
                origin = kind
                event = team.accept({"type": kind, "origin": origin, "seq": seq,
                                     "hash": 1, "value": value}, set())
                self.assertEqual(event is None, seq == 3)
            self.assertEqual(team.current[(kind, 1)]["value"], -5)
        self.assertIsNone(team.accept({"type": "bool_set", "origin": "denied", "seq": 1,
                                      "hash": 7, "value": True}, {7}))
        self.assertEqual(team.seen["denied"], 1)
        self.assertNotIn(("bool_set", 7), team.current)

    def test_pouch_dedup_and_noop_highwater(self):
        team = relay.Team()
        event = {"type": "pouch_add", "origin": "a", "seq": 1, "hash": 3, "count": 2}
        team.accept(event, set())
        self.assertIsNone(team.accept(event, set()))
        team.accept(dict(event, origin="b", count=4), set())
        team.accept({"type": "bool_set", "origin": "a", "seq": 3, "hash": 4, "value": False}, set())
        self.assertIsNone(team.accept(dict(event, seq=2, count=100), set()))
        self.assertEqual(team.current[("pouch_total", 3)]["count"], 6)
        self.assertEqual(team.snapshot()["seen"], {"a": 3, "b": 1})

    def test_snapshot_compaction_replays_without_losing_deltas(self):
        team = relay.Team()
        for seq in range(1, 601):
            team.accept({"type": "pouch_add", "origin": "a", "seq": seq, "hash": 7, "count": 2}, set())
        snapshot = team.snapshot()
        self.assertEqual(snapshot["revision"], 88)
        self.assertEqual(snapshot["changes"], [{"type": "pouch_total", "hash": 7, "count": 176}])
        self.assertEqual(len(snapshot["replay"]), 512)
        reconstructed = {relay.state_key(p): p for p in snapshot["changes"]}
        for index, packet in enumerate(snapshot["replay"], start=89):
            self.assertEqual(packet["rev"], index)
            relay.fold(reconstructed, packet)
        self.assertEqual(reconstructed, team.current)
        self.assertEqual(snapshot["seen"], {"a": 600})

    def test_pouch_overflow_is_transactional(self):
        team = relay.Team()
        event = {"type": "pouch_add", "origin": "a", "seq": 1, "hash": 3, "count": (1 << 31) - 1}
        team.accept(event, set())
        # A long-lived team can have many individually valid signed32 additions.
        total = {"type": "pouch_total", "hash": 3, "count": relay.MAX_SEQ}
        team.current[("pouch_total", 3)] = total.copy()
        team.checkpoint[("pouch_total", 3)] = total.copy()
        team.checkpoint_revision = team.revision
        team.replay.clear()
        before = relay.encode(team.snapshot())
        with self.assertRaisesRegex(relay.ProtocolError, "overflow"):
            team.accept(dict(event, seq=2, count=1), set())
        self.assertEqual(relay.encode(team.snapshot()), before)

    def test_state_capacity_and_frame_budget(self):
        team = relay.Team()
        for number in range(relay.MAX_KEYS):
            team.accept({"type": "bool_set", "origin": "a" * 64, "seq": number + 1,
                         "hash": number, "value": True}, set())
        with self.assertRaisesRegex(relay.ProtocolError, "capacity"):
            team.accept({"type": "bool_set", "origin": "a" * 64, "seq": relay.MAX_KEYS + 1,
                         "hash": relay.MAX_KEYS, "value": True}, set())
        self.assertLess(len(relay.encode(team.snapshot())), relay.MAX_FRAME)


    def memory_server(self):
        # Dispatcher coverage only: deliberately no socket substitution for the TCP tests.
        clock = mock.patch.object(relay.asyncio, "get_running_loop",
                                  return_value=SimpleNamespace(time=lambda: 0.0))
        clock.start()
        self.addCleanup(clock.stop)
        return relay.Relay()

    @staticmethod
    def memory_peer(server, client_id, **kwargs):
        messages = []
        peer = SimpleNamespace(send=messages.append, messages=messages)
        server.join(peer, client_sim.ClientSim(client_id, **kwargs).hello)
        return peer

    def test_dispatcher_routing_snapshot_presence_and_origin(self):
        server = self.memory_server()
        a = self.memory_peer(server, "a")
        b = self.memory_peer(server, "b")
        c = self.memory_peer(server, "c", team="other")
        d = self.memory_peer(server, "d", room="other")
        for peer in (a, b, c, d):
            peer.messages.clear()
        packet = {"type": "bool_set", "origin": "a", "seq": 1, "hash": 4, "value": True}
        server.receive(a, packet)
        self.assertEqual(a.messages, [{"type": "ack", "seq": 1}])
        self.assertEqual(b.messages, [dict(packet, rev=1)])
        self.assertEqual(c.messages, [])
        self.assertEqual(d.messages, [])
        server.receive(a, {"type": "client_state", "state": {"ready": True}})
        self.assertEqual(b.messages[-1], {"type": "client_state", "origin": "a", "state": {"ready": True}})
        server.receive(b, {"type": "team_state_request"})
        self.assertEqual(b.messages[-1]["replay"], [dict(packet, rev=1)])
        with self.assertRaisesRegex(relay.ProtocolError, "origin mismatch"):
            server.receive(b, packet)
        server.receive(a, {"type": "heartbeat"})
        self.assertEqual(a.messages[-1], {"type": "heartbeat_ack"})

    def test_dispatcher_handshake_refusals_and_reconnect_dedup(self):
        server = self.memory_server()
        a = self.memory_peer(server, "a")
        for kwargs, error in (({"build_id": "A" * 40, "team": "other"}, "build id mismatch"),
                              ({"seed_hash": "1234"}, "seed hash mismatch"),
                              ({}, "already connected")):
            with self.subTest(kwargs=kwargs), self.assertRaisesRegex(relay.ProtocolError, error):
                self.memory_peer(server, "a", **kwargs)
        packet = {"type": "pouch_add", "origin": "a", "seq": 1, "hash": 4, "count": 3}
        server.receive(a, packet)
        del a.team.clients["a"]
        again = self.memory_peer(server, "a")
        self.assertEqual(again.messages[1]["seen"], {"a": 1})
        server.receive(again, packet)
        self.assertEqual(again.team.current[("pouch_total", 4)]["count"], 3)
        self.assertEqual(len(again.team.replay), 1)

    def test_dispatcher_join_fits_outbox_at_max_clients(self):
        server = self.memory_server()
        for index in range(relay.MAX_CLIENTS - 1):
            self.memory_peer(server, str(index))
        last = self.memory_peer(server, "last")
        self.assertEqual(len(last.messages), relay.MAX_CLIENTS + 1)
        self.assertLessEqual(len(last.messages), relay.MAX_OUTBOX)
        self.assertLessEqual(sum(len(relay.encode(p)) for p in last.messages), relay.MAX_OUTBOX_BYTES)

    def test_outbox_bounds_disconnect_slow_client(self):
        self.memory_server()
        closed = []
        client = relay.Client(None, SimpleNamespace(close=lambda: closed.append(True)))
        for _ in range(relay.MAX_OUTBOX):
            client.send({"type": "heartbeat"})
        self.assertFalse(client.failed)
        client.send({"type": "heartbeat"})
        self.assertTrue(client.failed)
        self.assertEqual(closed, [True])
        self.assertEqual(client.outbox.qsize(), relay.MAX_OUTBOX)


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
        await client.connect("127.0.0.1", self.port)
        return client

    async def change(self, client, kind, **values):
        packet = await client.change(kind, **values)
        self.assertEqual(await client.receive("ack"), {"type": "ack", "seq": packet["seq"]})
        return packet

    async def snapshot(self, client):
        await client.send({"type": "team_state_request"})
        return await client.receive("team_state")

    def materialize(self, snapshot):
        state = {}
        for packet in snapshot["changes"]:
            state[relay.state_key(packet)] = packet
        previous = snapshot["revision"]
        for packet in snapshot["replay"]:
            self.assertEqual(packet["rev"], previous + 1)
            previous = packet["rev"]
            relay.fold(state, packet)
        return state

    async def test_team_routing_and_no_echo(self):
        a = await self.connect("a")
        b = await self.connect("b")
        c = await self.connect("c", team="other")
        await self.change(a, "bool_set", hash=100, value=True)
        self.assertEqual((await b.receive("bool_set"))["hash"], 100)
        for client in (a, c):
            with self.assertRaises(TimeoutError):
                await client.receive("bool_set", timeout=0.03)
        self.assertEqual(self.materialize(await self.snapshot(c)), {})

    async def test_room_routing(self):
        a = await self.connect("a")
        b = await self.connect("b", room="other")
        await self.change(a, "bool_set", hash=100, value=True)
        self.assertEqual(self.materialize(await self.snapshot(b)), {})

    async def test_bool_and_bit_or_preserves_uint64(self):
        a = await self.connect("a")
        b = await self.connect("b")
        actor = (1 << 64) - 1
        for kind, key in (("bool_set", {"hash": 101}), ("bit_set", {"actor": actor})):
            await self.change(a, kind, **key, value=False)
            await self.change(a, kind, **key, value=True)
            await self.change(b, kind, **key, value=False)
            await self.change(b, kind, **key, value=True)
        snapshot = await self.snapshot(a)
        self.assertEqual(len(snapshot["replay"]), 2)
        self.assertTrue(self.materialize(snapshot)[("bit_set", actor)]["value"])
        self.assertEqual(snapshot["seen"], {"a": 4, "b": 4})

    async def test_quest_forward_only_by_index(self):
        a = await self.connect("a")
        b = await self.connect("b")
        await self.change(a, "quest_step", challenge=100, step=900, index=3)
        await self.change(b, "quest_step", challenge=100, step=999, index=2)
        await self.change(b, "quest_step", challenge=100, step=888, index=3)
        await self.change(a, "quest_step", challenge=100, step=1, index=4)
        state = self.materialize(await self.snapshot(b))
        self.assertEqual(state[("quest_step", 100)]["step"], 1)
        self.assertEqual(state[("quest_step", 100)]["index"], 4)

    async def test_int_enum_last_arrival(self):
        a = await self.connect("a")
        b = await self.connect("b")
        for kind in ("int_set", "enum_set"):
            await self.change(a, kind, hash=42, value=100)
            await self.change(b, kind, hash=42, value=-5)
            await self.change(a, kind, hash=42, value=-5)
        snapshot = await self.snapshot(a)
        self.assertEqual(len(snapshot["replay"]), 4)
        state = self.materialize(snapshot)
        self.assertEqual(state[("int_set", 42)]["value"], -5)
        self.assertEqual(state[("enum_set", 42)]["value"], -5)

    async def test_pouch_dedup_and_sender_highwater(self):
        a = await self.connect("a")
        b = await self.connect("b")
        packet = await self.change(a, "pouch_add", hash=7, count=2)
        await a.send(packet)
        self.assertEqual((await a.receive("ack"))["seq"], 1)
        await self.change(b, "pouch_add", hash=7, count=3)
        await self.change(a, "bool_set", hash=8, value=False)
        await a.send(dict(packet, seq=2, count=99))
        await a.receive("ack")
        state = self.materialize(await self.snapshot(a))
        self.assertEqual(state[("pouch_total", 7)]["count"], 5)

    async def test_checkpoint_and_replay_over_512(self):
        a = await self.connect("a")
        for _ in range(520):
            await self.change(a, "pouch_add", hash=7, count=1)
        b = await self.connect("b")
        snapshot = b.snapshot
        self.assertEqual(snapshot["revision"], 8)
        self.assertEqual(len(snapshot["replay"]), 512)
        self.assertEqual(snapshot["changes"], [{"type": "pouch_total", "hash": 7, "count": 8}])
        self.assertEqual(self.materialize(snapshot)[("pouch_total", 7)]["count"], 520)
        self.assertEqual(snapshot["seen"]["a"], 520)
        self.assertEqual(await self.snapshot(b), snapshot)

    async def test_reconnect_keeps_dedup_and_state(self):
        a = await self.connect("a")
        packet = await self.change(a, "pouch_add", hash=7, count=2)
        await a.close()
        for _ in range(100):
            if not self.server.rooms["default"].teams["default"].clients:
                break
            await asyncio.sleep(0.001)
        await a.connect("127.0.0.1", self.port)
        await a.send(packet)
        await a.receive("ack")
        self.assertEqual(self.materialize(await self.snapshot(a))[("pouch_total", 7)]["count"], 2)
        self.assertEqual(a.seq, 1)

    async def test_build_seed_and_duplicate_identity_refusal(self):
        await self.connect("a")
        for kwargs, reason in (({"build_id": "A" * 40, "team": "other"}, "build id mismatch"),
                               ({"seed_hash": "1234"}, "seed hash mismatch"),
                               ({}, "client id already connected")):
            with self.assertRaisesRegex(ValueError, reason):
                await self.connect("a", **kwargs)

    async def test_client_state_cached_and_scoped(self):
        a = await self.connect("a")
        b = await self.connect("b")
        c = await self.connect("c", team="other")
        await a.send({"type": "client_state", "state": {"name": "Sam", "ready": True}})
        # The first notification is the initial join state.
        while True:
            packet = await b.receive("client_state")
            if packet["state"].get("ready"):
                break
        self.assertEqual(packet["origin"], "a")
        with self.assertRaises(TimeoutError):
            await c.receive("client_state", timeout=0.03)
        d = await self.connect("d")
        self.assertEqual((await d.receive("client_state"))["state"], {"name": "Sam", "ready": True})

    async def test_heartbeat_idle_and_room_cleanup(self):
        await self.server.close()
        self.server = relay.Relay(heartbeat_interval=0.02, idle_timeout=0.12,
                                  room_ttl=0.04, cleanup_interval=0.005)
        self.port = await self.server.start(port=0)
        a = await self.connect("a")
        b = await self.connect("b", room="idle", auto_heartbeat=False)
        await a.send({"type": "heartbeat"})
        self.assertEqual(await a.receive("heartbeat_ack"), {"type": "heartbeat_ack"})
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
        self.assertEqual((await self.snapshot(a))["type"], "team_state")

    async def test_runtime_deny_list(self):
        self.server.deny_list = frozenset({7, (1 << 64) - 1})
        a = await self.connect("a")
        for kind, fields in (("bool_set", {"hash": 7, "value": True}),
                             ("quest_step", {"challenge": 7, "step": 8, "index": 4}),
                             ("bit_set", {"actor": (1 << 64) - 1, "value": True}),
                             ("pouch_add", {"hash": 7, "count": 1})):
            await self.change(a, kind, **fields)
        snapshot = await self.snapshot(a)
        self.assertEqual(self.materialize(snapshot), {})
        self.assertEqual(snapshot["seen"]["a"], 4)

    async def test_invalid_changes_and_spoofed_origin(self):
        bad_values = [
            {"type": "bool_set", "hash": True, "value": True},
            {"type": "bool_set", "hash": 1, "value": 1},
            {"type": "bit_set", "actor": 1 << 64, "value": True},
            {"type": "pouch_add", "hash": 1, "count": -1},
            {"type": "quest_step", "challenge": 1, "step": 2, "index": 256},
            {"type": "int_set", "hash": 1, "value": 1 << 31},
            {"type": "bool_set", "hash": 1, "value": True, "targetTeamId": "other"},
            {"type": "bool_set", "hash": 1, "value": True, "origin": "victim"},
            {"type": "bool_set", "hash": 1, "value": True, "seq": relay.MAX_SEQ + 1},
        ]
        for index, values in enumerate(bad_values):
            with self.subTest(values=values):
                client = await self.connect(f"bad{index}")
                await client.send({"origin": client.hello["client_id"], "seq": 1, **values})
                self.assertEqual((await client.receive("error"))["type"], "error")
                self.assertEqual(self.server.rooms["default"].teams["default"].current, {})

    async def test_bad_json_duplicate_fields_and_oversized_frame(self):
        for data in (b'{bad}\n', b'{"type":"hello","type":"hello"}\n',
                     b'{"type":"client_state","state":{"x":NaN}}\n',
                     b"x" * (relay.MAX_FRAME + 1) + b"\n"):
            with self.subTest(size=len(data)):
                reader, writer = await asyncio.open_connection("127.0.0.1", self.port)
                try:
                    writer.write(data)
                    await writer.drain()
                    response = json.loads(await asyncio.wait_for(reader.readline(), 2))
                    self.assertEqual(response["type"], "error")
                finally:
                    writer.close()
                    await writer.wait_closed()

    async def test_pouch_count_range_and_state_capacity(self):
        a = await self.connect("a")
        await self.change(a, "pouch_add", hash=1, count=(1 << 31) - 1)
        await a.change("pouch_add", hash=1, count=1 << 31)
        self.assertIn("out of range", (await a.receive("error"))["reason"])
        team = relay.Team()
        for number in range(relay.MAX_KEYS):
            team.accept({"type": "bool_set", "origin": "a", "seq": number + 1,
                         "hash": number, "value": True}, set())
        with self.assertRaisesRegex(relay.ProtocolError, "capacity"):
            team.accept({"type": "bool_set", "origin": "a", "seq": relay.MAX_KEYS + 1,
                         "hash": relay.MAX_KEYS, "value": True}, set())
        self.assertLess(len(relay.encode(team.snapshot())), relay.MAX_FRAME)


if __name__ == "__main__":
    unittest.main(verbosity=2)
