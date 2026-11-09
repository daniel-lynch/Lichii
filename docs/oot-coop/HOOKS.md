# Static guest-hook evidence and application plan

Game-derived values (build ID, module offsets, hashes, instruction words) are written as
`<build-id>`, `<offset>`, `<hash>` and `<key>` here. The values themselves live only in the
user-supplied private key/offset file and the private research notes.

Target: title `<title-id>`, main build
`<build-id>`. All addresses below use the
Ghidra image base `<offset>`; relocate by subtracting that base and adding
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
| Bool setter `<offset>` (`G/out13/decomp/<offset>.c`; queue reservation `<offset>`) | `x0=GameData manager`, `w1=value & 1`, `w2=nonzero 32-bit key`. Hash lookup, then queued write; alternate array path calls `<offset>`. No useful success return is established. | **Inferred:** live manager, valid key, correct save generation and queue capacity must be checked; return is not persistence. **Proven:** not a pure flag store. |
| Int setter `<offset>` (exported bytes; stores at `<offset>`) | `x0=manager`, `w1=32-bit value`, `w2=nonzero key`. CAS reserves a 12-byte queue record through `manager+0x100`; value/key stored together. Array fallback calls `<offset>`. | **Proven** queued integer write; **inferred** signed/application meaning depends on the schema. Scalar grants excluded by protocol v2. |
| Enum setter `<offset>` (`G/out13/decomp/<offset>.c`) | `x0=manager`, `w1=value`, `w2=nonzero key`; queued value/key pair, array fallback `<offset>`. | **Inferred:** value must belong to the enum. No numeric-max rule for arbitrary enum hashes. Not a pure write, not a quest-transition substitute. |
| Bool64 reader `<offset>` (bytes `<offset>..<offset>`) | `x0=manager`, `x1=out byte*`, `x2=nonzero actor key`; sorted binary search; `w0=1` and writes the byte on match, else `w0=0`. | **Proven** layout below. **Inferred:** stable loaded table and concurrency guarantee still needed before an emulator reader uses it. |
| Bool64 setter `<offset>` (bytes `<offset>..<offset>`) | `x0=manager`, `w1=bool`, `x2=64-bit actor key`; finds record index in `w3`, changes `x0` to manager+`0xC58`, masks `w1` and tail-calls `<offset>`. Missing key returns without writing. | **Proven:** indexed deferred queue, not a direct OR. **Inferred:** first candidate guest call after field validation; leave runtime address gate closed until manager lifetime, frame site, queue and save behavior pass. **Field C3:** only static caller is `<offset>`; actor flags use the copy `<offset>`, and chests use struct `<hash>` (section below). |
| Bool64 queue helper `<offset>` (bytes through `<offset>`) | `x0=table`, `w1=bool`, `w3=resolved index`. CAS queue reservation; byte value at entry+0, index at +4. Capacity/generation mismatch branches to abort. | **Proven:** do not call with an unvalidated index; do not bypass by writing the record byte. Queue-consumer notifications/dirty tracking remain **unknown**. |
| Quest setter `<offset>`; observation `<offset>` (`G/out17r/decomp/<offset>.c`) | `x0=challenge manager`, `w1=challenge hash`, `w2=step hash`, `w3=UI choice`, `w4=mark current subentries`, `w5=force`. Three record sets; sets pending hash/flags; returns 1 for accepted/no-op, 0 for missing or protected state. | **Proven:** may move backwards, accepts an unresolved step hash as pending, and blocks terminal/not-ready states unless forced. A successful return does not prove a step applied. UI meaning inferred from caller. Do not force. |
| Quest apply `<offset>` (`G/out17r/decomp/<offset>.c`) | Integer argument `x0=record+0x18`; `w1` controls old-step processing. Resolves pending hash, invokes `<offset>`, writes index, invokes `<offset>`. | **Proven:** more than one field changes and callbacks run. Entry trace is before mutation. FP/vector argument needs in callees remain unverified; never inject this interior apply path. |
| Played event `<offset>` (`G/out17/decomp/<offset>.c`) | `x0=task wrapper`; task at +0x10, name pointer at task+8, length including NUL at task+0x10. Finds `HavePlayedEvent` struct then hashes the member name. | **Proven:** this is a struct/member operation, not `Murmur3("HavePlayedEvent." + name)` as the skeleton assumed. Trigger timing/creator and success are **inferred/unknown**. No v2 grant emitted from this hook. |
| Struct-bool helpers `<offset>`, `<offset>`, fallback `<offset>` (caller above and `.tmp/struct-bool-C2.txt`) | First two: `x0=manager+8` or `manager+0x68`, `w1=1`, `x2=local struct handle*`, `w3=member hash`, `w4=(manager+0xD0C == 0)`; low result bit controls fallback. Fallback adds `w4=0`, moves mode into `w5`. | **Proven** caller setup. Handle includes a resolved record and flags, not just an opaque hash. Member-key/type lookup must be reconstructed; no generic flat-bool adapter for these calls. |
| Frame marker `<offset>` (`G/out4/decomp/<offset>.c`, `G/out12/ctx/<offset>.txt`, exported bytes) | Starts `pacibsp`; calls `nn::os::WaitEvent` at `<offset>`. Existing names identify `GameFramework::waitPresentDone`. | **Inferred** safe injection boundary, supported by prior debugger experiment in `findings-scene-tour.md`, not revalidated here. C3 installs this marker through the same instruction-substitution path as the other trace hooks; it drives the co-op drain on the first guest thread that reaches it. With co-op on and `EDEN_NX2_GAMEDATA_TRACE` off, this is the **only** substituted instruction: the Bool64 and chest trace sites are installed by the trace alone. |
| Wallet entry `<offset>` (C5 static, C6 field) | `x0=pouchMgr` (`*(main+<offset>)`, GOT `main+<offset>`), `w1`=signed delta, `w2`=reason (7 in `OneshotIncreaseRupee`, 4 in another caller). Body: `ldadd w1` into `pouchMgr+0x3BD4`, `str w2` to `+0x3BD8`; then, if the global at `<offset>` is non-null, a tail call to the recorder `<offset>` (so it is not a pure leaf, as C5 said). The pouch update `main+<offset>` later clamps the pending delta into `PlayerStatus.CurrentRupee`. | **Proven** instruction bytes and callers; class `rupee` calls it from the frame drain (C6). |
| Pouch item entry | `<offset>` give-to-pouch by actor name (C5/C7 static): `x0`=pouchMgr, `x1`=&cell with cell=`{const char* name}` (not a SafeString), `w2` PouchCategory, `w3` count, `x4` PouchGetType, `w5..w7` flags, a ninth argument byte at `[sp]`; returns `w0&1`. It writes IsGet/StockNum through GameData struct members (GameData is authoritative: no runtime inventory cache), the capacity struct and sub-items (it calls itself), and touches HUD and equipment objects. The remove path starts with the same four words, so the offset identifies the give. A chest's flow give returns to one call site (profile `item_callers`). | C7 `item` class: trapped at its `pacibsp` on the opener, replayed on the peer. Field status: RESULT C7. |

