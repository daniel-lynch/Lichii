# Static guest-hook evidence and application plan

Target: title `0400486024DEC000`, main build
`E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348`. All addresses below use the
Ghidra image base `0x7100000000`; relocate by subtracting that base and adding
this process's verified main base. These are evidence, **not enabled write sites**.

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
`.tmp/struct-bool-C2.txt`. No native code or data tables are embedded here.

## Hook and setter table

| Site / evidence | Proven static ABI and behavior | Preconditions / remaining inference |
|---|---|---|
| Bool setter `0x7100682B20` (`G/out13/decomp/7100682b20.c`; queue reservation `0x7100682BEC`) | `x0=GameData manager`, `w1=value & 1`, `w2=nonzero 32-bit key`. Hash lookup, then queued write; alternate array path calls `0x710376A0B0`. No useful success return is established. | **Inferred:** live manager, valid key, correct save generation and queue capacity must be checked; return is not persistence. **Proven:** not a pure flag store. |
| Int setter `0x7100163B70` (exported bytes; stores at `0x7100163C5C`) | `x0=manager`, `w1=32-bit value`, `w2=nonzero key`. CAS reserves a 12-byte queue record through `manager+0x100`; value/key stored together. Array fallback calls `0x710376A590`. | **Proven** queued integer write; **inferred** signed/application meaning depends on the schema. Scalar grants excluded by protocol v2. |
| Enum setter `0x71003E0980` (`G/out13/decomp/71003e0980.c`) | `x0=manager`, `w1=value`, `w2=nonzero key`; queued value/key pair, array fallback `0x710376ABE0`. | **Inferred:** value must belong to the enum. No numeric-max rule for arbitrary enum hashes. Not a pure write, not a quest-transition substitute. |
| Bool64 reader `0x7103761680` (bytes `0x7103761684..0x7103761714`) | `x0=manager`, `x1=out byte*`, `x2=nonzero actor key`; sorted binary search; `w0=1` and writes the byte on match, else `w0=0`. | **Proven** layout below. **Inferred:** stable loaded table and concurrency guarantee still needed before an emulator reader uses it. |
| Bool64 setter `0x7103761720` (bytes `0x7103761724..0x71037617A8`) | `x0=manager`, `w1=bool`, `x2=64-bit actor key`; finds record index in `w3`, changes `x0` to manager+`0xC58`, masks `w1` and tail-calls `0x7100F071C0`. Missing key returns without writing. | **Proven:** indexed deferred queue, not a direct OR. **Inferred:** first candidate guest call after field validation; leave runtime address gate closed until manager lifetime, frame site, queue and save behavior pass. |
| Bool64 queue helper `0x7100F071C0` (bytes through `0x7100F07244`) | `x0=table`, `w1=bool`, `w3=resolved index`. CAS queue reservation; byte value at entry+0, index at +4. Capacity/generation mismatch branches to abort. | **Proven:** do not call with an unvalidated index; do not bypass by writing the record byte. Queue-consumer notifications/dirty tracking remain **unknown**. |
| Quest setter `0x7102CF54A0`; observation `0x7102CF54A4` (`G/out17r/decomp/7102cf54a0.c`) | `x0=challenge manager`, `w1=challenge hash`, `w2=step hash`, `w3=UI choice`, `w4=mark current subentries`, `w5=force`. Three record sets; sets pending hash/flags; returns 1 for accepted/no-op, 0 for missing or protected state. | **Proven:** may move backwards, accepts an unresolved step hash as pending, and blocks terminal/not-ready states unless forced. A successful return does not prove a step applied. UI meaning inferred from caller. Do not force. |
| Quest apply `0x7102CF1830` (`G/out17r/decomp/7102cf1830.c`) | Integer argument `x0=record+0x18`; `w1` controls old-step processing. Resolves pending hash, invokes `0x7102CF1160`, writes index, invokes `0x7102CF13A0`. | **Proven:** more than one field changes and callbacks run. Entry trace is before mutation. FP/vector argument needs in callees remain unverified; never inject this interior apply path. |
| Played event `0x7102BDC4E0` (`G/out17/decomp/7102bdc4e0.c`) | `x0=task wrapper`; task at +0x10, name pointer at task+8, length including NUL at task+0x10. Finds `HavePlayedEvent` struct then hashes the member name. | **Proven:** this is a struct/member operation, not `Murmur3("HavePlayedEvent." + name)` as the skeleton assumed. Trigger timing/creator and success are **inferred/unknown**. No v2 grant emitted from this hook. |
| Struct-bool helpers `0x7103745A10`, `0x7103745CD0`, fallback `0x7103745B70` (caller above and `.tmp/struct-bool-C2.txt`) | First two: `x0=manager+8` or `manager+0x68`, `w1=1`, `x2=local struct handle*`, `w3=member hash`, `w4=(manager+0xD0C == 0)`; low result bit controls fallback. Fallback adds `w4=0`, moves mode into `w5`. | **Proven** caller setup. Handle includes a resolved record and flags, not just an opaque hash. Member-key/type lookup must be reconstructed; no generic flat-bool adapter for these calls. |
| Frame marker `0x71011E7A50` (`G/out4/decomp/71011e7a50.c`, `G/out12/ctx/71011e7a50.txt`, exported bytes) | Starts `pacibsp`; calls `nn::os::WaitEvent` at `0x71011E7A68`. Existing names identify `GameFramework::waitPresentDone`. | **Inferred** safe injection boundary, supported by prior debugger experiment in `findings-scene-tour.md`, not revalidated here. Critically, this marker is **not hooked by the trace code in this tree**. No frame drain is claimed to exist. |
| Pouch grant entry | **Unknown.** Notification references are not a grant API; e.g. `G/out13/decomp/710110d570.c` and `71033a4fa0.c` reference pouch-content changes. | Need successful item-receive caller, exact manager/actor/count ABI, source check identity, ownership/equipment/ammo/notification/autosave effects. No address is guessed. |

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

