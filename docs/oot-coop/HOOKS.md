# Static guest-hook evidence and application plan

Target: title `0400486024DEC000`, main build
`E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348`. All addresses below use the
Ghidra image base `0x7100000000`; relocate by subtracting that base and adding
this process's verified main base. Writes require the explicit class/key policy described below; all field claims remain unverified.

`G` below means `~/Downloads/OOT/ghidra`. **Proven** means an operation is visible
in a supplied listing or the exported instruction bytes at the cited address.
It does not prove the current runtime object, synchronization or save durability.
**Inferred** means an interpretation or requirement that still needs validation.
Decompiler parameter numbering is not an ABI specification: FP/vector arguments
in particular distort Ghidra's guessed signatures. Ignore speculative function names.

For the previously unlisted small functions, this pass disassembled the read-only
`G/out10/dump/text_7100000000.bin` using the installed libcapstone.so.5, via
`python3 -I .tmp/disasm-C2.py`. Function boundaries were cross-checked against
`G/out11/functions.tsv`. Result listings are `.tmp/setters-disassembly-C2.txt`,
`.tmp/bool64-disassembly-C2.txt`, `.tmp/write-queues-C2.txt` and
`.tmp/struct-bool-C2.txt`. Only reviewed member/backing-Bool hashes are shipped; no game names or native code blobs are embedded.

## Hook and setter table

