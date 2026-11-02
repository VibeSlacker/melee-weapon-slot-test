# Dedicated Melee Slot

Carry your pistol AND a melee weapon at once in Helldivers 2 on HellForge. One download, two parts:

- **The slot** (C mod): spawns the melee weapon next to your pistol. Tap **2** while the secondary is drawn and it toggles between pistol and melee in about half a second. Survives death and respawn. Does nothing until a melee is picked.
- **The pick box** (Lua mod, needs Bingus Shared Loader): a small box on the loadout card where you pick the melee: Machete, Stun Lance, Saber, Stun Baton, Combat Hatchet, or none.

It works for me, and I need to know where it breaks for you.

## Install (HellForge 0.3.0)

1. Close the game and any HellForge session. Mods can only be changed with no session running.
2. If you tried the earlier test zips (`Dedicated-Melee-Weapon-Slot-1.4.0` and `Melee-Picker-1.0.6`), remove both from the Mods page first. This one replaces them.
3. Copy these into `Documents/HellForge/mods`:
   - `Dedicated-Melee-Slot-1.5.0.zip`
   - Bingus Shared Loader v20 (or newer; v15 is the minimum) if you do not have it
4. Open the Mods page and enable both. Put Bingus Shared Loader **last** in the order.
5. Start a session and open the player card (loadout screen). A box reading `MELEE: <name>` shows up with the other boxes.

## Use

1. On the loadout card, click the melee box and pick a weapon. (The box can be dragged; it remembers the spot.)
2. Drop into a mission. The weapon is loaded before the mission starts, so pick it **before** you drop.
3. Press **2** to draw your secondary, then **2** again: the melee is in your hand. Press 2 again to go back.

## If you also run DiverKit

The melee box and its list sit under DiverKit's panel on the player card. The box can be dragged anywhere and remembers the spot, and while the mouse is on the box or the list is open the pick box holds DiverKit still so a click does not press a DiverKit button underneath (setting `hold_diverkit` in `MeleePicker.cfg`, 1 = on).

## Known limits

- Tested on Linux, HellForge 0.3.0 (and 0.2.0). Windows is untested.
- Co-op is untested.
- Tap 2 can be ignored while carrying a heavy object (known, not fixed).
- With the melee in the secondary slot, picking a grenade shows the melee on the HUD (cosmetic).
- Built for game build 25480438 (the SDK manifest). After a game update the mod needs a matching release.
- Stick to the five weapons in the list. A wrong model package id is a fatal game error, so the mod only uses the game's own.

## Reporting a bug

Please include all of these:

- Your HellForge version and OS.
- `MeleePicker.log` and `SimpleArmorPassivePicker.log` (if you use it) from the game's `Logs` folder (CowboyBingus, Helldivers2).
- The mod log for the slot: `mod-vibeslacker.dedicated-melee-slot.log` from your HellForge session's `game` folder.
- What you picked, what you expected, what happened, and whether the game crashed.

## Checksum (sha256)

```
f42cd156d835e2aaa7dee16c163b6b5be25b4e3cd134efab3ef3eebde629d2f9  Dedicated-Melee-Slot-1.5.0.zip
```

## Source

- `slot/`: the C mod source and the package manifest (built against HellForge's MIT-0 mod SDK).
- `picker/`: the Lua pick box source (version 1.0.9), its signature table and the build scripts. Its three patch files ship in the zip's `data/` folder.

Made by vibeslacker. Made with AI. Tip jar, if you like it: https://ko-fi.com/vibeslacker
