# Gameplay kit

Five Actor Blueprints that cover the moving and reacting parts of a level, plus a physics crate
and two markers. Each recipe below is the exact sequence of tool calls. They are played by the
plugin's automation tests (`CursorUnreal.LevelDesign`) with the engine's default character, so
follow them as written; where a number comes from one of those runs it says "measured".

## Before building

- Check what exists: `asset_search` with `{"query":"BP_MovingPlatform"}` (and the other names).
  Reuse a kit Blueprint that is already there; do not make a second copy.
- Put them where `unreal-project-conventions` puts gameplay Blueprints. With that skill's default
  layout: `/Game/Agent/Blueprints/Gameplay/Platformer/BP_MovingPlatform` and so on. `<P>` below
  is the Blueprint's asset path and `<Name>` its last part.
- A component added to a Blueprint keeps its defaults at `<P>.<Name>_C:<Component>_GEN_VARIABLE`.
  That object exists only after a compile, so the order is always: add components,
  `blueprint_compile`, set their properties, `blueprint_compile`.
- Asset-valued properties take the full object path: `/Engine/BasicShapes/Cube.Cube`.
- `blueprint_add_node` returns the node's `id`. Wiring below is written
  `Node.pin -> Node.pin`, each one a `blueprint_connect_pins` call
  (`from_node`, `from_pin`, `to_node`, `to_pin`). Execution leaves a node by `then` and enters
  the next by `execute`; a function's target pin is `self`; its result is `ReturnValue`.
- Finish every Blueprint with `blueprint_compile`. The result must be `up_to_date` with no errors
  or warnings.

## BP_MovingPlatform

A cube carried along a path. No graph. Also used for doors, gates, bridges and lifts.

1. `blueprint_create` `{"path":"<P>","parent_class":"Actor"}`
2. `blueprint_add_component` `{"blueprint":"<P>","class":"StaticMeshComponent","name":"Mesh"}`
3. `blueprint_add_component` `{"blueprint":"<P>","class":"InterpToMovementComponent","name":"Mover"}`
4. `blueprint_compile`
5. `object_set_properties` on `<P>.<Name>_C:Mesh_GEN_VARIABLE`:
   `{"StaticMesh":"/Engine/BasicShapes/Cube.Cube"}`
6. `object_set_properties` on `<P>.<Name>_C:Mover_GEN_VARIABLE`:

   ```json
   {"ControlPoints":[
      {"PositionControlPoint":{"X":0,"Y":0,"Z":0},"bPositionIsRelative":true},
      {"PositionControlPoint":{"X":0,"Y":0,"Z":300},"bPositionIsRelative":true}],
    "Duration":3,"BehaviourType":"PingPong","bSweep":false}
   ```
7. `blueprint_compile`

The mesh is the first component, so it is the root and the mover moves it.

**Keep `bSweep` false.** With the component's default (true) the platform stops against the
first thing in its way, the player included: measured, a block sliding at a standing player
stopped after 152 of 580 cm. With it off the block completed its path and pushed the player
along. A player standing on top is carried either way.

### Placing one

`actor_spawn` with `"class":"<P>"`. Size it like a blockout cube: `scale` = size / 100,
`location` = centre of the cube at the *start* of its path. Then set the path on the instance,
`object_set_properties` on `<label>.Mover`:

- `ControlPoints`: two points, the first all zero, the second the offset to the far end in cm.
  Offsets are world centimetres from the start position and are not multiplied by the actor's
  scale. Up-down: `{"X":0,"Y":0,"Z":300}`. Along the course: `{"X":480,"Y":0,"Z":0}`.
- `Duration`: seconds for one way. Aim for 100 to 250 cm/s (distance / duration), slow enough to
  board at walking speed.
- `BehaviourType`: `PingPong` (back and forth for ever), `OneShot` (go once and stay: doors,
  gates, bridges), `Loop_Reset` (jump back to the start and repeat).
- `bAutoActivate`: `true` moves from the start of play. `false` waits for a `BP_Trigger`.

All of these can go in one call. Read them back afterwards (`object_get_properties` on
`<label>.Mover`); if only the first took effect, set the rest one property per call.

Measured: a character standing on a 300 x 300 platform was carried the full 300 cm up and 400 cm
sideways and was still standing on it.

Fit to the level:

- Crossing a gap: at each end of the path the platform's edge is 10 cm from the fixed platform's
  edge, level with it. Travel = gap - platform length - 20.
