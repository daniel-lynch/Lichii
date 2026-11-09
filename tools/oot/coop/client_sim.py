#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Small protocol client for tests and manual LAN checks; never launches a game."""

import argparse
import asyncio
import contextlib
import collections
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


class ClientSim:
    def __init__(self, client_id=None, *, room="default", team="default", name="Player",
                 build_id=BUILD_ID, seed_hash="0", policy_id="or-v2", auto_heartbeat=True,
                 on_pose=None):
        self.hello = {"type": "hello", "version": wire.VERSION, "build_id": build_id,
                      "seed_hash": seed_hash, "room": room, "team": team,
                      "client_id": client_id or uuid.uuid4().hex, "name": name, "policy_id": policy_id}
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
                    self.poses.append(packet)
                    self.pose_count += 1
                    self.pose_waiter.set()
                    if self.on_pose is not None:
                        self.on_pose(packet)
                    continue
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

    def pose_packet(self, banc, child, pos, yaw, loading=False, t=None, anim="idle", anim_seq=0,
                    rate=0.0, phase=0.0):
        """A v3 pose (no origin/seq: the relay stamps origin). t defaults to a counter + 1."""
        if t is None:
            t = self.pose_t + 1
        self.pose_t = t % (1 << 32)
        return {"type": "pose", "t": self.pose_t, "banc": banc, "child": bool(child),
                "loading": bool(loading), "pos": [p for p in pos], "yaw": yaw, "anim": anim,
                "anim_seq": anim_seq, "rate": rate, "phase": phase}

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


async def run(args):
    client = ClientSim(args.client_id, room=args.room, team=args.team, name=args.name,
                       build_id=args.build_id, seed_hash=args.seed, policy_id=args.policy,
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
