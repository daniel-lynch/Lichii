#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""LAN-only OoT co-op relay with optional campaign persistence. Protocol details: docs/oot-coop/PROTOCOL.md."""

import argparse
import asyncio
import contextlib
import json
import math
import re
import os
import secrets
import tempfile
import time
from datetime import datetime, timezone
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
SENDER_IDLE_EVICT = 3600  # an offline sender idle this long may give its high-water slot to a new one
MAX_CLIENTS = 128
MAX_ROOMS = 32
MAX_TEAMS = 16
MAX_LEDGER = 4096
CATCHUP_PAGE = 256
MAX_STATE_BYTES = 64 * 1024 * 1024
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


def mod_hash(value):
    if not isinstance(value, str) or not re.fullmatch(r"[0-9a-f]{64}", value):
        raise ProtocolError("invalid mod hash")
    return value


def validate_hello(packet):
    fields(packet, ("type", "version", "build_id", "seed_hash", "room", "team", "client_id", "name", "policy_id"),
           ("world_wallet", "animation", "ghost_equipment_v2", "mod_hash"))
    if "mod_hash" in packet:
        mod_hash(packet["mod_hash"])
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
    sender_active: dict = field(default_factory=dict)  # sender -> wall time of its last join/change
    replay: deque = field(default_factory=deque)
    clients: dict = field(default_factory=dict)
    revision: int = 0
    checkpoint_revision: int = 0

    ledger: deque = field(default_factory=deque)
    ledger_floor: int = 0
    chest_owner: dict = field(default_factory=dict)

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
        if kind in EVENT_TYPES:
            owner = (origin, packet.get("chest_transaction"))
            record = True
            if source and source[0] == "chest":
                chest = source[1]
                if chest in self.chest_owner:
                    record = self.chest_owner[chest] == owner
                elif len(self.chest_owner) < MAX_KEYS:
                    self.chest_owner[chest] = owner
            if record:
                self.ledger.append(event)
                if len(self.ledger) > MAX_LEDGER:
                    self.ledger_floor = self.ledger.popleft()["rev"]
        return event

    def catchup(self, epoch, after):
        integer(after, 0, MAX_SEQ)
        through = self.revision
        remaining = [event for event in self.ledger if after < event["rev"] <= through]
        grants = remaining[:CATCHUP_PAGE]
        while True:
            packet = {"type": "catchup", "epoch": epoch, "floor": self.ledger_floor,
                      "through": through, "after": after, "grants": grants,
                      "more": len(remaining) > len(grants)}
            try:
                encode(packet)
                return packet
            except ProtocolError:
                if not grants:
                    raise
                grants = grants[:-1]
                if not grants and remaining:
                    raise ProtocolError("catchup grant exceeds frame limit")

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
    epoch: str = field(default_factory=lambda: secrets.token_hex(8))
    mod_hash: str | None = None
    last_active: float = field(default_factory=time.time)


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
                 cleanup_interval=1, deny_list=(), state_file=None, room_ttl_days=30):
        if min(heartbeat_interval, idle_timeout, room_ttl, cleanup_interval) <= 0:
            raise ValueError("timeouts must be positive")
        if not math.isfinite(room_ttl_days) or room_ttl_days <= 0:
            raise ValueError("room TTL days must be positive")
        self.state_file = Path(state_file) if state_file is not None else None
        self.room_ttl_days = room_ttl_days
        self.dirty = False
        self.last_write = float("-inf")
        self.write_errors = set()
        self.heartbeat_interval = heartbeat_interval
        self.idle_timeout = idle_timeout
        self.room_ttl = room_ttl
        self.cleanup_interval = cleanup_interval
        self.deny_list = frozenset(integer(x, 0, (1 << 64) - 1) for x in deny_list)
        self.rooms = {}
        self.clients = set()
        self.tasks = set()
        self.server = self.maintenance_task = None
        if self.state_file is not None:
            self.load_state()
        self.rooms_loaded = len(self.rooms)

    def evict_sender(self, room_name, team):
        """Every emulator process is a new sender ID; a persistent room would fill MAX_SENDERS. Only an
        offline sender idle for SENDER_IDLE_EVICT gives up its high-water (a live process that kept an
        unacknowledged outbox reconnects within its 30 s backoff, long before that)."""
        now = time.time()
        idle = [(team.sender_active.get(sender, 0), sender) for sender in team.seen
                if sender not in team.clients and now - team.sender_active.get(sender, 0) >= SENDER_IDLE_EVICT]
        if not idle:
            raise ProtocolError("sender capacity reached")
        _, sender = min(idle)
        del team.seen[sender]
        team.sender_active.pop(sender, None)
        self.dirty = True
        print(f"OoT co-op relay: room {room_name}: idle sender {sender} forgotten (sender capacity)", flush=True)

    def evict_room(self):
        empty = [(room.last_active, name) for name, room in self.rooms.items()
                 if not any(team.clients for team in room.teams.values())]
        if not empty:
            raise ProtocolError("room capacity reached")
        active, name = min(empty)
        del self.rooms[name]
        self.dirty = True
        stamp = datetime.fromtimestamp(active, timezone.utc).isoformat()
        print(f"OoT co-op relay: room {name} evicted (capacity; idle since {stamp})", flush=True)

    def state_document(self):
        return {"format": 1, "rooms": {
            name: {"build_id": room.build_id, "seed_hash": room.seed_hash,
                   "policy_id": room.policy_id, "epoch": room.epoch, "mod_hash": room.mod_hash,
                   "last_active": room.last_active, "teams": {
                       name: {"revision": team.revision, "current": list(team.current.values()),
                              "seen": team.seen, "benefit_receipts": sorted(team.benefit_receipts),
                              "ledger": list(team.ledger), "ledger_floor": team.ledger_floor,
                              "sender_active": dict(team.sender_active),
                              "chest_owner": [[key, *owner] for key, owner in sorted(team.chest_owner.items())]}
                       for name, team in room.teams.items()}}
            for name, room in self.rooms.items()}}

    def persist(self, *, strict=False):
        if self.state_file is None or not self.dirty:
            return
        temporary = None
        try:
            data = json.dumps(self.state_document(), separators=(",", ":"), allow_nan=False).encode()
            if len(data) > MAX_STATE_BYTES:
                raise ValueError("state exceeds 64 MiB")
            fd, temporary = tempfile.mkstemp(prefix=self.state_file.name + ".", suffix=".tmp",
                                             dir=self.state_file.parent)
            with os.fdopen(fd, "wb") as output:
                output.write(data)
                output.flush()
                os.fsync(output.fileno())
            os.replace(temporary, self.state_file)
            temporary = None
            directory = os.open(self.state_file.parent, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
            try:
                os.fsync(directory)
            finally:
                os.close(directory)
            self.dirty = False
        except (OSError, ValueError) as exc:
            if strict:
                raise
            error = str(exc)
            if error not in self.write_errors:
                self.write_errors.add(error)
                print(f"OoT co-op relay: state write failed: {error}", flush=True)
        finally:
            if temporary is not None:
                with contextlib.suppress(OSError):
                    os.unlink(temporary)

    def load_state(self):
        try:
            with self.state_file.open("rb") as source:
                data = source.read(MAX_STATE_BYTES + 1)
        except FileNotFoundError as exc:
            if self.state_file.is_symlink():
                raise ValueError(f"cannot load relay state {self.state_file}: {exc}") from exc
            return
        except OSError as exc:
            raise ValueError(f"cannot load relay state {self.state_file}: {exc}") from exc
        try:
            if len(data) > MAX_STATE_BYTES:
                raise ValueError("state exceeds 64 MiB")
            def unique(items):
                result = {}
                for key, value in items:
                    if key in result:
                        raise ValueError("duplicate field")
                    result[key] = value
                return result
            doc = json.loads(data, object_pairs_hook=unique)
            fields(doc, ("format", "rooms"))
            if type(doc["format"]) is not int or doc["format"] != 1:
                raise ValueError("unsupported state format")
            if type(doc["rooms"]) is not dict or len(doc["rooms"]) > MAX_ROOMS:
                raise ValueError("invalid rooms")
            loaded = {}
            for name, value in doc["rooms"].items():
                identifier(name)
                fields(value, ("build_id", "seed_hash", "policy_id", "epoch", "mod_hash", "last_active", "teams"))
                validate_hello(dict(type="hello", version=VERSION, room=name, team="check", client_id="check",
                                    name="check", **{k: value[k] for k in ("build_id", "seed_hash", "policy_id")}))
                if not isinstance(value["epoch"], str) or not re.fullmatch(r"[0-9a-f]{16}", value["epoch"]):
                    raise ValueError("invalid room epoch")
                if value["mod_hash"] is not None:
                    mod_hash(value["mod_hash"])
                active = value["last_active"]
                if type(active) not in (int, float) or not math.isfinite(active) or active < 0:
                    raise ValueError("invalid room time")
                datetime.fromtimestamp(active, timezone.utc)  # Also valid for eviction diagnostics.
                if type(value["teams"]) is not dict or len(value["teams"]) > MAX_TEAMS:
                    raise ValueError("invalid teams")
                room = Room(value["build_id"], value["seed_hash"], value["policy_id"],
                            epoch=value["epoch"], mod_hash=value["mod_hash"], last_active=active)
                for team_name, saved in value["teams"].items():
                    identifier(team_name)
                    fields(saved, ("revision", "current", "seen", "benefit_receipts", "ledger", "ledger_floor", "chest_owner"),
                           ("sender_active",))
                    team = Team(revision=integer(saved["revision"], 0, MAX_SEQ),
                                ledger_floor=integer(saved["ledger_floor"], 0, MAX_SEQ))
                    if team.ledger_floor > team.revision:
                        raise ValueError("floor beyond revision")
                    for key, limit in (("current", MAX_KEYS), ("ledger", MAX_LEDGER),
                                       ("benefit_receipts", MAX_ORIGIN_RECEIPTS), ("chest_owner", MAX_KEYS)):
                        if type(saved[key]) is not list or len(saved[key]) > limit:
                            raise ValueError(f"invalid {key}")
                    if type(saved["seen"]) is not dict or len(saved["seen"]) > MAX_SENDERS:
                        raise ValueError("invalid senders")
                    team.seen = {identifier(sender): integer(seq, 0, MAX_SEQ) for sender, seq in saved["seen"].items()}
                    for flag in saved["current"]:
                        validate_change(dict(flag, origin="check", seq=1), "check")
                        if flag["type"] in EVENT_TYPES or flag.get("value") is not True or \
                                any(k in flag for k in ("origin", "seq", "rev")) or state_key(flag) in team.current:
                            raise ValueError("invalid folded flag")
                        fold(team.current, flag)
                    for receipt in saved["benefit_receipts"]:
                        if type(receipt) is not list or not receipt or receipt[0] not in ("bit", "token"):
                            raise ValueError("invalid benefit receipt")
                        source = {"origin_kind": receipt[0], "origin_key64": receipt[1]}
                        if len(receipt) != (3 if receipt[0] == "token" else 2):
                            raise ValueError("invalid benefit receipt")
                        if receipt[0] == "token":
                            source["origin_partner"] = receipt[2]
                        parsed = collectible_origin(source)
                        if parsed in team.benefit_receipts:
                            raise ValueError("duplicate receipt")
                        team.benefit_receipts.add(parsed)
                    for owner in saved["chest_owner"]:
                        if type(owner) is not list or len(owner) != 3:
                            raise ValueError("invalid chest owner")
                        key = integer(owner[0], 1, (1 << 32) - 1)
                        sender = identifier(owner[1])
                        tx = None if owner[2] is None else integer(owner[2], 1, MAX_SEQ)
                        if key in team.chest_owner:  # its sender may have been forgotten (evict_sender)
                            raise ValueError("inconsistent chest owner")
                        team.chest_owner[key] = sender, tx
                    previous = team.ledger_floor
                    ids, ledger_benefits, ledger_seen = set(), set(), {}
                    for event in saved["ledger"]:
                        rev = integer(event["rev"], previous + 1, team.revision)
                        change = {k: v for k, v in event.items() if k != "rev"}
                        validate_change(change, identifier(change["origin"]))
                        event_id = change["origin"], change["seq"]
                        if change["type"] not in EVENT_TYPES or (change["origin"] in team.seen and change["seq"] > team.seen[change["origin"]]) or event_id in ids:
                            raise ValueError("inconsistent ledger")
                        source = collectible_origin(change)
                        if source and source[0] == "chest" and source[1] in team.chest_owner and \
                                team.chest_owner[source[1]] != (change["origin"], change.get("chest_transaction")):
                            raise ValueError("ledger chest owner mismatch")
                        if source and source[0] in ("bit", "token") and source not in team.benefit_receipts:
                            raise ValueError("ledger missing benefit receipt")
                        if change["seq"] <= ledger_seen.get(change["origin"], 0):
                            raise ValueError("backward ledger sender sequence")
                        if source and source[0] in ("bit", "token"):
                            if source in ledger_benefits:
                                raise ValueError("duplicate ledger benefit")
                            ledger_benefits.add(source)
                        if source and source[0] == "chest" and source[1] not in team.chest_owner and len(team.chest_owner) < MAX_KEYS:
                            raise ValueError("ledger missing chest owner")
                        ledger_seen[change["origin"]] = change["seq"]
                        ids.add(event_id)
                        team.ledger.append(event)
                        previous = rev
                    if len(team.current) > team.revision or len(team.ledger) > team.revision:
                        raise ValueError("inconsistent revision")
                    active = saved.get("sender_active", {})
                    if type(active) is not dict or len(active) > MAX_SENDERS:
                        raise ValueError("invalid sender times")
                    for sender, when in active.items():
                        if sender not in team.seen or type(when) not in (int, float) or not math.isfinite(when):
                            raise ValueError("invalid sender time")
                        team.sender_active[sender] = when
                    team.checkpoint = dict(team.current)
                    team.checkpoint_revision = team.revision
                    room.teams[team_name] = team
                loaded[name] = room
            self.rooms = loaded
        except (ValueError, TypeError, KeyError, IndexError, AttributeError, RecursionError, OverflowError, OSError) as exc:
            raise ValueError(f"cannot load relay state {self.state_file}: {exc}") from exc

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
                self.evict_room()
            room = Room(packet["build_id"], packet["seed_hash"], packet["policy_id"],
                        empty_since=asyncio.get_running_loop().time())
            self.rooms[packet["room"]] = room
            self.dirty = True
        if room.build_id != packet["build_id"]:
            raise ProtocolError("build id mismatch")
        if room.seed_hash != packet["seed_hash"]:
            raise ProtocolError("seed hash mismatch")
        if room.policy_id != packet["policy_id"]:
            raise ProtocolError("policy id mismatch")
        supplied_hash = packet.get("mod_hash")
        if supplied_hash is not None and room.mod_hash is not None and supplied_hash != room.mod_hash:
            print(f"OoT co-op relay: handshake refused: mod mismatch room={packet['room']} "
                  f"team={packet['team']} client={packet['client_id']} "
                  f"room_hash={room.mod_hash[:12]} client_hash={supplied_hash[:12]}", flush=True)
            raise ProtocolError(f"mod mismatch (room {room.mod_hash[:12]}, client {supplied_hash[:12]})")
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
            self.evict_sender(packet["room"], team)
        if supplied_hash is None:
            print(f"OoT co-op relay: client {client_id} joined room {packet['room']} "
                  "without a mod hash (legacy client, unchecked)", flush=True)
        elif room.mod_hash is None:
            room.mod_hash = supplied_hash
        room.last_active = time.time()
        self.dirty = True
        team.seen.setdefault(client_id, 0)
        team.sender_active[client_id] = time.time()
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
        if supplied_hash is not None:
            ack["ledger"] = {"v": 1, "epoch": room.epoch, "floor": team.ledger_floor}
            ack["mod_hash"] = room.mod_hash
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
            previous = client.team.seen.get(client.client_id, 0)
            event = client.team.accept(packet, self.deny_list)
            if client.team.seen.get(client.client_id, 0) != previous:
                client.room.last_active = client.team.sender_active[client.client_id] = time.time()
                self.dirty = True
            client.send({"type": "ack", "seq": packet["seq"]})
            if event is not None:
                self.broadcast(client, event)
        elif kind == "catchup_request":
            fields(packet, ("type", "after"))
            client.send(client.team.catchup(client.room.epoch, packet["after"]))
        elif kind == "team_state_request":
            fields(packet, ("type",), ("world_wallet",))
            if "world_wallet" in packet:
                validate_wallet(packet["world_wallet"])
                client.state["world_wallet"] = dict(packet["world_wallet"])
                # Per recipient, like every other presence: a client_state is a full state, so a raw
                # client.state (no negotiated animation) would make v4 peers drop this client's animation.
                for peer in tuple(client.team.clients.values()):
                    if peer is not client:
                        self.send_presence(client, peer)
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
                    client.room.last_active = time.time()
                    self.dirty = True
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
                if self.state_file is not None:
                    expired = not any(t.clients for t in room.teams.values()) and \
                        time.time() - room.last_active >= self.room_ttl_days * 86400
                else:
                    expired = room.empty_since is not None and now - room.empty_since >= self.room_ttl
                if expired:
                    del self.rooms[name]
                    self.dirty = True
            if now - self.last_write >= 1:
                self.persist()
                self.last_write = now

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
        self.persist()


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


def configured(args):
    return Relay(deny_list=load_deny_list(args.deny_list),
                 state_file=None if args.no_state else args.state_file, room_ttl_days=args.room_ttl_days)


def reset_room(relay, name):
    if relay.state_file is None:
        raise ValueError("--reset-room requires a state file")
    name = identifier(name)
    if name not in relay.rooms:
        raise ValueError(f"room {name} does not exist; nothing reset")
    del relay.rooms[name]
    relay.dirty = True
    relay.persist(strict=True)
    print(f"OoT co-op relay: room {name} reset in {relay.state_file}", flush=True)


async def run(args):
    relay = configured(args)
    if args.reset_room is not None:
        reset_room(relay, args.reset_room)
        return
    await relay.start(args.bind, args.port)
    state = f"state {relay.state_file}" if relay.state_file is not None else "memory only"
    print(f"OoT co-op relay listening on {args.bind}:{args.port} ({state}; {relay.rooms_loaded} rooms loaded)", flush=True)
    try:
        await relay.server.serve_forever()
    finally:
        await relay.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=43390)
    parser.add_argument("--deny-list", help="user JSON array of opaque numeric hashes")
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--state-file", type=Path, default=Path(__file__).with_name("relay-state.json"))
    group.add_argument("--no-state", action="store_true")
    parser.add_argument("--room-ttl-days", type=float, default=30)
    parser.add_argument("--reset-room", metavar="NAME", help="offline: remove one room and exit")
    args = parser.parse_args()
    try:
        if args.reset_room is not None:
            reset_room(configured(args), args.reset_room)
        else:
            asyncio.run(run(args))
    except KeyboardInterrupt:
        pass
    except (ValueError, OSError) as exc:
        parser.exit(1, f"OoT co-op relay: {exc}\n")


if __name__ == "__main__":
    main()