| Site / evidence | Proven static ABI and behavior | Preconditions / remaining inference |
|---|---|---|
| Bool setter `0x7100682B20` (`G/out13/decomp/7100682b20.c`; queue reservation `0x7100682BEC`) | `x0=GameData manager`, `w1=value & 1`, `w2=nonzero 32-bit key`. Hash lookup, then queued write; alternate array path calls `0x710376A0B0`. No useful success return is established. | **Inferred:** live manager, valid key, correct save generation and queue capacity must be checked; return is not persistence. **Proven:** not a pure flag store. |
| Int setter `0x7100163B70` (exported bytes; stores at `0x7100163C5C`) | `x0=manager`, `w1=32-bit value`, `w2=nonzero key`. CAS reserves a 12-byte queue record through `manager+0x100`; value/key stored together. Array fallback calls `0x710376A590`. | **Proven** queued integer write; **inferred** signed/application meaning depends on the schema. Scalar grants excluded by protocol v2. |
| Enum setter `0x71003E0980` (`G/out13/decomp/71003e0980.c`) | `x0=manager`, `w1=value`, `w2=nonzero key`; queued value/key pair, array fallback `0x710376ABE0`. | **Inferred:** value must belong to the enum. No numeric-max rule for arbitrary enum hashes. Not a pure write, not a quest-transition substitute. |
| Bool64 reader `0x7103761680` (bytes `0x7103761684..0x7103761714`) | `x0=manager`, `x1=out byte*`, `x2=nonzero actor key`; sorted binary search; `w0=1` and writes the byte on match, else `w0=0`. | **Proven** layout below. **Inferred:** stable loaded table and concurrency guarantee still needed before an emulator reader uses it. |
| Bool64 setter `0x7103761720` (bytes `0x7103761724..0x71037617A8`) | `x0=manager`, `w1=bool`, `x2=64-bit actor key`; finds record index in `w3`, changes `x0` to manager+`0xC58`, masks `w1` and tail-calls `0x7100F071C0`. Missing key returns without writing. | **Proven:** indexed deferred queue, not a direct OR. **Inferred:** first candidate guest call after field validation; leave runtime address gate closed until manager lifetime, frame site, queue and save behavior pass. **Field C3:** only static caller is `0x71028E6460`; actor flags use the copy `0x7100F07130`, and chests use struct `0xCE0C4DFE` (section below). |
| Bool64 queue helper `0x7100F071C0` (bytes through `0x7100F07244`) | `x0=table`, `w1=bool`, `w3=resolved index`. CAS queue reservation; byte value at entry+0, index at +4. Capacity/generation mismatch branches to abort. | **Proven:** do not call with an unvalidated index; do not bypass by writing the record byte. Queue-consumer notifications/dirty tracking remain **unknown**. |
| Quest setter `0x7102CF54A0`; observation `0x7102CF54A4` (`G/out17r/decomp/7102cf54a0.c`) | `x0=challenge manager`, `w1=challenge hash`, `w2=step hash`, `w3=UI choice`, `w4=mark current subentries`, `w5=force`. Three record sets; sets pending hash/flags; returns 1 for accepted/no-op, 0 for missing or protected state. | **Proven:** may move backwards, accepts an unresolved step hash as pending, and blocks terminal/not-ready states unless forced. A successful return does not prove a step applied. UI meaning inferred from caller. Do not force. |
| Quest apply `0x7102CF1830` (`G/out17r/decomp/7102cf1830.c`) | Integer argument `x0=record+0x18`; `w1` controls old-step processing. Resolves pending hash, invokes `0x7102CF1160`, writes index, invokes `0x7102CF13A0`. | **Proven:** more than one field changes and callbacks run. Entry trace is before mutation. FP/vector argument needs in callees remain unverified; never inject this interior apply path. |
| Played event `0x7102BDC4E0` (`G/out17/decomp/7102bdc4e0.c`) | `x0=task wrapper`; task at +0x10, name pointer at task+8, length including NUL at task+0x10. Finds `HavePlayedEvent` struct then hashes the member name. | **Proven:** this is a struct/member operation, not `Murmur3("HavePlayedEvent." + name)` as the skeleton assumed. Trigger timing/creator and success are **inferred/unknown**. No v2 grant emitted from this hook. |
| Struct-bool helpers `0x7103745A10`, `0x7103745CD0`, fallback `0x7103745B70` (caller above and `.tmp/struct-bool-C2.txt`) | First two: `x0=manager+8` or `manager+0x68`, `w1=1`, `x2=local struct handle*`, `w3=member hash`, `w4=(manager+0xD0C == 0)`; low result bit controls fallback. Fallback adds `w4=0`, moves mode into `w5`. | **Proven** caller setup. Handle includes a resolved record and flags, not just an opaque hash. Member-key/type lookup must be reconstructed; no generic flat-bool adapter for these calls. |
| Frame marker `0x71011E7A50` (`G/out4/decomp/71011e7a50.c`, `G/out12/ctx/71011e7a50.txt`, exported bytes) | Starts `pacibsp`; calls `nn::os::WaitEvent` at `0x71011E7A68`. Existing names identify `GameFramework::waitPresentDone`. | **Inferred** safe injection boundary, supported by prior debugger experiment in `findings-scene-tour.md`, not revalidated here. C3 installs this marker through the same instruction-substitution path as the other trace hooks; it drives the co-op drain on the first guest thread that reaches it. With co-op on and `EDEN_NX2_GAMEDATA_TRACE` off, this is the **only** substituted instruction: the Bool64 and chest trace sites are installed by the trace alone. |
| Wallet entry `0x71033A6D70` (C5 static, C6 field) | `x0=pouchMgr` (`*(main+0x4E78E28)`, GOT `main+0x4C7E0D0`), `w1`=signed delta, `w2`=reason (7 in `OneshotIncreaseRupee`, 4 in another caller). Body: `ldadd w1` into `pouchMgr+0x3BD4`, `str w2` to `+0x3BD8`; then, if the global at `0x7104E6B718` is non-null, a tail call to the recorder `0x7100E46AB0` (so it is not a pure leaf, as C5 said). The pouch update `main+0x5EBE0` later clamps the pending delta into `PlayerStatus.CurrentRupee`. | **Proven** instruction bytes and callers; class `rupee` calls it from the frame drain (C6). |
| Pouch item entry | `0x710110D570` push-to-pouch by actor name (C5 static; `x1=&{char* name,...}`, `w2` category, `w3` count, `x4` get type). `71033a4fa0.c` is the remove path. | **Not field-tested.** Side effects (models, UI, autosave) and frame-thread safety unknown. |

## Storage layout and what must be changed

**Proven: typed table construction.** `G/out13/decomp/7100dff1c0.c` passes
manager+8 to the bool builder, +0xC8 to int, +0x248 to enum, +0xC08 to struct,
and +0xC58 to Bool64bitKey. Normal scalar/array tables are distinct. This is
runtime storage; offsets are not offsets inside `progress.sav`.

