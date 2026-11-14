---
name: unreal-level-design
description: >-
  Design and block out playable levels and base gameplay, especially from a
  short or vague prompt ("design a platformer level", "make a level", "block
  out a room", "add a moving platform, trigger, puzzle or hazard"). Gives the
  scale to build at (measured from the project's player character), a
  level-design quality bar, the cube blockout recipe, and a kit of gameplay
  Blueprints: moving platform, trigger, pressure plate, kill zone, jump pad,
  goal. Load it before placing level geometry or gameplay actors.
---

# Level design and base gameplay

A short prompt such as "design a platformer level" leaves size, difficulty, length and content
open. Do not fill those in by guessing, and do not answer with a list of questions. Build a
small, complete course on numbers measured from the player character, then tell the user what you
assumed so they can change it. This file is how.

Reference files, read with `load_skill` and its `file` argument:

- `platformer.md`: the elements of a platformer, with dimensions, and a default course to build
  when the prompt gives nothing else. Read it for any platformer or side-scroller request.
- `gameplay-kit.md`: the exact tool calls that create the kit Blueprints and place them. Read it
  before the first moving platform, trigger, hazard, jump pad or goal.
- `player.md`: a playable character with keyboard and gamepad controls, a side-on camera, and
  the game mode that spawns it. Read it when the project has no character, when the user asks for
  one, or when controls "do nothing".

## Order of work

1. `editor_get_context`: which level is open, and is the game running. Build in the open level.
2. Load `unreal-project-conventions`: it says which folders assets go in.
3. Measure the player (next section). Every size after this comes from those numbers.
4. Decide the view. A side-on course runs along +X with every piece centred on Y = 0; this works
   for a side-scroller and for a third-person character, so use it when the prompt does not say.
5. Write the plan as a list of beats (see "Quality bar") before placing anything.
6. Create whichever kit Blueprints the plan needs and the project does not have yet.
7. Block out beat by beat, one `batch` per beat (a batch holds at most 50 calls).
8. Check the result against the numbers ("Check before reporting").
9. Report ("What to tell the user").

Ask first only when no level is open, or when the request cannot be built without a decision that
is the user's (for example, replacing geometry that is already in the level). If the open level
already has content where the course would go, build beside it and say where.

## Measure the player

Sizes come from what the player character can do. Read it, in this order, with
`object_get_properties`:

1. The level's game mode: `{"object":"WorldSettings","names":["DefaultGameMode"]}`.
2. If that is `None`, the project's:
   `{"object":"/Script/EngineSettings.Default__GameMapsSettings","names":["GlobalDefaultGameMode"]}`.
3. The game mode's pawn: `DefaultPawnClass` on the game mode's defaults object.
4. On the pawn's defaults object:
   - `<defaults>:CharMoveComp`: `JumpZVelocity`, `GravityScale`, `MaxWalkSpeed`, `MaxStepHeight`,
     `AirControl`, `WalkableFloorAngle`, `bConstrainToPlane`, `PlaneConstraintNormal`
   - `<defaults>:CollisionCylinder`: `CapsuleRadius`, `CapsuleHalfHeight`
   - `<defaults>`: `JumpMaxCount`, `JumpMaxHoldTime`

The defaults object of a class: `/Script/Module.Name` is read at `/Script/Module.Default__Name`;
a Blueprint class `/Game/Path/BP_X.BP_X_C` is read at `/Game/Path/BP_X.Default__BP_X_C`.

If step 3 gives something that is not a Character, these reads fail. The engine's own
`GameModeBase` gives `DefaultPawn`, a flying camera, which is what a blank project has. Then:

- Look for a character the project owns: `asset_search` with `{"class":"Blueprint","query":"Character"}`.
  If there is one clear candidate, measure it, and say that the level is not using it yet.
- Otherwise use the engine `Character` row below, and say plainly that the project has no
  playable character, so the level cannot be played as a platformer until one is added. Offer
  to build one (`player.md`); build it without asking when the user asked for something they
  can play.
- If a game mode whose pawn is that character exists, offer to set it on the level
  (`object_set_properties` on `WorldSettings`, property `DefaultGameMode`). Do it only when the
  user says yes.

### Jump numbers

With `g = 980 x GravityScale` (980 is the engine's default gravity in cm/s²):

- Jump height `H = JumpZVelocity² / (2g)`, plus `JumpZVelocity x JumpMaxHoldTime` if that is not 0.
- Air time on flat ground `T = 2 x JumpZVelocity / g`, plus `JumpMaxHoldTime`.
- Reach on flat ground `R = MaxWalkSpeed x T`.
- Landing `h` cm higher (negative for lower):
  `T = (JumpZVelocity + sqrt(JumpZVelocity² - 2gh)) / g`, and reach is `MaxWalkSpeed x T`.
- `JumpMaxCount` 2: about `2H` high and `1.7R` far. Use this only when the double jump is the
  point of the beat; build the main path so a single jump is enough.

| Character | Capsule (radius / half height) | Walk | Jump, gravity | H | R | Step |
| --- | --- | --- | --- | --- | --- | --- |
| Engine `Character` | 34 / 88 (68 wide, 176 tall) | 600 | 420, x1 | 90 | 514 | 45 |
| Third Person template | 42 / 96 | 500 | 500, x1 | 128 | 510 | 45 |
| Side-scrolling template | 35 / 90 | 500 | 750, x1.75 | 164 | 437 | 45 |

The first row is measured: a standing jump rises 90 cm and a running jump covers 520 cm. Use
this table only when the project's own character cannot be read.

### What a jump may ask for

| | Required path | Test beats only | Never |
| --- | --- | --- | --- |
| Gap, same height | up to 0.6 R | up to 0.8 R | over 0.9 R |
| Step up | up to 0.7 H | up to 0.85 H | over 0.9 H |

- Where the player must *not* be able to jump (a gap that needs the moving platform, a ledge that
  needs the crate), make it at least 1.3 R wide or 1.3 H high. Nothing between 0.9 and 1.3: it
  looks possible and is not, or is possible by luck.
- A rise of `MaxStepHeight` or less is walked up without jumping. A step meant to need a jump is
  higher than that.
- Headroom over any surface the player jumps on: capsule height + H + 20.
- For a gap that also rises or drops, use the landing formula, then apply the same fractions.

## Space and scale

Units are centimetres, Z is up. The blockout piece is the engine cube,
`/Engine/BasicShapes/Cube`: 100 cm on each side with its pivot at the centre.

- `actor_spawn` with `"class":"/Engine/BasicShapes/Cube"` places a StaticMeshActor that blocks
  the player.
- `scale` = size / 100. `location` is the centre, so for a walkable top at height `top`:
  `z = top - thickness / 2`. A piece from `x0` to `x1` has `x = (x0 + x1) / 2`.
- Give every actor a `label` that says what it is (`Geo_Start`, `Geo_Gap2_Landing`,
  `Lift_ToUpperPath`) and a `folder` (`Blockout/<course name>`), so the user can find and move
  things.
- Static geometry gets the grid material, which shows scale at a glance:
  `object_set_properties` on `<label>.StaticMeshComponent0` with
  `{"OverrideMaterials":["/Engine/EngineMaterials/WorldGridMaterial"]}`. Moving pieces keep the
  plain material so they read as different.

Example, a platform 1000 x 400 x 40 from x = 0 with its top at z = 200:

```json
{"class":"/Engine/BasicShapes/Cube","location":[500,0,180],"scale":[10,4,0.4],
 "label":"Geo_Start","folder":"Blockout/Course01"}
```

### Sizes

In capsule widths `W` (2 x radius) along the direction of travel:

| Size | Length | Use it for |
| --- | --- | --- |
| Small | 1.5 to 2.5 W (about 100 to 170) | Precision landings. Never the first platform, never the landing of a test-size gap. |
| Medium | 3 to 6 W (about 200 to 400) | The normal platform. |
| Large | 9 W and up (600 to 1500) | Start, rest, checkpoint, puzzle floor, goal. |

- Depth (Y) of a side-on course: 400 for every piece, so pieces line up and a third-person
  character has room.
- Thickness: 20 to 50 for a floating platform, 100 or more for ground.
- A wall that says "not this way" is at least 2 H high. A wall of about H reads as a failed jump.
- Level size: small is 6 to 9 beats (about 80 to 110 m of travel), medium 12 to 16, large 20 or
  more with a second route. From a vague prompt build small: it is quick to judge and to change.

### Levels that are not platformers

For rooms, corridors and arenas the same rule holds: sizes come from the capsule. These are
common blockout conventions, not measurements; say so when you use them.

| Piece | Size |
| --- | --- |
| Doorway | 2 W wide or more, capsule height + 40 high (default character: 140 x 220) |
| Corridor | 3 W at the least (200); 300 to 400 is comfortable |
| Ceiling | 300 to 400 |
| Stair step | Rise 20, run 30. The rise must not exceed `MaxStepHeight` |
| Ramp | 30 degrees or less. `WalkableFloorAngle` (44.8 by default) is the limit |
| Low cover / full cover | 0.6 x capsule height (100) / capsule height + 20 (200) |

## Quality bar

A level is a sequence of beats. A beat is one idea the player meets: a gap, a lift, a hazard, a
puzzle. Write the beats down before building, and hold the plan to these rules:

1. **Safe start.** A large platform with nothing dangerous in reach, so the player can try the
   controls. The way forward is obvious from it.
2. **Teach, practise, test.** The first time an idea appears, failing is cheap: it cannot kill (a
   gap with ground under it), or the respawn marker is one step behind. Then it appears with a
   cost. Then it is combined with something already learned.
3. **One new idea per beat.** Combine only ideas that have each had their own beat.
4. **Harder, then rest.** Difficulty climbs in steps. After each test beat comes a large safe
   platform, and that is where the respawn marker goes.
5. **Short setbacks.** Failing returns the player to the start of the section, not of the level.
6. **Fair jumps.** Every jump on the required path is within the "required path" column. The
   landing is visible from the take-off. No leaps of faith.
7. **Readable danger.** A hazard is visible before the player reaches it and looks the same every
   time (the kit draws kill zones red).
8. **Moving things can be read and waited for.** The player has a safe place to stand and watch
   a full cycle. One way takes 2 to 4 seconds. The platform is medium size or larger.
9. **Height changes.** The course goes up and comes down at least once each. Height the jump
   cannot reach is what a lift, a jump pad or a crate is for.
10. **No dead ends.** Nothing the player needs can be lost for good. A crate stays on its puzzle
    floor: a wall or a stop block at each open end, and the puzzle works from wherever the crate
    ends up.
11. **An ending.** The last beat is the hardest and is followed by a visible goal.

## Gameplay elements

The kit is five small Actor Blueprints, built with the `blueprint_*` tools. Recipes and placement
are in `gameplay-kit.md`.

| Need | Use |
| --- | --- |
| Platform that moves up-down or side to side | `BP_MovingPlatform` |
| Platform, door or bridge that starts when the player touches something | `BP_MovingPlatform` set to wait, plus `BP_Trigger` pointing at it |
| Something that starts only when a level object is moved to a place | the same, with the trigger's `RequiredActor` set to that object (a crate) |
| Object the player pushes; a step to reach a ledge | a physics crate (a cube with physics on) |
| Pit, lava, spikes | `BP_KillZone` with a respawn marker |
| More height than the jump gives | `BP_JumpPad`, or a vertical `BP_MovingPlatform` |
| End of the level | `BP_Goal` |

## Check before reporting

Read the level back and check it; do not report from the plan.

- `actor_list` with the course's `folder`. For every jump on the required path, work out the gap
  between the two tops and the height difference from the actors' real locations and scales, and
  compare with the table in "What a jump may ask for". Fix what is out of range.
- For every moving platform you configured, `object_get_properties` on `<label>.Mover` and
  confirm `ControlPoints`, `Duration`, `BehaviourType` and `bAutoActivate` are what you set.
- Every trigger has its `Target`; every kill zone has its `RespawnPoint`.
- Every Blueprint you made compiles: `blueprint_compile` reports `up_to_date` and no errors.
- The player start is above a large platform, not inside geometry.

## What to tell the user

Keep it short and in this order:

1. What you built: the beats in order, one line each.
2. The numbers you built on: which character, its H and R, and where they came from (read from
   the project, or the fallback table).
3. What you assumed: view, length, difficulty. These are the first things to change.
4. What you could not check. You did not play the level; say so. Name anything that depends on
   feel (crate weight, platform speed, jump timing).
5. What is missing for it to be playable, if anything (no character, no game mode on the level).
6. Paths of the assets created, and that they are unsaved until the user asks for `asset_save`.

## Limits

- A playable character is built by reading keys from the player controller (`player.md`). There
  are no input-event nodes and no Enhanced Input actions, and no node that sets a property on
  another object, so sprint and a mouse-turned camera are out of reach of the graph tools.
- The Level Blueprint is not reachable. Level logic lives in the kit's actors, which point at
  each other through instance variables.
- Blueprint casts, timelines and events bound to a component are not available. The kit is
  designed around that; say so if a request needs them, and offer C++ (`unreal-cpp-gameplay`).
- You cannot play the level. Everything about feel is a first setting for the user to tune. Do
  not report that something works in play; report what you built and what you read back.

## If the user asks for geometry brushes (BSP)

Cubes are the default because they can be created directly. `actor_spawn` with class `Brush`
makes an empty brush that cannot be given a shape. When the user wants brushes and the level
already holds a box brush:

1. `actor_duplicate` the existing box brush, then `actor_get` the copy for its name.
2. Resize through its builder: `object_set_properties` on
   `<map path>:PersistentLevel.<brush name>.CubeBuilder_0` with `X`, `Y`, `Z` in cm. Keep the
   actor's scale at 1.
3. Move it with `actor_update` *after* resizing: the rebuild snaps the location to the grid.

If there is no box brush to copy, say so and build with cubes.

Brushes are for geometry that stays still. Anything that moves is a `BP_MovingPlatform`
(`gameplay-kit.md`, "Brushes do not move").
