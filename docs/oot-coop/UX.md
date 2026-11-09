# Co-op UX: toasts and name tags

Host-side only: nothing here writes guest memory or calls guest code.

- **Corner toast.** A receiver replay that returned successfully (item give with w0=1, wallet delta)
  queues "Anthony got 1x Kokiri Sword" / "Anthony got 20 rupees". A forwarded local give queues
  "You shared 1x ...". One toast per grant id (origin/seq), at most three on screen, 4 s each, upper
  right of the game rectangle; cleared when the session resets. Item names come from the private
  sidecar; an actor the table does not name shows "an item".
- **Name tag.** At the frame drain the active Main camera (view 3x4, projection 4x4) is read, pure
  reads, every pointer checked; a frame whose CPU projection is dirty keeps the previous tags. Each peer
  in our settled scene is projected at its ghost root plus a head height; the name is drawn above that
  point with depth off (visible through walls), hidden behind the camera or off screen, dropped when
  the peer leaves the scene or either side loads. Tags expire 250 ms after the last frame drain.
- **Drawing.** `src/video_core/coop_overlay.*` rasterises opaque text boxes (embedded Noto Sans Mono
  bitmap); `renderer_vulkan/vk_coop_overlay.*` copies them into the composited present frame after
  composition (and after frame generation consumed it) and before flush/present. Emulator screenshots
  blend the same boxes in (`EDEN_NX2_OOT_COOP_OVERLAY_SHOTS`).
- **Names on the wire.** Peer names come from the relay's `client_state.state.name`; our own
  `client_state` now carries `name`, so the relay's cache keeps it for late joiners.

## Switches

| Variable | Default | |
|---|---|---|
| `EDEN_NX2_OOT_COOP_TOAST` | on | corner toasts |
| `EDEN_NX2_OOT_COOP_TOAST_LOCAL` | on | toasts for what this player shares |
| `EDEN_NX2_OOT_COOP_NAMETAG` | on | name tags (needs camera keys for the build) |
| `EDEN_NX2_OOT_COOP_NAMETAG_DEBUG` | off | crosses at the ghost root and the head point, camera matrices logged |
| `EDEN_NX2_OOT_COOP_OVERLAY_SHOTS` | on | overlay in emulator screenshots |
| `EDEN_NX2_OOT_COOP_UX` | `coop-ux.json` beside `EDEN_NX2_OOT_COOP_KEYS` | the sidecar path |

## Sidecar (private, game-derived; never in source)

```json
{"item_names": {"<actor name>": "<display name>"},
 "builds": {"<main build id>": {"camera": {"registry_cell": "0x...", "...": "...",
                                            "head_child": 1.5, "head_adult": 1.5, "flip_y": false}}}}
```

Every camera field in `CameraKeys` (`src/core/arm/dynarmic/oot_coop_ux.h`) is required; an unknown
field rejects the camera object (toasts still work). The sidecar is separate from the co-op profile
because the profile decoder rejects unknown fields, which would switch co-op off in older binaries.

## Room and session notifications

Presence is tracked separately from ghost lifetime. Only transitions toast: `<name> connected`,
`<name> disconnected`, `<name> reconnected`. Names use the session name table and printable-ASCII
sanitization. Each peer has a 10-second guard: a change inside it is held and reported at its end
if the final state differs from the reported state. Duplicate presence/wallet updates are silent.
Our own transport loss instead shows `Relay connection lost`, followed by `Reconnected to relay`
once ready. Previously online peers re-announced after reconnect are silent; missing peers become
offline after five seconds, still subject to the guard. Presence logs are capped at 512 per session.

Each drained team snapshot counts entries actually queued for application (flags: Bool, Bit,
Quest, Int, Enum; items: Item, PouchAdd, PouchTotal). After its final HighWater entry and an idle
applier, or a 15-second settling timeout, it shows `Synced from team: N flags, M items`, with
singular forms and zero parts omitted. Zero totals log without a toast. These are queued-entry
counts, not a promise that every guest operation succeeded.

The optional finish Bool is read every 30 frames after the runtime baseline. Its first readable
value establishes a baseline; only a later false-to-true edge announces `You finished the game`.
It is never written. Receivers show `<name> finished the game` once per origin per session.
Completion persists in subsequent client state and reconnects, including the current wallet.