**Proven: ordinary bool queue.** The bool setter searches an open-addressed table
at manager+0x20 with capacity at +0x2C, 8-byte key/index slots and zero-key termination.
The scalar queue has capacity +0x30, buffer pointer +0x38 and CAS state +0x40.
Records are 8 bytes: value byte at +0, key at +4. An alternate hash table at +0x80
selects array records at +0x78 with stride 0x40. These queue addresses must never
be mistaken for authoritative save-value addresses. Queue-full abort is real.

**Proven: int/enum read-side records.** `G/out19/dis/7100837070.txt` at
`0x710083712C..0x7100837158` reads int count/data from manager+0xD0/+0xD8,
stride 0x28, value +0x1C. `G/out19/dis/71007e73d0.txt` at
`0x71007E7418..0x71007E7444` reads enum count/data from +0x250/+0x258,
stride 0x38, value +0x1C. These are getters (`x1=output`, `w2=key`), not setters.
A field location does not establish that writing it is sufficient.

**Proven: Bool64bitKey records.** Getter `0x7103761680` uses count at manager+0xC58
and data pointer at +0xC60. Sorted entries have stride 0x10, key u64 at +0 and
current bool byte at +0xC. Builder `0x71005AE6B0` writes key at `0x71005AE7E8`,
initialization word at +8 (`0x71005AE7F0`; default-value meaning **inferred**)
and metadata bytes at +0xD/+0xF. Queue fields
relative to table=manager+0xC58: capacity +0x10, buffer +0x18, CAS state +0x20;
entries are 8 bytes `(bool byte, padding, u32 record index)`.
**Inferred:** this is the first useful permanent-check class; classification of
individual keys as permanent vs transient must come from the private schema and tests.
No pure write is approved: setter and reader prove deferred application exists.

**Proven: quest sets.** Setter scans `(u32 count, pointer at +8)` at manager offsets
0x40/0x50/0x60. Each record is 0x160 bytes: challenge hash +8, current step hash +0x10,
flags u16 +0x16, state at +0x18. Relative to state: current index +0x64, count +0x68,
step array +0x70, pending hash +0x8C. Steps have stride 0x90 and hash +0x78.
The setter writes pending hash at record+0xA4 and flags including 0x400; it does not
commit an index immediately. The apply function updates index and then the other
state through callbacks. Syncing only the enum or taking the largest hash is wrong.

**Proven: played-event struct.** `0x7102BDC4E0` uses the struct lookup at manager+0xC20,
capacity +0xC2C, record count/data +0xC10/+0xC18, stride 0x20 and discriminator +0x18.
This is a two-level namespace. Protocol v2 has no struct-member grant; exclude it.

**Inferred / prior field evidence only: serialized item state.** `RESEARCH.md` §9
reports ownership keys with `IsGet.` prefix, stock keys `PouchData.<actor>.StockNum`,
autosave on some grants, and give-actor vs pouch-actor differences. Its observations
are not static proof of the live pouch layout. `G/tools/tour/save_edit.py` assumes a
serialized 8-byte `(u32 hash,u32 inline-value-or-offset)` directory from 0x30 to the
u32 boundary at file+8; values may instead point to strings/vectors. No complete save
schema, Bool64 serialization, inventory transaction or small-key counter ABI is
established here. Never patch disposable or real saves using those assumptions alone.

## Chest-opened state: struct 0xCE0C4DFE, not Bool64 (C3 field follow-up)

Source: `~/Downloads/OOT/field/coop-C3-20261108-0550/CHEST-STATE.md`. Static claims below
come from the exported text bytes (`G/out10/dump/text_7100000000.bin`, disassembled with
libcapstone). Save claims come from the C3 saves and the fieldb run saves.

**Proven (save + schema): where a chest's opened flag is stored.** It is a member of GameData
Struct `0xCE0C4DFE` (name unknown). The struct has 206 members. The member hash is
`Murmur3_32("<decimal u64 instance id>")`, and the member value is the hash of an ordinary
32-bit **Bool**. All 206 Bools are ResetTypeValue 24, SaveFileIndex 0. These members cover
184 of the randomizer's 190 chest locations; the six left out are `Obj_TBox_A_04`. In the saves,
six of six chest opens flipped exactly the backing Bool of the chest that was opened. C3's c1
(`0x3F38F9B9A399E19A`) is member `0xB3DEDFE4`, which maps to Bool `0xC8643846`; it went 0→1 in
slot_01. No save has a chest key in the Bool64 section, including real-play saves with 11 and
39 chest Bools set. The Bool64 section (save type 32) holds sorted u64 true keys with a zero
terminator; it contains pickups, breakables and enemies only.

