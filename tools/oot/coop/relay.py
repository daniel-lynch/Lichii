#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""LAN-only, in-memory OoT co-op relay. Protocol details: docs/oot-coop/PROTOCOL.md."""

import argparse
import asyncio
import contextlib
import json
import math
import re
from collections import deque
from dataclasses import dataclass, field
from pathlib import Path

VERSION = 4
LEGACY_VERSION = 3
MAX_FRAME = 1024 * 1024  # JSON payload only; the 4-byte length is additional.
MAX_SEQ = (1 << 53) - 1
MAX_KEYS = 2048
MAX_ORIGIN_RECEIPTS = 4096
MAX_SENDERS = 256
MAX_CLIENTS = 128
MAX_ROOMS = 32
MAX_TEAMS = 16
MAX_REPLAY = 512
MAX_OUTBOX = 256
MAX_OUTBOX_BYTES = 2 * MAX_FRAME
CHANGE_TYPES = {"bool_set", "bit_set", "rupee_add", "item_give", "key_add"}
EVENT_TYPES = {"rupee_add", "item_give", "key_add"}  # additive events: replay window only, never team state
RUPEE_LIMIT = 999
KEY_GAIN_LIMIT = 32
ITEM_COUNT_LIMIT = 999
POSE_FIELDS = ("type", "t", "banc", "child", "loading", "pos", "yaw", "anim", "anim_seq", "rate", "phase")
EQUIPMENT_FIELDS = ("eq32", "eqseq")
POSE_ANIMS = ("idle", "walk", "run")  # an animation hint inferred from speed; receivers may ignore it
ANIM_POSE_FIELDS = ("av", "aid", "af", "ar", "aseq")
MAX_POSE_FRAME = 512  # JSON payload, including origin when forwarded (animation and equipment fields included)
MAX_POSE_COORD = 100000
MAX_POSE_QUEUE = 64  # a receiver whose outbox holds more frames than this gets no new pose
POSE_RATE = 30  # poses per sender per second (sliding 1 s window); the excess is dropped


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


def validate_wallet(wallet):
    if type(wallet) is not dict:
        raise ProtocolError("invalid world wallet")
    fields(wallet, ("balance", "capacity"))
    capacity = integer(wallet["capacity"], 0, (1 << 31) - 1)
    integer(wallet["balance"], 0, capacity)


ORIGIN_FIELDS = ("origin_kind", "origin_key", "origin_key64", "origin_partner")


def collectible_origin(packet):
    """Validate the tagged union; legacy chest packets keep their u32 origin_key."""
    kind = packet.get("origin_kind", "chest" if "origin_key" in packet else "none")
    keys = set(packet) & set(ORIGIN_FIELDS[1:])
    if not isinstance(kind, str) or kind not in ("none", "chest", "bit", "token"):
        raise ProtocolError("invalid origin kind")
    expected = {"none": set(), "chest": {"origin_key"}, "bit": {"origin_key64"},
                "token": {"origin_key64", "origin_partner"}}[kind]
    if keys != expected:
        raise ProtocolError("invalid origin fields")
    if kind == "none":
        return None
    if kind == "chest":
        return kind, integer(packet["origin_key"], 1, (1 << 32) - 1)
    key = integer(packet["origin_key64"], 1, (1 << 64) - 1)
    if kind == "bit":
        return kind, key
    partner = integer(packet["origin_partner"], 1, (1 << 64) - 1)
    if partner == key:
        raise ProtocolError("invalid token partner")
    return kind, key, partner


