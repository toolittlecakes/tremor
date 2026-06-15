# Tremor

Turn the MacBook trackpad into an expressive musical instrument.

A single self-contained binary reads the private `MultitouchSupport` contact
field — per-finger position, area, **force in grams**, and ellipse angle — serves
an embedded web UI over HTTP, and streams every frame to the browser, where the
page synthesizes sound with the Web Audio API. Each finger is its own voice:

- **← →** pitch · **↑ ↓** timbre (filter cutoff) · **press harder** = louder
- Rest a few fingers on the trackpad and play.

Press **Space** to switch the trackpad out of *system* mode (no cursor / clicks)
into a pure instrument — no distractions. It's always restored on exit.

## Build & run

Needs only the macOS toolchain (`clang`, `xxd`) — no runtime dependencies.

```sh
make run        # build, then serve on http://127.0.0.1:8788 (opens the browser)
```

`make` builds `build/tremor`; the page (`public/index.html`) is baked into the
binary at build time. `PORT=<n>` overrides the port.

## How it works

```
tremor (one binary)
  ├─ reads MultitouchSupport contact frames  (CoreFoundation run loop)
  ├─ serves the embedded page + SSE stream    (HTTP on 127.0.0.1)
  └─ GET /parser?d=<i|-1>&on=<0|1>            toggle system-input mode
```

The browser does all audio and visualization; the binary is just the bridge to
the trackpad and the web server. macOS only.