**Proven (static): run-time write path.** All addresses use the image base:

| Site | Behavior |
|---|---|
| `0x7100F07040` (entry `ldr x8,[x0,#0x3d8]` = `F941EC08`; `pacibsp` at +8) | Actor GameData flag setter. `x0` = actor-side object, `w1` = value. `x8 = [x0+0x3D8]`; `ldset` on `[x8+0x98]`. If the value is set, flag bit 5 of `[x8+0x14]` is set, and `w19 = [x8+8]` (member hash) is nonzero, it calls handle `0x71009B3EB0(mgr,&h,0xCE0C4DFE)` and then `0x7100608A10(mgr,1,&h,w19)`. Then, if flag bit 3 is set and `[x8+0x88]` (instance u64) is nonzero, it calls the **second Bool64 setter** `0x7100F07130(mgr,1,id)`. `mgr = *(main+0x4E842A8)` through GOT `main+0x4C7E180`. Callers: `0x7100F06E80` (logic node, passes a bool input), `0x7102C251A0`, `0x7102DE6A60`, and the vtable thunk `0x71028663E0`. |
| `0x71009B3EB0` | Struct handle lookup: hash table at mgr+0xC20 (capacity +0xC2C); record = mgr+0xC18 + idx*0x20, idx < count at mgr+0xC10. `h = {rec*, u32 0, u8 alt}`, with `alt=1` when rec+0x18 ≥ 2. |
| `0x7100608A10` (`pacibsp`) | Struct-member Bool setter. `x0=mgr`, `w1=value`, `x2=&h`, `w3=member hash`. It reads the member table at `[rec+8]` (capacity `[rec+0x14]`, slots `{member u32, bool-key u32}`), finds the bool key in the flat bool table at mgr+0x20 (capacity +0x2C), and pushes an 8-byte record into the **ordinary bool queue**: capacity mgr+0x30, buffer +0x38, CAS +0x40; value byte +0, key +4. These are the same words that Bool setter `0x7100682B20` writes. When the queue is full it goes to `0x71034FC280`; when the key is missing it goes through mgr+0x68 (`0x7103744F00`). |
| `0x7100DCD300` (`pacibsp`) | `bool IsChestOpened(u64 id)`: decimal → Murmur3 → struct 0xCE0C4DFE → getter `0x7100581170`. Called from the generic flag evaluator `0x7100206290` (other branches of that function read a named Bool and Bool64 reader `0x7103761680`). The map's TBox code at `0x7100B64E60` uses the same construction inline. |
| `0x7100581170` | Struct-member Bool getter. The value is the flat bool record at mgr+0x18 + idx*0x18, byte +0x16 (count +0x10). |
| `0x7100F07130` (`cbz x2` then `mov x8,x0` = `AA0003E8` at +4) | **Second copy** of the Bool64 setter, byte-identical to `0x7103761720`; tail-calls the same queue helper `0x7100F071C0`. |
| `0x7100838960` (`ldr w8,[x0,#0xc58]` = `B94C5808` at +4) | Second copy of the Bool64 reader `0x7103761680`. |

**Correction to the C3 hook choice.** The reviewed Bool64 setter `0x7103761720` has exactly one
static caller, `0x71028E6460` (the InstanceGameParameter path). Actor dead/collected flags go
through `0x7100F07130`. The reviewed reader has three callers. That explains why the C3 field
run logged zero `bit-*` lines. A chest never reaches Bool64 at all. **Inferred:** a chest's
placement flags have bit 3 clear (its ActorParam names `ActorGameDataSetting/AlwaysRespawn`),
so its Bool64bitKey schema entry (189 TBox entries exist) is never written.

**Consequences (inferred, needs field validation).**
- A chest is a `bool`-class key, not a `bit` key. The 206 reviewed keys are the backing Bool
  hashes, not actor hashes.
- The manager comes from the cell `*(main+0x4E842A8)` on the frame hook. Do not wait for a
  reader or setter call to learn it.
- The read side for the baseline and the observer is the flat bool table (+0x20/+0x2C →
  index; +0x10/+0x18 stride 0x18, byte +0x16).
