#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""One-process fake co-op peer for single-emulator tests: no separate relay service.

Runs the relay in this process on a loopback port and joins it as a scripted peer. One real
emulator connects to the same port (EDEN_NX2_OOT_COOP_RELAY=127.0.0.1:PORT) with the same
room/team/policy/seed; the peer then plays "player B" by command and records what the
emulator broadcasts. Never launches a game; binds 127.0.0.1 only.

Commands are lines appended to CTL/cmd.txt; each gets one JSON result line in CTL/out.jsonl:
  grant HEX [HEX...]        bool_set value=true for each hash, all frames in ONE socket write
  bit HEX [HEX...]          bit_set (actor u64) the same way
  rupee DELTA [DELTA...]    rupee_add (wallet hash) with each signed delta, all in ONE socket write
  item NAME CAT COUNT [GET [F0 F1 F2 F3]]  item_give: one give call (get type default 7, flags 0)
  dup SEQ                   re-deliver this peer's accepted event SEQ to the other clients (a duplicate)
  expect-item NAME SECS     wait for the emulator's item_give of NAME
  expect HEX SECS           wait for the emulator's bool_set of HEX (any rev); result has the packet
  received                  every change packet received so far (from other origins)
  state                     the relay's team checkpoint as the peer sees it (team_state_request)
  mark TEXT                 a marker line in received.jsonl (for correlating with emulator logs)
  quit
Every packet the peer receives is also appended to CTL/received.jsonl with a wall-clock time."""

import argparse
import asyncio
import importlib.util
import json
from pathlib import Path
import sys
import time

sys.dont_write_bytecode = True


def _sibling(name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(name + ".py"))
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


client_sim = _sibling("client_sim")
relay_mod = client_sim.wire  # the relay module client_sim loaded; one copy of the wire format


class FakePeer:
    """Relay + one scripted client in one event loop. `port=0` picks an ephemeral port."""

    def __init__(self, *, port=43390, room="default", team="default", policy="or-v2", seed="0",
                 name="FakePeer", build_id=client_sim.BUILD_ID, record=None, wallet=0):
        self.relay = relay_mod.Relay()
        self.port = port
        self.client = client_sim.ClientSim(room=room, team=team, name=name, build_id=build_id,
                                           seed_hash=seed, policy_id=policy)
        self.record = Path(record) if record else None
        self.wallet = wallet  # the wallet key from the private profile (rupee_add)
        self.received = []
        self.seen = []
        self.waiters = []
        self.task = None

    async def start(self):
        self.port = await self.relay.start("127.0.0.1", self.port)
        await self.client.connect("127.0.0.1", self.port)
        self.task = asyncio.create_task(self.listen())
        return self.port

    def log(self, packet):
        if self.record:
            with self.record.open("a") as stream:
                stream.write(json.dumps({"t": round(time.time(), 3), **packet}) + "\n")

    async def listen(self):
        """The only reader of the client's queue; commands wait on predicates instead."""
        while True:
            try:
                packet = await self.client.receive(timeout=3600)
            except TimeoutError:
                continue
            self.log(packet)
            self.seen.append(packet)
            if packet.get("type") in relay_mod.CHANGE_TYPES:
                self.received.append(packet)
            for match, future in tuple(self.waiters):
                if not future.done() and match(packet):
                    future.set_result(packet)

    async def wait(self, match, timeout, since=0):
        for packet in self.seen[since:]:
            if match(packet):
                return packet
        future = asyncio.get_running_loop().create_future()
        waiter = (match, future)
        self.waiters.append(waiter)
        try:
            return await asyncio.wait_for(future, timeout)
        finally:
            self.waiters.remove(waiter)

    async def burst(self, kind, keys):
        """All changes in one write, so the relay forwards them back to back. For rupee_add the
        'keys' are the signed deltas."""
        field = "hash" if kind == "bool_set" else "actor"
        data = b""
        packets = []
        for key in keys:
            self.client.seq += 1
            packet = {"type": kind, "origin": self.client.hello["client_id"],
                      "seq": self.client.seq, field: key, "value": True}
            if kind == "rupee_add":
                packet = {"type": kind, "origin": self.client.hello["client_id"],
                          "seq": self.client.seq, "hash": self.wallet, "delta": key}
            packets.append(packet)
            data += relay_mod.encode(packet)
        since = len(self.seen)
        self.client.writer.write(data)
        await self.client.writer.drain()
        last = packets[-1]["seq"]
        await self.wait(lambda p: p.get("type") == "ack" and p.get("seq") == last, 5, since)
        return {"sent": [p["seq"] for p in packets], "keys": ["%x" % k for k in keys],
                "acked": last}

    async def expect(self, key, timeout, field="hash"):
        return await self.wait(lambda p: p.get("type") in relay_mod.CHANGE_TYPES and p.get(field) == key,
                               timeout)

    async def state(self):
        since = len(self.seen)
        await self.client.send({"type": "team_state_request"})
        return await self.wait(lambda p: p.get("type") == "team_state", 5, since)

    async def command(self, line):
        words = line.split()
        verb = words[0]
        if verb in ("grant", "bit"):
            keys = [int(word, 16) for word in words[1:]]
            if not keys:
                raise ValueError("no keys")
            return await self.burst("bool_set" if verb == "grant" else "bit_set", keys)
        if verb == "rupee":
            deltas = [int(word) for word in words[1:]]
            if not deltas:
                raise ValueError("no deltas")
            return await self.burst("rupee_add", deltas)
        if verb == "item":
            # item NAME CATEGORY COUNT [GET [F0 F1 F2 F3]]: one give call, as an opener would send it
            if len(words) < 4:
                raise ValueError("item NAME CATEGORY COUNT [GET [F0 F1 F2 F3]]")
            flags = [int(word, 0) for word in words[5:9]] or [0, 0, 0, 0]
            self.client.seq += 1
            packet = {"type": "item_give", "origin": self.client.hello["client_id"], "seq": self.client.seq,
                      "name": words[1], "category": int(words[2], 0), "count": int(words[3], 0),
                      "get": int(words[4], 0) if len(words) > 4 else 7, "flags": flags}
            since = len(self.seen)
            await self.client.send(packet)
            await self.wait(lambda p: p.get("type") == "ack" and p.get("seq") == packet["seq"], 5, since)
            return {"sent": packet["seq"], "item": packet}
        if verb == "dup":
            # dup SEQ: deliver this peer's accepted event SEQ (same origin/seq/rev) once more to every
            # other client of the team, as a duplicate delivery would (the relay itself never repeats)
            seq, me = int(words[1]), self.client.hello["client_id"]
            for room in self.relay.rooms.values():
                for team in room.teams.values():
                    for event in team.replay:
                        if event.get("origin") == me and event.get("seq") == seq:
                            targets = [cid for cid, c in team.clients.items() if cid != me]
                            for cid in targets:
                                team.clients[cid].send(event)
                            return {"duplicated": seq, "to": targets}
            raise ValueError("no such accepted event")
        if verb == "expect-item":
            try:
                return {"packet": await self.wait(lambda p: p.get("type") == "item_give" and
                                                  p.get("name") == words[1], float(words[2]))}
            except TimeoutError:
                return {"timeout": True}
        if verb == "expect":
            try:
                return {"packet": await self.expect(int(words[1], 16), float(words[2]))}
            except TimeoutError:
                return {"timeout": True}
        if verb == "received":
            return {"received": self.received}
        if verb == "state":
            return {"state": await self.state()}
        if verb == "mark":
            self.log({"type": "mark", "text": " ".join(words[1:])})
            return {"marked": " ".join(words[1:])}
        raise ValueError("unknown command " + verb)

    async def close(self):
        if self.task:
            self.task.cancel()
        await self.client.close()
        await self.relay.close()


