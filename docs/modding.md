# Modding

The game's models can be replaced with your own, in glTF (`.glb` or `.gltf`) from Blender,
Sketchfab or any other 3D tool. The project ships no mods and no game content: you make or
find your own. Only use models you have the right to use and share (see
[Where to find models](#where-to-find-models)).

## Quick start: replace a character

Drop a `.glb` file into the `mods/` folder next to the `game` folder (beside `save/` and
`cache/`), on its own or in a folder of its own:

```
mods/
  peter.glb            <- a model on its own, or
  my_bear/
    bear.glb           <- a folder with a model
```

Start the game and pick, in the mods menu, which character it replaces (Naughty Bear unless
another mod already has him). The model is turned to face the way the character does,
scaled to its height and put where it stands, and it animates with it.

## The mods menu

When mods are installed, a menu opens before the game starts. Each mod has a **Replaces**
drop-down: Off, or one of the game's characters. Each character can have only one mod;
choosing a character another mod has turns that mod off. The menu also shows each mod's
credits (the author and license, from `mod.toml` or from the model file, where Sketchfab
puts them). Press **Play** to start. It works with the mouse, the keyboard and a controller.

Unticking "Show this menu when the game starts" skips it from then on; start the game with
`--mods-menu` to see it again. `--no-mods-menu` skips it once, and `NB_NO_MODS=1` starts the
game without any mods. The choices are kept in `mods/settings.toml`.

### Characters

| In the menu | Data package |
|-|-|
| Naughty Bear | `naughtybear` |
| Naughty Bear (alien, army, cop, danger, ninja, pirate, robot, SWAT, unibear, vampire, zombie costume) | `naughtyalien`, `naughtyarmy`, ... `naughtyzombie` |
| Island bears | `normalbear` |
| Alien, army, cop, danger, ninja, pirate, robot, SWAT, vampire, zombie bears; Unibear | `alienbear`, `armybear`, ... `zombiebear`, `unibear` |
| Impersonator (alien ... zombie) | `impersonatoralien`, ... |
| Multiplayer bears (Naughty, red, blue, green, yellow, zombie) | `mp_naughtybear`, `mp_redbear`, ... |

The costume-select screen shows pictures of the costumes, not models, so it doesn't change.

## How it works

The game keeps each character in a data package (`lu/<name>.lu`). The runtime follows the
packages as the game reads, unpacks and copies them into memory, so it knows which package
every mesh came from; a character's meshes are the skinned (animated) meshes from its
package. Every mesh also has a **mesh ID**, a hash of its vertex data and the part of it
that's drawn, the same in every level and every run; mods can replace single meshes by ID.

A replacement is drawn with the game's own shaders, so it gets the game's lighting and
effects. It doesn't need its own skeleton: each vertex of your model takes its bone weights
(and colours and tangents) from the nearest vertex of the game's mesh, so it animates with
the game's skeleton. That works best when the model is shaped and posed like the original:
similar proportions, arms near where the original's arms are, and so on. Positions, normals
and texture coordinates come from your model; a skinned model is used in its rest pose, as
model viewers show it. The fitting is done in the background the first time the mesh is
drawn (the original shows until it's ready, a fraction of a second).

The game mesh's textures are replaced slot by slot, judged from what each looks like: normal
maps (bluish) get a flat one, masks (mostly black: glows, effects) get black, the largest
other textures (the colours) get your model's colours (all its materials' base colour
textures, combined), and the rest (lighting ramps, fur noise) stay. Your model's colours are
scaled so its bright parts are as bright as the character's own: the game's shaders are made
for their textures. The log shows what was chosen, e.g.
`textures 1:f 2:b 4:m 5:b 6:m; brightness 0.63` (flat normal, black, model).

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

With `fit = "bounds"` (the default for characters), the model is turned, scaled and placed
automatically. To place it yourself, import the exported mesh into Blender (File > Import >
glTF 2.0). It's in the game's bind pose: Naughty stands in a T-pose, Y up in glTF (so
upright in Blender), facing +Z. Import your model, then scale, rotate and pose it over the
reference until the two overlap, delete the reference, export your model (File > Export >
glTF 2.0, `.glb`) into your mod's folder, and use `fit = "none"`.

### 3. Write mod.toml

A model for a character:

```toml
name = "My bear"
author = "Me"
license = "CC-BY-4.0"
model = "bear.glb"
character = "ninjabear"   # the menu's default for it
```

Or models for single meshes, by ID:

```toml
name = "My props"

[[mesh]]
replace = "0123456789abcdef"
model = "chair.glb"

[[mesh]]
replace = "fedcba9876543210"
hide = true
```

The log shows what each mod loaded and when a replacement is first drawn.

## mod.toml reference

| Key | Default | Meaning |
|-|-|-|
| `name`, `author`, `license` | folder name, none, none | Shown in the mods menu |
| `model` | | `.glb`/`.gltf` file, relative to the mod's folder; all its meshes are drawn as one (a character mod) |
| `character` | `"naughtybear"` | The character it replaces unless the menu says otherwise (a data package from [Characters](#characters)) |
| `[[mesh]]` | | One entry per mesh replacement (a mesh mod), with `replace` and the keys below |
| `replace` | | Mesh ID(s) from `NB_MOD_EXPORT`, as a string or a list of strings |
| `hide` | `false` | `true`: draw nothing for these meshes (no `model` needed) |
| `fit` | `"bounds"` for characters, `"none"` for meshes | `"bounds"`: turn the model to face the way the game mesh does, scale it to its height and centre it on it |
| `textures` | all `"auto"` | Per texture slot: `"auto"`, `"model"` (the model's colours), `"flat_normal"`, `"black"`, `"white"` or `"game"`; e.g. `textures = { 4 = "model", 1 = "flat_normal" }` |
| `texture_slot` | | Shorthand: the slot that gets the model's colours |
| `brightness` | `1.0` | Multiplies the model's colours, after they're matched to the game's |
| `scale`, `rotate`, `offset` | `1`, `[0, 0, 0]`, `[0, 0, 0]` | Applied to the model in that order, before fitting; `rotate` is degrees about X, Y, then Z |

When two mesh mods replace the same mesh, the one whose name (folder or file) sorts last
wins. Folders whose name starts with `_` or `.` aren't mods.

## Limits

- Only the models' base colour textures are used (PNG or JPEG, embedded or next to the
  file). Textures that repeat across a surface are wrapped, which can show seams.
- The skeleton is the game's: a model can't add bones or animations, and parts far from any
  original vertex follow whatever bone is nearest (hands held low on a model whose original
  holds its arms out may move with the hips).
- Character names come from the data packages; which bears each package holds is the
  game's business (the island bears, for example, may share one package).
- Menus and HUD (drawn by Scaleform) can't be replaced this way.

## Where to find models

Use models you made, or ones whose license lets you change and share them. CC0 is the
easiest; CC-BY needs credit (the mods menu shows it); avoid "no derivatives" licenses. Good
sources include [Quaternius](https://quaternius.com), [Kenney](https://kenney.nl),
[Poly Pizza](https://poly.pizza), [Poly Haven](https://polyhaven.com),
[OpenGameArt](https://opengameart.org) and [Sketchfab](https://sketchfab.com) (filter by
downloadable and check each license). Don't use models taken from other games.
