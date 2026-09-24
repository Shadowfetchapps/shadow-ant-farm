# Assets, provenance and licences

Shadow Ant Farm contains **no third-party art, textures, models or recorded sounds.** Everything seen and heard
is generated at run time by code in this repository, and is covered by the project's MIT licence. That includes
streamed and recorded use.

## Original, generated at run time (this repository, MIT)

| Asset | Where it comes from |
|---|---|
| Soil, grains, pebbles, stones, roots, tunnel walls | `game/shaders/soil.gdshader`, driven by the simulated terrain (`core/src/world.cpp`) |
| Ant bodies (head, antennae, mandibles, mesosoma, petiole, gaster, legs) | Procedural mesh in `game/scripts/ant_model.gd`; gait and actions in `game/shaders/ant.gdshader` |
| Soil pellets, seeds, loose grains | `game/scripts/ant_model.gd` (`build_pellet`), `game/shaders/load.gdshader` |
| Glass pane, reflection, dust | `game/shaders/glass.gdshader` |
| Frame | Godot `BoxMesh` primitives with `StandardMaterial3D` (`game/scripts/farm_view.gd`) |
| All sound (room tone, digging scrapes, crumbling pellets, trickling spoil, seed ticks, footfalls) | Synthesised from filtered noise and clicks in `extension/src/ant_audio.cpp`; no samples |
| Icon | `packaging/shadow-ant-farm.svg` (hand-written SVG) |

## Third-party software

| Component | Version | Licence | Use |
|---|---|---|---|
| [Godot Engine](https://godotengine.org) | 4.7.2-stable (official build and export templates) | MIT | Engine and runtime; its own third-party notices ship with Godot |
| [godot-cpp](https://github.com/godotengine/godot-cpp) | 10.0.0-stable (git submodule, commit `507ed9d`) | MIT | C++ bindings for the GDExtension |
| ffmpeg (system package, run as a separate program for live streaming) | system | LGPL/GPL (as packaged by the distribution) | Encoding and RTMP/RTMPS output; not bundled |
| secret-tool (libsecret, system package) | system | LGPL-2.1+ | Stores stream keys in the desktop keyring; not bundled |
| C++ standard library (GCC libstdc++) | system | GPL-3.0 with the GCC Runtime Library Exception | Standard runtime |

No fonts, textures, audio files, models or data sets are downloaded or bundled.