## Storage layout and what must be changed

**Proven: typed table construction.** `G/out13/decomp/<offset>.c` passes
manager+8 to the bool builder, +0xC8 to int, +0x248 to enum, +0xC08 to struct,
and +0xC58 to Bool64bitKey. Normal scalar/array tables are distinct. This is
runtime storage; offsets are not offsets inside `progress.sav`.

**Proven: ordinary bool queue.** The bool setter searches an open-addressed table
at manager+0x20 with capacity at +0x2C, 8-byte key/index slots and zero-key termination.
The scalar queue has capacity +0x30, buffer pointer +0x38 and CAS state +0x40.
Records are 8 bytes: value byte at +0, key at +4. An alternate hash table at +0x80
selects array records at +0x78 with stride 0x40. These queue addresses must never
be mistaken for authoritative save-value addresses. Queue-full abort is real.

**Proven: int/enum read-side records.** `G/out19/dis/<hash>.txt` at
`<offset>..<offset>` reads int count/data from manager+0xD0/+0xD8,
stride 0x28, value +0x1C. `G/out19/dis/<offset>.txt` at
`<offset>..<offset>` reads enum count/data from +0x250/+0x258,
stride 0x38, value +0x1C. These are getters (`x1=output`, `w2=key`), not setters.
A field location does not establish that writing it is sufficient.

