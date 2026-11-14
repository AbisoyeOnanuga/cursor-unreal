# Playable character

A character the player can run and jump with, a side-on camera, and the game mode that makes Play
spawn it. For a project that has no character of its own. If the project already has one (the
Third Person template's, the user's own), use that instead and skip this file.

The recipe is played by the plugin's automation tests (`CursorUnreal.LevelDesign.Controls`): the
character is built with these calls, then keys are pressed on its player controller. Measured,
with the project's input set to Enhanced Input and to the older input system alike: one second of
D or the left stick moves it 517 cm, Space and the gamepad's bottom button jump 90 cm, a held
jump key jumps once, and the camera sits 900 cm to the side.

It keeps the engine `Character`'s movement numbers, so the first row of the table in `SKILL.md`
(walk 600, jump height 90, reach 514) is this character's.

## How input is read here

The graph asks the player controller, every frame, which keys are down. That needs nothing set
up in Project Settings and no input assets.

- Held key: `IsInputKeyDown`. Pressed this frame: `WasInputKeyJustPressed`. Stick or mouse axis,
  -1 to 1: `GetInputAnalogKeyState`. All three are functions of `PlayerController`.
- Get the controller with `GetPlayerController` from `GameplayStatics`, and connect its
  `ReturnValue` to each reader's `self`.
- The key goes on the reader's `Key` pin with `blueprint_set_pin_default`, as a plain name:
  `D`, `Right`, `SpaceBar`, `LeftShift`, `Gamepad_LeftX`, `Gamepad_FaceButton_Bottom` (A on an
  Xbox pad). Not `(KeyName="D")`. The call's result shows the name that was stored, and a name
  that is not a key is refused with the close matches.
- Do not use `GetInputAxisValue` or `GetInputAxisKeyValue`, in a graph or from C++ called by one.
  They read an axis that only the Blueprint editor's own input node binds; without that binding
  they return 0 and log "not bound on this input component". `blueprint_add_node` refuses them.
- Do not change the project's input classes or add axis mappings in `Config/DefaultInput.ini`.
  Reading keys from the controller works under either input system.

## BP_PlatformerCharacter

`<P>` is the asset path, `<Name>` its last part. With the default folder layout:
`/Game/Agent/Characters/Platformer/BP_PlatformerCharacter`.

### Structure

1. `blueprint_create` `{"path":"<P>","parent_class":"Character"}`
2. `blueprint_add_component` `{"blueprint":"<P>","class":"StaticMeshComponent","name":"Body"}`
3. `blueprint_add_component` `{"blueprint":"<P>","class":"SpringArmComponent","name":"CameraBoom"}`
4. `blueprint_add_component`
   `{"blueprint":"<P>","class":"CameraComponent","name":"Camera","parent":"CameraBoom"}`
5. `blueprint_compile`
6. `object_set_properties` on `<P>.<Name>_C:Body_GEN_VARIABLE`, a 50 x 50 x 170 block to see the
   character by. The capsule does the colliding, so the block must not:

   ```json
   {"StaticMesh":"/Engine/BasicShapes/Cube","RelativeScale3D":{"X":0.5,"Y":0.5,"Z":1.7},
    "BodyInstance.CollisionProfileName":"NoCollision","bGenerateOverlapEvents":false}
   ```
7. `object_set_properties` on `<P>.<Name>_C:CameraBoom_GEN_VARIABLE`. The arm points down -Y
   whichever way the character faces, so the camera is 900 cm out on +Y looking at the course:

   ```json
   {"TargetArmLength":900,"bDoCollisionTest":false,"bUsePawnControlRotation":false,
    "bInheritPitch":false,"bInheritYaw":false,"bInheritRoll":false,
    "RelativeRotation":{"Pitch":0,"Yaw":-90,"Roll":0}}
   ```
8. `object_set_properties` on `<P>.Default__<Name>_C`: `{"bUseControllerRotationYaw":false}`
9. `object_set_properties` on `<P>.Default__<Name>_C:CharMoveComp`. The character turns to face
   the way it runs and cannot leave the course's plane (Y stays where it started):

   ```json
   {"bOrientRotationToMovement":true,"bConstrainToPlane":true,"PlaneConstraintAxisSetting":"Y"}
   ```
10. `blueprint_compile`

With the plane lock the player cannot fall off the near or far edge, so a side-on course needs no
rails.

### Graph

Nodes (`blueprint_add_node`; `call_function` unless it says otherwise):

