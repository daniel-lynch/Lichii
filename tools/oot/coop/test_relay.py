#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run ProtocolTests without sockets; run the full suite in the external test lane."""

import asyncio
import importlib.util
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
fake_peer = load_sibling("fake_peer")


def framed(payload):
    return len(payload).to_bytes(4, "big") + payload


def grant(seq=1, *, origin="a", key=1, kind="bool_set", value=True):
    return {"type": kind, "origin": origin, "seq": seq,
            "hash" if kind == "bool_set" else "actor": key, "value": value}


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
        clock = mock.patch.object(relay.asyncio, "get_running_loop",
                                  return_value=SimpleNamespace(time=lambda: 0.0))
        clock.start()
        self.addCleanup(clock.stop)
        return relay.Relay()

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
        for changes in ({"version": True}, {"version": 1}, {"build_id": "F" * 39},
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
            return dict({"type": "rupee_add", "origin": "b", "seq": seq, "hash": relay.WALLET,
                         "delta": delta}, **extra)
        for bad in (rupee(1, 0), rupee(1, 1000), rupee(1, -1000), rupee(1, True), rupee(1, 2.5),
                    rupee(1, hash=1), rupee(1, value=True)):
            with self.subTest(bad=bad), self.assertRaises(relay.ProtocolError):
                relay.validate_change(bad, "b")
        team = relay.Team()
        team.accept(grant(1), set())
        self.assertEqual(team.accept(rupee(1), set())["rev"], 2)
        self.assertEqual(team.accept(rupee(2, -999), set())["rev"], 3)
        self.assertIsNone(team.accept(rupee(2), set()))  # duplicate sequence: no second +5
        self.assertIsNone(team.accept(rupee(3), {relay.WALLET}))
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


class FakePeerTests(unittest.IsolatedAsyncioTestCase):
    """The single-emulator fake peer: in-process relay plus a scripted peer (TCP loopback)."""

    async def asyncSetUp(self):
        self.peer = fake_peer.FakePeer(port=0, room="r", team="t", policy="p")
        self.port = await self.peer.start()
        self.emu = client_sim.ClientSim("emu", room="r", team="t", policy_id="p")
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


if __name__ == "__main__":
    unittest.main(verbosity=2)
