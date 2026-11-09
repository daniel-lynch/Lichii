#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Small protocol client for tests and manual LAN checks; never launches a game."""

import argparse
import asyncio
import contextlib
import collections
import hashlib
import json
import importlib.util
from pathlib import Path
import sys
import uuid

# A placeholder build id (40 hex digits); pass the real one with --build-id (it is game-derived).
BUILD_ID = "0123456789ABCDEF0123456789ABCDEF01234567"
# Explicit sibling loading keeps the documented python3 -I entry point isolated.
_spec = importlib.util.spec_from_file_location("oot_coop_relay", Path(__file__).with_name("relay.py"))
wire = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = wire
_spec.loader.exec_module(wire)
MAX_FRAME = wire.MAX_FRAME


def origin_fields(text):
    """Controller syntax: none, chest:HEX, bit:HEX, token:HEX:PARTNER_HEX."""
    words = text.split(":")
    result = {"origin_kind": words[0]}
    if words[0] == "chest" and len(words) == 2:
        result["origin_key"] = int(words[1], 16)
    elif words[0] == "bit" and len(words) == 2:
        result["origin_key64"] = int(words[1], 16)
    elif words[0] == "token" and len(words) == 3:
        result.update(origin_key64=int(words[1], 16), origin_partner=int(words[2], 16))
    elif words != ["none"]:
        raise ValueError("origin must be none, chest:HEX, bit:HEX or token:HEX:PARTNER_HEX")
    wire.collectible_origin(result)
    return result


def animation_descriptor(path):
    """Hash the exact private sidecar bytes; never normalize or transmit its contents."""
    if path is None:
        return None
    with Path(path).open("rb") as source:
        data = source.read(16 * 1024 * 1024 + 1)
    if not data or len(data) > 16 * 1024 * 1024:
        raise ValueError("animation sidecar size out of range")
    return {"av": 1, "table_sha256": hashlib.sha256(data).hexdigest()}


class ClientSim:
    def __init__(self, client_id=None, *, room="default", team="default", name="Player",
                 build_id=BUILD_ID, seed_hash="0", policy_id="or-v2", auto_heartbeat=True,
                 on_pose=None, world_wallet=None, animation=None):
        self.hello = {"type": "hello", "version": wire.VERSION if animation else wire.LEGACY_VERSION, "build_id": build_id,
                      "seed_hash": seed_hash, "room": room, "team": team,
                      "client_id": client_id or uuid.uuid4().hex, "name": name, "policy_id": policy_id}
        if world_wallet is not None:
            wire.validate_wallet(world_wallet)
            self.hello["world_wallet"] = dict(world_wallet)
        if animation is not None:
            wire.validate_animation(animation)
            self.hello["animation"] = dict(animation)
        self.peer_animations = {}
        self.auto_heartbeat = auto_heartbeat
        self.seq = 0
        self.messages = asyncio.Queue(2048)
        self.backlog = []
        self.reader = self.writer = self.task = None
        self.ack = self.snapshot = None
        # Received poses bypass the message queue (they arrive at ~15 Hz): the newest are kept
        # here, counted, optionally handed to on_pose(packet), and awaited with receive_pose().
        self.poses = collections.deque(maxlen=1024)
        self.pose_count = 0
        self.pose_waiter = asyncio.Event()
        self.on_pose = on_pose
        self.pose_t = 0

    async def connect(self, host, port):
        self.reader, self.writer = await asyncio.open_connection(host, port, limit=MAX_FRAME)
        await self.send(self.hello)
        self.ack = await self.read()
        if self.ack["type"] != "hello_ack":
            await self.close()
            raise ValueError(self.ack.get("reason", "handshake refused"))
        if (self.ack.get("version") != self.hello["version"] or
                self.ack.get("animation") != self.hello.get("animation")):
            await self.close()
            raise ValueError("animation handshake mismatch")
        self.snapshot = await self.read()
        if self.snapshot["type"] != "team_state":
            await self.close()
            raise ValueError("missing initial snapshot")
        self.seq = max(self.seq, self.snapshot["seen"].get(self.hello["client_id"], 0))
        self.task = asyncio.create_task(self.pump())
        return self

    async def read(self):
        return wire.decode(await wire.read_frame(self.reader))

    async def pump(self):
        try:
            while True:
                packet = await self.read()
                if packet["type"] == "heartbeat" and self.auto_heartbeat:
                    await self.send({"type": "heartbeat_ack"})
                if packet["type"] == "pose":
                    if "av" in packet and not self.animation_matches(packet.get("origin")):
                        packet = {key: value for key, value in packet.items() if key not in wire.ANIM_POSE_FIELDS}
                    self.poses.append(packet)
                    self.pose_count += 1
                    self.pose_waiter.set()
                    if self.on_pose is not None:
                        self.on_pose(packet)
                    continue
                if packet["type"] == "client_state":
                    state = packet.get("state", {})
                    self.peer_animations.pop(packet["origin"], None)
                    if state.get("online") is not False and state.get("animation") is not None:
                        wire.validate_animation(state["animation"])
                        self.peer_animations[packet["origin"]] = state["animation"]
                await self.messages.put(packet)
        except (EOFError, ConnectionError, ValueError) as exc:
            await self.messages.put(exc)

    async def send(self, packet):
        self.writer.write(wire.encode(packet))
        await self.writer.drain()

    async def change(self, kind, **values):
        self.seq += 1
        packet = {"type": kind, "origin": self.hello["client_id"], "seq": self.seq, **values}
        await self.send(packet)
        return packet

    async def baseline(self, world_wallet=None):
        packet = {"type": "team_state_request"}
        if world_wallet is not None:
            wire.validate_wallet(world_wallet)
            packet["world_wallet"] = dict(world_wallet)
        await self.send(packet)

    def animation_matches(self, origin):
        animation = self.hello.get("animation")
        return animation is not None and self.peer_animations.get(origin) == animation

    def pose_packet(self, banc, child, pos, yaw, loading=False, t=None, anim="idle", anim_seq=0,
                    rate=0.0, phase=0.0, aid=None, af=None, ar=1.0, aseq=0):
        """Keep the legacy approximation alongside any negotiated animation state."""
        if t is None:
            t = self.pose_t + 1
        self.pose_t = t % (1 << 32)
        packet = {"type": "pose", "t": self.pose_t, "banc": banc, "child": bool(child),
                "loading": bool(loading), "pos": [p for p in pos], "yaw": yaw, "anim": anim,
                "anim_seq": anim_seq, "rate": rate, "phase": phase}
        if aid is not None:
            if not any(self.animation_matches(origin) for origin in self.peer_animations):
                raise ValueError("no peer with matching animation table")
            packet.update(av=1, aid=aid, af=af, ar=ar, aseq=aseq)
        return packet

    async def pose(self, banc, child, pos, yaw, loading=False, t=None, **anim):
        packet = self.pose_packet(banc, child, pos, yaw, loading, t, **anim)
        wire.validate_pose(packet)  # a malformed pose is a protocol error: never send one (use send())
        await self.send(packet)
        return packet

    async def receive_pose(self, since=None, timeout=2):
        """Wait until more than `since` poses (default: the count now) have arrived; return the newest."""
        target = self.pose_count if since is None else since
        async def wait():
            while self.pose_count <= target:
                self.pose_waiter.clear()
                await self.pose_waiter.wait()
            return self.poses[-1]
        return await asyncio.wait_for(wait(), timeout)

    async def receive(self, kind=None, timeout=2):
        for index, packet in enumerate(self.backlog):
            if kind is None or packet["type"] == kind:
                return self.backlog.pop(index)
        async def wait():
            while True:
                packet = await self.messages.get()
                if isinstance(packet, Exception):
                    raise packet
                if kind is None or packet["type"] == kind:
                    return packet
                self.backlog.append(packet)
        return await asyncio.wait_for(wait(), timeout)

    async def close(self):
        if self.task is not None:
            self.task.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await self.task
            self.task = None
        if self.writer is not None:
            self.writer.close()
            with contextlib.suppress(ConnectionError):
                await self.writer.wait_closed()
            self.writer = None
        self.backlog.clear()
        self.messages = asyncio.Queue(2048)
        self.poses.clear()
        self.pose_count = 0
        self.peer_animations.clear()