| Name here | Node | Pin values (`blueprint_set_pin_default`) |
| --- | --- | --- |
| Tick | kind `event`, name `Tick` | |
| PC | `GetPlayerController`, class `GameplayStatics` | |
| DownD, DownRight, DownA, DownLeft | `IsInputKeyDown`, class `PlayerController` | `Key`: `D`, `Right`, `A`, `Left` |
| Stick | `GetInputAnalogKeyState`, class `PlayerController` | `Key`: `Gamepad_LeftX` |
| JumpKey, JumpButton | `WasInputKeyJustPressed`, class `PlayerController` | `Key`: `SpaceBar`, `Gamepad_FaceButton_Bottom` |
| RightHeld, LeftHeld, JumpNow | `BooleanOR`, class `KismetMathLibrary` | |
| RightAmount, LeftAmount | `SelectFloat`, class `KismetMathLibrary` | `A`: `1.0`, `B`: `0.0` |
| MoveRight, MoveStick | `AddMovementInput`, class `Pawn` | `WorldDirection`: `(X=1,Y=0,Z=0)` |
| MoveLeft | `AddMovementInput`, class `Pawn` | `WorldDirection`: `(X=-1,Y=0,Z=0)` |
| Branch | kind `branch` | |
| Jump | `Jump`, class `Character` | |

Wiring:

- `PC.ReturnValue -> self` of DownD, DownRight, DownA, DownLeft, Stick, JumpKey and JumpButton
- `DownD.ReturnValue -> RightHeld.A`, `DownRight.ReturnValue -> RightHeld.B`,
  `RightHeld.ReturnValue -> RightAmount.bPickA`, `RightAmount.ReturnValue -> MoveRight.ScaleValue`
- `DownA.ReturnValue -> LeftHeld.A`, `DownLeft.ReturnValue -> LeftHeld.B`,
  `LeftHeld.ReturnValue -> LeftAmount.bPickA`, `LeftAmount.ReturnValue -> MoveLeft.ScaleValue`
- `Stick.ReturnValue -> MoveStick.ScaleValue`
- `JumpKey.ReturnValue -> JumpNow.A`, `JumpButton.ReturnValue -> JumpNow.B`,
  `JumpNow.ReturnValue -> Branch.Condition`
- Execution: `Tick.then -> MoveRight.execute`, `MoveRight.then -> MoveLeft.execute`,
  `MoveLeft.then -> MoveStick.execute`, `MoveStick.then -> Branch.execute`,
  `Branch.then -> Jump.execute`

Then `blueprint_compile`.

Controls this gives: D or Right arrow runs toward +X, A or Left arrow toward -X, the left stick
does both, Space or the gamepad's bottom button jumps. Tell the user exactly these.

## BP_PlatformerGameMode

1. `blueprint_create` `{"path":"<G>","parent_class":"GameModeBase"}`, then `blueprint_compile`
2. `object_set_properties` on `<G>.Default__<GName>_C`: `{"DefaultPawnClass":"<P>"}`
3. `blueprint_compile`
4. Make the level use it, `object_set_properties` on `WorldSettings`:
   `{"DefaultGameMode":"<G>"}`. This changes what Play does in the user's level: do it when the
   user asked for a playable level or character, otherwise offer it.
5. Place a `PlayerStart` over the first platform if the level has none (see `gameplay-kit.md`).

The engine's standard player controller is enough. Do not make a controller Blueprint for this.

## Check, and what to say

- Read the keys back: `blueprint_get_nodes` on `<P>`, and confirm every `Key` pin shows the key
  name you meant. An empty `Key` is a control that does nothing.
- Both Blueprints compile `up_to_date`.
- `object_get_properties` on `WorldSettings` shows the game mode.
- You cannot press Play. Say so once, and do not say the controls work; say what they are and
  that the recipe is tested. Ask the user to press Play and click inside the game view once:
  until the view has focus the keys go to the editor, not the game.
- If the user says a control does nothing, read the `Key` pins and the editor log
  (`editor_log_tail`, look for "not bound" and for Blueprint runtime errors) before changing
  anything. Do not rebuild the controls on a guess.

## Changing it

- Walking toward and away from the camera (a 2.5D course): leave out `bConstrainToPlane` and
  `PlaneConstraintAxisSetting`, and add two more pairs like MoveRight and MoveLeft: W or Up with
  `WorldDirection` `(X=0,Y=-1,Z=0)` (away from the camera), S or Down with `(X=0,Y=1,Z=0)`. The
  course then needs something to stop the player at the near and far edges: see "Invisible
  walls" in `gameplay-kit.md`. This variation is not covered by the tests.
- Jump height and speed: `JumpZVelocity`, `MaxWalkSpeed`, `GravityScale`, `AirControl` on
  `<P>.Default__<Name>_C:CharMoveComp`. Changing them changes H and R: measure again
  (`SKILL.md`, "Measure the player") and check the course's jumps against the new numbers.
- Camera distance: `TargetArmLength` on the boom. Height: add `"SocketOffset":{"X":0,"Y":0,"Z":150}`
  to raise the camera without tilting it.

## Limits

- Sprint, crouch speed, or anything else that changes a property of the movement component while
  playing cannot be built: the graph tools have no node that sets a property on another object.
- A camera the player turns with the mouse or the right stick, and peeking ahead, are not in this
  recipe and not tested. Say so if asked, and offer C++ (`unreal-cpp-gameplay`).
- No input-event nodes (the red "key pressed" nodes), no Enhanced Input actions. Reading keys
  from the controller, as above, is the supported way.
