/* Dedicated Melee Slot 1.6.2 (vibeslacker.dedicated-melee-slot). 1.6.2: a double tap of the game's own primary/secondary slot key (1 and 2 by default, following rebinds) is the built-in melee gesture; a single tap stays the firearm. The two optional bindings-page keys still work. Swaps stop on the other weapon slot, never on the grenade (a grenade selected while fire is held is thrown). 1.6.0: a melee for the PRIMARY slot too (melee-slot-primary.txt, picked in the Melee Picker's left half; the secondary pick stays melee-slot-weapon.txt), and two keys on the game's bindings page (Mod Bindings Menu, registered by the Melee Picker, which signals this mod through named events): "Melee 1 (primary slot)" and "Melee 2 (secondary slot)". The player sets each key and its trigger (press, long press, double tap...) in the game's own picker. A plain press of a slot key is ALWAYS the firearm (the slot's own weapon comes back after 250 ms), a double tap / long press / bindings key is ALWAYS the melee (never a toggle; with the melee already in the field it only draws it); gestures are debounced 350 ms per slot. Swaps hold on the grenade slot (no wait there) and follow the player's own slot keys: the primary, secondary and grenade keys are read from the game's input_settings.config (checked every 2 s, parsed only when it changes; default 1, 2, 4), and a Hold/LongPress key is held long enough. The old tap-again-2 toggle is off. 1.5.2: Entrenchment Tool stats popup (picker). 1.5.1: adds the CQC-72 Entrenchment Tool (shovel). 1.5.0: renamed, shipped as one package with the picker (no behaviour change). 1.4.0: config files are read from Documents\\HellForge AND the game's Logs folder (newer wins), so the sandboxed HellForge 0.3.0 works. 1.1.0 to 1.3.0: pick the melee by name (machete, stun-lance, saber, stun-baton,
 * hatchet) in melee-slot-weapon.txt; a melee's built-in package set is used unless melee-slot-package.txt overrides it; melee-slot-probe.txt
 * turns on the package-list dump used to find the sets of the melees that have none yet. Carry a pistol AND a melee weapon: tapping the secondary key (2) while the
 * secondary is already drawn toggles slot 2 between the real secondary and the melee weapon, in about half a second.
 *
 * How it works (game.dll build 25480438):
 *  - Right after the game spawns the local Helldiver's secondary, the mod spawns ONE extra weapon (the melee, from melee-slot-weapon.txt) from a
 *    persistent copy of the game's own spawn data, attaches it to the diver as the loadout builder does (RVA 0x8BF870) and requests the model
 *    packages it needs (melee-slot-package.txt, the game's OWN package ids only: a guessed id is a fatal BUNDLE error).
 *  - A tap of 2 with the secondary drawn: the key thread sends a real key press of 1 (the game undoes direct SelectSlot calls), waits for the
 *    holster, writes the other weapon into the inventory record's secondary field ([game+0x3326738] record +4), then presses 2.
 *  - After a death the new diver's inventory is empty: the copy is dropped and a fresh one is spawned with the new secondary.
 * No melee configured (missing/zero melee-slot-weapon.txt): the mod does nothing.
 * Lineage: built from the Weapon Logger 0.1 to 0.9.5 test series (kept in ~/hd2-weapon-logger). */
#include <windows.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "hellforge_mod.h"

#define INV_GLOBAL 0x3326738u
#define ENT_STATE_GLOBAL 0x3326DC0u
#define ENTITIES_GLOBAL 0x346BF98u
#define SPAWN_REQ_RVA 0xFD9710u
#define ATTACH_RVA 0x8BF870u
#define PKG_REQUEST_RVA 0x12FE350u
#define PKG_SYSTEM_GLOBAL 0x347CEB0u
#define SWAP_WAIT_MS 2500
#define HOLSTER_MS 0   /* wait on the hold slot, before the write */
#define HOLD_ON_GRENADE 0 /* swaps stop on the OTHER weapon slot, never on the grenade: with fire held in combat a selected grenade gets thrown (Wilson, 2026-11-06). 1 = old grenade stop-over (unsafe). */
#define HOLD_SEL 4    /* the grenade slot (selection value 4, key 4 by default): the hold stop of every swap */
#define DRAW_MS 0      /* wait after the write, before the key press of 2 */

/* One slot the mod can put a melee in: index 0 = primary (inventory field +0), 1 = secondary (field +4). */
typedef struct {
    volatile uint32_t clone, original;        /* entity ids of the melee copy and of the slot's own weapon */
    volatile uint64_t copyRes, origRes;       /* resource ids of the melee and of the slot's own weapon */
    volatile LONG probeDone, regDone, rearm, pkgDone;
    volatile ULONGLONG tick;
    const char* file;                         /* the pick file */
    const char* name; const char* packages;   /* the pick as last read */
    int field;
} Slot;
static Slot g_slot[2] = { { .file = "melee-slot-primary.txt", .name = "", .packages = "", .field = 0 },
                          { .file = "melee-slot-weapon.txt",  .name = "", .packages = "", .field = 4 } };
static volatile uint32_t g_avatar;
static volatile LONG g_missionOn, g_missionSeq, g_regOk;
static volatile ULONGLONG g_missionTick;
static volatile LONG g_tapFlag, g_swapStage, g_inject, g_retries, g_swapSlot, g_waitVk;
static volatile LONG g_keyFlag[2];
static HANDLE g_keyEvent[2];   /* named events the Melee Picker sets when a bindings-page key is pressed (0 = primary, 1 = secondary) */
static volatile ULONGLONG g_swapDeadline, g_swapAt, g_lastPress, g_swapStarted;
static volatile uint32_t g_swapTarget, g_heldSel;
static volatile LONG g_holdScan, g_holdFallback;   /* g_holdScan: the SELECTION VALUE whose key is the hold stop */
/* The game's own slot keys, by selection value (1 primary, 2 secondary, 4 grenade): scan code, virtual key and how long a press is held. Defaults 1, 2, 4;
 * LoadSlotKeys replaces them from the player's input_settings.config. */
static volatile LONG g_keyScan[5] = { 0, 2, 3, 0, 5 }, g_keyVk[5] = { 0, '1', '2', 0, '4' }, g_keyMs[5] = { 0, 25, 25, 0, 25 };
static volatile LONG g_injectMs = 25;
static void Inject(int sel) { g_injectMs = g_keyMs[sel]; InterlockedExchange(&g_inject, g_keyScan[sel]); }
/* single tap back (1.8.0): a plain tap of a slot key, with that slot holding its melee, brings the slot's own weapon back after TAP_BACK_MS
 * unless a gesture (double tap, long press, the bindings key) arrives. Gestures are also debounced per slot. */
