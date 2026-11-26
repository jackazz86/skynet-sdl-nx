# Controls on Switch (physical only; touch is ignored)

Source of the defaults: the game's own gamepad bindings (`padBindings` in
`skynet-csharp-settings.json`, "leftmoves" sticks). Face buttons follow the Nintendo labels.

| Switch | Action |
|--------|--------|
| Left stick | Walk / strafe |
| Right stick | Look |
| ZR | Fire |
| ZL | Throw (grenade etc.) |
| A | Jump (confirms in menus) |
| B | Crouch (back in menus) |
| Y | Use / open |
| X | Next weapon (also R) |
| L | Previous weapon |
| R | Next weapon |
| L3 (click left stick) | Run |
| R3 (click right stick) | Automap |
| D-pad | Menu navigation (automap: tilt / turn) |
| + | Menu / pause |
| - | Quick save |
| L3 + R3 together | Flip menu pointer <-> gamepad by hand |
| L + R + ZL + ZR together | Gyro mode: mouse-style -> off -> added to the right stick |
| Y on a text screen | Open the Switch keyboard (+ = OK, B = cancel); A A passes the screen with the saved name |
| Gyro (console or Pro Controller) | Turns and tilts the view (added to the right stick); tune in `/switch/skynet_sdl_nx/gyro.cfg` |

Menus: left stick moves a pointer (arrow) whose hover highlights an item; A confirms the highlighted item (the engine's own Accept), ZR is a real mouse click, right stick up/down scrolls. The
pointer is on from boot and switches to the gamepad when the game logs `[Session] enter`.
In menus the engine also navigates natively with D-pad + A/B.