**Proven: Bool64bitKey records.** Getter `<offset>` uses count at manager+0xC58
and data pointer at +0xC60. Sorted entries have stride 0x10, key u64 at +0 and
current bool byte at +0xC. Builder `<offset>` writes key at `<offset>`,
initialization word at +8 (`<offset>`; default-value meaning **inferred**)
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

**Proven: played-event struct.** `<offset>` uses the struct lookup at manager+0xC20,
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

## Chest-opened state: struct <hash>, not Bool64 (C3 field follow-up)

Source: `~/Downloads/OOT/field/coop-C3-20261108-0550/CHEST-STATE.md`. Static claims below
come from the exported text bytes (`G/out10/dump/text_7100000000.bin`, disassembled with
libcapstone). Save claims come from the C3 saves and the fieldb run saves.

**Proven (save + schema): where a chest's opened flag is stored.** It is a member of GameData
Struct `<hash>` (name unknown). The struct has 206 members. The member hash is
`Murmur3_32("<decimal u64 instance id>")`, and the member value is the hash of an ordinary
32-bit **Bool**. All 206 Bools are ResetTypeValue 24, SaveFileIndex 0. These members cover
184 of the randomizer's 190 chest locations; the six left out are `Obj_TBox_A_04`. In the saves,
six of six chest opens flipped exactly the backing Bool of the chest that was opened. C3's c1
(`<key>`) is member `<hash>`, which maps to Bool `<hash>`; it went 0→1 in
slot_01. No save has a chest key in the Bool64 section, including real-play saves with 11 and
39 chest Bools set. The Bool64 section (save type 32) holds sorted u64 true keys with a zero
terminator; it contains pickups, breakables and enemies only.

**Proven (static): run-time write path.** All addresses use the image base:

| Site | Behavior |
|---|---|
| `<offset>` (entry `ldr x8,[x0,#0x3d8]` = `<hash>`; `pacibsp` at +8) | Actor GameData flag setter. `x0` = actor-side object, `w1` = value. `x8 = [x0+0x3D8]`; `ldset` on `[x8+0x98]`. If the value is set, flag bit 5 of `[x8+0x14]` is set, and `w19 = [x8+8]` (member hash) is nonzero, it calls handle `<offset>(mgr,&h,<hash>)` and then `<offset>(mgr,1,&h,w19)`. Then, if flag bit 3 is set and `[x8+0x88]` (instance u64) is nonzero, it calls the **second Bool64 setter** `<offset>(mgr,1,id)`. `mgr = *(main+<offset>)` through GOT `main+<offset>`. Callers: `<offset>` (logic node, passes a bool input), `<offset>`, `<offset>`, and the vtable thunk `<offset>`. |
| `<offset>` | Struct handle lookup: hash table at mgr+0xC20 (capacity +0xC2C); record = mgr+0xC18 + idx*0x20, idx < count at mgr+0xC10. `h = {rec*, u32 0, u8 alt}`, with `alt=1` when rec+0x18 ≥ 2. |
| `<offset>` (`pacibsp`) | Struct-member Bool setter. `x0=mgr`, `w1=value`, `x2=&h`, `w3=member hash`. It reads the member table at `[rec+8]` (capacity `[rec+0x14]`, slots `{member u32, bool-key u32}`), finds the bool key in the flat bool table at mgr+0x20 (capacity +0x2C), and pushes an 8-byte record into the **ordinary bool queue**: capacity mgr+0x30, buffer +0x38, CAS +0x40; value byte +0, key +4. These are the same words that Bool setter `<offset>` writes. When the queue is full it goes to `<offset>`; when the key is missing it goes through mgr+0x68 (`<offset>`). |
| `<offset>` (`pacibsp`) | `bool IsChestOpened(u64 id)`: decimal → Murmur3 → struct <hash> → getter `<offset>`. Called from the generic flag evaluator `<offset>` (other branches of that function read a named Bool and Bool64 reader `<offset>`). The map's TBox code at `<offset>` uses the same construction inline. |
| `<offset>` | Struct-member Bool getter. The value is the flat bool record at mgr+0x18 + idx*0x18, byte +0x16 (count +0x10). |
| `<offset>` (`cbz x2` then `mov x8,x0` = `<hash>` at +4) | **Second copy** of the Bool64 setter, byte-identical to `<offset>`; tail-calls the same queue helper `<offset>`. |
| `<offset>` (`ldr w8,[x0,#0xc58]` = `<hash>` at +4) | Second copy of the Bool64 reader `<offset>`. |

