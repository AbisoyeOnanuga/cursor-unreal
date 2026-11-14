# Platformer levels

What a platformer level is made of, how big each part is, and a default course to build when
the prompt says no more than "design a platformer level".

All sizes are in terms of the player's numbers from `SKILL.md`: `H` jump height, `R` flat reach,
`W` capsule width. Numbers in brackets are for the engine's default character (H 90, R 514,
W 68), rounded. Recompute them for the project's character.

## Side-on layout

- The course runs along +X, left to right on screen. Every piece is centred on Y = 0 and is 400
  deep. Z is height.
- A side-scrolling character is held on a plane: `bConstrainToPlane` true on its movement
  component, with `PlaneConstraintNormal` (0, 1, 0). If the normal is (1, 0, 0) the course runs
  along Y instead: swap X and Y everywhere, and use `YZPlane` for crates.
- The side-scrolling camera looks from +Y towards -Y. Nothing taller than the floor goes on the
  +Y side of the course: it would hide the player. Backdrop walls go behind, at Y of -400 or
  less.
- The same layout plays with a third-person character, which is why it is the default when the
  view is unknown.

## Elements

What a platformer is built from. A good level uses several, each introduced on its own.

| Element | What it asks of the player | Size |
| --- | --- | --- |
| Gap | Judge distance | 0.35 R first time, 0.5 R normal, up to 0.6 R on the path, up to 0.8 R as a test |
| Step up | Judge height | 0.4 to 0.7 H; more than `MaxStepHeight` or it is just walked up |
| Drop | Commit | Landing visible from the edge; no more than 6 H down in one fall |
| Small platform | Precision | 1.5 to 2.5 W long, only after medium ones |
| Wall or cliff | "Find another way" | 2 H or more |
| Moving platform, side to side | Timing | Crosses a gap of 1.3 R or more; platform medium or larger; 100 to 250 cm/s |
| Moving platform, up-down (lift) | Timing, height | Climbs 1.3 H or more; 80 to 150 cm/s |
| Platform that starts on touch | Notice cause and effect | Waits until the player steps on, then keeps cycling |
| Door, gate, bridge on a trigger | Cause and effect at a distance | The trigger is before the door and the door is in view from it |
| Pressure plate and crate | Move an object to a place | Push of 3 to 6 m on a walled floor; the gate it opens is in view |
| Crate as a step | Use an object as a tool | Ledge between 1.3 H and crate + 0.75 H |
| Hazard (pit, lava, spikes) | Risk | Visible before reached; red; sends the player back one section |
| Jump pad | A burst of height | Lands on a large platform; aimed, never straight up |
| Rest platform | Nothing: a breath | Large; holds the respawn marker |
| Goal | The reason to move | Large platform, green box, a tall flag |

How to build each one is in `gameplay-kit.md`.

## Verticality

Height is what makes a course a platformer and not a corridor. Force it with the character's own
limits:

- Stairs of platforms, each 0.4 to 0.7 H above the last, climb as far as you like on jumps alone.
- A ledge of 1.3 H or more cannot be jumped. That is where a lift, a jump pad or a crate goes, and
  it is how the level makes the player use them.
- Come down by drops and descending gaps; a landing lower than the take-off extends the reach,
  so descending gaps are easier than level ones of the same width.
- Put the goal high or far and in view, so the player sees where they are going.
- Extra jumps (`JumpMaxCount` above 1) and wall jumps are for a route the user asks for. Do not
  build the only path on a move you have not confirmed the character has.

## Default course

Nine beats, about 110 m, four sections. Build this when the prompt gives nothing more specific,
and say that you did. It introduces each idea alone, then combines them, and uses every part of
the kit once.

Lay the pieces out left to right, each starting where the previous one ends plus the gap. Tops
are cumulative: "0.4 H higher" means higher than the piece before.