def validate_hello(packet):
    fields(packet, ("type", "version", "build_id", "seed_hash", "room", "team", "client_id", "name", "policy_id"),
           ("world_wallet", "animation", "ghost_equipment_v2"))
    if "ghost_equipment_v2" in packet and type(packet["ghost_equipment_v2"]) is not bool:
        raise ProtocolError("expected equipment capability boolean")
    if packet["type"] != "hello" or type(packet["version"]) is not int or packet["version"] not in (LEGACY_VERSION, VERSION):
        raise ProtocolError("protocol version mismatch")
    if "animation" in packet:
        if packet["version"] != VERSION:
            raise ProtocolError("animation needs protocol v4")
        validate_animation(packet["animation"])
    if not isinstance(packet["build_id"], str) or not re.fullmatch(r"[0-9A-F]{40}", packet["build_id"]):
        raise ProtocolError("invalid build id")
    if not isinstance(packet["seed_hash"], str) or not re.fullmatch(r"[0-9A-Fa-f]{1,128}", packet["seed_hash"]):
        raise ProtocolError("invalid seed hash")
    for key in ("room", "team", "client_id", "policy_id"):
        identifier(packet[key])
    name = packet["name"]
    if not isinstance(name, str) or not 1 <= len(name.encode("utf-8")) <= 96 or any(ord(c) < 32 for c in name):
        raise ProtocolError("invalid name")
    if "world_wallet" in packet:
        validate_wallet(packet["world_wallet"])


def validate_animation(value):
    if type(value) is not dict:
        raise ProtocolError("invalid animation negotiation")
    fields(value, ("av", "table_sha256"))
    integer(value["av"], 1, 1)
    if not isinstance(value["table_sha256"], str) or not re.fullmatch(r"[0-9a-f]{64}", value["table_sha256"]):
        raise ProtocolError("invalid animation table hash")


def validate_change(packet, origin):
    kind = packet["type"]
    common = ("type", "origin", "seq")
    if kind not in CHANGE_TYPES:
        raise ProtocolError("unsupported grant class")
    extra = {"bool_set": ("hash", "value"), "bit_set": ("actor", "value"), "rupee_add": ("hash", "delta"),
             "item_give": ("name", "category", "count", "get", "flags"), "key_add": ("hash", "delta")}[kind]
    chest_fields = ("chest_transaction", "chest_part", "chest_final")
    optional = ORIGIN_FIELDS if kind in ("item_give", "bit_set") else ()
    if kind == "item_give":
        optional += chest_fields
    fields(packet, common + extra, optional)
    source = collectible_origin(packet)
    if any(field in packet for field in chest_fields):
        if source is None or source[0] != "chest" or not all(field in packet for field in chest_fields):
            raise ProtocolError("chest receipt requires a chest and all transaction fields")
        integer(packet["chest_transaction"], 1, MAX_SEQ)
        integer(packet["chest_part"], 0, 63)
        if type(packet["chest_final"]) is not bool:
            raise ProtocolError("chest completion must be boolean")
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
        if source is not None and (source[0] not in ("bit", "token") or
                                   packet["actor"] not in source[1:]):
            raise ProtocolError("bit flag does not belong to origin")
    if kind == "rupee_add":
        # The wallet key is game-derived: each receiver admits only its private profile's wallet.
        if packet["hash"] == 0:
            raise ProtocolError("rupee_add needs a wallet key")
        if integer(packet["delta"], -RUPEE_LIMIT, RUPEE_LIMIT) == 0:
            raise ProtocolError("zero rupee delta")
    if kind == "key_add":
        # A dungeon's small-key gain (C13): the hash names the dungeon's total-keys key (private).
        if packet["hash"] == 0:
            raise ProtocolError("key_add needs a dungeon key")
        integer(packet["delta"], 1, KEY_GAIN_LIMIT)
    if kind == "item_give":
        # The game's own give call, as the opener made it: an event, applied by each receiver.
        if not isinstance(packet["name"], str) or not re.fullmatch(r"[A-Za-z0-9_]{1,63}", packet["name"]):
            raise ProtocolError("invalid item name")
        integer(packet["category"], 0, 15)
        integer(packet["count"], 1, ITEM_COUNT_LIMIT)
        integer(packet["get"], 0, 15)
        flags = packet["flags"]
        if type(flags) is not list or len(flags) != 4:
            raise ProtocolError("item flags are four bytes")
        for flag in flags:
            integer(flag, 0, 255)


