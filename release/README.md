# csgo_gc

> [!WARNING]
> This project is incomplete and not ready for general use.

In Valve games, the Game Coordinator (GC) is a backend service most notably responsible for matchmaking and inventory management (loadouts, skins, cases, etc.). This project redirects GC traffic to a custom, in-process implementation.

This repository is based on the original `csgo_gc` project and includes additional work around prime toggling, competitive cooldown emulation, P2P party features, store handling, prestige coins, and various inventory fixes.

## Why would you want this?

While it is still possible to connect CS:GO to CS2's GC by spoofing the version number, this may break in the future if Valve updates the GC protocol. This project aims to restore most GC-related functionality without relying on a centralized server.

## Current features

- Editable inventory (`csgo_gc/inventory.txt`)
- Item equipping and loadouts
- Case opening, including:
  - weapon cases
  - sticker capsules
  - patch packs
  - graffiti boxes
  - music kit boxes
- Weapon StatTrak support
- Stickers and patches
- Graffiti support
- Name tags
- StatTrak swaps
- Storage Units (caskets)
- Prestige coins
- Majors & operations passes (`csgo_gc/passes.txt`)
- Prime toggle (`has_prime` in `csgo_gc/config.txt`)
- Level / XP / rank configuration
- Prime-aware level and rank display
- Competitive cooldown emulation (`competitive_cooldown_seconds`)
- Recent teammates & Party search (configurable via the `friends` block)
- In-game store (`csgo_gc/price_sheet.txt`)
- Steam P2P party support:
  - invites
  - join relay
  - lobby updates
  - heartbeats
  - host migration
  - party chat
  - profile queries
- Global chat stub
- Functional server browser (can show only `csgo_gc` servers)
- Dedicated server support
- Automatic reload of watched files after startup:
  - `csgo_gc/config.txt`
  - `csgo_gc/inventory.txt`
  - `csgo_gc/price_sheet.txt`
  - `csgo_gc/passes.txt`
  - `csgo_gc/unusual_loot_lists.txt`
- Windows, Linux, and macOS build support via CMake
- Works without full Steam API emulation

## Planned features

- Trade ups
- Souvenirs
- Remaining item actions
- Graphical inventory editor
- A tool to copy your CS2 inventory over

## Not planned

- Real matchmaking (can't be implemented without a centralized server)
- Real VAC / secure mode emulation
- Real SDR tickets signed by Valve

## Installation

1. Download [CS:GO from Steam](steam://install/4465480).
2. Download the latest release for your platform from the [Releases](../../releases) page.
3. Navigate to the game's installation directory.
4. Back up your existing launcher executables, as they will be overwritten:
   - `csgo.exe`
   - `srcds.exe`
5. Extract the contents of the downloaded archive into your game directory.
6. Replace the executables when prompted.
7. Launch the game.

If you get the annoying VAC message box, launch the game with the `-steam` argument.

## Configuration

The main configuration file is:

```text
csgo_gc/config.txt
```

Example:

```text
"config"
{
    "appid_override" "4465480"
    "show_csgo_gc_servers_only" "1"

    "ranks"
    {
        "competitive_rank" "0"
        "competitive_wins" "0"
        "wingman_rank" "0"
        "wingman_wins" "0"
        "dangerzone_rank" "0"
        "dangerzone_wins" "0"
    }

    "force_max_rarity" "0"
    "destroy_used_items" "1"
    "randomize_item_float" "1"

    "rarity_weights"
    {
        "1" "10000000"
        "2" "2000000"
        "3" "400000"
        "4" "80000"
        "5" "16000"
        "6" "3200"
        "99" "1280"
    }

    "friends"
    {
        "1140104601" "1"
    }

    "vac_banned" "0"
    "has_prime" "1"
    "competitive_cooldown_seconds" "0"
    "cmd_friendly" "0"
    "cmd_teaching" "0"
    "cmd_leader" "0"
    "player_level" "0"
    "player_cur_xp" "0"
    "country" "RU"
    "currency" "3"
}
```

### Important options

| Key | Description |
| --- | --- |
| `appid_override` | App ID used by the GC. Default is `4465480`. Use `730` for original behavior. |
| `show_csgo_gc_servers_only` | Adds the `csgo_gc` tag filter to the server browser. |
| `force_max_rarity` | If enabled, case opening always rolls the highest rarity available and loot lists are filtered accordingly. |
| `destroy_used_items` | Destroys used cases, keys, name tags, stickers, etc. after use. |
| `randomize_item_float` | Randomizes wear float when creating painted items. |
| `rarity_weights` | Weights used for case-opening rarity rolls. Rarity values: `1` common, `2` uncommon, `3` rare, `4` mythical, `5` legendary, `6` ancient, `99` unusual. |
| `friends` | Account IDs used for Party search, recent teammates, and co-play stubs. |
| `has_prime` | Enables or disables Prime status. Affects level/rank display and store handling. |
| `competitive_cooldown_seconds` | Emulated competitive cooldown. `0` disables it. |
| `player_level` / `player_cur_xp` | Player level and current XP. XP is normalized into levels on load. |
| `country` / `currency` | Country code and currency sent to the client. |

## Inventory editing

Use [csgo_gc_inventory-editor](https://github.com/dricotec/csgo_gc_inventory-editor/releases/latest).

The inventory is stored in:

```text
csgo_gc/inventory.txt
```

## Building

Requirements:

- Git
- CMake 3.20 or newer
- C++ compiler with C++17 support:
  - VS 2017 or later
  - Clang 5 or later
  - GCC 7 or later

The game is 32-bit on Windows, so you need to build as 32-bit.

### Windows

```bat
cmake -A Win32 -B build
cmake --build build --config Release
```

### Linux / macOS

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## License

This project is licensed under the 2-Clause BSD License. See [LICENSE.md](LICENSE.md) for details.

## Credits

- **Mikko Kokko** — Author
- **Theeto** — Code reused from the predecessor project, unusual loot lists
- **Shashlik226** — For making most of the features work (passes, StatTrak swaps, etc.)
- **sasha190409** — Prime toggle fork and additional fixes/features

## Third party dependencies

- [Crypto++](https://github.com/weidai11/cryptopp) — [Boost Software License](https://github.com/weidai11/cryptopp/blob/master/License.txt)
- [funchook](https://github.com/kubo/funchook) — [GPL v2 with Classpath Exception](https://github.com/kubo/funchook/blob/master/LICENSE)
- [diStorm3](https://github.com/gdabah/distorm) — [3-Clause BSD License](https://github.com/gdabah/distorm/blob/master/COPYING)
- [protobuf](https://github.com/protocolbuffers/protobuf) — [3-Clause BSD License](https://github.com/protocolbuffers/protobuf/blob/main/LICENSE)


<<<<<<< HEAD >>>>>>> 832b225 (added cooldown reasons, made config.txt russian and added config_en.txt)
