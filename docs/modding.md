# Modding

The game's models can be replaced with your own, in glTF (`.glb` or `.gltf`) from Blender,
Sketchfab or any other 3D tool. The project ships no mods and no game content: you make or
find your own. Only use models you have the right to use and share (see
[Where to find models](#where-to-find-models)).

## Quick start: replace Naughty Bear

Drop a `.glb` file into the `mods/` folder next to the `game` folder (beside `save/` and
`cache/`), on its own or in a folder of its own:

```
mods/
  peter.glb            <- a model on its own, or
  my_bear/
    bear.glb           <- a folder with a model
```

Start the game. A model without a `mod.toml` replaces Naughty Bear: it's turned to face the
way he does, scaled to his height and put where he stands, and it animates with him.

## The mods menu

When mods are installed, a menu opens before the game starts. It lists each mod with what it
replaces and its credits (the author and license, from `mod.toml` or from the model file,
where Sketchfab puts them). Tick or untick mods, then press **Play**. It works with the
mouse, the keyboard and a controller.

Unticking "Show this menu when the game starts" skips it from then on; start the game with
`--mods-menu` to see it again. `--no-mods-menu` skips it once. The choices are kept in
`mods/settings.toml`.

## How it works

Every mesh the game draws has a **mesh ID**, a hash of its vertex data and the part of it
that's drawn, so it's the same in every level and every run. Some meshes are known by name
(**targets**); a mod replaces targets or mesh IDs.

A replacement is drawn with the game's own shaders, so it gets the game's lighting and
effects. It doesn't need its own skeleton: each vertex of your model takes its bone weights
(and colours and tangents) from the nearest vertex of the game's mesh, so it animates with
the game's skeleton. That works best when the model is shaped and posed like the original:
similar proportions, arms near where the original's arms are, and so on. Positions, normals
and texture coordinates come from your model; a skinned model is used in its rest pose, as
model viewers show it.

The game mesh's textures are replaced slot by slot: its colours with your model's (all its
materials' base colour textures, combined), its normal map with a flat one, and effect masks
with black. Textures that don't depend on the model's layout (lighting ramps, fur noise)
stay.

### Targets

| Target | Replaces |
|-|-|
| `naughty_bear` | Naughty Bear in his default costume (mesh `a62ef2ebbde0f1ab`) |

Naughty's textures: 0 the lighting ramp, 1 the normal map, 2 a glow mask (the eyes), 3 fur
noise, 4 the colours, 5 effect masks, 6 a tinted copy of the colours (the fur's sheen). His
shader is made for his dark fur, so models' colours are scaled to 60% for him.

## Making a mod with mod.toml

For other meshes, or to control the fit, give the mod a folder with a `mod.toml`.

### 1. Find the mesh

Export the meshes drawn in a frame where the character is on screen:

```bash
NB_MOD_EXPORT=6600 ./build/game/NaughtyBear
```

Each mesh of that frame is written to `mods/_export/frame6600/<mesh ID>.glb`, with its
textures as `<mesh ID>_texture<slot>.png` and a list in `meshes.txt`: vertex and triangle
counts, whether it's skinned (animated by a skeleton), its size and its texture slots.
Characters are the skinned ones; open their `.glb` files to see which is which, and the
PNGs to see what each texture slot holds. Mods that are loaded are also written as the game
fitted them (`mods/_export/<mod>_on_<mesh ID>.glb`), to compare with the original.

These exports come from your copy of the game. Use them as references and don't
redistribute them.

### 2. Line your model up (optional)

With `fit = "bounds"` (the default for targets), the model is turned, scaled and placed
automatically. To place it yourself, import the exported mesh into Blender (File > Import >
glTF 2.0). It's in the game's bind pose: Naughty stands in a T-pose, Y up in glTF (so
upright in Blender), facing +Z. Import your model, then scale, rotate and pose it over the
reference until the two overlap, delete the reference, export your model (File > Export >
glTF 2.0, `.glb`) into your mod's folder, and use `fit = "none"`.

### 3. Write mod.toml

```toml
name = "My bear"
author = "Me"
license = "CC-BY-4.0"

[[mesh]]
target = "naughty_bear"
model = "bear.glb"
```

The log shows what each mod loaded and when a replacement is first drawn.

## mod.toml reference

| Key | Default | Meaning |
|-|-|-|
| `name`, `author`, `license` | folder name, none, none | Shown in the mods menu |
| `[[mesh]]` | | One entry per replacement; a mod can have any number |
| `target` | | A named mesh (see [Targets](#targets)); sets the texture slots, brightness and fit |
| `replace` | | Mesh ID(s) from `NB_MOD_EXPORT`, as a string or a list of strings (instead of `target`) |
| `model` | | `.glb`/`.gltf` file, relative to the mod's folder; all its meshes are drawn as one |
| `fit` | `"bounds"` with a target, else `"none"` | `"bounds"`: turn the model to face the way the game mesh does, scale it to its height and centre it on it |
| `textures` | from the target | Per texture slot: `"model"` (the model's colours), `"flat_normal"`, `"black"`, `"white"` or `"game"`; e.g. `textures = { 4 = "model", 1 = "flat_normal" }` |
| `texture_slot` | | Shorthand: the slot that gets the model's colours |
| `brightness` | `1.0` | Multiplies the model's colours (after the target's scaling) |
| `scale`, `rotate`, `offset` | `1`, `[0, 0, 0]`, `[0, 0, 0]` | Applied to the model in that order, before fitting; `rotate` is degrees about X, Y, then Z |
| `hide` | `false` | `true`: draw nothing for these meshes (no `model` needed) |

When two mods replace the same mesh, the one whose name (folder or file) sorts last wins.
Folders whose name starts with `_` or `.` aren't mods.

## Limits

- Only the models' base colour textures are used (PNG or JPEG, embedded or next to the
  file). Textures that repeat across a surface are wrapped, which can show seams.
- The skeleton is the game's: a model can't add bones or animations, and parts far from any
  original vertex follow whatever bone is nearest (hands held low on a model whose original
  holds its arms out may move with the hips).
- The first time a replacement is drawn, it's fitted to the game mesh, which can take a
  moment for large models.
- Only Naughty's default costume is a named target so far; other costumes and characters
  need their mesh IDs.
- Menus and HUD (drawn by Scaleform) can't be replaced this way.

## Where to find models

Use models you made, or ones whose license lets you change and share them. CC0 is the
easiest; CC-BY needs credit (the mods menu shows it); avoid "no derivatives" licenses. Good
sources include [Quaternius](https://quaternius.com), [Kenney](https://kenney.nl),
[Poly Pizza](https://poly.pizza), [Poly Haven](https://polyhaven.com),
[OpenGameArt](https://opengameart.org) and [Sketchfab](https://sketchfab.com) (filter by
downloadable and check each license). Don't use models taken from other games.
