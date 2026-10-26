# Pokémon Shield on Android: controls and UI design

The player's design brief for the app phase (Phase 3), with what each item needs from the port. Status: design; nothing here is built yet.

## Principles

- Context over clutter: only what the player needs right now.
- Thumb-first: everything in the lower 60 % of the screen.
- No friction between exploration and battle.
- Respect the phone: haptics, layouts adapted to aspect ratio and cut-outs, density and size under the player's control.
- Input latency under 40–50 ms end to end; buttons react visually (scale + colour) on touch-down, before the game sees the input.

## How "context" is known

The game runs unmodified, so the overlay learns the context by reading game memory, as the Link's Awakening port does: the scene (field / battle / menu / Pokédex / bag), whether a dialogue box waits for A, whether the player can ride, and in battle the active Pokémon, its moves, PP, types and the targets. Each of these is a fixed structure in `main` at known offsets, found once for build 1.3.2 by tracing on the Mac (a probe at the battle UI's own move-list draw, the field's interact prompt, etc.) and kept in the profile keyed by build ID, as with every per-build address. Until a value is found, its control falls back to the plain button.

## Exploration

- Virtual stick bottom-left, movable and resizable; free-follow or fixed (a setting).
- Right cluster: A largest, B beside it; context buttons appear only when relevant (Talk / Inspect / Ride / bike, Camp, Rotom, quick menu X) with icons drawn from the game's own UI textures where possible.
- Haptics on accepted interactions and when the stick reaches its edge.
- Double-tap the right side (or a small button) for the X menu. The game itself decides whether the world pauses; the port does not change game logic.

## Battle

- Moves as large cards in the lower half: name, type icon, PP, power / accuracy hint; they light up and scale on touch. A tap sends the game's own cursor moves + A, so the game's menu logic stays authoritative.
- Pokémon / Bag / Run as a secondary row that slides in when needed.
- Active Pokémon's HP / status / level in a top bar clear of the cut-out (Android display cut-out insets).
- Double-battle targeting: valid targets highlighted and tappable; non-targets dimmed by the overlay.
- Short haptic pulses on hits, status changes and faints (read from battle state).

## Menus

- System back gesture = B; large touch targets.
- Bag / Pokédex search and filters as an overlay that drives the game's own list cursor.

## Orientation

- Landscape: game full screen, controls over it.
- Portrait: the game's 16:9 picture at the top, the control deck (stick, buttons, battle cards) below it, for one-handed play. The game renders 16:9 either way; nothing is stretched.

## Platform pieces

- SDL3 for window, touch, controllers, audio (Link's Awakening Android port uses it too); controller in use hides the touch layer, a touch brings it back.
- Editable layout (drag, resize, hide, opacity), saved per orientation.
- Vulkan renderer with optional SGSR upscaling, power profiles (Android performance hint sessions), optional driver packages through libadrenotools.
- The game code is rewritten on the Mac and shipped in the personal APK (no on-device build needed: the APK is built from the player's own dump, as the IPAs are).