def finite(value, limit):
    if type(value) not in (int, float) or abs(value) > limit or not math.isfinite(value):
        raise ProtocolError("invalid pose number")
    return value


def validate_equipment(word, child):
    integer(word, 0, (1 << 32) - 1)
    if word >> 28 != 1 or word & 0x0FE00000:
        raise ProtocolError("equipment version/reserved bits")
    if (word & 7) not in (0, 1, 2, 3, 7) or ((word >> 3) & 7) not in (0, 1, 2, 3, 7):
        raise ProtocolError("invalid equipment identity")
    if ((word >> 14) & 15) not in (*range(11), 15):
        raise ProtocolError("invalid equipment mask")
    age = (word >> 18) & 3
    if age == 2 or (age != 3 and age != (0 if child else 1)):
        raise ProtocolError("invalid equipment age")


def validate_pose(packet, size=None):
    """A live position for drawing a ghost; never a grant. Malformed poses are protocol errors."""
    fields(packet, POSE_FIELDS, ANIM_POSE_FIELDS + EQUIPMENT_FIELDS)
    integer(packet["t"], 0, (1 << 32) - 1)
    banc = packet["banc"]
    if not isinstance(banc, str) or not re.fullmatch(r"(?:[A-Za-z0-9_]{1,63})?", banc):
        raise ProtocolError("invalid pose banc")
    if type(packet["child"]) is not bool or type(packet["loading"]) is not bool:
        raise ProtocolError("expected boolean")
    pos = packet["pos"]
    if type(pos) is not list or len(pos) != 3:
        raise ProtocolError("pose position is three numbers")
    for value in pos:
        finite(value, MAX_POSE_COORD)
    finite(packet["yaw"], 360)
    if packet["anim"] not in POSE_ANIMS:
        raise ProtocolError("invalid pose anim")
    integer(packet["anim_seq"], 0, (1 << 32) - 1)
    if finite(packet["rate"], 10) < 0 or not 0 <= finite(packet["phase"], 1) < 1:
        raise ProtocolError("pose rate/phase out of range")
    if any(key in packet for key in ANIM_POSE_FIELDS):
        if not all(key in packet for key in ANIM_POSE_FIELDS):
            raise ProtocolError("incomplete animation pose")
        integer(packet["av"], 1, 1)
        integer(packet["aid"], 0, (1 << 32) - 1)
        integer(packet["aseq"], 0, (1 << 32) - 1)
        if packet["af"] is not None and finite(packet["af"], 1000000) < 0:
            raise ProtocolError("animation frame out of range")
        if finite(packet["ar"], 4) < 0:
            raise ProtocolError("animation rate out of range")
    if ("eq32" in packet) != ("eqseq" in packet):
        raise ProtocolError("equipment fields must be paired")
    if "eq32" in packet:
        validate_equipment(packet["eq32"], packet["child"])
        integer(packet["eqseq"], 0, (1 << 16) - 1)
    if (len(encode(packet)) - 4 if size is None else size) > MAX_POSE_FRAME:
        raise ProtocolError("pose too large")


def state_key(packet):
    return packet["type"], packet.get("hash", packet.get("actor"))


def fold(state, packet):
    if packet["type"] in EVENT_TYPES:
        return  # a delta is an event in the replay window, never team state
    state[state_key(packet)] = {k: v for k, v in packet.items() if k not in ("origin", "seq", "rev")}