async def serve(args):
    ctl = Path(args.ctl)
    ctl.mkdir(parents=True, exist_ok=True)
    commands, results = ctl / "cmd.txt", ctl / "out.jsonl"
    commands.touch()
    peer = FakePeer(port=args.port, room=args.room, team=args.team, policy=args.policy, seed=args.seed,
                    name=args.name, build_id=args.build_id, record=ctl / "received.jsonl",
                    wallet=args.wallet)
    port = await peer.start()
    print(f"fake peer ready on 127.0.0.1:{port} room={args.room} team={args.team}", flush=True)
    done = len(commands.read_text().splitlines())
    try:
        while True:
            lines = commands.read_text().splitlines()
            while done < len(lines):
                line = lines[done].strip()
                done += 1
                if not line:
                    continue
                if line == "quit":
                    with results.open("a") as stream:
                        stream.write(json.dumps({"t": round(time.time(), 3), "cmd": line, "ok": True}) + "\n")
                    return
                try:
                    result = {"ok": True, **await peer.command(line)}
                except (ValueError, TimeoutError, ConnectionError, EOFError) as error:
                    result = {"ok": False, "error": repr(error)}
                with results.open("a") as stream:
                    stream.write(json.dumps({"t": round(time.time(), 3), "cmd": line, **result}) + "\n")
            await asyncio.sleep(0.1)
    finally:
        await peer.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--ctl", required=True, help="directory for cmd.txt, out.jsonl, received.jsonl")
    parser.add_argument("--port", type=int, default=43390)
    parser.add_argument("--room", default="default")
    parser.add_argument("--team", default="default")
    parser.add_argument("--policy", default="or-v2")
    parser.add_argument("--seed", default="0")
    parser.add_argument("--name", default="FakePeer")
    parser.add_argument("--build-id", default=client_sim.BUILD_ID)
    parser.add_argument("--wallet", type=lambda text: int(text, 0), default=0,
                        help="wallet GameData key for rupee_add (from the private profile)")
    args = parser.parse_args()
    try:
        asyncio.run(serve(args))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