## One application path

The selected first field candidate is an allowlisted Bool64 setter call, using
`x0=verified live GameData manager`, `w1=1`, `x2=actor key`, entry main+0x3761720.
A matching setter return restores the interrupted context, then waits for the reader
above to report true at a later safe drain. There is no success boolean to trust.
No production adapter is installed by this change.

1. Decode/validate network input off-thread. Queue only true grants allowed by both
   class and key policy; match title/build/seed/policy. At a verified frame boundary,
   detach a bounded batch from the queue lock and validate guest thread/save generation.
2. Read authoritative state. Already true means satisfied without a write. Validate
   object lifetime, key lookup, instruction bytes, queue capacity, save-loaded state,
   absence of a transition/event and sufficient private guest stack before starting.
3. Prefer an atomic masked data write **only if** the specific class has proved no
   queue, notifications, cache or dirty-state obligations. None of the researched
   setters qualifies. The fake backend exercises this branch for future proven flags.
4. Otherwise capture full CPU state, redirect PC to the callable entry, set x0..x7,
   align a separate guest stack to 16 bytes and set LR to a dedicated executable
   sentinel. The sentinel cannot be the original frame marker: distinguish thread,
   generation and return SP. Preserve GPR/SIMD/PC/SP/PSTATE/FPCR/FPSR/TLS and explicitly
   handle exclusive-monitor state. Never restore only callee-saved registers.
5. At the owned sentinel restore the full interrupted context and resume the original
   instruction exactly once. A caller must not also execute the trace's normal Resume
   after redirecting execution. Await authoritative commit; do not retry on timeout.
6. Echo receipts survive host-thread changes, sentinel return and delayed application;
   they are keyed by grant class/key within one save generation. During an uncertain
   failure retain suppression and quarantine further grants. Never unwind from an
   arbitrary PC that may hold locks. Restore only when the adapter proves a recoverable
   boundary; otherwise stop injection and let the owner diagnose/restart the disposable
   session. Reset receipts only once the old guest cannot execute any further.

`oot_coop_apply.h` implements this state machine against a backend contract; tests use
fake memory/registers. It is **not wired to Session or Dynarmic**, so it cannot alter
guest execution. Its receipts persist in memory through the full operation, not on disk.
Process restart requires baseline reread; durable receipts would need atomic binding to
the save and a protocol epoch before any additive rewards could be considered.
`EDEN_NX2_OOT_COOP_ALLOW_CLASSES` controls the current disabled writer's class gate;
the backend additionally requires an explicit key set. Default class/key sets are empty.

## Trace integration correction

`oot_gamedata_trace.h` substitutes three original instructions only: StepSet+4,
StepApply entry, PlayedEvent entry. `arm_dynarmic_64.cpp` invokes co-op alongside the
read-only trace, then resumes once. `nso.cpp` checks the main build ID. ROMFS tracing
locates relevant files and timing; it neither identifies a loaded save manager nor
provides a frame callback. The co-op hook now emits only a diagnostic marker and drains
the disabled writer. Neither StepSet nor PlayedEvent fabricates a protocol grant.

The field adapter must install and test the frame marker separately. Exact original
instruction `pacibsp` is known from exported bytes, but safe scheduling and guest-stack
ownership are not. Do not turn `GuestWritesVerified` on to bypass that missing work.
