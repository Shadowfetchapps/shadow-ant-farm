# How the colony works

Shadow Ant Farm is a *display model*: a decentralised simulation tuned to look and behave believably on screen for
a day or longer. Its parameters are design choices, not measured biological constants (see
`core/include/antfarm/config.hpp`, where every value is annotated).

## Layers

```
core/        plain C++20, deterministic, no engine code
  World        excavatable grid (960 × 540 cells, a cell ≈ a quarter of an ant's length)
  TrailField   decaying, diffusing information fields that workers sense locally
  ExitDistance navigation cache over space the colony has actually opened
  Simulation   workers, colony drives, keeper feeding, events, checkpoints
extension/   GDExtension: fixed-tick driver, terrain texture, MultiMesh buffers, procedural audio
game/        Godot 4.7.2 presentation: shaders, glass and frame, operator window, command line
```

The presentation only reads the simulation. Tests, the headless tools and the Godot app all run the same core.

## Time, randomness and determinism

- The simulation advances on a fixed 30 Hz tick. Rendering interpolates between the last two ticks, so a
  different frame rate never changes what the colony does. `frame_independence` tests this.
- Each launch draws a 64-bit session seed from the operating system (`getrandom`). SplitMix64 derives separate
  streams from it for terrain, behaviour, decoration and audio. Each worker has its own PCG32 stream, so adding
  sound or decoration never perturbs behaviour.
- The core is built with `-fno-fast-math -ffp-contract=off`. The same seed gives the same colony, bit for bit
  (`determinism`), and a checkpoint continues exactly as the original run would have (`persistence`).
- Workers make their movement decisions on a staggered cadence, so the work of any single tick stays small.

## The habitat

The world generator lays down an undulating, gently tilted surface. Below it, a stack of soil layers of random
thickness (topsoil loam, then sand, loam and clay), thin lenses, sparse gravel, embedded stones and root
fragments. Moisture rises with depth, and hardness depends on material and moisture. Stones and roots cannot be
dug; ants work around them.

## Excavation

- Only an **exposed face** can be dug: intact, diggable material with an open 4-neighbour, within the worker's
  reach, in front of its mandibles. A face is **reserved** by the worker digging it, and at most two workers dig
  within four cells of each other; the rest queue.
- Digging is **work-based**. A pellet takes about 11 s of reference-hardness work, scaled by hardness, moisture
  and the worker's strength. The worker then picks the pellet up, carries it out and drops it on a spoil pile.
- **Volume is conserved exactly** with integer units: excavated = deposited + carried at every tick
  (`conservation`). Spoil piles slump to a natural angle of repose (about 27°) without losing volume. A worker
  that cannot get out in reasonable time packs its load into an old dead-end pocket instead (backfilling, which
  real ants also do).
- **Where to dig** is decided by local rules:
  - a passage is widened until it is 2–3 cells across, and only then advanced;
  - only the dead end of a passage advances (its neighbouring open cells must be the farthest from the exit
    locally);
  - a thick wall is kept to other passages (a clearance cone ahead of the face);
  - new branches start only from flat walls with untouched soil ahead, by workers who found no work;
  - where many workers rest, a few of them enlarge a bounded chamber.

  Galleries, forks and chambers emerge from these rules. The renderer adds no tunnel shapes of its own.
- **Pacing:** the colony's appetite for space grows with time and the number of workers and is capped at 42% of
  the substrate. In practice it digs a few percent in the first hours and reaches roughly 5–12% by 24 hours, still
  digging at the end (`pacing_24h`). Most of the soil stays intact, there is no automatic reset, and the farm is
  never emptied.

## Behaviour

Each worker has a task (dig, forage, rest or patrol), chosen with a softmax over utilities from colony demand,
its own traits and its energy. It moves through these states:

`EXPLORE`, `FOLLOW_TRAIL`, `SELECT_DIG_FACE`, `EXCAVATE`, `PICK_UP_SOIL`, `CARRY_SOIL_OUT`, `DEPOSIT_SOIL`,
`SEARCH_FOR_FOOD`, `CARRY_FOOD`, `GROOM`, `REST`

Movement scores 13 candidate headings. The terms are task direction, trail gradient, persistence, individual
variation, congestion, obstacle clearance and turning cost. A heading is sampled with a bounded softmax, then
held for a short while. Carriers follow the shortest open path to the surface: a greedy descent of the exit
distance over discovered space only. Workers never block each other outright. In a tight spot they squeeze past
or climb over, and an unloaded worker pauses to let a loaded one by. A worker that makes no progress for 4 s
re-plans; after five failed attempts it gives up the errand.

**Trails** are laid only by the action they describe: soil carriers mark the dig trail, food carriers the food
trail, resting workers the rest trail, and freshly dug faces the site mark. Each trail decays with a half-life in
simulated time and diffuses only through open space. Workers sense trails only where they stand.

**Food and keeper feeding:** the feeding stone on the surface is topped up every 3 simulated hours when it runs
low. This is the one keeper action, a defined habitat mechanism. Foragers carry seeds into the nest and store
them in caches, and resting workers eat from the store now and then.

## Rendering

- **Terrain.** The grid goes to the GPU as one small RGBA8 texture: material, remaining volume, moisture, and a
  signed distance to the tunnel boundary. The distance is an exact Euclidean transform, lightly blurred, and is
  rebuilt on a worker thread when the world changes. The soil shader generates everything finer than a cell:
  individual sand grains and gaps between them, quartz and dark mineral grains, pebbles, soft domain-warped layer
  boundaries, crumbly tunnel edges, and the curved walls and dim back wall of each passage, lit from the upper
  left through the glass.
- **Ants.** A procedural anatomical mesh: mandibles, head, geniculate antennae, mesosoma, petiole, gaster, and six
  two-segment legs. It is drawn as a single MultiMesh. A vertex shader animates an alternating tripod gait driven
  by the stride phase from the simulation, plus digging, grooming, resting and antenna probing. Loads (soil
  pellets, seeds) sit in the mandibles.
- **Glass and frame.** A nearly invisible front pane with faint reflection, dust and smudges, and a thin dark
  metal frame. Loose grains fall and roll briefly where soil is dug or dropped.

## Sound

All sound is synthesised in the extension from filtered noise and short clicks; there are no recorded samples:

- room tone, as a decorrelated stereo bed;
- mandible scrapes, whose pitch rises with soil hardness;
- crumbling pellets, trickling spoil and seed ticks;
- a faint footfall crackle that scales with the number of workers walking.

Every sound is triggered by a simulation event and panned by its position on the glass. The synth is limited to
28 voices and a per-category event rate, then passes through a gentle soft limiter. Output is 48 kHz stereo.