- A lift: at the bottom its top is level with the lower floor, at the top level with the upper
  one. Travel = the height difference.
- Do not run a path through a `BP_Trigger` or `BP_KillZone` box: the platform sets them off.
- Nothing should stand under a platform that comes down, and no path should pass through where
  the player waits.

## Trigger base

`BP_Trigger`, `BP_KillZone`, `BP_JumpPad` and `BP_Goal` all start the same way:

1. `blueprint_create` `{"path":"<P>","parent_class":"Actor"}`
2. `blueprint_add_component` `{"blueprint":"<P>","class":"BoxComponent","name":"Trigger"}`
3. `blueprint_compile`
4. `object_set_properties` on `<P>.<Name>_C:Trigger_GEN_VARIABLE`:
   `{"BoxExtent":{"X":50,"Y":50,"Z":50},"bHiddenInGame":false,"ShapeColor":<colour>}`

The box is 100 cm on each side at scale 1, the same convention as the cube, so an instance is
sized with `scale` = size / 100. It is drawn in the game as a wire box in its colour, so the
player and the user can see it. Colours:

| Blueprint | `ShapeColor` |
| --- | --- |
| `BP_Trigger` | `{"R":255,"G":210,"B":0,"A":255}` (yellow) |
| `BP_KillZone` | `{"R":255,"G":0,"B":0,"A":255}` (red) |
| `BP_JumpPad` | `{"R":0,"G":200,"B":255,"A":255}` (cyan) |
| `BP_Goal` | `{"R":0,"G":255,"B":0,"A":255}` (green) |

The box reacts to anything that generates overlap events: the player, a physics crate set up as
below, and a kit platform (measured: a platform passing through a trigger started its target).
Plain blockout cubes have overlap events off and do not set it off.

## BP_Trigger

When something enters the box, the mover on the actor `Target` points at starts. If
`RequiredActor` is set, only that actor counts.

After the trigger base:

1. `blueprint_add_variable` `{"blueprint":"<P>","name":"Target","type":"object:Actor","instance_editable":true}`
2. The same for `RequiredActor`.
3. `blueprint_compile`
4. Nodes (`blueprint_add_node`):
   - `Event`: `{"kind":"event","name":"ActorBeginOverlap"}`
   - `Required`: `{"kind":"variable_get","name":"RequiredActor"}`
   - `Target`: `{"kind":"variable_get","name":"Target"}`
   - `IsAnyone`: `{"kind":"call_function","function":"EqualEqual_ObjectObject","class":"KismetMathLibrary"}`
   - `IsRequired`: the same function again
   - `Either`: `{"kind":"call_function","function":"BooleanOR","class":"KismetMathLibrary"}`
   - `Branch`: `{"kind":"branch"}`
   - `GetMover`: `{"kind":"call_function","function":"GetComponentByClass","class":"Actor"}`
   - `Activate`: `{"kind":"call_function","function":"Activate","class":"ActorComponent"}`
5. Wiring:
   - `Required.RequiredActor -> IsAnyone.A` (leave `B` empty: true when no actor is required)
   - `Event.OtherActor -> IsRequired.A`
   - `Required.RequiredActor -> IsRequired.B`
   - `IsAnyone.ReturnValue -> Either.A`
   - `IsRequired.ReturnValue -> Either.B`
   - `Either.ReturnValue -> Branch.Condition`
   - `Event.then -> Branch.execute`
   - `Target.Target -> GetMover.self`
   - `GetMover.ReturnValue -> Activate.self`
   - `Branch.then -> Activate.execute`
6. `blueprint_set_pin_default` `{"blueprint":"<P>","node":"<GetMover>","pin":"ComponentClass","value":"InterpToMovementComponent"}`
7. `blueprint_compile`

### Placing one

`actor_spawn` the trigger, then `object_set_properties` on its label with actor labels as values:
`{"Target":"Gate_Puzzle"}` or `{"Target":"Gate_Puzzle","RequiredActor":"Crate_Puzzle"}`.
The target is a `BP_MovingPlatform` instance with `bAutoActivate` false.

- **Platform that starts when the player steps on it.** Trigger as wide and deep as the platform,
  100 tall, centred 60 cm above the platform's top at its start position. That leaves 10 cm
  between the box and the platform, so the platform does not set it off itself, and a player
  standing on the platform is inside it. Target `PingPong`, so a player who falls off can catch
  it again. Measured: the lift stayed put until the player stepped on, then carried them up.