**Correction to the C3 hook choice.** The reviewed Bool64 setter `<offset>` has exactly one
static caller, `<offset>` (the InstanceGameParameter path). Actor dead/collected flags go
through `<offset>`. The reviewed reader has three callers. That explains why the C3 field
run logged zero `bit-*` lines. A chest never reaches Bool64 at all. **Inferred:** a chest's
placement flags have bit 3 clear (its ActorParam names `ActorGameDataSetting/AlwaysRespawn`),
so its Bool64bitKey schema entry (189 TBox entries exist) is never written.

**Consequences (inferred, needs field validation).**
- A chest is a `bool`-class key, not a `bit` key. The 206 reviewed keys are the backing Bool
  hashes, not actor hashes.
- The manager comes from the cell `*(main+<offset>)` on the frame hook. Do not wait for a
  reader or setter call to learn it.
- The read side for the baseline and the observer is the flat bool table (+0x20/+0x2C →
  index; +0x10/+0x18 stride 0x18, byte +0x16).
- A grant is a synthetic call to `<offset>(mgr, 1, bool_key)`. It produces the same queued
  record that `<offset>` produces for a local open. It does **not** run
  `<offset>`'s actor-side `ldset [x8+0x98]`, it does not animate a chest that is already
  loaded (spawn reads the state), and it grants **no item**. Items are separate
  `IsGet.<actor>` Bools and `PouchData.<actor>.StockNum` Ints, and the pouch grant ABI is
  still unknown.
- `bit` stays the class for pickups and breakables. If it is kept, it must also observe the
  copies at `<offset>` / `<offset>`, which have the same original words as the
  current hooks.

## C4 frame-drain application path

`oot_gamedata_trace.h` now substitutes the verified `pacibsp` at main+<offset>.
At that frame it binds the manager directly from `*(main+<offset>)`, without waiting
for any accessor call. The trace includes the Bool64 accessor copies at +<offset> and
+<offset>. Trace-only `pacibsp` sites +<offset> and +<offset> log struct member/value
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

The chest class is `bool`: flat hash/index table + deferred setter at main+<offset>,
x0=manager, w1=1, w2=backing Bool key. The reviewed chest Bools come from the user-supplied key
file (below); nothing game-derived is compiled in. The runtime only polls
configured keys, at most 206 chest flags per frame. Each lookup probes at most 64
hash slots (modulo capacity, linear probing with wraparound); missing keys, invalid
indices/bytes and exhausted probe budgets fail closed. No entire Bool table scan runs.
The `bit` class remains the Bool64 adapter for pickups, breakables and enemies, with
deferred setter main+<offset> and x2=reviewed actor key. Both adapters validate mapped
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

## Private profile (EDEN_NX2_OOT_COOP_KEYS "offsets")

Every game-derived constant the emulator needs is read at main-module load from the same
user-supplied file as the key policy, under `"offsets"` keyed by the main build id. Without an
entry for the running build, co-op logs `OoT co-op off: ...` and the GameData trace logs
`gamedata trace off: ...`; no instruction is substituted. (The trace reads the file too, so
`EDEN_NX2_GAMEDATA_TRACE` now also needs `EDEN_NX2_OOT_COOP_KEYS`.) Numbers are JSON integers
or `"0x..."` strings; unknown fields reject the file.

