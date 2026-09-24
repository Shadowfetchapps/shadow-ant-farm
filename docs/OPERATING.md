# Operating guide

## Starting and stopping

| Command | What it does |
|---|---|
| `shadow-ant-farm` | New colony from a fresh random seed, full screen |
| `shadow-ant-farm --resume` | Continue the newest valid checkpoint (starts a new colony if there is none) |
| `shadow-ant-farm --seed 12345` | New colony from a specific seed (reproducible) |
| `shadow-ant-farm --load FILE` | Continue a specific checkpoint file |
| `shadow-ant-farm --operator` | Also open the operator window |
| `shadow-ant-farm --windowed` | Run in a 1600 × 900 window instead of full screen |
| `shadow-ant-farm --fps 30` | Lighter 30 FPS preset (default 60; also switchable in the operator window) |
| `shadow-ant-farm --config FILE` | Simulation tuning file (`key = value` lines; see `core/include/antfarm/config.hpp`) |

The desktop entry also offers *Resume last colony*, *Resume with operator window* and *New colony in a window*.

**Quit** from the operator window: press *Quit…*, then press it again within 5 seconds. Closing the farm window
through the desktop (for example Super+Q) also saves a checkpoint first. Nothing on the farm window itself quits
it, so a stray key press on stream can't end the show.

## Keys (farm window)

| Key | Action |
|---|---|
| F2 | Show or hide the operator window |
| F11 | Toggle full screen / window |

The mouse cursor is always hidden over the farm.

## Operator window

A separate desktop window (it never draws over the farm) showing:

- **Status:** seed, simulated time, ants, share of soil excavated, deepest tunnel, entrances, workers carrying soil,
  food on the stone and in the stores, frame rate, dropped ticks, stuck recoveries, memory and the log path.
- **Save checkpoint now** and **Save screenshot** (to `~/.local/share/shadow-ant-farm/screenshots`).
- **Frame rate:** 60 FPS or 30 FPS.
- **Sound:** master volume plus five categories (room tone, digging, soil and spoil, footsteps, food). Settings
  are remembered.
- **Start a new colony…**, guarded: type `NEW COLONY` to enable the button. The running colony is checkpointed
  first, so it can still be resumed with `--load`.
- **Quit…**, guarded: press twice within 5 seconds. It saves a checkpoint first.

Closing the operator window only hides it; the farm keeps running.

## Using it with Shadow Recorder (or any capture tool)

- The farm window has a stable identity: Wayland app id `Shadow Ant Farm` (it runs as a native Wayland client), title
  `Shadow Ant Farm`. The operator window's title is `Shadow Ant Farm — Operator`, so a window-capture source can
  select the farm alone.
- Capture the farm window, or the monitor it is full screen on. Keep the farm visible on its monitor while
  streaming. The simulation keeps to real time whatever happens: when the desktop hides or blanks the window, the
  compositor slows its frames, and the farm catches up on the next frame without losing time. The picture a
  capture tool receives, however, can only be as fresh as the frames the compositor lets the farm draw.
- The farm keeps running when it isn't focused, and it keeps the screen awake (idle inhibit) while running.
- It never opens a camera or microphone, and it doesn't change any streaming configuration. It doesn't talk to
  Shadow Recorder at all.
- Sound plays through the default output at 48 kHz stereo. Capture the farm's audio stream (application
  "Shadow Ant Farm") or desktop audio.
- At 1080p/6 Mb/s H.264 the ants survive encoding: see [TESTING.md](TESTING.md#encoded-capture).

## Where things are kept

Everything lives in `~/.local/share/shadow-ant-farm/`:

| Folder / file | Contents | Bound |
|---|---|---|
| `checkpoints/` | `colony-<seed>-t<tick>.antfarm`, about 8–9 MB each, written every 10 minutes and on quit | Newest 4, plus one per simulated hour for 8 hours, plus the newest of 2 previous colonies (about 130 MB at most) |
| `logs/shadow-ant-farm.log` | Start-up, checkpoints, an hourly status line, warnings | Rotated at 4 MB, 5 kept |
| `logs/engine.log` | Godot's own log | 6 kept |
| `settings.cfg` | Frame-rate preset and volumes | |
| `soak/`, `screenshots/` | Soak reports and stills | Only what you create |

Checkpoints are written atomically (temporary file, `fsync`, rename) and carry a format version, a
configuration hash, a state hash and a whole-file checksum. `--resume` skips any damaged checkpoint and falls
back to the next newest.

## Long-run and inspection tools

**Real-time soak test** (the only way to show that a 24-hour run holds up; it takes 24 real hours):

```bash
shadow-ant-farm --soak 24 --operator
```

Every real minute it writes a JSON line to `~/.local/share/shadow-ant-farm/soak/soak-<start>.jsonl`: frame rate,
99th-percentile and worst frame time, memory, dropped ticks, excavation, audio statistics. It saves stills at
0, 1, 4, 8, 12, 18 and 24 real hours and a `…-summary.json` when done, then saves a checkpoint and quits. The
summary records the elapsed real hours. A soak only counts as passed if it actually ran for 24 real hours.

**Accelerated visual inspection** (renders stills without waiting in real time):

```bash
shadow-ant-farm --seed 12345 --capture ~/antfarm-stills --capture-hours 0,1,4,8,12,18,24
```

The farm renders offscreen at 3840 × 2160 (`--capture-size` to change), fast-forwards the same simulation to each
hour, and saves a PNG. For long horizons it is faster to fast-forward headless with `antfarm_sim --seed 12345
--hours 24 --checkpoint-dir DIR --checkpoint-hours 1,4,8,12,18,24`, then render each checkpoint with
`--load DIR/colony_XXh.antfarm --capture OUT --capture-hours XX`.

**Frame-time benchmark:** `--bench 30` (add `--render-size 3840x2160` to render 4K offscreen, and `--uncapped`
to measure headroom without the frame cap).

**Headless checks through the extension:** `godot --headless --path game -- --headless-test 1` covers
determinism, resume, conservation, fresh seeds, audio range and terrain buffer size.

## Troubleshooting

- **No sound:** check the output device and the operator window's volume sliders (the settings persist).
- **The farm looks frozen in a capture preview:** the farm window is probably hidden or the display is asleep,
  so the compositor isn't asking it for frames. Show it on its monitor. The colony itself kept running.
- **Heavy GPU load elsewhere:** switch to the 30 FPS preset. The simulation is unaffected.
- **Start over:** use *Start a new colony…* in the operator window, or simply launch without `--resume`.
