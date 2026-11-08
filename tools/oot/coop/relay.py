#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""LAN-only, in-memory OoT co-op relay. Protocol details: docs/oot-coop/PROTOCOL.md."""

import argparse
import asyncio
import contextlib
import json
import re
from collections import deque
from dataclasses import dataclass, field
from pathlib import Path

VERSION = 2
MAX_FRAME = 1024 * 1024  # JSON payload only; the 4-byte length is additional.
MAX_SEQ = (1 << 53) - 1
MAX_KEYS = 2048
MAX_SENDERS = 256
MAX_CLIENTS = 128
MAX_ROOMS = 32
MAX_TEAMS = 16
MAX_REPLAY = 512
MAX_OUTBOX = 256
MAX_OUTBOX_BYTES = 2 * MAX_FRAME
CHANGE_TYPES = {"bool_set", "bit_set", "rupee_add"}
RUPEE_LIMIT = 999


class ProtocolError(ValueError):
    pass


def integer(value, low, high):
    if type(value) is not int or not low <= value <= high:
        raise ProtocolError("integer out of range")
    return value


def identifier(value):
    if not isinstance(value, str) or not re.fullmatch(r"[A-Za-z0-9_.-]{1,64}", value):
        raise ProtocolError("invalid identifier")
    return value


def encode(packet):
    data = json.dumps(packet, separators=(",", ":"), allow_nan=False).encode("utf-8")
    if len(data) > MAX_FRAME:
        raise ProtocolError("frame too large")
    return len(data).to_bytes(4, "big") + data


async def read_frame(reader):
    try:
        header = await reader.readexactly(4)
    except asyncio.IncompleteReadError as exc:
        if not exc.partial:
            raise EOFError("relay disconnected") from exc
        raise ProtocolError("incomplete frame header") from exc
    size = int.from_bytes(header, "big")
    if not 1 <= size <= MAX_FRAME:
        raise ProtocolError("invalid frame length")
    try:
        return header + await reader.readexactly(size)
    except asyncio.IncompleteReadError as exc:
        raise ProtocolError("incomplete frame payload") from exc


def decode(data):
    if len(data) < 4 or not 1 <= int.from_bytes(data[:4], "big") <= MAX_FRAME or int.from_bytes(data[:4], "big") != len(data) - 4:
        raise ProtocolError("frame too large or incomplete")
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ProtocolError("duplicate field")
            result[key] = value
        return result
    def constant(_):
        raise ProtocolError("non-finite number")
    try:
        text = data[4:].decode("utf-8")
        depth, quoted, escaped = 0, False, False
        for char in text:
            if quoted:
                if escaped:
                    escaped = False
                elif char == "\\":
                    escaped = True
                elif char == '"':
                    quoted = False
            elif char == '"':
                quoted = True
            elif char in "[{":
                depth += 1
                if depth > 16:
                    raise ProtocolError("JSON nesting limit exceeded")
            elif char in "]}":
                depth -= 1
        packet = json.loads(text, object_pairs_hook=pairs, parse_constant=constant)
    except (ValueError, UnicodeError, RecursionError) as exc:
        raise ProtocolError("invalid JSON") from exc
    if not isinstance(packet, dict) or not isinstance(packet.get("type"), str):
        raise ProtocolError("expected typed object")
    return packet


def fields(packet, required, optional=()):
    if not set(required) <= packet.keys() or packet.keys() - set(required) - set(optional):
        raise ProtocolError("missing or unknown fields")


def validate_hello(packet):
    fields(packet, ("type", "version", "build_id", "seed_hash", "room", "team", "client_id", "name", "policy_id"))
    if packet["type"] != "hello" or type(packet["version"]) is not int or packet["version"] != VERSION:
        raise ProtocolError("protocol version mismatch")
    if not isinstance(packet["build_id"], str) or not re.fullmatch(r"[0-9A-F]{40}", packet["build_id"]):
        raise ProtocolError("invalid build id")
    if not isinstance(packet["seed_hash"], str) or not re.fullmatch(r"[0-9A-Fa-f]{1,128}", packet["seed_hash"]):
        raise ProtocolError("invalid seed hash")
    for key in ("room", "team", "client_id", "policy_id"):
        identifier(packet[key])
    name = packet["name"]
    if not isinstance(name, str) or not 1 <= len(name.encode("utf-8")) <= 96 or any(ord(c) < 32 for c in name):
        raise ProtocolError("invalid name")


