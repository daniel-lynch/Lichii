#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Small protocol client for tests and manual LAN checks; never launches a game."""

import argparse
import asyncio
import contextlib
import json
import importlib.util
from pathlib import Path
import sys
import uuid

BUILD_ID = "E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348"
# Explicit sibling loading keeps the documented python3 -I entry point isolated.
_spec = importlib.util.spec_from_file_location("oot_coop_relay", Path(__file__).with_name("relay.py"))
wire = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = wire
_spec.loader.exec_module(wire)
MAX_FRAME = wire.MAX_FRAME


class ClientSim:
    def __init__(self, client_id=None, *, room="default", team="default", name="Player",
                 build_id=BUILD_ID, seed_hash="0", policy_id="or-v2", auto_heartbeat=True):
        self.hello = {"type": "hello", "version": wire.VERSION, "build_id": build_id,
                      "seed_hash": seed_hash, "room": room, "team": team,
                      "client_id": client_id or uuid.uuid4().hex, "name": name, "policy_id": policy_id}
        self.auto_heartbeat = auto_heartbeat
        self.seq = 0
        self.messages = asyncio.Queue(2048)
        self.backlog = []
        self.reader = self.writer = self.task = None
        self.ack = self.snapshot = None

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


async def run(args):
    client = ClientSim(args.client_id, room=args.room, team=args.team, name=args.name,
                       build_id=args.build_id, seed_hash=args.seed, policy_id=args.policy)
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
    args = parser.parse_args()
    try:
        asyncio.run(run(args))
    except (KeyboardInterrupt, EOFError):
        pass


if __name__ == "__main__":
    main()