async def run(args):
    client = ClientSim(args.client_id, room=args.room, team=args.team, name=args.name,
                       build_id=args.build_id, seed_hash=args.seed, policy_id=args.policy,
                       world_wallet=dict(zip(("balance", "capacity"), args.world_wallet))
                       if args.world_wallet else None,
                       animation=animation_descriptor(args.anim_table),
                       on_pose=lambda packet: print(json.dumps(packet), flush=True) if args.show_poses else None)
    try:
        await client.connect(args.host, args.port)
        print(json.dumps(client.ack), flush=True)
        print(json.dumps(client.snapshot), flush=True)
        for raw in args.send:
            packet = json.loads(raw)
            if packet.get("type") in wire.CHANGE_TYPES:
                kind = packet.pop("type")
                await client.change(kind, **packet)
            else:
                await client.send(packet)
        for raw in args.pose:
            # BANC CHILD X Y Z YAW [LOADING]; BANC "-" is the empty banc
            words = raw.split()
            if len(words) not in (6, 7):
                raise SystemExit("--pose 'BANC CHILD X Y Z YAW [LOADING]'")
            await client.pose("" if words[0] == "-" else words[0], words[1] == "1",
                              [float(w) for w in words[2:5]], float(words[5]),
                              len(words) == 7 and words[6] == "1")
        until = asyncio.get_running_loop().time() + args.duration
        while asyncio.get_running_loop().time() < until:
            try:
                packet = await client.receive(timeout=until - asyncio.get_running_loop().time())
            except TimeoutError:
                break
            print(json.dumps(packet), flush=True)
    finally:
        await client.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=43390)
    parser.add_argument("--room", default="default")
    parser.add_argument("--team", default="default")
    parser.add_argument("--name", default="Player")
    parser.add_argument("--client-id", help="reuse only with the original sender's sequence history")
    parser.add_argument("--build-id", default=BUILD_ID)
    parser.add_argument("--seed", default="0")
    parser.add_argument("--policy", default="or-v2")
    parser.add_argument("--world-wallet", type=int, nargs=2, metavar=("BALANCE", "CAPACITY"))
    parser.add_argument("--anim-table", help="private coop-anim.json sidecar (exact-byte SHA256 negotiation)")
    parser.add_argument("--duration", type=float, default=30)
    parser.add_argument("--send", action="append", default=[], help="JSON packet; change origin/seq are automatic")
    parser.add_argument("--pose", action="append", default=[], help="'BANC CHILD X Y Z YAW [LOADING]' (v3 pose)")
    parser.add_argument("--show-poses", action="store_true", help="print received poses (otherwise only counted)")
    args = parser.parse_args()
    try:
        asyncio.run(run(args))
    except (KeyboardInterrupt, EOFError):
        pass


if __name__ == "__main__":
    main()