#define TAP_BACK_MS 250
#define TAP_AGAIN 0   /* the old 'tap 2 again toggles the secondary melee': off since 1.9.0, a plain press is always the firearm */
#define GESTURE_DEBOUNCE_MS 350
#define DOUBLE_TAP_MS 350   /* a second press of the same slot key within this window is the built-in melee gesture */
static volatile ULONGLONG g_pendingAt[2], g_gestureUntil[2], g_lastGesture[2], g_quietUntil, g_lastDown[2];
static HfMod* g_mod;
static const HfApi* g_hf;

typedef uint64_t (*SpawnReqFn)(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f);
typedef void (*AttachFn)(uint64_t table, uint64_t entity, uint64_t owner, uint64_t flag, uint64_t e, uint64_t f);
typedef void (*PkgReqFn)(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f);
static SpawnReqFn g_spawnReq;
static PkgReqFn g_pkgReq;

/* ---- helpers ------------------------------------------------------------------------------------------------------- */

static int Ptr(uintptr_t at, uintptr_t* out)
{
    uint64_t v;
    if (g_hf->read(at, &v, 8) != 0 || v < 0x10000 || (v >> 32) > 0x7FFF) return 0;
    *out = (uintptr_t)v;
    return 1;
}

/* The game's u32 hash map (header {u64 buckets, u32 capacity, u32 empty key, u32 multiplier}, 8-byte buckets {key, value}). */
static int MapGet(uintptr_t header, uint32_t key, uint32_t* value)
{
    uint8_t h[20];
    if (key == 0 || g_hf->read(header, h, sizeof h) != 0) return 0;
    uint64_t buckets; uint32_t capacity, empty, multiplier;
    memcpy(&buckets, h, 8); memcpy(&capacity, h + 8, 4); memcpy(&empty, h + 12, 4); memcpy(&multiplier, h + 16, 4);
    if (buckets < 0x10000 || capacity == 0 || capacity > 1048576) return 0;
    uint32_t start = key * multiplier, probes = capacity < 4096 ? capacity : 4096;
    for (uint32_t i = 0; i < probes; i++)
    {
        uint32_t b[2];
        if (g_hf->read((uintptr_t)buckets + (uintptr_t)((start + i) % capacity) * 8, b, 8) != 0) return 0;
        if (b[0] == key) { if (b[1] == 0xFFFFFFFFu) return 0; *value = b[1]; return 1; }
        if (b[0] == empty) return 0;
    }
    return 0;
}

/* The avatar's 0x30-byte inventory record: +0 primary, +4 secondary, +0x1C selection. */
static int InvRecord(uintptr_t game, uint32_t avatar, uintptr_t* inv, uintptr_t* record)
{
    uintptr_t records;
    uint32_t index;
    if (!Ptr(game + INV_GLOBAL, inv) || !MapGet(*inv + 0x28, avatar, &index) || index >= 0x10000 || !Ptr(*inv + 0x50, &records)) return 0;
    *record = records + (uintptr_t)index * 0x30;
    return 1;
}