- **Door, gate or bridge opened from somewhere else.** Trigger across the path, 200 tall, before
  the door. Target `OneShot`.
- **Pressure plate that needs an object.** Trigger about 150 x 400 x 50 with its centre 30 cm
  above the floor, `RequiredActor` set to the crate. Measured: the player walking through did
  nothing; the crate arriving opened the gate. Put a thin cube (5 cm) under it so the plate is
  visible as a place.

A trigger starts its target; it does not stop it or send it back. A gate stays open.

## BP_KillZone

Whatever enters is moved to the actor `RespawnPoint` refers to.

After the trigger base:

1. `blueprint_add_variable` `{"blueprint":"<P>","name":"RespawnPoint","type":"object:Actor","instance_editable":true}`
2. `blueprint_compile`
3. Nodes:
   - `Event`: `{"kind":"event","name":"ActorBeginOverlap"}`
   - `Respawn`: `{"kind":"variable_get","name":"RespawnPoint"}`
   - `GetLocation`: `{"kind":"call_function","function":"GetActorLocation","class":"Actor"}`
   - `SetLocation`: `{"kind":"call_function","function":"SetActorLocation","class":"Actor"}`
4. Wiring:
   - `Respawn.RespawnPoint -> GetLocation.self`
   - `Event.OtherActor -> SetLocation.self`
   - `GetLocation.ReturnValue -> SetLocation.NewLocation`
   - `Event.then -> SetLocation.execute`
5. `blueprint_compile`

### Placing one

- The marker: `actor_spawn` with `"class":"TargetPoint"`, on the section's rest platform, at the
  platform's top + the capsule half height + 20. The player is moved so that their centre is at
  the marker, so a marker at floor height would put them in the floor.
- The zone: `actor_spawn` the kill zone, then `object_set_properties` on its label:
  `{"RespawnPoint":"<marker label>"}`.
- A pit: one zone per section, as long as the section, 1000 deep (Y), 100 thick, its top 300
  below the lowest platform of that section. A hazard on the path (lava strip, spikes): a zone
  the size of the hazard, 30 to 50 tall, sitting on the floor.
- One marker per section, so a fall costs one section.

Measured: the player was put at the marker and landed. A physics crate that fell in was put back
at the marker as well, so a crate pushed off the edge is not lost. A kit platform sets off these
boxes like anything else and would be moved to the marker, so keep platform paths out of kill
zones.

## BP_JumpPad

Launches the player when the player touches it.

After the trigger base:

1. `blueprint_add_variable` `{"blueprint":"<P>","name":"LaunchVelocity","type":"vector","default_value":"(X=0,Y=0,Z=1000)","instance_editable":true}`
2. `blueprint_compile`
3. Nodes:
   - `Event`: `{"kind":"event","name":"ActorBeginOverlap"}`
   - `Player`: `{"kind":"call_function","function":"GetPlayerCharacter","class":"GameplayStatics"}`
   - `Velocity`: `{"kind":"variable_get","name":"LaunchVelocity"}`
   - `IsPlayer`: `{"kind":"call_function","function":"EqualEqual_ObjectObject","class":"KismetMathLibrary"}`
   - `Branch`: `{"kind":"branch"}`
   - `Launch`: `{"kind":"call_function","function":"LaunchCharacter","class":"Character"}`
4. Wiring:
   - `Event.OtherActor -> IsPlayer.A`
   - `Player.ReturnValue -> IsPlayer.B`
   - `IsPlayer.ReturnValue -> Branch.Condition`
   - `Event.then -> Branch.execute`
   - `Player.ReturnValue -> Launch.self`
   - `Velocity.LaunchVelocity -> Launch.LaunchVelocity`
   - `Branch.then -> Launch.execute`
5. `blueprint_set_pin_default` on `Launch`: pin `bXYOverride` value `true`, and pin `bZOverride`
   value `true`. The launch then replaces the player's velocity, so where they land does not
   depend on how fast they arrived.
6. `blueprint_compile`

### Placing and aiming one

`actor_spawn` the pad: about 150 x 150 x 40 (`scale` `[1.5,1.5,0.4]`), its centre 30 cm above
the floor. Then `object_set_properties` on its label: `{"LaunchVelocity":{"X":..,"Y":0,"Z":..}}`.

To land on a surface `h` cm above the pad's floor, `d` cm ahead, with `g = 980 x GravityScale`
and `H` the character's jump height:

