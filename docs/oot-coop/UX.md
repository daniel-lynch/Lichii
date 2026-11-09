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
`EDEN_NX2_OOT_COOP_GUI_OPEN=<game file>` opens that game's Properties at start.