def validate_change(packet, origin):
    kind = packet["type"]
    common = ("type", "origin", "seq")
    if kind not in CHANGE_TYPES:
        raise ProtocolError("unsupported grant class")
    extra = {"bool_set": ("hash", "value"), "bit_set": ("actor", "value"), "rupee_add": ("hash", "delta")}[kind]
    fields(packet, common + extra)
    if packet["origin"] != origin:
        raise ProtocolError("origin mismatch")
    integer(packet["seq"], 1, MAX_SEQ)
    for key in ("hash", "challenge", "step"):
        if key in packet:
            integer(packet[key], 0, (1 << 32) - 1)
    if kind in ("bool_set", "bit_set"):
        if type(packet["value"]) is not bool:
            raise ProtocolError("expected boolean")
    if kind == "bit_set":
        integer(packet["actor"], 0, (1 << 64) - 1)
    if kind == "rupee_add":
        # The wallet key is game-derived: each receiver admits only its private profile's wallet.
        if packet["hash"] == 0:
            raise ProtocolError("rupee_add needs a wallet key")
        if integer(packet["delta"], -RUPEE_LIMIT, RUPEE_LIMIT) == 0:
            raise ProtocolError("zero rupee delta")



def state_key(packet):
    return packet["type"], packet.get("hash", packet.get("actor"))


def fold(state, packet):
    if packet["type"] == "rupee_add":
        return  # a delta is an event in the replay window, never team state
    state[state_key(packet)] = {k: v for k, v in packet.items() if k not in ("origin", "seq", "rev")}


@dataclass
class Team:
    current: dict = field(default_factory=dict)
    checkpoint: dict = field(default_factory=dict)
    seen: dict = field(default_factory=dict)
    replay: deque = field(default_factory=deque)
    clients: dict = field(default_factory=dict)
    revision: int = 0
    checkpoint_revision: int = 0

    def accept(self, packet, deny):
        validate_change(packet, packet["origin"])
        origin, seq = packet["origin"], packet["seq"]
        if seq <= self.seen.get(origin, 0):
            return None
        key = state_key(packet)
        old = self.current.get(key)
        kind = packet["type"]
        accepted = key[1] not in deny
        if kind in ("bool_set", "bit_set"):
            accepted &= packet["value"] and old is None
        if accepted:
            if kind != "rupee_add" and old is None and len(self.current) >= MAX_KEYS:
                raise ProtocolError("team state capacity reached")
            if self.revision == MAX_SEQ:
                raise ProtocolError("team revision exhausted")
        self.seen[origin] = seq  # Also remember ignored false, denied and backward writes.
        if not accepted:
            return None
        self.revision += 1
        event = dict(packet, rev=self.revision)
        fold(self.current, event)
        self.replay.append(event)
        if len(self.replay) > MAX_REPLAY:
            oldest = self.replay.popleft()
            fold(self.checkpoint, oldest)
            self.checkpoint_revision = oldest["rev"]
        return event

    def snapshot(self):
        # seen is current, after replay and no-ops. Reconstruct checkpoint + replay first;
        # do not use these highwaters to discard the replay that accompanies this snapshot.
        return {"type": "team_state", "revision": self.checkpoint_revision,
                "changes": list(self.checkpoint.values()), "seen": dict(self.seen),
                "replay": list(self.replay)}


@dataclass
class Room:
    build_id: str
    seed_hash: str
    policy_id: str
    teams: dict = field(default_factory=dict)
    empty_since: float | None = None