| Variable | Default | Purpose |
|---|---|---|
| `EDEN_NX2_OOT_COOP_ROOM_WINDOW` | `trigger` | `on`, `off`, or visible while a trigger file exists |
| `EDEN_NX2_OOT_COOP_ROOM_TRIGGER` | `coop-room.show` beside the key file | trigger path, checked at most once/second |
| `EDEN_NX2_OOT_COOP_MINIMAP` | off | decode the build's minimap configuration; guest reader still pending |

The upper-left room panel shows room, seed prefix, policy prefix (after `flags-`), relay state,
heartbeat RTT, our own player, and up to eight known peers, online first and then by name.
Without poses the panel still shows presence; offline peers retain their last area and age.
Area display uses the private map, otherwise a runtime identifier prettifier. Rows are ASCII,
at most 56 characters, with quantized last-seen ages. Text is rebuilt at most twice/second and
published only on change. Raster and per-Vulkan-slot packing caches avoid repeating unchanged
panel work. Screenshots use the same panel/marker layout when `OVERLAY_SHOTS` is enabled.

Additional optional sidecar fields (placeholders only):

```json
{
  "finish": {"bool": "0x<8 hex>"},
  "area_names": {"<banc id>": "<place name>"},
  "builds": {
    "<main build id>": {
      "minimap": {
        "centre": ["<screen centre x>", "<screen centre y>"],
        "half": ["<screen half width>", "<screen half height>"],
        "world_centre": ["<world x>", "<world z>"],
        "scale": "<positive map units per world unit>",
        "clip": "<circle|rectangle>",
        "centre_source": "<player|fixed>",
        "rotation": "<north-up|player-yaw>",
        "yaw_sign": "<-1|1>",
        "swap_xz": "<boolean>",
        "x_sign": "<-1|1>",
        "z_sign": "<-1|1>"
      }
    }
  }
}
```

Replace numeric/boolean placeholders with JSON numbers/booleans. `finish` requires exactly one
field, a nonzero u32 encoded as `0x` plus exactly eight hex digits. An invalid finish disables
local detection. Unknown top-level fields remain ignored, preserving older-sidecar compatibility.
Area values are sanitized, bounded to 56 characters (16 in peer rows), with at most 4096 mappings.

Minimap fields are all required; unknown fields or invalid/nonfinite/degenerate values reject
that minimap. Centre and half-extents are normalized to the game rectangle; the clip must fit
inside it. Subtract player or fixed world centre, optionally swap X/Z, apply each axis sign,
then rotate by signed player yaw in degrees. One map unit reaches a clip half-extent; outlying
points clamp along their ray to the rectangle/ellipse boundary. `north-up` applies no rotation.
The pure helper returns a position and a clamped flag. Published markers are bordered coloured
letter squares and expire after 250 ms, like name tags.

**Minimap plumbing only:** `Room::ReadMinimap` intentionally returns no transform. The integrator
must wire researched guest reads and visibility validation before any markers appear. Enabling
the switch alone does not assume a transform or draw markers.

## Desktop co-op controls

For a supported title, Properties → Co-op saves connection and kit settings in the
per-game `Coop` section. Launcher environment variables take precedence. Select a
private key file; the page lists only filenames, sizes and short SHA-256 digests.
Session switches apply only to existing typed leaves. Changes create a session
copy beside copies of the available kit sidecars; unchanged settings retain the
original file bytes. Both players must select matching switches because the mod
hash covers the key file. Ghost mod enablement uses the existing disabled add-ons
list and only changes when the checkbox is explicitly changed.

View → Co-op Panel shows published peers and provides room-window, teleport and
team-state request controls. The panel never reads the private key file. A build
without a status publisher displays “No live co-op status”; no connection or ping
is inferred. Disconnect requires future runtime support. Reconnect restarts the
game using page settings, subject to launcher overrides. Changing co-op enablement
requires restarting the application because the core caches enablement at first
boot. The status publisher must use `steady_clock` milliseconds for `published_ms`
and peer `seen_ms`, and clear the board when the session ends.

Field-test hooks (off unless set): `EDEN_NX2_OOT_COOP_GUI_SHOTS=<dir>` shows the Co-op Panel at
start, saves `panel-latest.png` grabs into `<dir>` and executes `<dir>/panel.cmd` lines (`teleport ROW`,
`resync`, `room on|off`, `copyall`, `shot NAME`) by clicking the real controls, logging to
`<dir>/panel.log`; the Co-op page saves `page-dialog.png` / `page-full.png` there.
`EDEN_NX2_OOT_COOP_GUI_OPEN=<game file>` opens that game's Properties at start;
`EDEN_NX2_OOT_COOP_GUI_APPLY=<room>` then types that room into the page and presses OK.