```json
{"bool": [<hash>, ...],
 "offsets": {"<build-id>": {
   "manager_cell": "<offset>", "pouch_cell": "<offset>", "wallet_hash": "<hash>",
   "sites": {"frame": ["<offset>"], "step_set": [...], "step_apply": [...], "played_event": [...],
             "bit_read": [...], "bit_set": [...], "struct_bool_set": [...], "actor_flag_set": [...]},
   "entries": {"bool_setter": {"at": "<offset>", "words": ["<word>", ...]},
               "bit_setter": {...}, "rupee_entry": {...}, "item_entry": {...}},
   "item_callers": ["<offset>"], "event_got": "<offset>",
   "item_guards": [["<cell>", "<field>"], ...],
   "pickup": {"callers": ["<offset>", ...], "names": ["<item>", ...]}}}}
```

- `pickup` (optional, class `item`, sub-class pickup): the return addresses of the world-pickup
  path's give calls (the collect routine of a freestanding drop calls the same give-to-pouch entry
  as a chest, count 1, get type 0) and the exact item names forwarded from there. A give from a
  pickup caller whose name is not listed is logged `item-give seen (pickup, per-player)` and never
  forwarded. Decision: rupees and permanent pickups (heart pieces, heart containers) are shared;
  recovery hearts, magic jars and ammo drops (sticks, nuts, seeds, arrows, bombs) and fairies are
  per-player: each player refills their own. Silver rupees are puzzle pieces of a room, not wallet
  money, and are not listed. A forwarded pickup is an `item_give` event like a chest give (deduped
  by origin/sequence on the receiver, never folded into state) and is replayed through the same
  give call, which adds the rupees to the receiver's wallet the way the chest's own nested
  rupee give does. The receiver does not get the pickup's HUD/notification call (next pass).
- `item_give` is a site kind too (the give entry's `pacibsp`); co-op traps it whenever the profile
  names it, logging each give (`item-give observed|seen|replayed ...`, at most 256 lines).
- `event_got` is the GOT slot of the event manager's cell; a give waits while
  `*(*(*(main+event_got)) + 0x68)` is non-zero (an event running). Each `item_guards` pair
  `{cell, field}` must give a non-zero `*(*(main+cell)+field)`.
- A replayed give runs on a frame 0x100 bytes below the top of the private stack (now 64 KiB):
  the stack byte at `[sp]`, the name cell at `sp+0x20`, the name at `sp+0x30`.

- Cells are 8-aligned module offsets of a pointer; sites and entries are 4-aligned module
  offsets below 256 MiB, at most four per site kind, never two at one offset.
- A site's replaced instruction is fixed by its kind (`ReplacedInstruction`: `mov w8, #1` for
  `step_set`, `ldr w8, [x0, #0xc58]` for `bit_read`, `mov x8, x0` for `bit_set`, `pacibsp`
  otherwise), because `Resume` emulates exactly that word; any other word there means no hook.
- An entry's `words` (1-16) must match guest memory before every synthetic call.
- The wallet key gates `rupee_add` on the receiver (`ApplyPolicy::wallet`); the wire and the
  relay accept any non-zero 32-bit key.

## Policy and logs

- `EDEN_NX2_OOT_COOP=1`: enable loopback transport, default `127.0.0.1:43390`;
  `EDEN_NX2_OOT_COOP_RELAY=host:port` overrides it. The older `COOP=host:port` form works.
- `EDEN_NX2_OOT_COOP_ALLOW`: comma-separated `bool`, `bit`, `rupee`, `item` and `pose` tokens, no spaces or empty
  tokens. Default is the **empty set**. Invalid tokens reject session creation.
  The old `ALLOW_CLASSES` variable is no longer consulted.
- `EDEN_NX2_OOT_COOP_KEYS`: JSON policy with typed arrays, for example
  `{"bool":[<hash>],"bit":[123]}` (numeric hashes). At most 4096 keys in total;
  `bool` keys are 32-bit; the file itself is the reviewed list. A legacy bare array still
  means `bit` only. Default is empty; class selection alone cannot grant anything.
  Deny-list keys are removed. Both peers must use the same reviewed key set/policy ID.
  The file is **user-supplied**: game-derived key lists are not part of this repository.
  Generate one from your own GameDataList JSON export (the BYML reader must preserve
  integer hashes):