class Client:
    def __init__(self, reader, writer):
        self.reader, self.writer = reader, writer
        self.outbox = asyncio.Queue(MAX_OUTBOX)
        self.queued_bytes = 0
        self.room = self.team = None
        self.client_id = None
        self.state = {}
        self.last_receive = asyncio.get_running_loop().time()
        self.last_heartbeat = self.last_receive
        self.failed = False

    def send(self, packet):
        if self.failed:
            return
        data = encode(packet)
        if self.outbox.full() or self.queued_bytes + len(data) > MAX_OUTBOX_BYTES:
            self.failed = True
            self.writer.close()
            return
        self.queued_bytes += len(data)
        self.outbox.put_nowait(data)

    async def write_loop(self):
        try:
            while True:
                data = await self.outbox.get()
                try:
                    self.writer.write(data)
                    await asyncio.wait_for(self.writer.drain(), 5)
                finally:
                    self.queued_bytes -= len(data)
                    self.outbox.task_done()
        except OSError:
            self.failed = True
            self.writer.close()


class Relay:
    def __init__(self, *, heartbeat_interval=30, idle_timeout=90, room_ttl=300,
                 cleanup_interval=1, deny_list=()):
        if min(heartbeat_interval, idle_timeout, room_ttl, cleanup_interval) <= 0:
            raise ValueError("timeouts must be positive")
        self.heartbeat_interval = heartbeat_interval
        self.idle_timeout = idle_timeout
        self.room_ttl = room_ttl
        self.cleanup_interval = cleanup_interval
        self.deny_list = frozenset(integer(x, 0, (1 << 64) - 1) for x in deny_list)
        self.rooms = {}
        self.clients = set()
        self.tasks = set()
        self.server = self.maintenance_task = None

    async def start(self, bind="127.0.0.1", port=43390):
        self.server = await asyncio.start_server(self.connected, bind, port, limit=MAX_FRAME)
        self.maintenance_task = asyncio.create_task(self.maintain())
        return self.server.sockets[0].getsockname()[1]

    def connected(self, reader, writer):
        if len(self.clients) >= MAX_CLIENTS:
            writer.close()
            return
        client = Client(reader, writer)
        self.clients.add(client)
        task = asyncio.create_task(self.handle(client))
        self.tasks.add(task)
        task.add_done_callback(self.tasks.discard)

    def join(self, client, packet):
        validate_hello(packet)
        room = self.rooms.get(packet["room"])
        if room is None:
            if len(self.rooms) >= MAX_ROOMS:
                raise ProtocolError("room capacity reached")
            room = Room(packet["build_id"], packet["seed_hash"], packet["policy_id"],
                        empty_since=asyncio.get_running_loop().time())
            self.rooms[packet["room"]] = room
        if room.build_id != packet["build_id"]:
            raise ProtocolError("build id mismatch")
        if room.seed_hash != packet["seed_hash"]:
            raise ProtocolError("seed hash mismatch")
        if room.policy_id != packet["policy_id"]:
            raise ProtocolError("policy id mismatch")
        team = room.teams.get(packet["team"])
        if team is None:
            if len(room.teams) >= MAX_TEAMS:
                raise ProtocolError("team capacity reached")
            team = Team()
            room.teams[packet["team"]] = team
        client_id = packet["client_id"]
        if client_id in team.clients:
            raise ProtocolError("client id already connected")
        if client_id not in team.seen and len(team.seen) >= MAX_SENDERS:
            raise ProtocolError("sender capacity reached")
        team.seen.setdefault(client_id, 0)
        client.client_id, client.room, client.team = client_id, room, team
        client.state = {"name": packet["name"]}
        room.empty_since = None
        team.clients[client_id] = client
        client.send({"type": "hello_ack", "version": VERSION, "build_id": room.build_id,
                     "seed_hash": room.seed_hash, "policy_id": room.policy_id, "client_id": client_id})
        client.send(team.snapshot())
        for peer in team.clients.values():
            if peer is not client:
                client.send({"type": "client_state", "origin": peer.client_id, "state": peer.state})
        self.broadcast(client, {"type": "client_state", "origin": client_id, "state": client.state})

    @staticmethod
    def broadcast(client, packet):
        for peer in tuple(client.team.clients.values()):
            if peer is not client:
                peer.send(packet)

    def receive(self, client, packet):
        kind = packet["type"]
        if kind in CHANGE_TYPES:
            validate_change(packet, client.client_id)
            event = client.team.accept(packet, self.deny_list)
            client.send({"type": "ack", "seq": packet["seq"]})
            if event is not None:
                self.broadcast(client, event)
        elif kind == "team_state_request":
            fields(packet, ("type",))
            client.send(client.team.snapshot())
        elif kind in ("heartbeat", "heartbeat_ack"):
            fields(packet, ("type",))
            if kind == "heartbeat":
                client.send({"type": "heartbeat_ack"})
        elif kind == "client_state":
            fields(packet, ("type", "state"))
            state = packet["state"]
            if not isinstance(state, dict) or len(encode(packet)) > 4096:
                raise ProtocolError("invalid client state")
            client.state = state
            self.broadcast(client, {"type": "client_state", "origin": client.client_id, "state": state})
        else:
            raise ProtocolError("unsupported message type")

    async def handle(self, client):
        sending = asyncio.create_task(client.write_loop())
        try:
            while not client.writer.is_closing():
                data = await asyncio.wait_for(read_frame(client.reader), self.idle_timeout)
                packet = decode(data)
                client.last_receive = asyncio.get_running_loop().time()
                if client.team is None:
                    self.join(client, packet)
                else:
                    self.receive(client, packet)
        except (ProtocolError, ValueError, UnicodeError, RecursionError) as exc:
            client.send({"type": "error", "reason": str(exc)[:160]})
            with contextlib.suppress(TimeoutError):
                await asyncio.wait_for(client.outbox.join(), 0.5)
        except (OSError, EOFError):
            pass
        finally:
            self.clients.discard(client)
            if client.team is not None:
                client.team.clients.pop(client.client_id, None)
                self.broadcast(client, {"type": "client_state", "origin": client.client_id,
                                        "state": {"online": False}})
                if not any(team.clients for team in client.room.teams.values()):
                    client.room.empty_since = asyncio.get_running_loop().time()
            sending.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await sending
            client.writer.close()
            with contextlib.suppress(OSError):
                await client.writer.wait_closed()

    async def maintain(self):
        while True:
            await asyncio.sleep(self.cleanup_interval)
            now = asyncio.get_running_loop().time()
            for client in tuple(self.clients):
                if now - client.last_receive >= self.idle_timeout:
                    client.writer.close()
                elif client.team is not None and now - client.last_heartbeat >= self.heartbeat_interval:
                    client.send({"type": "heartbeat"})
                    client.last_heartbeat = now
            for name, room in tuple(self.rooms.items()):
                if room.empty_since is not None and now - room.empty_since >= self.room_ttl:
                    del self.rooms[name]

    async def close(self):
        if self.server is not None:
            self.server.close()
            await self.server.wait_closed()
        if self.maintenance_task is not None:
            self.maintenance_task.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await self.maintenance_task
        for client in tuple(self.clients):
            client.writer.close()
        await asyncio.gather(*tuple(self.tasks), return_exceptions=True)


def load_deny_list(path):
    if path is None:
        return []
    with Path(path).open("rb") as source:
        data = source.read(MAX_FRAME + 1)
    if len(data) > MAX_FRAME:
        raise ValueError("deny-list file too large")
    values = json.loads(data)
    if not isinstance(values, list):
        raise ValueError("deny-list must be a JSON array of numeric hashes")
    return [integer(x, 0, (1 << 64) - 1) for x in values]


async def run(args):
    relay = Relay(deny_list=load_deny_list(args.deny_list))
    await relay.start(args.bind, args.port)
    print(f"OoT co-op relay listening on {args.bind}:{args.port} (LAN only; no authentication)", flush=True)
    try:
        await relay.server.serve_forever()
    finally:
        await relay.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=43390)
    parser.add_argument("--deny-list", help="user JSON array of opaque numeric hashes")
    args = parser.parse_args()
    try:
        asyncio.run(run(args))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
