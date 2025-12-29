# LiveDashboard (local library)

Config-driven LVGL dashboard UI (grid tiles, gauges, buttons, splash) for ESP32/Arduino.

## Quick start

1. Put `config.json` (and any assets like `rovi.bmp`) into the same filesystem you pass to `begin()`.
2. Create + init the dashboard:

```cpp
#include <LiveDashboard.h>

live_dashboard::LiveDashboard ui;

live_dashboard::LiveDashboardOptions opt{};
opt.demo_replay = false;          // optional JSONL replay from internal FS
opt.demo_path = "/test.jsonl";    // used only if demo_replay=true
opt.demo_period_ms = 1000;        // used only if demo_replay=true

ui.begin(flashFs, "/config.json", width, height, 'F', opt);
```

3. In your loop:

```cpp
ui.tick();  // handles stale gauges and optional JSONL replay
```

## Runtime API

- `bool publishGauge(const char* id, int32_t value, const char* text)`
  - Updates a configured item by its `id` (arc gauge or `hz_lists` row).
  - Returns `false` if `id` is unknown.
- `bool ingestLine(char* line)`
  - Stops JSONL replay (if enabled) on the first successfully handled external input.
  - If the line starts with `{` or `[`, it is parsed as a JSON event line (same format as JSONL).
  - Otherwise it is treated as a command and matched against configured button `action_id` values (invokes the registered `onAction()` callback).
- `bool ingestEventLine(char* line)`
  - Parses and applies a single JSON “event line” (same format as JSONL).
  - Accepts either an object or an array of objects:
    - `{"id":"voltage","value":121,"text":"12.1V"}`
    - `[{"id":"voltage","value":121,"text":"12.1V"},{"id":"cpu","value":37,"text":"37%"}]`
  - Limits (hard errors): max line length 1024 chars; max 5 events per line; `text` is mandatory.
  - Also stops JSONL replay (if enabled) after a line is successfully applied.
- `bool onAction(const char* action_id, ActionCallback cb, void* user)`
  - Binds a C callback to buttons whose `action_id` matches.

## JSONL replay (demo)

If `LiveDashboardOptions.demo_replay == true`, `tick()` will read one line from `demo_path` every `demo_period_ms` and apply it via `ingestEventLine()`.

- Missing `demo_path` file is a **fatal error** (dashboard init fails and shows an error screen).
- Any external input via `ingestLine()` / `ingestEventLine()` stops JSONL replay immediately (so real data can take over).

## Config schema (`config.json`)

Top-level keys used by the library:

- `robot_name` (string, required)
- `ui` (object, required)
  - `dark_theme` (bool, required)
  - `stale_timeout_ms` (uint32, required) — if a gauge is older than this, it shows `--`
  - `background` (string, optional) — color like `"#0B1220"` or `"red"`/`"amber"`…
  - `splash` (object, optional)
    - `path` (string) — image path in the FS; if no drive is included, it’s prefixed with the drive letter passed to `begin()` (e.g. `"F:/rovi.bmp"`)
    - `duration_ms` (uint32)
- `layout` (object, required)
  - `cols` (uint8, required)
  - `rows` (uint8, required)
  - `tiles` (array, required) — must be exactly `cols * rows` items, left→right then top→bottom
    - each item: `{ "id": "tile_voltage" }`
    - to make a tile span multiple grid cells, repeat the same `id` in each cell of a rectangular region (non-rectangular repeats are rejected)
- `gauges` (array, optional)
  - each item (required keys): `id`, `tile_id`, `title`, `min`, `max`, `initial`, `initial_text`, `accent`
  - optional: `min_label`, `max_label`, `stale_text`, `stages`
  - `stages` is an array of `{ "t": <threshold>, "c": <color> }` (higher thresholds should come first; the library sorts them)
- `buttons` (array, optional)
  - each item: `tile_id`, `tile_title`, `label`, `color`, `action_id` (optional `height`)
- `text_tiles` (array, optional)
  - each item: `tile_id`, `title`, `subtitle` (optional), `body`
- `hz_lists` (array, optional)
  - each item: `tile_id`, `title`, `rows`
  - `rows` is an array (max 6) of `{ "id": "hz_nav", "label": "nav", "target": 20 }`
  - updates come from events by `id` (use `value` as current Hz, and `text` for display, e.g. `"18/20Hz"`); the bar is computed from `value/target` and capped at 100%

## Notes

- Current implementation is intentionally “strict”: invalid/missing required config keys show a CONFIG ERROR screen.
- This library currently uses a single global instance internally (singleton-style). Multiple dashboards at once isn’t supported yet.