```sh
python3 -I tools/oot/coop/chest_keys.py "$GAMEDATA_JSON" --struct <hash> --keys my-keys.json
```

Source: `GameData__GameDataList.Product.110.byml`, `Data.Struct` entry `Hash=<hash>`,
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

## Ghost (class `pose`, protocol v3)

The private profile's `offsets.<build-id>` may carry a `ghost` object. Binaries older than the
ghost merge reject it (they reject every unknown profile field), so the key file that carries it
serves this head and later only:

```json
"ghost": {"playing_info_cell": "<offset>", "pos": "<field>", "rot": "<field>",
          "actor_mgr_cell": "<offset>", "scale_const": "<offset>",
          "banc_hash": "<hash>", "child_hash": "<hash>", "loading_hash": "<hash>",
          "str64": ["<table>", "<stride>", "<field>"],
          "create": {"at": "<offset>", "words": [...]}, "get": {...}, "release": {...}, "set_mtx": {...},
          "actor_fields": ["<field>", ...], "link": ["<field>", "<field>", "<field>", "<field>"],
          "lifecycle": ["<field>", "<lo>", "<hi>"],
          "anim": {"play": {"at": "<offset>", "words": [...]}, "idle": "<command>", "walk": "<command>",
                   "run": "<command>"},
          "alpha": {"set": {"at": "<offset>", "words": [...]}, "chain": ["<field>", "<field>"], "percent": 100},
          "tint": {"set": {"at": "<offset>", "words": [...]}, "colors": [[r, g, b, a], ...]},
          "watch": {"pos": "<actor field>", "hide": "<model component byte>"},
          "actor_child": "<actor>", "actor_adult": "<actor>", "actor_fallback": "<actor>"}
```

- **Sender (pure reads at the frame hook).** Position and rotation from the player-info object,
  yaw = atan2(m02, m22); scene from the String64 key, age and loading from Bool keys. About 15 Hz
  while moving, 1 Hz keepalive; after a scene change `loading` stays true until the position moves
  (the player-info position lags the load by about a second, field P1/P4).
- **Receiver (one synthetic call per frame at most, only when no grant call started).** Per peer:
  create the puppet by name through the actor manager's create call (x0 manager, x1 &name pointer,
  x2 &0xA0-byte create argument, x3 &handle, w4 1, three zero stack arguments); poll the request
  record's state (pure read); on "created" fetch the actor with the get call (which consumes the
  handle); move it each frame with the actor's own set-transform call (actor, &Matrix34f, 0);
  release a failed or orphaned request with the release call. Hidden peers (other scene, loading,
  stale, offline) are parked below the map once; nothing is ever deleted by us.
- **Lifetime.** The game deletes the puppet with its scene (and when it is parked out of bounds).
  Before every call the actor's vtable and the profile's `actor_fields` must be unchanged and
  non-null; a local scene change, load or GameData move drops the pointer without any call. The
  handle, argument block, matrix and name live in our sentinel page past +0x400 (persistent, never
  a reused stack frame).
- <a id="ghost-lifetime"></a>**Ghost lifetime: why no borrow is taken.** The pointer is checked the
  way the game's own actor-link check does it: the actor's record still names the actor with the
  generation it had when it was taken, and (optional `lifecycle` field) the actor's lifecycle word
  is outside the range the game's check and its link bind both reject (teardown). The check runs
  in the frame drain and again immediately before the set-transform call. The game's own check
  holds no reference across the use either: on its own threads it reads without a lock, elsewhere
  it takes the record's mutex for the check only and releases it before returning. The link bind
  sets an exclusive "bound" flag on the record, not a reference count, so binding a link of our
  own would not keep the actor's memory alive and is not done. Residual window (unproven either
  way): another guest core could start the teardown between the check and the call's first
  instruction. Deletion is staged (lifecycle first, then the link record, the vtable and the
  memory later), which is what the lifecycle check catches.