/* One candidate folder for a config file. ok = the file opened; *when = its last-write time. */
static int OpenConfig(const char* dir, const char* name, HANDLE* out, FILETIME* when)
{
    char path[MAX_PATH + 96];
    path[0] = 0;
    if (dir[0] == 'U') {   /* %USERPROFILE%\Documents\HellForge */
        DWORD n = GetEnvironmentVariableA("USERPROFILE", path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return 0;
        strcat(path, "\\Documents\\HellForge\\");
    } else {               /* %LOCALAPPDATA%\CowboyBingus\Helldivers2\Logs: the one folder a sandboxed (HellForge 0.3.0) game can write */
        DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return 0;
        strcat(path, "\\CowboyBingus\\Helldivers2\\Logs\\");
    }
    strcat(path, name);
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    memset(when, 0, sizeof *when);
    GetFileTime(h, NULL, NULL, when);
    *out = h;
    return 1;
}

/* A text file, from %USERPROFILE%\Documents\HellForge or from the game's own Logs folder (where the Melee Picker writes when the sandbox blocks
 * Documents); when both exist the newer one wins. 1 when read. */
static int ReadConfig(const char* name, char* text, DWORD size)
{
    HANDLE a = INVALID_HANDLE_VALUE, b = INVALID_HANDLE_VALUE, h;
    FILETIME ta, tb;
    int okA = OpenConfig("U", name, &a, &ta), okB = OpenConfig("L", name, &b, &tb);
    if (!okA && !okB) return 0;
    if (okA && okB) {
        if (CompareFileTime(&tb, &ta) > 0) { CloseHandle(a); h = b; } else { CloseHandle(b); h = a; }
    } else h = okA ? a : b;
    DWORD got = 0;
    memset(text, 0, size);
    ReadFile(h, text, size - 1, &got, NULL);
    CloseHandle(h);
    return 1;
}

/* The secondary melee weapons. A package set is the game's OWN model packages the weapon needs (a weapon without them draws as a purple question
 * mark). Found 2026-10-29/30 by diffing the engine's resident-package list between runs; every set shown here was seen as a real model in game
 * (not yet reduced to the minimum: a set may hold ids the weapon does not need). */
typedef struct { const char* name; const char* alias; uint64_t resource; const char* packages; } MeleeInfo;
static const MeleeInfo kMelee[] = {
    { "machete",    "cqc-42", 0x792D5D2A340FD6E6ull, "8f295b4d04c3a06c 1538ce9a1eaee5d8 15f53d03eba8ed2f 4436a04638261bc4 9b6d3eb4c698079b "
                                                       "b727a8ed2075eeb7 b7567b8c3013c576 db719d81aa6e13f7 f5014993c8402d87" },
    { "stun-lance", "cqc-19", 0xE3B6AEDD07FCB464ull, "1f251a64eba0f770 56aa04adbbeea69f 5811f7986ed3c168 61cfc905a70886ab 69a0588d8634765b "
                                                       "75a938c3382765a4 961b0e91ff7f7ac1 c496e7b7fe1988aa" },
    { "saber",      "cqc-2",  0xFCD8A6E67EAC635Aull, "3cfc68e6bf19f67d 519980a75ebd50ad 732c5526b8d0d87e 9c1a4f2727e37e43 a5b0f1d352402864 "
                                                       "bbde37ac8e06e209 f4dc2361985c3026" },
    { "stun-baton", "cqc-30", 0x52CDBFBACA3CB397ull, "f3c65eaab75f1fc2 11b6940091b3ee80 3b9e4d171fbb6160 405b1a417f98ac5e 4d455c1bb18c8564 "
                                                       "b4910c2012022e5c" },
    { "hatchet",    "cqc-5",  0x75816077C139C850ull, "1982853d15d996da 055f887c3bc616fe 17c3fae85b906d79 2be8dbbfc211e97f 473b8f1e3b759384 "
                                                       "681275af9e93cb2c 69a0588d8634765b 93b8833ac9229a55 a68c0263a22a26dd ac1950d1411393f2 "
                                                       "b6e4fb7620b72aee c98cdbe584ec4dfa" },
    /* UNTESTED set: only the shovel's own loadout package (it holds the unit, bones, state machine, animations and audio bank). Shared
     * extras the other melees carry may be needed; if it draws as a purple question mark, run a probe mission and diff as for the others. */
    { "shovel",     "cqc-72", 0xE85E623F93F96FB3ull, "49de16e1343250e4" },
};

/* The melee in a slot's pick file (melee-slot-primary.txt / melee-slot-weapon.txt): a name from kMelee (machete, stun-lance, saber, stun-baton, hatchet, shovel; spaces and underscores count as
 * hyphens) or 16 hex digits (optional 0x) of a resource id. 0 when missing or unreadable. */
static uint64_t ReadMeleeResource(Slot* sl)
{
    char text[64], word[32];
    size_t n = 0;
    sl->name = ""; sl->packages = "";
    if (!ReadConfig(sl->file, text, sizeof text)) return 0;
    const char* t = text;
    while (*t == ' ' || *t == '\t' || *t == '\r' || *t == '\n') t++;
    for (; *t && *t != '\r' && *t != '\n' && n + 1 < sizeof word; t++, n++)
        word[n] = (*t == ' ' || *t == '_') ? '-' : (*t >= 'A' && *t <= 'Z') ? (char)(*t + 32) : *t;
    while (n && word[n - 1] == '-') n--;
    word[n] = 0;
    for (size_t i = 0; i < sizeof kMelee / sizeof kMelee[0]; i++)
        if (strcmp(word, kMelee[i].name) == 0 || strcmp(word, kMelee[i].alias) == 0)
        { sl->name = kMelee[i].name; sl->packages = kMelee[i].packages; return kMelee[i].resource; }
    t = word;
    if (t[0] == '0' && t[1] == 'x') t += 2;
    char* end = NULL;
    uint64_t v = strtoull(t, &end, 16);
    return end && end != t ? v : 0;
}

/* Test aid: with melee-slot-probe.txt present, log every package in the engine's list (resident first, then still loading; 12 ids a line) at
 * the extra weapon's spawn, up to 6 times a session. Diffing these lists between runs that carry different melees finds each melee's packages. */
#define PKG_EXE_ENGINE 0x1A10208u
static volatile LONG g_dumps;
static void DumpResident(const char* why)
{
    uintptr_t exe = (uintptr_t)GetModuleHandleW(NULL), engine, manager, resources, list;
    uint32_t count = 0;
    char probe[8];
    if (g_dumps >= 6 || !ReadConfig("melee-slot-probe.txt", probe, sizeof probe)) return;
    InterlockedIncrement(&g_dumps);
    if (!Ptr(exe + PKG_EXE_ENGINE, &engine) || !Ptr(engine + 0x400, &manager) || !Ptr(manager + 0x208, &resources) ||
        g_hf->read(resources + 0x80, &count, 4) != 0 || count > 8192 || !Ptr(resources + 0x88, &list))
    {
        g_hf->log(g_mod, "PKGRES %s: engine list unreadable", why);
        return;
    }
    for (int pass = 0; pass < 2; pass++)
    {
        char line[12 * 18 + 1];
        int inLine = 0, total = 0;
        line[0] = 0;
        for (uint32_t i = 0; i < count; i++)
        {
            uintptr_t package, partList;
            uint64_t pid = 0;
            uint32_t n = 0, loaded = 0;
            if (!Ptr(list + (uintptr_t)i * 8, &package) || g_hf->read(package + 0x10, &pid, 8) != 0) continue;
            if (g_hf->read(package + 0x18, &n, 4) != 0 || n > 64) n = 0;
            if (n && Ptr(package + 0x20, &partList))
                for (uint32_t k = 0; k < n; k++)
                {
                    uintptr_t part;
                    uint32_t st = 0;
                    if (Ptr(partList + (uintptr_t)k * 8, &part) && g_hf->read(part + 0x1C, &st, 4) == 0 && st == 4) loaded++;
                }
            if ((loaded == n && n) != (pass == 0)) continue;
            size_t len = strlen(line);
            snprintf(line + len, sizeof line - len, "%016llx ", (unsigned long long)pid);
            total++;
            if (++inLine == 12) { g_hf->log(g_mod, "PKGRES %s %s: %s", why, pass == 0 ? "resident" : "loading", line); inLine = 0; line[0] = 0; }
        }
        if (inLine) g_hf->log(g_mod, "PKGRES %s %s: %s", why, pass == 0 ? "resident" : "loading", line);
        g_hf->log(g_mod, "PKGRES %s %s: %d package(s) of %u in the engine list", why, pass == 0 ? "resident" : "loading", total, count);
    }
}

/* ---- events -------------------------------------------------------------------------------------------------------- */

static void Weapon(HfMod* mod, const HfEventInfo* info, void* user)
{
    const HfWeaponEvent* w = (const HfWeaponEvent*)info;
    if (w->avatar != 0) g_avatar = w->avatar;
}

static void Mission(HfMod* mod, const HfEventInfo* info, void* user)
{
    if (info->kind == HF_ON_MISSION_START)
    {
        g_missionTick = GetTickCount64(); g_missionSeq = 0;
        for (int i = 0; i < 2; i++) { g_slot[i].probeDone = 0; g_slot[i].rearm = 0; }
        g_missionOn = 1;
        HF_API(mod)->log(mod, "mission start");
    }
    else { g_missionOn = 0; HF_API(mod)->log(mod, "mission end"); }
}

static void Player(HfMod* mod, const HfEventInfo* info, void* user)
{
    const HfPlayerEvent* p = (const HfPlayerEvent*)info;
    if (p->local && info->kind == HF_ON_PLAYER_SPAWN) g_avatar = p->entity;
}

/* A dead diver leaves an emptied inventory record: drop the copies and re-arm for the new diver's weapons. */
static DWORD WINAPI InvThread(LPVOID unused)
{
    for (;;)
    {
        Sleep(100);
        uint32_t avatar = g_avatar;
        if (avatar == 0) continue;
        uintptr_t game = g_hf->module_base("game.dll", NULL), inv, rec;
        uint32_t w[2];
        if (!game || !InvRecord(game, avatar, &inv, &rec) || g_hf->read(rec, w, sizeof w) != 0) continue;
        if (w[0] == 0 && w[1] == 0 && g_missionOn && (g_slot[0].clone || g_slot[1].clone))
        {
            for (int i = 0; i < 2; i++)
                if (g_slot[i].clone) { g_slot[i].clone = 0; g_slot[i].original = 0; g_slot[i].regDone = 1; g_slot[i].pkgDone = 0; g_slot[i].rearm = 1; }
            g_hf->log(g_mod, "respawn: the diver died, dropping the melee copies; fresh ones come with the next spawn");
        }
    }
    return 0;
}

/* ---- spawn the melee beside the primary / the secondary ------------------------------------------------------------- */

static uint64_t SpawnReqDetour(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f)
{
    uint64_t resource = b; /* req(uint32_t* out, uint64_t resource, SpawnData* d) */
    LONG seq = g_missionOn ? InterlockedIncrement(&g_missionSeq) : 0;
    int idx = -1, rearm = 0;
    for (int i = 0; i < 2 && idx < 0; i++)
        if (g_slot[i].rearm && g_missionOn && g_slot[i].origRes && resource == g_slot[i].origRes) { idx = i; rearm = 1; }
    if (idx < 0 && g_missionOn && GetTickCount64() - g_missionTick < 2000)
    {   /* the 1st spawn request of a mission is taken to be the primary and the 2nd the secondary (the primary one is checked at the attach) */
        if (seq == 1 && !g_slot[0].probeDone) idx = 0;
        else if (seq == 2 && !g_slot[1].probeDone) idx = 1;
    }
    int probe = idx >= 0 && c > 0x10000 && a > 0x10000;
    uint8_t before[0x58];
    if (probe && g_hf->read((uintptr_t)c, before, sizeof before) != 0) probe = 0;
    uint64_t r = g_spawnReq(a, b, c, d, e, f);
    if (!probe) return r;

    Slot* sl = &g_slot[idx];
    sl->probeDone = 1;
    sl->origRes = resource;
    if (rearm) sl->rearm = 0;
    uint64_t melee = ReadMeleeResource(sl);
    if (melee == 0)
    {
        if (idx == 1) g_hf->log(g_mod, "no melee set in melee-slot-weapon.txt; the secondary slot stays idle");
        return r;
    }
    uint32_t original = 0, copy = 0;
    g_hf->read((uintptr_t)a, &original, 4);
    /* The game reads far more than 0x58 bytes from the spawn data and builds it in a stack frame that is gone after the call, so the extra
     * spawn uses a PERSISTENT copy of up to 0x2000 bytes from the spawn data pointer, with the context pointer moved into the copy. */
    uint8_t* region = (uint8_t*)VirtualAlloc(NULL, 0x8000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    uint8_t* data = region ? region + 0x1000 : NULL;
    uint64_t ctxPtr = 0;
    size_t span = 0, ctxSize = 0;
    memcpy(&ctxPtr, before + 0x48, 8);
    if (region)
    {
        for (size_t n = 0x2000; n >= 0x100 && span == 0; n -= 0x100)
            if (g_hf->read((uintptr_t)c, data, n) == 0) span = n;
        if (span)
        {
            uint64_t mine;
            if (ctxPtr >= c && ctxPtr < c + span) mine = (uint64_t)(uintptr_t)data + (ctxPtr - c);
            else
            {
                uint8_t* spare = region + 0x5000;
                size_t got = 0;
                for (size_t n = 0x600; n >= 0x100 && got == 0; n -= 0x100) if (g_hf->read((uintptr_t)ctxPtr, spare, n) == 0) got = n;
                mine = got ? (uint64_t)(uintptr_t)spare : ctxPtr;
            }
            memcpy(data + 0x48, &mine, 8);
            ctxSize = span;
        }
    }
    g_spawnReq((uint64_t)(uintptr_t)&copy, melee, ctxSize ? (uint64_t)(uintptr_t)data : c, d, e, f);
    sl->clone = copy; sl->original = original; sl->tick = GetTickCount64(); sl->regDone = 0;
    sl->copyRes = melee;
    g_hf->log(g_mod, "%s %s: melee %s %016llx spawned as entity %u beside the %s weapon %016llx (entity %u)%s", idx == 0 ? "primary" : "secondary",
        rearm ? "respawn" : "spawn", *sl->name ? sl->name : "(by id)", (unsigned long long)melee, copy, idx == 0 ? "primary" : "secondary",
        (unsigned long long)resource, original, ctxSize ? "" : " (WARNING: game's own spawn context, not copied)");
    DumpResident(rearm ? "at the respawn copy" : "at the copy spawn");
    return r;
}

/* ---- attach the melee to the diver ------------------------------------------------------------------------------------ */

static int UnitOfEntity(uintptr_t game, uint32_t entity, uint64_t* resource, uint32_t* unit)
{
    uintptr_t em;
    uint32_t index;
    uint8_t d[24];
    if (!entity || !Ptr(game + ENTITIES_GLOBAL, &em) || !MapGet(em + 0xF1AEB0, entity, &index) || index >= 0x100000 ||
        g_hf->read(em + 0xF32F18 + (uintptr_t)index * 24, d, sizeof d) != 0) return 0;
    uint32_t named;
    memcpy(&named, d + 8, 4);
    if (named != entity) return 0;
    memcpy(resource, d, 8); memcpy(unit, d + 12, 4);
    return 1;
}

/* The word 0x8BF870 writes at its end: object(game+0x3326468) + index * 0x38 + 0x2E8 (the renderer reads it; the call must not change it). */
static int LocalPlayerWord(uintptr_t game, uintptr_t* where)
{
    uintptr_t obj, ids;
    uint32_t zero = 0, id, index;
    if (!Ptr(game + 0x3326468u, &obj) || g_hf->read(obj + 0x88, &zero, 4) != 0 || zero == 0) return 0;
    if (!Ptr(obj + 0xE8, &ids) || g_hf->read(ids + 8, &id, 4) != 0) return 0;
    if (!MapGet(obj + 0xD0, id, &index) || index >= 0x10000) return 0;
    *where = obj + (uintptr_t)index * 0x38 + 0x2E8;
    return 1;
}

static void AttachSlot(HfMod* mod, int si)
{
    const HfApi* hf = HF_API(mod);
    Slot* sl = &g_slot[si];
    uint32_t copy = sl->clone, avatar = g_avatar, owner = 0, state = 2;
    if (sl->regDone || !copy || !avatar || !g_missionOn || !g_regOk || GetTickCount64() < sl->tick + 1500) return;
    uintptr_t game = hf->module_base("game.dll", NULL), tbl, recs, inv, irec;
    uint32_t index;
    if (!game || !Ptr(game + ENT_STATE_GLOBAL, &tbl) || !MapGet(tbl + 0x20, copy, &index) || index >= 0x100000 || !Ptr(tbl + 0x40, &recs)) return;
    /* the builder has finished with the diver: the original weapon's record names the avatar as its owner */
    uint32_t orig = sl->original, oindex;
    if (!MapGet(tbl + 0x20, orig, &oindex) || oindex >= 0x100000 || hf->read(recs + (uintptr_t)oindex * 0x30 + 4, &owner, 4) != 0 || owner != avatar)
        return; /* try again next frame */
    if (!InvRecord(game, avatar, &inv, &irec)) return;
    if (si == 0)
    {   /* the primary copy rests on a guess (1st spawn request = primary): the inventory's primary field must name that very weapon */
        uint32_t field = 0;
        if (hf->read(irec, &field, 4) != 0 || field == 0) return;
        if (field != orig)
        {
            sl->regDone = 1;
            hf->log(mod, "primary attach skipped: the 1st spawned weapon (entity %u) is not the primary (field holds %u)", orig, field);
            return;
        }
    }
    sl->regDone = 1;
    uint64_t have = 0;
    uint32_t unit = 0;
    if (!UnitOfEntity(game, copy, &have, &unit) || have != sl->copyRes || !unit)
    {
        hf->log(mod, "attach skipped: entity %u is not the melee (wanted %016llx, found %016llx): the spawn produced no such weapon", copy,
            (unsigned long long)sl->copyRes, (unsigned long long)have);
        return;
    }
    uintptr_t word = 0;
    uint32_t wordBefore = 0, wordAfter = 0;
    int haveWord = LocalPlayerWord(game, &word) && hf->read(word, &wordBefore, 4) == 0;
    ((AttachFn)(game + ATTACH_RVA))((uint64_t)tbl, copy, avatar, 0, 0, 0);
    if (haveWord && hf->read(word, &wordAfter, 4) == 0 && wordAfter != wordBefore) hf->write(word, &wordBefore, 4);
    hf->write(recs + (uintptr_t)index * 0x30, &state, 4);
    hf->log(mod, "%s melee entity %u attached to the diver%s", si == 0 ? "primary" : "secondary", copy, haveWord ? "" : " (player word not found)");
}

/* One slot: request every package id of its melee (the built-in set; for the secondary melee-slot-package.txt overrides it) once the diver exists. */
static void RequestSlotPackages(HfMod* mod, int si)
{
    static char requestedFor[2][40];   /* the melee name (or "(file)") whose set was last requested, per slot */
    Slot* sl = &g_slot[si];
    char text[2048], key[40];
    /* The melee files are polled twice a second, so a pick made on the loadout screen (Melee Picker) gets its set requested before the mission;
     * a respawn (pkgDone cleared) requests the same set again. */
    int fromFile = si == 1 && ReadConfig("melee-slot-package.txt", text, sizeof text);
    ReadMeleeResource(sl);
    snprintf(key, sizeof key, "%s", fromFile ? "(file)" : sl->name);
    if (sl->pkgDone && strcmp(key, requestedFor[si]) == 0) return;
    sl->pkgDone = 1;
    snprintf(requestedFor[si], sizeof requestedFor[si], "%s", key);
    if (!fromFile)
    {
        if (!*sl->packages)
        {
            if (*sl->name) HF_API(mod)->log(mod, "packages: no model package set known for %s yet; it may draw as a purple question mark", sl->name);
            return;
        }
        snprintf(text, sizeof text, "%s", sl->packages);
    }
    uintptr_t game = HF_API(mod)->module_base("game.dll", NULL), system;
    if (!game || !Ptr(game + PKG_SYSTEM_GLOBAL, &system)) { HF_API(mod)->log(mod, "packages: the package system is not readable"); return; }
    int requested = 0;
    for (char* p = text; *p && requested < 40;)
    {
        while (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t') p++;
        if (!*p) break;
        char* end = p;
        uint64_t id = strtoull(p, &end, 16);
        if (end == p || end - p != 16) { HF_API(mod)->log(mod, "packages: skipped a token that is not 16 hex digits"); while (*p && *p != ' ' && *p != '\n' && *p != '\r') p++; continue; }
        p = end;
        g_pkgReq((uint64_t)system, (uint64_t)(uintptr_t)&id, 1, 0, 0, 0);
        requested++;
    }
    HF_API(mod)->log(mod, "packages: %d requested for the %s melee %s", requested, si == 0 ? "primary" : "secondary", key[0] ? key : "(by id)");
}

static void RequestPackages(HfMod* mod)
{
    static ULONGLONG nextCheck;
    ULONGLONG now = GetTickCount64();
    if (!g_avatar || !g_pkgReq || now < nextCheck) return;
    nextCheck = now + 500;
    RequestSlotPackages(mod, 0);
    RequestSlotPackages(mod, 1);
}

/* ---- the toggle ------------------------------------------------------------------------------------------------------ */

/* Swap one slot between its own weapon and its melee. A tap of 2 with the secondary drawn (hotkey 0, raised by the key thread) or a key of the
 * bindings page (hotkey 1, with any slot drawn) starts it. The slot being changed must not be drawn while its field is written, so the OTHER
 * slot's key is pressed first (secondary: key 1; primary: key 2), the field is written, and the slot's own key draws it. */
static void DoSwap(HfMod* mod, int si, int hotkey)
{
    const HfApi* hf = HF_API(mod);
    Slot* sl = &g_slot[si];
    uintptr_t game = hf->module_base("game.dll", NULL), inv, rec;
    uint32_t avatar = g_avatar, clone = sl->clone, orig = sl->original, cur, selection;
    if (!g_missionOn || !game || !avatar || !clone || !orig || g_swapStage) return;
    if (!InvRecord(game, avatar, &inv, &rec) || hf->read(rec + sl->field, &cur, 4) != 0 || hf->read(rec + 0x1C, &selection, 4) != 0) return;
    uint32_t holdSel = si == 1 ? 1 : 2, drawSel = si == 1 ? 2 : 1;
    if (!hotkey && selection != drawSel) return;
    if (hotkey == 2 && cur != clone) return;   /* tap back: only when the melee is what the field holds */
    if (cur != clone && cur != orig) { hf->log(mod, "swap ignored: the %s field holds %u, neither the weapon %u nor the melee %u", si == 0 ? "primary" : "secondary", cur, orig, clone); return; }
    /* a gesture (hotkey 1) always means the melee, a plain tap back (hotkey 2) always the slot's own weapon; only the old tap-again (hotkey 0) toggles */
    if (hotkey == 1 && cur == clone)   /* the melee is already in the field: just draw it */
    {
        g_waitVk = g_keyVk[si == 1 ? 2 : 1];
        if (selection != drawSel) Inject(drawSel);
        return;
    }
    g_swapSlot = si;
    g_swapTarget = hotkey == 1 ? clone : hotkey == 2 ? orig : (cur == clone ? orig : clone);
    g_waitVk = g_keyVk[si == 1 ? 2 : 1];   /* the player's own finger on the slot key leaves it before any key press is sent */
    g_swapStarted = GetTickCount64();
    g_swapDeadline = g_swapStarted + SWAP_WAIT_MS + 1500;
    g_retries = 0; g_lastPress = g_swapStarted; g_swapStage = 1;
    g_holdFallback = 0; g_heldSel = 0; g_holdScan = HOLD_ON_GRENADE ? HOLD_SEL : holdSel;
    if (selection == drawSel) Inject(g_holdScan);
}

/* The later steps of the swap, on the game-thread tick. Stages fall through when their wait is already over:
 * 1 wait for the hold key's press to read as the hold slot being selected (re-sent after 600 ms, twice at most); 3 write the slot's field;
 * 2 key press of the slot's own key; 4 wait for the draw to show. */
static void SwapStages(HfMod* mod)
{
    const HfApi* hf = HF_API(mod);
    ULONGLONG now = GetTickCount64();
    if (!g_swapStage) return;
    int si = g_swapSlot;
    Slot* sl = &g_slot[si];
    uint32_t holdSel = si == 1 ? 1 : 2, drawSel = si == 1 ? 2 : 1;
    uintptr_t game = hf->module_base("game.dll", NULL), inv, rec;
    uint32_t avatar = g_avatar, selection = 0;
    if (!game || !avatar || !InvRecord(game, avatar, &inv, &rec) || hf->read(rec + 0x1C, &selection, 4) != 0) { g_swapStage = 0; return; }
    if (g_swapStage == 1)
    {
        if (selection == drawSel)
        {
            if (now - g_lastPress > 600 && g_retries < 2) { g_retries++; g_lastPress = now; Inject(g_holdScan); return; }
            if (HOLD_ON_GRENADE && now - g_lastPress > 600 && !g_holdFallback)
            {
                g_holdFallback = 1; g_retries = 0; g_lastPress = now; g_holdScan = holdSel;
                hf->log(mod, "the grenade slot never showed as selected (selection %u); holding on the other weapon slot instead", selection);
                Inject(g_holdScan); return;
            }
            if (now > g_swapDeadline) { hf->log(mod, "swap aborted: selection still %u after the hold key press and %d retries; nothing written", selection, (int)g_retries); g_swapStage = 0; }
            return;
        }
        g_heldSel = selection; g_swapAt = now + HOLSTER_MS; g_swapStage = 3;
    }
    if (g_swapStage == 3)
    {
        if (now < g_swapAt) return;
        if (selection == drawSel)   /* the player's own key press put the slot back while the hold was up: hold again, three times at most */
        {
            if (g_retries < 3 && now < g_swapDeadline + 1500) { g_retries++; g_lastPress = now; g_swapStage = 1; Inject(g_holdScan); return; }
            hf->log(mod, "swap aborted: selection reads %u after the holster; nothing written", selection); g_swapStage = 0; g_quietUntil = now + 400; return;
        }
        uint32_t target = g_swapTarget;
        if (hf->write(rec + sl->field, &target, 4) != 0) { hf->log(mod, "swap aborted: could not write the %s field", si == 0 ? "primary" : "secondary"); g_swapStage = 0; return; }
        g_swapAt = now + DRAW_MS; g_swapStage = 2;
    }
    if (g_swapStage == 2)
    {
        if (now < g_swapAt) return;
        Inject(drawSel);
        g_swapDeadline = now + SWAP_WAIT_MS; g_swapStage = 4;
        return;
    }
    if (selection == drawSel || now > g_swapDeadline)
    {
        uint32_t after = 0;
        hf->read(rec + sl->field, &after, 4);
        hf->log(mod, "%s swap to %s: %llu ms (held on selection %u)%s%s", si == 0 ? "primary" : "secondary", after == sl->clone ? "melee" : "original weapon",
            (unsigned long long)(now - g_swapStarted), (unsigned)g_heldSel, g_retries ? " (key press retried)" : "", selection == drawSel ? "" : " (the draw did not show in time)");
        g_swapStage = 0; g_quietUntil = now + 400;
    }
}

static void Tick(HfMod* mod, float dt, void* user)
{
    AttachSlot(mod, 0);
    AttachSlot(mod, 1);
    RequestPackages(mod);
    if (InterlockedExchange(&g_tapFlag, 0)) DoSwap(mod, 1, 0);
    ULONGLONG now = GetTickCount64();
    for (int i = 0; i < 2; i++)
    {
        if (InterlockedExchange(&g_keyFlag[i], 0))
        {
            if (now - g_lastGesture[i] >= GESTURE_DEBOUNCE_MS) { g_lastGesture[i] = now; DoSwap(mod, i, 1); }
        }
        else if (g_pendingAt[i] && now >= g_pendingAt[i])
        {
            g_pendingAt[i] = 0;
            if (!g_swapStage) DoSwap(mod, i, 2);
        }
    }
    SwapStages(mod);
}


/* ---- the game's own slot keys ------------------------------------------------------------------------------------------ */

/* The player may move the game's primary, secondary and grenade keys. The game saves only what was CHANGED to input_settings.config, an action
 * missing from it keeps its default. The file is checked every two seconds (one stat call) and parsed only when it changed. */
typedef struct { const char* name; WORD scan; } KeyName;
static const KeyName kKeyNames[] = {
    {"1",2},{"2",3},{"3",4},{"4",5},{"5",6},{"6",7},{"7",8},{"8",9},{"9",10},{"0",11},
    {"q",0x10},{"w",0x11},{"e",0x12},{"r",0x13},{"t",0x14},{"y",0x15},{"u",0x16},{"i",0x17},{"o",0x18},{"p",0x19},
    {"a",0x1E},{"s",0x1F},{"d",0x20},{"f",0x21},{"g",0x22},{"h",0x23},{"j",0x24},{"k",0x25},{"l",0x26},
    {"z",0x2C},{"x",0x2D},{"c",0x2E},{"v",0x2F},{"b",0x30},{"n",0x31},{"m",0x32},
    {"space",0x39},{"tab",0x0F},{"enter",0x1C},{"backspace",0x0E},{"caps lock",0x3A},{"backtick",0x29},{"minus",0x0C},{"equals",0x0D},
    {"left ctrl",0x1D},{"left shift",0x2A},{"left alt",0x38},{"right shift",0x36},
    {"f1",0x3B},{"f2",0x3C},{"f3",0x3D},{"f4",0x3E},{"f5",0x3F},{"f6",0x40},{"f7",0x41},{"f8",0x42},{"f9",0x43},{"f10",0x44},{"f11",0x57},{"f12",0x58},
};

/* The text of one field ( key = "value" or key = value ) inside [s, e). 1 when found. */
static int CfgField(const char* s, const char* e, const char* key, char* out, int size)
{
    size_t kl = strlen(key);
    for (const char* p = s; p + kl < e; p++)
    {
        if (memcmp(p, key, kl) != 0 || (p > s && p[-1] != ' ' && p[-1] != '\t' && p[-1] != '\n' && p[-1] != '\r' && p[-1] != '{')) continue;
        const char* q = p + kl;
        while (q < e && (*q == ' ' || *q == '\t')) q++;
        if (q >= e || *q != '=') continue;
        q++;
        while (q < e && (*q == ' ' || *q == '\t')) q++;
        int n = 0;
        if (q < e && *q == '"') { q++; while (q < e && *q != '"' && n < size - 1) out[n++] = *q++; }
        else while (q < e && *q != ' ' && *q != '\t' && *q != '\n' && *q != '\r' && *q != '}' && n < size - 1) out[n++] = *q++;
        out[n] = 0;
        return 1;
    }
    return 0;
}

/* The keyboard binding of one action: 1 when the action is in the file with a keyboard key this mod can press. *known = the action is in the file at all. */
static int CfgActionKey(const char* text, const char* action, WORD* scan, LONG* ms, char* keyName, char* trig, int* known)
{
    size_t al = strlen(action);
    for (const char* p = strstr(text, action); p; p = strstr(p + 1, action))
    {
        if (p > text && p[-1] != '\n' && p[-1] != '\t' && p[-1] != ' ') continue;
        const char* q = p + al;
        while (*q == ' ' || *q == '\t') q++;
        if (*q != '=') continue;
        const char* end = strchr(q, ']');
        if (!end) return 0;
        *known = 1;
        for (const char* b = strchr(q, '{'); b && b < end; b = strchr(b + 1, '{'))
        {
            const char* c = strchr(b, '}');
            if (!c || c > end) break;
            char device[32], input[32], trigger[32], threshold[32];
            if (!CfgField(b, c, "device_type", device, sizeof device) || strcmp(device, "Keyboard") != 0) continue;
            if (!CfgField(b, c, "input", input, sizeof input)) continue;
            if (!CfgField(b, c, "trigger", trigger, sizeof trigger)) strcpy(trigger, "Press");
            if (!CfgField(b, c, "threshold", threshold, sizeof threshold)) strcpy(threshold, "0");
            for (size_t k = 0; k < sizeof kKeyNames / sizeof kKeyNames[0]; k++)
                if (strcmp(input, kKeyNames[k].name) == 0)
                {
                    *scan = kKeyNames[k].scan;
                    double t = atof(threshold);
                    *ms = (strcmp(trigger, "LongPress") == 0 || strcmp(trigger, "Hold") == 0) ? (LONG)(t * 1000.0 + 100.0) : 25;
                    if (*ms > 1500) *ms = 1500;
                    if (*ms < 25) *ms = 25;
                    snprintf(keyName, 32, "%s", input); snprintf(trig, 32, "%s", trigger);
                    return 1;
                }
            snprintf(keyName, 32, "%s", input);   /* a keyboard key with no scan code in the table */
            return 0;
        }
        return 0;
    }
    return 0;
}

static void LoadSlotKeys(void)
{
    static FILETIME lastWhen; static char lastPath[MAX_PATH + 64]; static int announced;
    char base[MAX_PATH + 64], path[MAX_PATH + 160], best[MAX_PATH + 160] = {0};
    DWORD n = GetEnvironmentVariableA("USERPROFILE", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    strcat(base, "\\Documents\\HellForge\\userdata\\profiles\\");
    snprintf(path, sizeof path, "%s*", base);
    WIN32_FIND_DATAA fd; FILETIME bestWhen = {0, 0};
    HANDLE f = FindFirstFileA(path, &fd);
    if (f != INVALID_HANDLE_VALUE)
    {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
            WIN32_FILE_ATTRIBUTE_DATA info;
            snprintf(path, sizeof path, "%s%s\\cloud\\input_settings.config", base, fd.cFileName);
            if (GetFileAttributesExA(path, GetFileExInfoStandard, &info) && CompareFileTime(&info.ftLastWriteTime, &bestWhen) > 0)
            { bestWhen = info.ftLastWriteTime; snprintf(best, sizeof best, "%s", path); }
        } while (FindNextFileA(f, &fd));
        FindClose(f);
    }
    if (!best[0])
    {
        if (!announced) { announced = 1; g_hf->log(g_mod, "slot keys: no input_settings.config found; the default keys 1, 2 and 4 are used"); }
        return;
    }
    if (strcmp(best, lastPath) == 0 && CompareFileTime(&bestWhen, &lastWhen) == 0) return;
    snprintf(lastPath, sizeof lastPath, "%s", best); lastWhen = bestWhen;
    static char text[65536];
    HANDLE h = CreateFileA(best, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { lastWhen.dwLowDateTime = 0; lastWhen.dwHighDateTime = 0; return; }   /* try again in two seconds */
    DWORD got = 0;
    memset(text, 0, sizeof text);
    ReadFile(h, text, sizeof text - 1, &got, NULL);
    CloseHandle(h);
    static const char* actions[5] = { 0, "ChangeEquipmentPrimary", "ChangeEquipmentSecondary", 0, "ChangeEquipmentGrenade" };
    static const WORD defScan[5] = { 0, 2, 3, 0, 5 };
    static const LONG defVk[5] = { 0, '1', '2', 0, '4' };
    static const char* label[5] = { 0, "primary", "secondary", 0, "grenade" };
    for (int sel = 1; sel <= 4; sel += sel == 2 ? 2 : 1)
    {
        WORD scan = defScan[sel]; LONG ms = 25; char name[32] = "", trig[32] = ""; int known = 0;
        int ok = CfgActionKey(text, actions[sel], &scan, &ms, name, trig, &known);
        if (!ok) { scan = defScan[sel]; ms = 25; }
        UINT vk = MapVirtualKeyA(scan, MAPVK_VSC_TO_VK);
        LONG newVk = ok ? (LONG)(vk ? vk : defVk[sel]) : defVk[sel];
        if (g_keyScan[sel] != scan || g_keyVk[sel] != newVk || g_keyMs[sel] != ms || !announced)
            g_hf->log(g_mod, "slot keys: %s = %s%s%s (scan %u, held %ld ms)%s", label[sel], ok ? name : "the default",
                ok ? " " : "", ok ? trig : "", (unsigned)scan, (long)ms,
                known && !ok ? (name[0] ? "; that key is not in this mod's key table, the default is used" : "; no keyboard key set for it, the default is used") : "");
        g_keyScan[sel] = scan; g_keyVk[sel] = newVk; g_keyMs[sel] = ms;
    }
    announced = 1;
}

/* Sends the key presses of the swap (the game's own slot keys, 1, 2 and 4 by default), watches for a tap of 2, and listens for the Melee Picker's key events. */
static DWORD WINAPI KeyThread(LPVOID unused)
{
    int was2 = 0, prev1 = 0, prev2 = 0;
    ULONGLONG nextKeys = 0;
    for (;;)
    {
        Sleep(4);
        { ULONGLONG t = GetTickCount64(); if (t >= nextKeys) { nextKeys = t + 2000; LoadSlotKeys(); } }
        for (int i = 0; i < 2; i++)
            if (g_keyEvent[i] && WaitForSingleObject(g_keyEvent[i], 0) == WAIT_OBJECT_0)
            {
                InterlockedExchange(&g_keyFlag[i], 1);
                g_pendingAt[i] = 0; g_gestureUntil[i] = GetTickCount64() + 700;   /* a gesture cancels the tap back, and so does the release that ends it */
            }
        LONG scan = InterlockedExchange(&g_inject, 0);
        if (scan)
        {
            INPUT in = {0};
            int vk = g_waitVk;
            if (vk)   /* let the player's own finger leave the slot key first */
                for (int w = 0; w < 100 && (GetAsyncKeyState(vk) & 0x8000); w++) Sleep(4);
            in.type = INPUT_KEYBOARD; in.ki.wScan = (WORD)scan; in.ki.dwFlags = KEYEVENTF_SCANCODE;
            SendInput(1, &in, sizeof in);
            Sleep((DWORD)g_injectMs);
            in.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
            SendInput(1, &in, sizeof in);
        }
        /* At the key-down edge of 2 the game has not reacted yet, so a selection of 2 means the secondary was ALREADY drawn: a tap-again. */
        int down2 = (GetAsyncKeyState(g_keyVk[2]) & 0x8000) != 0;
        if (TAP_AGAIN && down2 && !was2 && g_missionOn && g_avatar && g_slot[1].clone)
        {
            uintptr_t game = g_hf->module_base("game.dll", NULL), inv, rec;
            uint32_t selection = 0;
            if (game && InvRecord(game, g_avatar, &inv, &rec) && g_hf->read(rec + 0x1C, &selection, 4) == 0 && selection == 2)
                { InterlockedExchange(&g_tapFlag, 1); g_pendingAt[1] = 0; g_gestureUntil[1] = GetTickCount64() + 700; }   /* the tap-again toggle is this tap's answer */
        }
        was2 = down2;
        /* a plain tap of 1 or 2: the release arms the tap back, the next press (a double tap in the making) cancels it */
        int down1 = (GetAsyncKeyState(g_keyVk[1]) & 0x8000) != 0;
        for (int i = 0; i < 2; i++)
        {
            int dn = i == 0 ? down1 : down2, was = i == 0 ? prev1 : prev2;
            ULONGLONG now = GetTickCount64();
            if (dn && !was)
            {
                g_pendingAt[i] = 0;
                /* default melee gesture: a quick second press of the slot key itself (no bindings-page key needed) */
                if (g_swapStage || now < g_quietUntil) g_lastDown[i] = 0;   /* the mod's own injected presses during a swap are not taps */
                else if (g_lastDown[i] && now - g_lastDown[i] <= DOUBLE_TAP_MS)
                {
                    g_lastDown[i] = 0;
                    g_gestureUntil[i] = now + 700;
                    InterlockedExchange(&g_keyFlag[i], 1);
                }
                else g_lastDown[i] = now;
            }
            if (!dn && was && g_missionOn && g_avatar && g_slot[i].clone && !g_swapStage && now >= g_quietUntil && now >= g_gestureUntil[i])
            {
                uintptr_t game = g_hf->module_base("game.dll", NULL), inv, rec;
                uint32_t cur = 0;
                if (game && InvRecord(game, g_avatar, &inv, &rec) && g_hf->read(rec + g_slot[i].field, &cur, 4) == 0 && cur == g_slot[i].clone)
                    g_pendingAt[i] = now + TAP_BACK_MS;
            }
        }
        prev1 = down1; prev2 = down2;
    }
    return 0;
}

/* ---- load -------------------------------------------------------------------------------------------------------------- */

static void GameLoaded(HfMod* mod, HfEvent event, void* user)
{
    const HfApi* hf = HF_API(mod);
    uintptr_t game = hf->module_base("game.dll", NULL);
    if (!game) { hf->log(mod, "game.dll not found; the mod stays idle"); return; }
    /* the first bytes of each function, checked against the reviewed build before anything is hooked or called */
    if (!hf->code_matches(game + SPAWN_REQ_RVA, "4c 8b dc 49 89 5b 10 49 89 73 20 57 48 81 ec a0 00 00 00"))
    { hf->log(mod, "spawn function 0xFD9710 is not the reviewed one; the mod stays idle"); return; }
    int rc = hf->hook(mod, game + SPAWN_REQ_RVA, (void*)SpawnReqDetour, (void**)&g_spawnReq);
    hf->log(mod, "spawn hook: %s", rc == 0 ? "installed" : "FAILED");
    if (hf->code_matches(game + PKG_REQUEST_RVA, "45 85 c0 0f 84 75 01 00 00 41 56 41 57 48 83 ec 38")) g_pkgReq = (PkgReqFn)(game + PKG_REQUEST_RVA);
    else hf->log(mod, "package function 0x12FE350 is not the reviewed one; no packages will be requested");
    g_regOk = hf->code_matches(game + ATTACH_RVA, "4c 8b dc 55 53 41 56 49 8d 6b a8 48 81 ec 40 01 00 00");
    if (!g_regOk) hf->log(mod, "attach function 0x8BF870 is not the reviewed one; the melee cannot be attached");
}

HF_MOD_EXPORT int hellforge_mod_init(const HfApi* hf, HfMod* mod)
{
    if (hf->version < 11) return 1; /* HF_ON_WEAPON is SDK 11 */
    int ok = hf->subscribe(mod, HF_ON_WEAPON, Weapon, NULL, 0);
    hf->subscribe(mod, HF_ON_MISSION_START, Mission, NULL, 0);
    hf->subscribe(mod, HF_ON_MISSION_END, Mission, NULL, 0);
    hf->subscribe(mod, HF_ON_PLAYER_SPAWN, Player, NULL, 0);
    g_mod = mod; g_hf = hf;
    g_keyEvent[0] = CreateEventA(NULL, FALSE, FALSE, "Local\\VibeSlackerDedicatedMeleeSlotKey1");
    g_keyEvent[1] = CreateEventA(NULL, FALSE, FALSE, "Local\\VibeSlackerDedicatedMeleeSlotKey2");
    hf->on(mod, HF_EVENT_GAME_LOADED, GameLoaded, NULL);
    HANDLE t = CreateThread(NULL, 0, InvThread, NULL, 0, NULL);
    if (t) CloseHandle(t);
    HANDLE k = CreateThread(NULL, 0, KeyThread, NULL, 0, NULL);
    if (k) CloseHandle(k);
    int tick = hf->size >= offsetof(HfApi, on_game_thread) + sizeof hf->on_game_thread ? hf->on_game_thread(mod, Tick, NULL) : 0;
    hf->log(mod, "Dedicated Melee Slot 1.6.2 loaded (weapon event %s, inventory thread %s, key thread %s, game tick %s, bindings-key events %s)",
        ok ? "subscribed" : "REFUSED", t ? "started" : "FAILED", k ? "started" : "FAILED", tick ? "hooked" : "NOT hooked",
        g_keyEvent[0] && g_keyEvent[1] ? "ready" : "FAILED");
    return 0;
}
