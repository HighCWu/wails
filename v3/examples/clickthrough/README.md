# clickthrough — Transparent window per-pixel hit-testing

Demonstrates **transparent overlay windows with per-pixel click-through**:
the overlay renders an irregular scene (circle / ring / card) on a fully
transparent window, and clicks on transparent regions pass through to
whatever is underneath (the bundled underlay window counts them), while
opaque regions receive normal interaction.

This example is engine-agnostic by design: it only uses
`Window.SetIgnoreMouseEvents` + `application.MousePosition()` (native cursor
polling), so the same scenario runs on the system webview today and is the
hit-test reference scenario for the Electron backend (see
`history/electron-backend-design.md` M4).

## Run

```bash
go build -tags gtk3 -o clickthrough ./examples/clickthrough/
./clickthrough
```

Two windows open: a frameless transparent **overlay** (top-left) and a solid
**underlay** (offset right/down). Switch modes in the overlay card:

- **Off** — hit-testing disabled, whole window interactive.
- **Alpha mask** — the page rasterises itself offscreen, uploads a coarse
  alpha grid (8px cells) via `ct:mask`, and the native engine toggles
  passthrough as the cursor crosses transparent/opaque regions.
- **Ignore all** — whole-window passthrough.

## How it works

```
frontend                    native (engine goroutine, 16ms)
────────                    ──────────────────────────────
rasterise page ──ct:mask──▶ mask grid (packed bits)
                            loop: MousePosition() − window.Position()
                                  → mask lookup
                                  → SetIgnoreMouseEvents(flip)
                            flips ──ct:state──▶ status UI
```

On Linux GTK3, `SetIgnoreMouseEvents` applies an X11 **input shape** to the
window: a 1×1 input region makes the entire window hierarchy (including the
web content) transparent to the pointer, so clicks land on whatever is
stacked below — the X server itself stops attributing the point to the
window (`xdotool getmouselocation` reports `window:0`). Disabling unsets the
shape entirely, so no stale region is left behind when the flag flips or the
window resizes.

## Design rules (distilled from a prior Tauri implementation, see below)

1. **Never drive hit-testing from JS mousemove.** Once the window ignores
   mouse events it stops receiving them and locks itself in the
   passthrough state. The toggle must be decided by a native polling loop
   over the OS cursor position (`application.MousePosition`).
2. **Toggle whole-window passthrough, not X11 input shapes.** Input shape
   regions are applied on the X server and survive window resizes, leaving
   invisible click-through holes. GDK passthrough is a window property with
   no region to leak. (Wails GTK sets `gdk_window_set_pass_through`.)
3. **Fail open.** No mask, unsupported platform, cursor read error → the
   window stays fully interactive. The Wayland backend has no global cursor
   position; hit-testing degrades to "off" there.
4. **Fixed window size for v1.** The mask is uploaded once per page size;
   resize handling (re-upload on resize) is future work.

## Known caveats

- DPI: window `Position()` is logical while `MousePosition()` is physical;
  the lookup assumes scale factor 1 (Xvfb CI, typical Linux). Mixed-DPI is
  future work.
- 8px cell granularity: a 1px cursor sliver at a block edge may flip one
  cell early. Reduce `CELL` for finer hit boxes.
- GTK4: the upstream `ignoreMouse` is an empty stub on the GTK4 variant —
  hit-testing currently requires the `gtk3` build tag.
- Wayland: no global cursor position in the protocol; the engine fails open
  (fully interactive window).

## CI status

The GitHub Actions workflow (`.github/workflows/clickthrough-test.yml`) runs
the full scenario — mask upload, engine flips, and real click delivery — on
all three platforms: Linux (Xvfb + xdotool), Windows (user32 injected
input), and macOS (CGWarp + CGEventPost). The macOS driver grants itself
Accessibility by inserting into the system TCC database (SIP is disabled on
hosted runner images) and restarting tccd; if a future runner image blocks
that, the click phase degrades to a documented SKIP while the flip
assertions keep the job honest.

## Verified locally (Xvfb :95 + openbox, 2026-09-29)

- pointer over opaque region → `flip ignoring=false`, card button clickable
- pointer over transparent region → `flip ignoring=true`, click lands on the
  underlay (its counter increments, `getmouselocation` → `window:0`)
- state echoes back to the overlay status line via `ct:state`

## CI notes

The app logs stable markers to stdout for automated verification:

- `clickthrough: mask uploaded cell=8 w=60 h=80 ...`
- `clickthrough: mode mode=alpha`
- `clickthrough: flip ignoring=true mode=alpha`

Suggested Xvfb(:95)+openbox scenario: query the overlay's actual geometry
first (the WM may reposition frameless windows; on this setup openbox placed
it at +400+130), then move the pointer to an opaque point (window origin +
circle centre 200,160) and assert `flip ignoring=false`; move to a
transparent point over the underlay and assert `flip ignoring=true`; click
there and assert the underlay counter increments; click the card button and
assert the overlay card counter increments. Log markers (`clickthrough: …`)
are the primary assertions; screenshots are the cross-check.

## Provenance

The toggling design and its failure modes come from a Tauri 2 +
WebKitGTK/X11 translation-overlay project; its troubleshooting record
documented the JS-driven deadlock (rule 1), the X11 input-shape residue
after window resize (rule 2), and a working native polling state machine
whose structure this example follows (rounded card + alpha mask + global
cursor polling + fail-open on Wayland).
