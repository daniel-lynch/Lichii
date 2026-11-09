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
  itemk HEX NAME CAT COUNT [GET [F0 F1 F2 F3]]  item_give tagged with origin_key HEX (the chest Bool)
  item ... origin=bit:HEX   give with a collectible origin (also chest:HEX or token:HEX:PARTNER_HEX)
  bit HEX ... origin=token:HEX:PARTNER_HEX   flags belonging to the same collectible transaction
  wallet BALANCE CAPACITY   publish wallet baseline and request the team's wallet baselines
  key HEX DELTA             key_add: a small-key gain of DELTA for the dungeon whose total key is HEX
  quest CHALLENGE STEP INDEX  send a raw committed quest step (hex hashes; requires --quest)
  expect-quest CHALLENGE SECS wait for an emulator quest commit; result includes packet
  expect-type TYPE SECS     wait for the emulator's next change of TYPE (rupee_add, key_add, ...)
  dup SEQ                   re-deliver this peer's accepted event SEQ to the other clients (a duplicate)
  expect-item NAME SECS     wait for the emulator's item_give of NAME
  expect HEX SECS           wait for the emulator's bool_set of HEX (any rev); result has the packet
  received                  every change packet received so far (from other origins)
  state                     the relay's team checkpoint as the peer sees it (team_state_request)
  mark TEXT                 a marker line in received.jsonl (for correlating with emulator logs)
  pose BANC CHILD X Y Z YAW [LOADING] [aid=N af=F|null ar=R aseq=N] [eq32=WORD [eqseq=N]]
                            one pose (CHILD/LOADING 0|1; BANC "-" = ""); animation options need --anim-table
                            and a matching peer. Omitted aseq advances on each command (including frozen
                            frame steps). Reuse an explicit aseq for rate-only updates; frame changes need a
                            new sequence. WORD accepts 0x hex (needs the equipment capability).
  poseanim BANC X Y Z YAW ANIM RATE      one child pose with an explicit animation hint (idle|walk|run),
                            a new anim_seq each time (the receiver replays the category's command)
  posecircle BANC CX CY CZ R PERIOD_S HZ DURATION_S
                            start (returns at once) a pose stream walking a circle of radius R
                            around (CX,CY,CZ) in the X/Z plane, one lap per PERIOD_S, HZ poses/s,
                            for DURATION_S (0 = until posestop); yaw = atan2(dx, dz) of travel,
                            child, not loading, t += round(60/HZ) per pose. Replaces any stream.
  posepath BANC HZ SPEED X,Y,Z [X,Y,Z...]
                            start (returns at once) a pose stream walking the waypoints in order at
                            SPEED m/s, HZ poses/s, then holding the last one (1 Hz keepalive) until
                            posestop. Replaces any stream.
  poseequip WORD|off [child=0|1]
                            what later pose streams (posecircle, posepath) carry: an eq32 word (eqseq
                            tracks known changes) and the age; off = no equipment fields
  tp-request [TARGET]       with --teleport: request current destination (default sole online peer),
                            wait up to 5 s; result contains teleport_to or timeout
  tp-answer ok|deny REASON|silent
                            answer from the last pose SENT (default ok); no pose means unready
  tp-log                    bounded received requests and sent answers
  posestop                  stop the stream; result has how many poses it sent
  holdout SECS              kick other clients and refuse their hellos for SECS
  badmod HEX                probe a second client with this mod hash; return ack or error reason
  catchup                   return count and (origin, seq, rev, type, name) of ledger grants
  kick                      close the relay's connection to every other client (the emulator), as a
                            network drop would; the peer itself stays connected
  poses [N]                 the last N (default 20) poses received from other origins, and the total
  leave                     close this peer only (relay announces offline); stops pose streams
  join                      reconnect with the same client_id, name and completed state
  finish                    announce finished=true in the full client state
  presence                  repeat the full client state (no presence transition)
  quit
Every packet the peer receives is also appended to CTL/received.jsonl with a wall-clock time,
except poses: only every 15th received pose (the 1st, 16th, ...) is recorded there."""

import argparse
import asyncio
import importlib.util
import collections
import contextlib
import json
import math
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
                 name="FakePeer", build_id=client_sim.BUILD_ID, record=None, wallet=0, anim_table=None, mod_hash=None, state_file=None, quest=False,
                 teleport=False):
        self.relay = relay_mod.Relay(state_file=state_file)
        self.holdout_until = 0.0
        self.badmod_count = 0
        join = self.relay.join
        def held_join(client, packet):
            if time.monotonic() < self.holdout_until and packet.get("client_id") != self.client.hello["client_id"]:
                raise relay_mod.ProtocolError("held out (test)")
            return join(client, packet)
        self.relay.join = held_join
        self.port = port
        self.client = client_sim.ClientSim(room=room, team=team, name=name, build_id=build_id,
                                           seed_hash=seed, policy_id=policy,
                                           animation=client_sim.animation_descriptor(anim_table), mod_hash=mod_hash,
                                           quest=quest)
        self.client.hello["ghost_equipment_v2"] = True
        if teleport:
            self.client.hello["teleport"] = 1
        self.teleport = teleport
        self.tp_mode, self.tp_reason = "ok", "unready"
        self.tp_seq = 0
        self.tp_history = collections.deque(maxlen=256)
        self.tp_seen = {}
        self.current_pose = None
        self.equipment_seq = 0
        self.equipment_known = {}
        self.stream_eq32, self.stream_child = None, True  # poseequip: what the pose streams carry
        self.record = Path(record) if record else None
        self.finished = False
        self.world_wallet = None
        self.wallet = wallet  # the wallet key from the private profile (rupee_add)
        self.received = []
        self.seen = []
        self.waiters = []
        self.task = None
        self.client.on_pose = self.pose_received
        self.poses = collections.deque(maxlen=1000)  # poses from other origins (the emulator's)
        self.pose_count = 0
        self.stream = None  # the posecircle task
        self.stream_sent = 0

    async def start(self):
        self.port = await self.relay.start("127.0.0.1", self.port)
        await self.client.connect("127.0.0.1", self.port)
        self.task = asyncio.create_task(self.listen())
        return self.port

    def log(self, packet):
        if self.record:
            with self.record.open("a") as stream:
                stream.write(json.dumps({"t": round(time.time(), 3), **packet}) + "\n")

    def pose_received(self, packet):
        self.pose_count += 1
        self.poses.append(packet)
        if self.pose_count % 15 == 1:
            self.log(dict(packet, pose_n=self.pose_count))

    async def send_pose(self, banc, child, pos, yaw, loading=False, step=1, eq32=None, eqseq=None, **anim):
        packet = self.client.pose_packet(banc, child, pos, yaw, loading,
                                         t=self.client.pose_t + step, **anim)
        known = dict(self.equipment_known)
        if eq32 is not None:
            if self.client.ack.get("ghost_equipment_v2") is not True:
                raise ValueError("equipment capability not negotiated")
            relay_mod.validate_equipment(eq32, child)
            unsettled = set()
            for identity_shift, state_shift in ((0, 6), (3, 8)):
                identity, state = (eq32 >> identity_shift) & 7, (eq32 >> state_shift) & 3
                if not ((identity == 0 and state == 0) or (1 <= identity <= 3 and state in (1, 2))):
                    unsettled.update((identity_shift, state_shift))
            for shift, mask, unknown in ((0, 7, 7), (3, 7, 7), (6, 3, 3), (8, 3, 3),
                                         (10, 3, 3), (12, 3, 3), (14, 15, 15), (18, 3, 3)):
                value = (eq32 >> shift) & mask
                if value != unknown and shift not in unsettled:
                    known[shift] = value
            if eqseq is None:
                eqseq = (self.equipment_seq + (known != self.equipment_known)) & 0xFFFF
            packet.update(eq32=eq32, eqseq=eqseq)
        elif eqseq is not None:
            raise ValueError("eqseq requires eq32")
        relay_mod.validate_pose(packet)
        await self.client.send(packet)
        self.current_pose = dict(packet)
        if eq32 is not None:
            self.equipment_known, self.equipment_seq = known, eqseq
        return packet

    async def circle(self, banc, centre, radius, period, hz, duration):
        loop = asyncio.get_running_loop()
        start, step, n = loop.time(), max(1, round(60 / hz)), 0
        cx, cy, cz = centre
        omega = 2 * math.pi / period
        # The animation hint the emulator's sender would infer from this speed (metres per second).
        speed = omega * radius
        anim = "idle" if speed < 0.3 else "walk" if speed < 2.6 else "run"
        nominal, stride = (1.5, 1.2) if anim == "walk" else (4.5, 2.4)
        self.anim_seq = getattr(self, "anim_seq", 0) + 1
        while duration == 0 or n / hz < duration:
            # Absolute schedule: tick n at start + n/hz, so sends never drift.
            delay = start + n / hz - loop.time()
            if delay > 0:
                await asyncio.sleep(delay)
            angle = omega * (n / hz)
            pos = [round(cx + radius * math.cos(angle), 3), cy, round(cz + radius * math.sin(angle), 3)]
            dx, dz = -math.sin(angle), math.cos(angle)  # direction of travel (counter-clockwise in X/Z)
            yaw = round(math.degrees(math.atan2(dx, dz)), 3)
            phase = 0.0 if anim == "idle" else (speed * n / hz / stride) % 1.0
            await self.send_pose(banc, self.stream_child, pos, yaw, False, step, eq32=self.stream_eq32, anim=anim, anim_seq=self.anim_seq,
                                 rate=round(min(10.0, 1.0 if anim == "idle" else speed / nominal), 2),
                                 phase=min(0.99, round(phase, 2)))
            self.stream_sent += 1
            n += 1

    async def path(self, banc, points, hz, speed):
        loop = asyncio.get_running_loop()
        step = max(1, round(60 / hz))
        self.anim_seq = getattr(self, "anim_seq", 0) + 1
        anim = "idle" if speed < 0.3 else "walk" if speed < 2.6 else "run"
        nominal, stride = (1.5, 1.2) if anim == "walk" else (4.5, 2.4)
        pos, yaw, travelled = list(points[0]), 0.0, 0.0
        for target in points[1:]:
            while True:
                dx, dy, dz = (target[i] - pos[i] for i in range(3))
                dist = math.sqrt(dx * dx + dy * dy + dz * dz)
                if dist < 1e-6:
                    break
                yaw = round(math.degrees(math.atan2(dx, dz)), 3) if dx or dz else yaw
                move = min(dist, speed / hz)
                pos = [round(pos[i] + (target[i] - pos[i]) * move / dist, 3) for i in range(3)]
                travelled += move
                await self.send_pose(banc, self.stream_child, pos, yaw, False, step, eq32=self.stream_eq32, anim=anim, anim_seq=self.anim_seq,
                                     rate=round(min(10.0, speed / nominal), 2),
                                     phase=min(0.99, round((travelled / stride) % 1.0, 2)))
                self.stream_sent += 1
                await asyncio.sleep(1 / hz)
        self.anim_seq += 1
        while True:  # hold: idle keepalive
            await self.send_pose(banc, self.stream_child, pos, yaw, False, step, eq32=self.stream_eq32, anim="idle", anim_seq=self.anim_seq,
                                 rate=1.0, phase=0.0)
            self.stream_sent += 1
            await asyncio.sleep(1.0)

    async def stop_stream(self):
        stopped = self.stream is not None and not self.stream.done()
        if self.stream is not None:
            self.stream.cancel()
            try:
                await self.stream
            except asyncio.CancelledError:
                pass
            except (ConnectionError, OSError):
                pass
            self.stream = None
        return stopped

    async def listen(self):
        """The only reader of the client's queue; commands wait on predicates instead."""
        while True:
            try:
                packet = await self.client.receive(timeout=3600)
            except TimeoutError:
                continue
            if packet.get("type") == "teleport_request" and self.teleport:
                self.tp_history.append(packet)
                origin, n = packet["origin"], packet["request_id"]
                if n > self.tp_seen.get(origin, 0):
                    if len(self.tp_seen) < 128 or origin in self.tp_seen:
                        self.tp_seen[origin] = n
                        if self.tp_mode != "silent":
                            answer = {"type": "teleport_to", "target": origin, "request_id": n,
                                      "status": "denied", "reason": self.tp_reason}
                            pose = self.current_pose
                            if self.tp_mode == "ok" and pose and pose["banc"] and not pose["loading"]:
                                answer = {"type": "teleport_to", "target": origin, "request_id": n,
                                          "status": "ok", "room": None, "banc": pose["banc"],
                                          "pos": list(pose["pos"]), "yaw": pose["yaw"],
                                          "age": "child" if pose["child"] else "adult"}
                            elif self.tp_mode == "ok":
                                answer["reason"] = "loading" if pose and pose["loading"] else "unready"
                            await self.client.send(answer)
                            self.tp_history.append(answer)
            self.log(packet)
            self.seen.append(packet)
            if packet.get("type") in relay_mod.CHANGE_TYPES | {"quest_step"}:
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

    async def burst(self, kind, keys, source=None):
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
            if source:
                packet.update(source)
            relay_mod.validate_change(packet, self.client.hello["client_id"])
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

    async def state(self, world_wallet=None):
        if world_wallet is not None:
            self.world_wallet = dict(world_wallet)
            self.client.hello["world_wallet"] = dict(world_wallet)
        since = len(self.seen)
        await self.client.baseline(world_wallet)
        return await self.wait(lambda p: p.get("type") == "team_state", 5, since)

    async def presence(self):
        if self.client.writer is None:
            raise ConnectionError("peer is offline; join first")
        state = {"name": self.client.hello["name"]}
        if self.finished:
            state["finished"] = True
        if self.world_wallet is not None:
            state["world_wallet"] = dict(self.world_wallet)
        await self.client.send({"type": "client_state", "state": state})
        return {"state": state}

    async def leave(self):
        await self.stop_stream()
        if self.task is not None:
            self.task.cancel()
            with contextlib.suppress(asyncio.CancelledError, EOFError, ConnectionError):
                await self.task
            self.task = None
        await self.client.close()
        # Wait for the local relay's finally block before reusing this identity.
        async with asyncio.timeout(5):
            while any(getattr(c, "client_id", None) == self.client.hello["client_id"]
                      for c in self.relay.clients):
                await asyncio.sleep(0.01)

    async def command(self, line):
        words = line.split()
        if not words:
            raise ValueError("empty command")
        verb = words[0]
        if verb in ("leave", "join", "finish", "presence"):
            if len(words) != 1:
                raise ValueError(verb + " takes no arguments")
            if verb == "leave":
                await self.leave()
                return {"online": False}
            if verb == "join":
                if self.client.writer is None:
                    await self.client.connect("127.0.0.1", self.port)
                    self.task = asyncio.create_task(self.listen())
                return {"online": True, **await self.presence()}
            if verb == "finish":
                self.finished = True
            return await self.presence()
        source = {}
        options = [word for word in words[1:] if word.startswith("origin=")]
        if options:
            if len(options) != 1 or verb not in ("item", "bit"):
                raise ValueError("one origin option is allowed on item or bit")
            source = client_sim.origin_fields(options[0].split("=", 1)[1])
            words.remove(options[0])
        if verb in ("grant", "bit"):
            keys = [int(word, 16) for word in words[1:]]
            if not keys:
                raise ValueError("no keys")
            return await self.burst("bool_set" if verb == "grant" else "bit_set", keys, source)
        if verb == "wallet":
            if len(words) != 3:
                raise ValueError("wallet BALANCE CAPACITY")
            return await self.state({"balance": int(words[1], 0), "capacity": int(words[2], 0)})
        if verb == "rupee":
            deltas = [int(word) for word in words[1:]]
            if not deltas:
                raise ValueError("no deltas")
            return await self.burst("rupee_add", deltas)
        if verb == "itemk":
            if len(words) < 5:
                raise ValueError("itemk HEX NAME CATEGORY COUNT [GET [F0 F1 F2 F3]]")
            source, words = {"origin_key": int(words[1], 16)}, ["item"] + words[2:]
            verb = "item"
        if verb == "item":
            # item NAME CATEGORY COUNT [GET [F0 F1 F2 F3]]: one give call, as an opener would send it
            if len(words) not in (4, 5, 9):
                raise ValueError("item NAME CATEGORY COUNT [GET [F0 F1 F2 F3]]")
            flags = [int(word, 0) for word in words[5:9]] or [0, 0, 0, 0]
            self.client.seq += 1
            packet = {"type": "item_give", "origin": self.client.hello["client_id"], "seq": self.client.seq,
                      "name": words[1], "category": int(words[2], 0), "count": int(words[3], 0),
                      "get": int(words[4], 0) if len(words) > 4 else 7, "flags": flags}
            packet.update(source)
            relay_mod.validate_change(packet, self.client.hello["client_id"])
            since = len(self.seen)
            await self.client.send(packet)
            await self.wait(lambda p: p.get("type") == "ack" and p.get("seq") == packet["seq"], 5, since)
            return {"sent": packet["seq"], "item": packet}
        if verb == "quest":
            if len(words) != 4 or self.client.ack.get("quest") != 1:
                raise ValueError("quest CHALLENGE STEP INDEX requires --quest")
            packet = {"type": "quest_step", "origin": self.client.hello["client_id"],
                      "seq": self.client.seq + 1, "challenge": int(words[1], 16),
                      "step": int(words[2], 16), "index": int(words[3], 0)}
            relay_mod.validate_quest(packet, self.client.hello["client_id"])
            self.client.seq += 1
            since = len(self.seen)
            await self.client.send(packet)
            await self.wait(lambda p: p.get("type") == "ack" and p.get("seq") == packet["seq"], 5, since)
            return {"sent": packet["seq"], "quest": packet}
        if verb == "expect-quest":
            if len(words) != 3:
                raise ValueError("expect-quest CHALLENGE SECS")
            challenge = int(words[1], 16)
            try:
                return {"packet": await self.wait(lambda p: p.get("type") == "quest_step" and
                                                  p.get("challenge") == challenge, float(words[2]))}
            except TimeoutError:
                return {"timeout": True}
        if verb == "key":
            if len(words) != 3:
                raise ValueError("key HEX DELTA")
            self.client.seq += 1
            packet = {"type": "key_add", "origin": self.client.hello["client_id"], "seq": self.client.seq,
                      "hash": int(words[1], 16), "delta": int(words[2], 0)}
            since = len(self.seen)
            await self.client.send(packet)
            await self.wait(lambda p: p.get("type") == "ack" and p.get("seq") == packet["seq"], 5, since)
            return {"sent": packet["seq"], "key": packet}
        if verb == "expect-type":
            try:
                return {"packet": await self.wait(lambda p: p.get("type") == words[1], float(words[2]))}
            except TimeoutError:
                return {"timeout": True}
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
        if verb.startswith("tp-"):
            if not self.teleport:
                raise ValueError("requires --teleport")
            if verb == "tp-log" and len(words) == 1:
                return {"teleport_log": list(self.tp_history)}
            if verb == "tp-answer":
                if words[1:] in (["ok"], ["silent"]):
                    self.tp_mode = words[1]
                elif len(words) == 3 and words[1] == "deny" and words[2] in relay_mod.TELEPORT_REASONS:
                    self.tp_mode, self.tp_reason = "deny", words[2]
                else:
                    raise ValueError("tp-answer ok|deny REASON|silent")
                return {"answer": self.tp_mode}
            if verb == "tp-request" and len(words) <= 2:
                peers = [p.client_id for p in self.relay.clients
                         if p.client_id != self.client.hello["client_id"] and p.team is not None
                         and p.teleport and p.team is self.relay.rooms[self.client.hello["room"]].teams[self.client.hello["team"]]]
                if len(words) == 1 and len(peers) != 1:
                    raise ValueError("specify TARGET when there is not exactly one peer")
                target = words[1] if len(words) == 2 else peers[0]
                relay_mod.identifier(target)
                self.tp_seq += 1
                since = len(self.seen)
                await self.client.send({"type": "teleport_request", "target": target, "request_id": self.tp_seq})
                try:
                    reply = await self.wait(lambda p: p.get("type") == "teleport_to" and
                        p.get("request_id") == self.tp_seq and p.get("origin") == target, 5, since)
                    return {"teleport_to": reply}
                except TimeoutError:
                    return {"teleport_to": "timeout"}
            raise ValueError("invalid teleport command")
        if verb == "pose":
            if len(words) < 7:
                raise ValueError("pose BANC CHILD X Y Z YAW [LOADING] [aid=N af=F|null ar=R aseq=N] [eq32=WORD [eqseq=N]]")
            tail, loading, animation, equipment = words[7:], False, {}, {}
            if tail and "=" not in tail[0]:
                if tail[0] not in ("0", "1"):
                    raise ValueError("LOADING must be 0 or 1")
                loading, tail = tail[0] == "1", tail[1:]
            for option in tail:
                key, separator, value = option.partition("=")
                if key in ("eq32", "eqseq") and separator and key not in equipment:
                    equipment[key] = int(value, 0)
                    continue
                if not separator or key not in ("aid", "af", "ar", "aseq") or key in animation:
                    raise ValueError("invalid animation pose option")
                animation[key] = (int(value, 0) if key in ("aid", "aseq") else
                                  None if key == "af" and value == "null" else float(value))
            if animation:
                if "aid" not in animation:
                    raise ValueError("animation pose needs aid")
                animation.setdefault("aseq", (getattr(self, "aseq", 0) + 1) & 0xffffffff)
            if words[2] not in ("0", "1"):
                raise ValueError("CHILD must be 0 or 1")
            packet = await self.send_pose("" if words[1] == "-" else words[1], words[2] == "1",
                                          [float(w) for w in words[3:6]], float(words[6]),
                                          loading, **equipment, **animation)
            if animation:
                self.aseq = animation["aseq"]
            return {"sent": packet}
        if verb == "poseequip":
            if len(words) not in (2, 3) or (len(words) == 3 and words[2] not in ("child=0", "child=1")):
                raise ValueError("poseequip WORD|off [child=0|1]")
            child = self.stream_child if len(words) == 2 else words[2] == "child=1"
            word = None if words[1] == "off" else int(words[1], 0)
            if word is not None:
                relay_mod.validate_equipment(word, child)
            self.stream_eq32, self.stream_child = word, child
            return {"stream_eq32": word, "stream_child": child}
        if verb == "poseanim":
            if len(words) != 8 or words[6] not in ("idle", "walk", "run"):
                raise ValueError("poseanim BANC X Y Z YAW idle|walk|run RATE")
            self.anim_seq = getattr(self, "anim_seq", 0) + 1
            packet = await self.send_pose("" if words[1] == "-" else words[1], True,
                                          [float(w) for w in words[2:5]], float(words[5]), False,
                                          anim=words[6], anim_seq=self.anim_seq, rate=float(words[7]), phase=0.0)
            return {"sent": packet}
        if verb == "posecircle":
            if len(words) != 9:
                raise ValueError("posecircle BANC CX CY CZ R PERIOD_S HZ DURATION_S")
            banc = "" if words[1] == "-" else words[1]
            cx, cy, cz, radius, period, hz, duration = (float(w) for w in words[2:9])
            if period <= 0 or not 0 < hz <= 60 or duration < 0 or radius < 0:
                raise ValueError("need PERIOD_S > 0, 0 < HZ <= 60, R >= 0, DURATION_S >= 0")
            await self.stop_stream()
            self.stream_sent = 0
            self.stream = asyncio.create_task(self.circle(banc, (cx, cy, cz), radius, period, hz, duration))
            return {"started": {"banc": banc, "centre": [cx, cy, cz], "r": radius, "period": period,
                                "hz": hz, "duration": duration}}
        if verb == "posepath":
            if len(words) < 5:
                raise ValueError("posepath BANC HZ SPEED X,Y,Z [X,Y,Z...]")
            banc = "" if words[1] == "-" else words[1]
            hz, speed = float(words[2]), float(words[3])
            points = [[float(v) for v in w.split(",")] for w in words[4:]]
            if not 0 < hz <= 60 or speed <= 0 or any(len(p) != 3 for p in points):
                raise ValueError("need 0 < HZ <= 60, SPEED > 0, points X,Y,Z")
            if len(points) == 1:
                points.append(points[0])
            await self.stop_stream()
            self.stream_sent = 0
            self.stream = asyncio.create_task(self.path(banc, points, hz, speed))
            return {"started": {"banc": banc, "points": points, "hz": hz, "speed": speed}}
        if verb == "catchup":
            # listen() owns the client's receive queue; use its predicate waiters.
            grants, after = [], 0
            while True:
                waiter = asyncio.create_task(self.wait(lambda p: p["type"] == "catchup", 5, len(self.seen)))
                await asyncio.sleep(0)
                await self.client.send({"type": "catchup_request", "after": after})
                page = await waiter
                grants.extend(page["grants"])
                if not page["more"]:
                    return {"count": len(grants), "grants": [[p["origin"], p["seq"], p["rev"], p["type"], p.get("name", "-")] for p in grants]}
                if not page["grants"] or len(grants) > 16384:
                    raise ValueError("invalid catchup progress")
                after = page["grants"][-1]["rev"]
        if verb == "badmod":
            self.badmod_count += 1
            h = self.client.hello
            probe = client_sim.ClientSim(f"badmod-{self.badmod_count}", room=h["room"], team=h["team"],
                build_id=h["build_id"], seed_hash=h["seed_hash"], policy_id=h["policy_id"], mod_hash=words[1])
            try:
                await probe.connect("127.0.0.1", self.port)
                return {"result": probe.ack}
            except ValueError as exc:
                return {"result": str(exc)}
            finally:
                await probe.close()
        if verb in ("kick", "holdout"):
            if verb == "holdout":
                seconds = float(words[1])
                if not math.isfinite(seconds) or seconds < 0:
                    raise ValueError("invalid holdout duration")
                self.holdout_until = time.monotonic() + seconds
            me, kicked = self.client.hello["client_id"], []
            for client in list(self.relay.clients):
                if getattr(client, "client_id", None) not in (None, me):
                    kicked.append(client.client_id)
                    client.writer.close()
            return {"kicked": kicked}
        if verb == "posestop":
            stopped = await self.stop_stream()
            return {"stopped": stopped, "sent": self.stream_sent}
        if verb == "poses":
            count = int(words[1]) if len(words) > 1 else 20
            return {"poses": list(self.poses)[-count:] if count > 0 else [], "count": self.pose_count}
        if verb == "mark":
            self.log({"type": "mark", "text": " ".join(words[1:])})
            return {"marked": " ".join(words[1:])}
        raise ValueError("unknown command " + verb)

    async def close(self):
        await self.stop_stream()
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
                    wallet=args.wallet, anim_table=args.anim_table, mod_hash=args.mod_hash, state_file=args.state_file,
                    quest=args.quest, teleport=args.teleport)
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
    parser.add_argument("--anim-table", help="private coop-anim.json sidecar; enables v4 animation negotiation")
    parser.add_argument("--quest", action="store_true", help="enable quest commands and reception")
    parser.add_argument("--teleport", action="store_true", help="enable teleport revision 1")
    parser.add_argument("--wallet", type=lambda text: int(text, 0), default=0,
                        help="wallet GameData key for rupee_add (from the private profile)")
    parser.add_argument("--mod-hash", type=relay_mod.mod_hash)
    parser.add_argument("--state-file", type=Path, help="optional relay campaign persistence")
    args = parser.parse_args()
    try:
        asyncio.run(serve(args))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