@dataclass
class Team:
    current: dict = field(default_factory=dict)
    checkpoint: dict = field(default_factory=dict)
    seen: dict = field(default_factory=dict)
    benefit_receipts: set = field(default_factory=set)
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
        source = collectible_origin(packet)
        # Chest sources stay a client-side rule (C13 ChestClaims: one opener per chest, several gives).
        benefit = source if kind == "item_give" and source and source[0] != "chest" else None
        if source is not None:
            accepted &= not any(value in deny for value in source[1:])
        if benefit is not None:
            accepted &= benefit not in self.benefit_receipts
        if kind in ("bool_set", "bit_set"):
            accepted &= packet["value"] and old is None
        if accepted:
            if kind not in EVENT_TYPES and old is None and len(self.current) >= MAX_KEYS:
                raise ProtocolError("team state capacity reached")
            if self.revision == MAX_SEQ:
                raise ProtocolError("team revision exhausted")
            if benefit is not None and len(self.benefit_receipts) >= MAX_ORIGIN_RECEIPTS:
                raise ProtocolError("origin receipt capacity reached")
        self.seen[origin] = seq  # Also remember ignored false, denied and backward writes.
        if not accepted:
            return None
        if benefit is not None:
            self.benefit_receipts.add(benefit)
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
        packet = {"type": "team_state", "revision": self.checkpoint_revision,
                  "changes": list(self.checkpoint.values()), "seen": dict(self.seen),
                  "replay": list(self.replay)}
        wallets = {client_id: dict(client.state["world_wallet"])
                   for client_id, client in self.clients.items() if "world_wallet" in client.state}
        if wallets:
            packet["world_wallet"] = wallets
        return packet


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
        # Entries are [data, pose_origin]; pose_origin is None for every non-pose frame.
        self.outbox = deque()
        self.pending = {}  # pose origin -> its still-unsent outbox entry (latest wins)
        self.wakeup = asyncio.Event()
        self.writing = False
        self.queued_bytes = 0
        self.poses_dropped = 0  # poses not queued for this receiver (outbox busy) or evicted
        self.poses_rate_dropped = 0  # this sender's poses over POSE_RATE
        self.poses_coalesced = 0  # queued poses replaced by a newer one from the same origin
        self.pose_times = deque()
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
        # Poses are dropped before any frame that matters: evict queued poses to make room.
        while (len(self.outbox) >= MAX_OUTBOX or self.queued_bytes + len(data) > MAX_OUTBOX_BYTES) \
                and self.pending:
            entry = self.pending.pop(next(iter(self.pending)))
            self.outbox.remove(entry)
            self.queued_bytes -= len(entry[0])
            self.poses_dropped += 1
        if len(self.outbox) >= MAX_OUTBOX or self.queued_bytes + len(data) > MAX_OUTBOX_BYTES:
            self.failed = True
            self.writer.close()
            return
        self.queued_bytes += len(data)
        self.outbox.append([data, None])
        self.wakeup.set()

    def send_pose(self, packet):
        """Best effort, never a disconnect: latest wins per origin; dropped when the outbox is busy."""
        if self.failed:
            return False
        data = encode(packet)
        entry = self.pending.get(packet["origin"])
        if entry is not None:  # still unsent: replace it in place
            self.queued_bytes += len(data) - len(entry[0])
            entry[0] = data
            self.poses_coalesced += 1  # the replaced pose was never sent
            return True
        if len(self.outbox) > MAX_POSE_QUEUE or self.queued_bytes + len(data) > MAX_OUTBOX_BYTES // 2:
            self.poses_dropped += 1
            return False
        entry = [data, packet["origin"]]
        self.pending[packet["origin"]] = entry
        self.queued_bytes += len(data)
        self.outbox.append(entry)
        self.wakeup.set()
        return True

    async def flush(self):
        while self.outbox or self.writing:
            await asyncio.sleep(0.01)

    async def write_loop(self):
        try:
            while True:
                while not self.outbox:
                    self.wakeup.clear()
                    await self.wakeup.wait()
                entry = self.outbox.popleft()
                if entry[1] is not None:
                    self.pending.pop(entry[1], None)
                data = entry[0]
                self.writing = True
                try:
                    self.writer.write(data)
                    await asyncio.wait_for(self.writer.drain(), 5)
                finally:
                    self.queued_bytes -= len(data)
                    self.writing = False
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
        client.version = packet["version"]
        client.animation = packet.get("animation")
        client.ghost_equipment_v2 = packet.get("ghost_equipment_v2", False)
        client.state = {"name": packet["name"]}
        if "world_wallet" in packet:
            client.state["world_wallet"] = dict(packet["world_wallet"])
        room.empty_since = None
        team.clients[client_id] = client
        ack = {"type": "hello_ack", "version": client.version, "build_id": room.build_id,
               "seed_hash": room.seed_hash, "policy_id": room.policy_id, "client_id": client_id}
        if client.animation is not None:
            ack["animation"] = client.animation
        if "ghost_equipment_v2" in packet:
            ack["ghost_equipment_v2"] = client.ghost_equipment_v2
        client.send(ack)
        client.send(team.snapshot())
        for peer in team.clients.values():
            if peer is not client:
                self.send_presence(peer, client)
                self.send_presence(client, peer)

    @staticmethod
    def send_presence(source, recipient):
        state = {key: value for key, value in source.state.items() if key != "animation"}
        if recipient.version == VERSION and source.animation is not None:
            state["animation"] = source.animation
        recipient.send({"type": "client_state", "origin": source.client_id, "state": state})

    @staticmethod
    def broadcast(client, packet):
        for peer in tuple(client.team.clients.values()):
            if peer is not client:
                peer.send(packet)

    def receive(self, client, packet, size=None):
        kind = packet["type"]
        if kind == "pose":
            self.pose(client, packet, size)
        elif kind in CHANGE_TYPES:
            validate_change(packet, client.client_id)
            event = client.team.accept(packet, self.deny_list)
            client.send({"type": "ack", "seq": packet["seq"]})
            if event is not None:
                self.broadcast(client, event)
        elif kind == "team_state_request":
            fields(packet, ("type",), ("world_wallet",))
            if "world_wallet" in packet:
                validate_wallet(packet["world_wallet"])
                client.state["world_wallet"] = dict(packet["world_wallet"])
                self.broadcast(client, {"type": "client_state", "origin": client.client_id,
                                        "state": client.state})
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
            if "world_wallet" in state:
                validate_wallet(state["world_wallet"])
            client.state = state
            for peer in tuple(client.team.clients.values()):
                if peer is not client:
                    self.send_presence(client, peer)
        else:
            raise ProtocolError("unsupported message type")

    @staticmethod
    def pose(client, packet, size=None):
        validate_pose(packet, size)
        if "av" in packet and client.animation is None:
            raise ProtocolError("animation not negotiated")
        if "eq32" in packet and not client.ghost_equipment_v2:
            raise ProtocolError("equipment capability not negotiated")
        forwarded = {"type": "pose", "origin": client.client_id,
                     **{key: packet[key] for key in POSE_FIELDS[1:]}}
        animation = {key: packet[key] for key in ANIM_POSE_FIELDS if key in packet}
        equipment = {key: packet[key] for key in EQUIPMENT_FIELDS if key in packet}
        if len(encode(dict(forwarded, **animation, **equipment))) - 4 > MAX_POSE_FRAME:
            raise ProtocolError("pose too large after origin")
        now = asyncio.get_running_loop().time()
        times = client.pose_times
        while times and now - times[0] >= 1:
            times.popleft()
        if len(times) >= POSE_RATE:
            client.poses_rate_dropped += 1
            return
        times.append(now)
        for peer in tuple(client.team.clients.values()):
            if peer is not client:
                outgoing = forwarded
                if animation and client.animation is not None and peer.animation == client.animation:
                    outgoing = dict(outgoing, **animation)
                if equipment and peer.ghost_equipment_v2:
                    outgoing = dict(outgoing, **equipment)
                peer.send_pose(outgoing)

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
                    self.receive(client, packet, len(data) - 4)
        except (ProtocolError, ValueError, UnicodeError, RecursionError) as exc:
            client.send({"type": "error", "reason": str(exc)[:160]})
            with contextlib.suppress(TimeoutError):
                await asyncio.wait_for(client.flush(), 0.5)
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