- **Animation (optional `anim`).** Without it the puppet is a statue (whatever its own AI plays).
  With it, each time a peer's pose category (`anim`/`anim_seq`: idle, walk, run) changes, the
  receiver calls the actor's own AS play call once (x0 = a 0x30-byte request in the slot: actor,
  command string, slot 0, the pose's `rate` clamped to 0.25..4, morph and fade-in -1, ignore-same 0)
  with the category's command; a category without a command plays nothing. The call restarts the
  clip, so it is never re-issued per packet, and a rate change inside one category is not applied
  (the play call ignores it without a restart). Phase (`phase`) is not applied: the frame setter
  takes its frame in s0; the synthetic call path can pass s0 since the alpha change, but phase
  sync is not wired.
- **Fallback puppet (optional `actor_fallback`).** When the age's puppet cannot be created (a
  dedicated ghost pack that is not installed: the create call refuses at once, field C9), the slot
  switches to the fallback once and retries within half a second. Animation and alpha apply to
  the primary puppet only.
- **Alpha (optional `alpha`).** Once per primary puppet, after it is first shown: the model
  component is read through `chain` from the actor and the setter is called with it in x0 and
  `percent`/100 in s0. `percent` defaults to 100 (opaque), and at 100 the setter is not called at
  all (the puppet's own alpha is 1); the line `alpha 100% (opaque, no call)` says so.
- **Colour (optional `tint`, needs `alpha`'s chain).** Once per primary puppet, after the alpha
  step: the model component's extra-colour setter (x0 = component, x1 = 16 bytes of RGBA floats in
  the slot at +0x1E0), one colour per ghost slot (slot index modulo the list, 1-4 colours, each
  component 0..1). Field C10: this colour reaches only the eyes, not body or clothes. A whole-body
  colour needs the material blend colour (`p_model_blend_color`), whose compositing is not
  decoded; not wired.
- **Watchdog (optional `watch`).** Pure reads every 30 frames on a validated live, placed, shown
  puppet: the actor's own world position (`pos`: three floats at actor + field) against what was
  last drawn, and the model component's hide-reason byte (`hide`, through `alpha`'s chain). Three
  bad reads in a row (more than 2 m apart, or a non-zero hide byte while we show it) abandon the
  puppet: it is parked once (set-transform to y -10000) and forgotten, and a new one is created.
  At most four abandons per slot per local scene. Hide-byte changes are logged.
- **Status and transitions.** Every transition is one line: `assigned`, `create requested`,
  `live`, `shown (peer in <banc> at (x, y, z))`, `parked (peer gone | peer stale | peer loading
  <banc> | peer elsewhere: peer in <banc>, we are in <banc> | local scene <banc> not ready)`,
  `dropped (...)`, `watchdog: ...`, `abandoned`, `reassigned`. Every 1800 frames (about 30 s)
  each ghost logs a `status` line: the peer's pose (scene, position, category, distance from us),
  our scene, the slot state, the drawn position, the read-back actor position, hide byte and
  lifecycle. Transition lines are capped at 4096 per session, status lines at 2000.
- **Peers that come back.** A peer missing from the list (relay gone, `client_state` offline) parks
  its ghost (`parked (peer gone)`); the same identity coming back is shown again. A new identity
  with no free slot takes over a live ghost whose peer has been gone for 600 frames (`reassigned to
  peer <id> (its peer is gone; actor kept)`), so a restarted peer does not leak a fifth actor.
- **Age.** `actor_adult` is optional. Without it an adult peer is drawn with the child puppet, and
  the create line says so.
- **Receiver timing.** Poses are drawn about 100 ms behind the newest arrival, blended linearly
  between the two samples around that time; past the newest sample the newest is held (never
  extrapolated). A forward "slide then snap" of the body while walking or running is the run and
  walk clips' own root translation (field C10), not prediction; the in-place animation banks of
  the ghost mod (C11) remove it.
- Field status: `RESULT` of C8-C11 (private).