- A grant is a synthetic call to `0x7100682B20(mgr, 1, bool_key)`. It produces the same queued
  record that `0x7100608A10` produces for a local open. It does **not** run
  `0x7100F07040`'s actor-side `ldset [x8+0x98]`, it does not animate a chest that is already
  loaded (spawn reads the state), and it grants **no item**. Items are separate
  `IsGet.<actor>` Bools and `PouchData.<actor>.StockNum` Ints, and the pouch grant ABI is
  still unknown.
- `bit` stays the class for pickups and breakables. If it is kept, it must also observe the
  copies at `0x7100838964` / `0x7100F07134`, which have the same original words as the
  current hooks.

## C4 frame-drain application path

`oot_gamedata_trace.h` now substitutes the verified `pacibsp` at main+0x11E7A50.
At that frame it binds the manager directly from `*(main+0x4E842A8)`, without waiting
for any accessor call. The trace includes the Bool64 accessor copies at +0x838964 and
+0xF07134. Trace-only `pacibsp` sites +0x608A10 and +0xF07048 log struct member/value
and actor member/instance key/value; they require `EDEN_NX2_GAMEDATA_TRACE` and are
not installed merely by enabling co-op. Entry observations are **not** receipts:
the observer polls committed values at the frame boundary and emits only false→true
changes (or true baseline values).

`arm_dynarmic_64.cpp` resumes each substituted instruction once, then invokes
`Session::Hook`. At a frame, the resumed PC is frame+4. `Session` detaches at most 64
incoming entries under its transport lock, releases that lock, and feeds its persistent
`FrameDrain<GameDataBackend>` on the owning guest thread. A separate session mutex
protects guest bookkeeping across host workers. The network thread never touches guest
memory or registers. Other trace hooks do not drain.

The chest class is `bool`: flat hash/index table + deferred setter at main+0x682B20,
x0=manager, w1=1, w2=backing Bool key. `oot_coop_chest_keys.inc` contains 206 member/hash
pairs from GameDataList struct 0xCE0C4DFE; `ChestBool` resolves a member hash and
`IsChestBool` restricts the production adapter to that catalog. The runtime only polls
configured keys, at most 206 chest flags per frame. Each lookup probes at most 64
hash slots (modulo capacity, linear probing with wraparound); missing keys, invalid
indices/bytes and exhausted probe budgets fail closed. No entire Bool table scan runs.
The `bit` class remains the Bool64 adapter for pickups, breakables and enemies, with
deferred setter main+0x3761720 and x2=reviewed actor key. Both adapters validate mapped
ranges, stable table/queue identity, original setter instructions and queue headroom.
Application requires two frame visits with a valid manager binding. Each frame first
compares the manager cell and every bound table/queue identity with the binding. If the
cell was cleared or moved, or a table was reallocated (title screen ↔ save load), and no
synthetic call is in flight, the drain forgets that epoch's baseline, unbinds, logs
`manager moved <old> -> <new>; baseline re-bound` (first 16 times) and exports a new
baseline once the new tables are readable. A grant awaiting its commit is re-read
against the new tables; receipts and echo suppression survive. A move while a call is
in flight, unreadable post-baseline state, or an observed true→false rollback under the
same binding quarantines injection for this process; receipts are never reset while
the old guest can return.
The process epoch is currently 1. This is conservative detection, **not a proven
save-load/transition lifecycle hook**; same-address reloads and event scheduling still
need the field checks below.

The loader allocates an owned 16 KiB RW guest stack and a separate one-page sentinel
after loading main, before guest execution, through `KProcess::AllocateDebugCallMemory`.
Both are Stack-region mappings. The sentinel page stays RW: in a 39-bit address space the
AliasCode/Code region spans the heap and alias regions, so mapping it there fails
(`ResultInvalidCurrentMemory`, 0xd401, first seen in the C4 field launch), and only Code
states can be reprotected to RX. Dynarmic fetches code with a mapped-address check only.
Both allocations are charged to the process resource limit and heap budget and released
with its page table. No allocation/page-table locks are taken by the frame callback. It does not borrow a game's stack or executable padding.
The backend captures all x0–x30, all 32 SIMD registers, PC, SP, PSTATE, FPCR, FPSR,
TPIDR_EL0 and TPIDRRO_EL0. It clears both Dynarmic's local and the core's global
exclusive reservations on redirect/restore (STXR retries, as after a context switch).
It redirects to the setter with a private SP and sentinel LR. Only the matching
thread, epoch, sentinel and return SP restore that context. Invalid sentinel hits
halt through the normal instruction-breakpoint path. No normal Resume follows a
sentinel restore. Setter return means enqueueing; `applied` is logged only when a
later frame reads true. A 120-frame commit timeout quarantines without replay.
A setter that never reaches another hook/frame cannot be timed out by this frame
clock; it must be diagnosed by the owner. No arbitrary-PC recovery is implemented.