| # | Beat | Build | Why |
| --- | --- | --- | --- |
| 1 | Start | Ground, large (1200), top at 0. Player start 200 in from the left end. | Safe place to try the controls. |
| 2 | First jumps | The ground continues. On it, two blocks 0.6 H (55) high and 400 long, 0.35 R (180) apart. Marker M1 on the second block. | A step up and a gap that cannot kill: a miss lands on the ground between. |
| 3 | Gaps | Three medium platforms (300), gaps 0.5 R (260), each 0.4 H (35) higher. The ground ends: a pit below. One more 0.5 R gap to beat 4. | The same jump, now with a cost. |
| 4 | Rest | Large (800), level with the last platform. Marker M2. | Breath; checkpoint. |
| 5 | Moving platform | A gap of 1.5 R (800), too wide to jump. One `BP_MovingPlatform` 300 x 400 x 30, `PingPong` across it, travel = gap - 320 (480), `Duration` 3. Then a large platform (600) at the same height with marker M3. | Timing. The player can stand on the rest platform and watch it. |
| 6 | Lift | After the 600 platform, a shaft 320 wide, then a cliff: the upper floor is 3.5 H (320) higher. A lift 300 x 400 x 30 fills the shaft, top level with the lower floor, `PingPong` up by the cliff height, `Duration` 3, waiting. A `BP_Trigger` over it starts it when the player steps on. A wall under the upper floor's edge, down past the lower floor, makes the cliff face. | Height the jump cannot reach; touching something makes it move. |
| 7 | Puzzle | Upper floor, large (1700). From the lift end: 150 clear; a stop block 50 long and 0.65 H (60) high; marker M4; the crate 250 past the block; the plate 600 past the crate; the gate 300 past the plate; 300 of floor after the gate with marker M5. Gate: a `BP_MovingPlatform` 50 x 400 x 300, `OneShot`, travel down by its height + 10, `Duration` 2, waiting. Plate: `BP_Trigger` with `RequiredActor` the crate and `Target` the gate. | Move an object to a place to open the way. The stop block keeps the crate on the floor. |
| 8 | Test | Three gaps of 0.6 R (310) going down: a medium platform (300) 0.5 H (45) lower, a small one (170) 0.5 H lower again, then a medium one (400) 0.5 H lower again. | The hardest jumps, combining distance, height change and a small landing. |
| 9 | Finale | A jump pad on the last platform of beat 8, aimed at the goal platform: 4 H (360) higher, landing 450 ahead of the launch point. Goal platform large (1000). `BP_Goal` box and a flag (a cube 30 x 30 x 400) near its far end. | A burst of height, then the end in view. |

Kill zones, one per section, each as long as the pit it covers, top 300 below the lowest platform
above it:

| Zone | Covers | Sends the player to |
| --- | --- | --- |
| 1 | Under beat 3, from the end of the ground to beat 4 | M1 |
| 2 | Under the gap of beat 5 | M2 |
| 3 | The lift shaft of beat 6 | M3 |
| 4 | Under beats 8 and 9, from the end of the upper floor to the goal platform | M5 |

Zone 3's top must stay below the lift's lowest position, or the lift sets it off.

Check these as well as the general list in `SKILL.md`:

- The beat 5 gap and the beat 6 cliff cannot be jumped (1.3 R and 1.3 H or more).
- The gate cannot be jumped and spans the full depth of the floor.
- The crate is between the stop block and the plate, and the plate between the crate and the gate.
- The jump pad's numbers come from the formula in `gameplay-kit.md`, and the goal platform's near
  edge is at 0.6 of the launch distance or further.

## Changing it

When the user asks for more, less, harder or easier, these are the handles. Offer them in the
report so the next request can be specific.

- **Difficulty.** Gap size (0.5 R towards 0.7 R), landing size (medium towards small), platform
  speed (`Duration`), and whether a miss costs anything (ground below, or a pit).
- **Length.** Beats 3, 5 and 8 can each be repeated with a variation: a second moving platform
  going the other way, a lift after a moving platform. Keep a rest platform and a marker between.
- **Theme of the beats.** More puzzles (crate as a step, two plates), more timing (several moving
  platforms in a row), more height (a tower of lifts and jump pads).
- **A second route.** A harder optional path to something visible: a test-size gap or a small
  platform leading to a shortcut.
- **Free-roaming 3D.** The same beats, turning 90 degrees at each rest platform so the course is
  an L or a U and not a line. Platforms are medium or larger in both directions, because the
  jump direction is less exact. A crate's plane lock follows world axes: run a puzzle floor
  along X with `XZPlane`, or along Y with `YZPlane`.
