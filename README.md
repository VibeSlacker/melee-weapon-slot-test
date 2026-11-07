# Dedicated Melee Slot

Carry your pistol AND a melee weapon at once in Helldivers 2 on HellForge. One download, two parts:

- **The slot** (C mod): puts a melee weapon in your secondary slot beside your pistol, and another in your primary slot beside your rifle. It works with no setup: **double tap 1** swaps the primary slot to its melee, **double tap 2** the secondary. A single press of the slot key always gives you the firearm back. Survives death and respawn. Does nothing until a melee is picked.
- **The pick box** (Lua mod, needs Bingus Shared Loader): a small box on the loadout card with two halves, one for the primary slot and one for the secondary: Machete, Stun Lance, Saber, Stun Baton, Combat Hatchet, Entrenchment Tool, or none.

It works for me, and I need to know where it breaks for you.

## Install (HellForge 0.3.0)

1. Close the game and any HellForge session. Mods can only be changed with no session running.
2. If you tried the earlier test zips (`Dedicated-Melee-Weapon-Slot-1.4.0` and `Melee-Picker-1.0.6`), remove both from the Mods page first. This one replaces them.
3. Copy these into `Documents/HellForge/mods`:
   - `Dedicated-Melee-Slot-1.6.2.zip`
   - Bingus Shared Loader v20 (or newer; v15 is the minimum) if you do not have it
   - Mod Bindings Menu, only if you want the optional extra swap keys (the built-in double tap needs nothing)
4. Open the Mods page and enable both. Put Bingus Shared Loader **last** in the order.
5. Start a session and open the player card (loadout screen). A box reading `MELEE: <name>` shows up with the other boxes.

## Use

1. On the loadout card, click the melee box and pick a weapon in each half you want: left half for the primary slot, right half for the secondary. (The box can be dragged; it remembers the spot.)
2. Drop into a mission. The weapons are loaded before the mission starts, so pick them **before** you drop.
3. Drop in and **double tap 1** (primary slot) or **double tap 2** (secondary slot): the slot's melee is in your hand in about half a second. A single press of the slot's own key always gives you the firearm back.
4. Optional: open the game's bindings page, find **MODS > DEDICATED MELEE SLOT**, and set **Melee 1** and **Melee 2** to any key and trigger you prefer (press, long press, double tap).

The mod follows the slot keys you set in the game's own bindings, and notices a change in the middle of a session.

## If you also run DiverKit

The melee box and its list sit under DiverKit's panel on the player card. The box can be dragged anywhere and remembers the spot, and while the mouse is on the box or the list is open the pick box holds DiverKit still so a click does not press a DiverKit button underneath (setting `hold_diverkit` in `MeleePicker.cfg`, 1 = on).

## Known limits

- Tested on Linux and Windows, HellForge 0.3.0.
- Co-op is untested.
- A swap can be ignored while carrying a heavy object (seen on earlier versions, not fixed).
- Keys the mod cannot press for you: right Ctrl, right Alt, the arrow keys, the numpad and mouse buttons. Use a plain keyboard key for the slot keys.
- With the melee in the secondary slot, picking a grenade shows the melee on the HUD (cosmetic).
- Built for game build 25480438 (the SDK manifest). After a game update the mod needs a matching release.
- Stick to the six weapons in the list. A wrong model package id is a fatal game error, so the mod only uses the game's own.

## Changes in 1.6.2

NEW
- A melee for the primary slot too, picked in the left half of the pick box (the right half is the secondary).
- Built-in default: double tap the game's primary slot key (1) or secondary slot key (2) to swap that slot to its melee. No setup needed, and it follows your own rebinds.
- Two optional keys on the game's bindings page, Melee 1 (primary) and Melee 2 (secondary), each with its own trigger: press, long press or double tap.
- The mod follows your own slot keys from the game's bindings, including changes made mid-session.

CHANGED
- A single press of a slot key is always the firearm; the swap is always the melee. The old "tap 2 again to toggle" is gone.
- The pick box is twice as wide: it spans the whole card, edge to edge with the two passive picker boxes.
- Pick box (Melee Picker) updated to 1.2.0.

## Reporting a bug

Please include all of these:

- Your HellForge version and OS.
- `MeleePicker.log` and `SimpleArmorPassivePicker.log` (if you use it) from the game's `Logs` folder (CowboyBingus, Helldivers2).
- The mod log for the slot: `mod-vibeslacker.dedicated-melee-slot.log` from your HellForge session's `game` folder.
- What you picked, what you expected, what happened, and whether the game crashed.

## Checksum (sha256)

```
f5a2dd45b01204a7d29cf13181c9b1dec2131ec7d68880191ec5c1f308f2af62  Dedicated-Melee-Slot-1.6.2.zip
```

## License

MIT License, copyright (c) 2026 vibeslacker; the full text is in [`LICENSE`](LICENSE). If you use this mod or its idea in your own work, please credit vibeslacker for the idea.

## Source

- `slot/`: the C mod source and the package manifest (built against HellForge's MIT-0 mod SDK).
- `picker/`: the Lua pick box source (version 1.2.0), its signature table and the build scripts. Its three patch files ship in the zip's `data/` folder.

Made by vibeslacker. Made with AI. Tip jar, if you like it: https://ko-fi.com/vibeslacker