The shared grant engine retains its atomic masked `data-or` branch and tests it with
fake memory. **No production pure-write class is enabled.** Every GameData layout
researched above either uses a deferred queue or has unknown dirty/notification
obligations. Writing the Bool64 byte directly would contradict the evidence.
The `bool` adapter only marks chest locations checked. It grants no item or wallet
value. The `rupee` class (C6) is the only wallet path: a signed delta through the game's
wallet entry above, applied by the game's own pouch update; it is grant-only (the local
wallet is not observed). Neither int/enum/quest/item-pouch nor arbitrary struct-member writes are enabled.

## Policy and logs

- `EDEN_NX2_OOT_COOP=1`: enable loopback transport, default `127.0.0.1:43390`;
  `EDEN_NX2_OOT_COOP_RELAY=host:port` overrides it. The older `COOP=host:port` form works.
- `EDEN_NX2_OOT_COOP_ALLOW`: comma-separated `bool,bit` tokens, no spaces or empty
  tokens. Default is the **empty set**. Invalid tokens reject session creation.
  The old `ALLOW_CLASSES` variable is no longer consulted.
- `EDEN_NX2_OOT_COOP_KEYS`: JSON policy with typed arrays, for example
  `{"bool":[3362011206],"bit":[123]}` (numeric hashes). At most 4096 keys in total;
  `bool` keys must belong to the reviewed 206-key catalog. A legacy bare array still
  means `bit` only. Default is empty; class selection alone cannot grant anything.
  Deny-list keys are removed. Both peers must use the same reviewed key set/policy ID.
  `tools/oot/coop/chest-bools.json` configures all 206 chest Bools. No actor names or
  instance IDs are included in that file or the member table.

Regenerate both hash-only files from a local GameDataList JSON export (the lane's BYML
reader must preserve integer hashes):

```sh
python3 -I tools/oot/coop/chest_keys.py "$GAMEDATA_JSON" \
  --table src/core/arm/dynarmic/oot_coop_chest_keys.inc \
  --keys tools/oot/coop/chest-bools.json
```

Source: `GameData__GameDataList.Product.110.byml`, `Data.Struct` entry `Hash=0xCE0C4DFE`,
`DefaultValue[].{Hash,Value}`. The generator verifies distinct members/backing keys and
that each backing key is in `Data.Bool` with ResetTypeValue 24 and SaveFileIndex 0.

The observer consults the engine's class/key receipts before emitting any value.
Receipts survive queueing, host worker changes, sentinel return, delayed commit,
duplicate delivery and quarantine. Each application emits exactly one line:

```
OoT co-op: applied class=bool key=<16-hex> value=1 path=setter echo=1-bool-<16-hex>
```

`started`, `returned` and `suppressed` carry the same key and echo ID. `already-true`
is a satisfied receipt without a write. `baseline` and `observed` identify outbound
baseline/local mutations. A complete allowed-set read logs `baseline exported`;
a FIFO state-request after its outgoing acknowledgements logs `baseline exchanged`
on reply. That is a relay snapshot barrier, not proof of guest/save convergence.
Until every reviewed key is readable, the first frame that cannot read one logs
`baseline waiting: class=bool key=<16-hex> unreadable` once; no baseline is exported.
The frame hook reads the manager cell every time; a changed or cleared manager re-binds
the baseline (above) instead of quarantining. Per-class table identity is also rechecked
when reading flags, and a change found there mid-frame still quarantines.

Unit tests exercise the real JIT's frame→synthetic setter→sentinel→continuation path
and a fake two-peer observer/queue/application path. These do not validate this game's
manager lifetime, event exclusion, queue-consumer effects or save durability. Follow
FIELD-PLAN.md on disposable saves before claiming the field candidate works.