- `Z = sqrt(2 x g x (h + H))`: the peak is one jump height above the landing.
- Air time `T = (Z + sqrt(Z² - 2 x g x h)) / g`.
- `X = d / T`.
- Count `d` from where the player is launched: the pad's near edge less the capsule radius, not
  the pad's centre. Aim at the middle of a large platform whose near edge is at 0.6 `d` or
  further.

Measured with the default character: `h` 360 and `d` 450 give `Z` 940, `T` 1.39, `X` 324; the
player ran onto the pad at full speed, landed on the platform and came to rest within 30 cm of
the computed spot. A launch of `Z` 1000 rises 512 cm.

A pad that launches straight up (`X` 0) catches the player again on the way down and keeps
bouncing them until they steer off, which a character with little air control cannot do. Always
aim a pad at a landing.

## BP_Goal

After the trigger base:

1. Nodes:
   - `Event`: `{"kind":"event","name":"ActorBeginOverlap"}`
   - `Print`: `{"kind":"call_function","function":"PrintString","class":"KismetSystemLibrary"}`
2. `blueprint_set_pin_default` on `Print`: pin `InString` value `Goal reached`; pin `Duration`
   value `5.0`.
3. `Event.then -> Print.execute`
4. `blueprint_compile`

Place it on the last platform, about 100 x 400 x 200, its centre 110 cm above the floor, with a
tall thin cube beside it as a flag the player can see from a distance. It prints a message and
nothing else: no level change, no score. Say so.

## Physics crate

A cube the player pushes by walking into it.

1. `actor_spawn` `{"class":"/Engine/BasicShapes/Cube","scale":[0.7,0.7,0.7],"location":[x,0,top + 36],"label":"Crate_Puzzle","folder":...}`
2. `object_set_properties` on `Crate_Puzzle.StaticMeshComponent0`:

   ```json
   {"Mobility":"Movable","BodyInstance.bSimulatePhysics":true,
    "BodyInstance.LinearDamping":3,"BodyInstance.AngularDamping":5,
    "BodyInstance.DOFMode":"XZPlane","bGenerateOverlapEvents":true}
   ```

- `bGenerateOverlapEvents` is what lets triggers and kill zones notice it.
- `DOFMode` `XZPlane` keeps it on the line of a side-on course and stops it turning. Measured
  without it: pushed 15 cm off-centre, the crate slid 90 cm sideways and the player walked past
  it. Leave it out only in a level where the crate is meant to move in any direction.
- The damping makes it stop when the player lets go. Measured with these values and the plane
  lock: about 360 cm/s while pushed, 85 cm of slide after. With no damping it slid 570 cm.
  Raise `LinearDamping` for a heavier feel. This is feel: tell the user it wants a play-test.
- Size 70: lower than the default jump (90), so the player can also jump onto it. Measured: the
  player stands on it and a jump from it reaches 165 cm above the floor.

**Crate as a step.** A ledge higher than the jump, reached from the crate: ledge height between
1.3 H and crate size + 0.75 H (default character: 117 to 137, use 130). The crate starts where
it must be pushed 300 to 600 cm to the foot of the ledge.

## Invisible walls

A blocker the player cannot see: the edge of a course where the player can walk toward and away
from the camera, or a limit to the play area. A blockout cube with drawing switched off.

1. `actor_spawn` `{"class":"/Engine/BasicShapes/Cube","scale":[..],"location":[..],"label":"Wall_NearEdge","folder":"Blockout/<course>/Walls"}`
2. `object_set_properties` on `Wall_NearEdge.StaticMeshComponent0`: `{"bVisible":false}`

It still blocks; only drawing is off, in the editor view as well, so the label and folder are how
the user finds it. Make it at least 2 H high so it cannot be jumped, and as long as the platform
it guards. Tell the user the walls exist and which folder they are in.

A side-on course whose character is locked to the plane (`player.md`) does not need them.

## Brushes do not move

A geometry brush (BSP) is fixed level geometry. Moving one while the game runs changes nothing
the player sees or stands on. Anything that moves is a `BP_MovingPlatform`, which has its own
mesh; never point a mover at a brush.

## Markers

- Player start: `actor_spawn` `{"class":"PlayerStart","location":[x,0,top + capsule half height + 10]}`,
  200 cm in from the start platform's edge. One per level.
- Respawn marker: `TargetPoint`, as under `BP_KillZone`.
