/* Dedicated Melee Slot 1.5.0 (vibeslacker.dedicated-melee-slot). 1.5.0: renamed, shipped as one package with the picker (no behaviour change). 1.4.0: config files are read from Documents\\HellForge AND the game's Logs folder (newer wins), so the sandboxed HellForge 0.3.0 works. 1.1.0 to 1.3.0: pick the melee by name (machete, stun-lance, saber, stun-baton,
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
#define HOLSTER_MS 0   /* wait after the primary shows, before the write */
#define DRAW_MS 0      /* wait after the write, before the key press of 2 */

static volatile uint32_t g_avatar, g_probeEntity, g_probeOriginal;
static volatile LONG g_missionOn, g_missionSeq, g_probeDone, g_regDone, g_regOk, g_rearm, g_pkgDone;
static volatile ULONGLONG g_missionTick, g_probeTick;
static volatile LONG g_tapFlag, g_swapStage, g_inject, g_retries;
static volatile ULONGLONG g_swapDeadline, g_swapAt, g_lastPress, g_swapStarted;
static volatile uint32_t g_swapTarget;
static volatile uint64_t g_copyResource, g_secondaryRes;
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
};
static const char* g_meleeName = "";
static const char* g_meleePackages = "";

/* The melee in melee-slot-weapon.txt: a name from kMelee (machete, stun-lance, saber, stun-baton, hatchet; spaces and underscores count as
 * hyphens) or 16 hex digits (optional 0x) of a resource id. 0 when missing or unreadable. */
static uint64_t ReadMeleeResource(void)
{
    char text[64], word[32];
    size_t n = 0;
    g_meleeName = ""; g_meleePackages = "";
    if (!ReadConfig("melee-slot-weapon.txt", text, sizeof text)) return 0;
    const char* t = text;
    while (*t == ' ' || *t == '\t' || *t == '\r' || *t == '\n') t++;
    for (; *t && *t != '\r' && *t != '\n' && n + 1 < sizeof word; t++, n++)
        word[n] = (*t == ' ' || *t == '_') ? '-' : (*t >= 'A' && *t <= 'Z') ? (char)(*t + 32) : *t;
    while (n && word[n - 1] == '-') n--;
    word[n] = 0;
    for (size_t i = 0; i < sizeof kMelee / sizeof kMelee[0]; i++)
        if (strcmp(word, kMelee[i].name) == 0 || strcmp(word, kMelee[i].alias) == 0)
        { g_meleeName = kMelee[i].name; g_meleePackages = kMelee[i].packages; return kMelee[i].resource; }
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
        g_missionTick = GetTickCount64(); g_missionSeq = 0; g_probeDone = 0; g_rearm = 0; g_missionOn = 1;
        HF_API(mod)->log(mod, "mission start");
    }
    else { g_missionOn = 0; HF_API(mod)->log(mod, "mission end"); }
}

static void Player(HfMod* mod, const HfEventInfo* info, void* user)
{
    const HfPlayerEvent* p = (const HfPlayerEvent*)info;
    if (p->local && info->kind == HF_ON_PLAYER_SPAWN) g_avatar = p->entity;
}

/* A dead diver leaves an emptied inventory record: drop the copy and re-arm for the new diver's secondary. */
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
        if (w[0] == 0 && w[1] == 0 && g_probeEntity && g_missionOn)
        {
            g_probeEntity = 0; g_probeOriginal = 0; g_regDone = 1; g_pkgDone = 0; g_rearm = 1;
            g_hf->log(g_mod, "respawn: the diver died, dropping the melee copy; a fresh one comes with the next secondary");
        }
    }
    return 0;
}

/* ---- spawn the melee beside the secondary ---------------------------------------------------------------------------- */

static uint64_t SpawnReqDetour(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f)
{
    uint64_t resource = b; /* req(uint32_t* out, uint64_t resource, SpawnData* d) */
    LONG seq = g_missionOn ? InterlockedIncrement(&g_missionSeq) : 0;
    int rearm = g_rearm && g_missionOn && resource == g_secondaryRes;
    int probe = ((seq == 2 && !g_probeDone && GetTickCount64() - g_missionTick < 2000) || rearm) && c > 0x10000 && a > 0x10000;
    uint8_t before[0x58];
    if (probe && g_hf->read((uintptr_t)c, before, sizeof before) != 0) probe = 0;
    uint64_t r = g_spawnReq(a, b, c, d, e, f);
    if (!probe) return r;

    g_probeDone = 1;
    g_secondaryRes = resource;
    if (rearm) g_rearm = 0;
    uint64_t melee = ReadMeleeResource();
    if (melee == 0) { g_hf->log(g_mod, "no melee set in melee-slot-weapon.txt; the mod stays idle"); return r; }
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
    g_probeEntity = copy; g_probeOriginal = original; g_probeTick = GetTickCount64(); g_regDone = 0;
    g_copyResource = melee;
    g_hf->log(g_mod, "%s: melee %s %016llx spawned as entity %u beside the secondary %016llx (entity %u)%s", rearm ? "respawn" : "spawn",
        *g_meleeName ? g_meleeName : "(by id)", (unsigned long long)melee, copy, (unsigned long long)resource, original,
        ctxSize ? "" : " (WARNING: game's own spawn context, not copied)");
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

static void AttachCopy(HfMod* mod)
{
    const HfApi* hf = HF_API(mod);
    uint32_t copy = g_probeEntity, avatar = g_avatar, owner = 0, state = 2;
    if (g_regDone || !copy || !avatar || !g_missionOn || !g_regOk || GetTickCount64() < g_probeTick + 1500) return;
    uintptr_t game = hf->module_base("game.dll", NULL), tbl, recs, inv, irec;
    uint32_t index;
    if (!game || !Ptr(game + ENT_STATE_GLOBAL, &tbl) || !MapGet(tbl + 0x20, copy, &index) || index >= 0x100000 || !Ptr(tbl + 0x40, &recs)) return;
    /* the builder has finished with the diver: the original secondary's record names the avatar as its owner */
    uint32_t orig = g_probeOriginal, oindex;
    if (!MapGet(tbl + 0x20, orig, &oindex) || oindex >= 0x100000 || hf->read(recs + (uintptr_t)oindex * 0x30 + 4, &owner, 4) != 0 || owner != avatar)
        return; /* try again next frame */
    if (!InvRecord(game, avatar, &inv, &irec)) return;
    g_regDone = 1;
    uint64_t have = 0;
    uint32_t unit = 0;
    if (!UnitOfEntity(game, copy, &have, &unit) || have != g_copyResource || !unit)
    {
        hf->log(mod, "attach skipped: entity %u is not the melee (wanted %016llx, found %016llx): the spawn produced no such weapon", copy,
            (unsigned long long)g_copyResource, (unsigned long long)have);
        return;
    }
    uintptr_t word = 0;
    uint32_t wordBefore = 0, wordAfter = 0;
    int haveWord = LocalPlayerWord(game, &word) && hf->read(word, &wordBefore, 4) == 0;
    ((AttachFn)(game + ATTACH_RVA))((uint64_t)tbl, copy, avatar, 0, 0, 0);
    if (haveWord && hf->read(word, &wordAfter, 4) == 0 && wordAfter != wordBefore) hf->write(word, &wordBefore, 4);
    hf->write(recs + (uintptr_t)index * 0x30, &state, 4);
    hf->log(mod, "melee entity %u attached to the diver%s", copy, haveWord ? "" : " (player word not found)");
}

/* Once the diver exists: request every package id in melee-slot-package.txt (hex, whitespace separated; the game's OWN ids only). */
static void RequestPackages(HfMod* mod)
{
    static ULONGLONG nextCheck;
    static char requestedFor[40];   /* the melee name (or "(file)") whose set was last requested */
    ULONGLONG now = GetTickCount64();
    if (!g_avatar || !g_pkgReq || now < nextCheck) return;
    nextCheck = now + 500;
    char text[2048], key[40];
    /* The melee file is polled twice a second, so a pick made on the loadout screen (Melee Picker) gets its set requested before the mission;
     * a respawn (g_pkgDone cleared) requests the same set again. melee-slot-package.txt, when present, overrides the built-in set. */
    int fromFile = ReadConfig("melee-slot-package.txt", text, sizeof text);
    ReadMeleeResource();
    snprintf(key, sizeof key, "%s", fromFile ? "(file)" : g_meleeName);
    if (g_pkgDone && strcmp(key, requestedFor) == 0) return;
    g_pkgDone = 1;
    snprintf(requestedFor, sizeof requestedFor, "%s", key);
    if (!fromFile)
    {
        if (!*g_meleePackages)
        {
            if (*g_meleeName) HF_API(mod)->log(mod, "packages: no model package set known for %s yet; it may draw as a purple question mark", g_meleeName);
            return;
        }
        snprintf(text, sizeof text, "%s", g_meleePackages);
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
    HF_API(mod)->log(mod, "packages: %d requested for %s", requested, key[0] ? key : "(by id)");
}

/* ---- the toggle ------------------------------------------------------------------------------------------------------ */

/* A tap of 2 with the secondary drawn (raised by the key thread): start the staged swap. */
static void DoSwap(HfMod* mod)
{
    const HfApi* hf = HF_API(mod);
    uintptr_t game = hf->module_base("game.dll", NULL), inv, rec;
    uint32_t avatar = g_avatar, clone = g_probeEntity, orig = g_probeOriginal, cur, selection;
    if (!g_missionOn || !game || !avatar || !clone || !orig || g_swapStage) return;
    if (!InvRecord(game, avatar, &inv, &rec) || hf->read(rec + 4, &cur, 4) != 0 || hf->read(rec + 0x1C, &selection, 4) != 0) return;
    if (selection != 2) return;
    if (cur != clone && cur != orig) { hf->log(mod, "tap ignored: the secondary field holds %u, neither the pistol %u nor the melee %u", cur, orig, clone); return; }
    g_swapTarget = cur == clone ? orig : clone;
    g_swapStarted = GetTickCount64();
    g_swapDeadline = g_swapStarted + SWAP_WAIT_MS + 1500;
    g_retries = 0; g_lastPress = g_swapStarted; g_swapStage = 1;
    InterlockedExchange(&g_inject, 0x02);
}

/* The later steps of the swap, on the game-thread tick. Stages fall through when their wait is already over:
 * 1 wait for the key press of 1 to read as selection 1 (re-sent after 600 ms, twice at most); 3 holster, then write the secondary field;
 * 2 key press of 2; 4 wait for the draw to show. */
static void SwapStages(HfMod* mod)
{
    const HfApi* hf = HF_API(mod);
    ULONGLONG now = GetTickCount64();
    if (!g_swapStage) return;
    uintptr_t game = hf->module_base("game.dll", NULL), inv, rec;
    uint32_t avatar = g_avatar, selection = 0;
    if (!game || !avatar || !InvRecord(game, avatar, &inv, &rec) || hf->read(rec + 0x1C, &selection, 4) != 0) { g_swapStage = 0; return; }
    if (g_swapStage == 1)
    {
        if (selection != 1)
        {
            if (now - g_lastPress > 600 && g_retries < 2) { g_retries++; g_lastPress = now; InterlockedExchange(&g_inject, 0x02); return; }
            if (now > g_swapDeadline) { hf->log(mod, "swap aborted: selection still %u after the key press of 1 and %d retries; nothing written", selection, (int)g_retries); g_swapStage = 0; }
            return;
        }
        g_swapAt = now + HOLSTER_MS; g_swapStage = 3;
    }
    if (g_swapStage == 3)
    {
        if (now < g_swapAt) return;
        if (selection != 1) { hf->log(mod, "swap aborted: selection reads %u after the holster; nothing written", selection); g_swapStage = 0; return; }
        uint32_t target = g_swapTarget;
        if (hf->write(rec + 4, &target, 4) != 0) { hf->log(mod, "swap aborted: could not write the secondary field"); g_swapStage = 0; return; }
        g_swapAt = now + DRAW_MS; g_swapStage = 2;
    }
    if (g_swapStage == 2)
    {
        if (now < g_swapAt) return;
        InterlockedExchange(&g_inject, 0x03);
        g_swapDeadline = now + SWAP_WAIT_MS; g_swapStage = 4;
        return;
    }
    if (selection == 2 || now > g_swapDeadline)
    {
        uint32_t after = 0;
        hf->read(rec + 4, &after, 4);
        hf->log(mod, "swap to %s: %llu ms%s%s", after == g_probeEntity ? "melee" : "pistol", (unsigned long long)(now - g_swapStarted),
            g_retries ? " (key press retried)" : "", selection == 2 ? "" : " (the draw did not show in time)");
        g_swapStage = 0;
    }
}

static void Tick(HfMod* mod, float dt, void* user)
{
    AttachCopy(mod);
    RequestPackages(mod);
    if (InterlockedExchange(&g_tapFlag, 0)) DoSwap(mod);
    SwapStages(mod);
}

/* Sends the key presses of the swap (scan codes 2 = key 1, 3 = key 2) and watches for a tap of 2. */
static DWORD WINAPI KeyThread(LPVOID unused)
{
    int was2 = 0;
    for (;;)
    {
        Sleep(4);
        LONG scan = InterlockedExchange(&g_inject, 0);
        if (scan)
        {
            INPUT in = {0};
            if (scan == 0x02)   /* let the player's own finger leave the 2 key first */
                for (int w = 0; w < 100 && (GetAsyncKeyState('2') & 0x8000); w++) Sleep(4);
            in.type = INPUT_KEYBOARD; in.ki.wScan = (WORD)scan; in.ki.dwFlags = KEYEVENTF_SCANCODE;
            SendInput(1, &in, sizeof in);
            Sleep(25);
            in.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
            SendInput(1, &in, sizeof in);
        }
        /* At the key-down edge of 2 the game has not reacted yet, so a selection of 2 means the secondary was ALREADY drawn: a tap-again. */
        int down2 = (GetAsyncKeyState('2') & 0x8000) != 0;
        if (down2 && !was2 && g_missionOn && g_avatar && g_probeEntity)
        {
            uintptr_t game = g_hf->module_base("game.dll", NULL), inv, rec;
            uint32_t selection = 0;
            if (game && InvRecord(game, g_avatar, &inv, &rec) && g_hf->read(rec + 0x1C, &selection, 4) == 0 && selection == 2)
                InterlockedExchange(&g_tapFlag, 1);
        }
        was2 = down2;
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
    hf->on(mod, HF_EVENT_GAME_LOADED, GameLoaded, NULL);
    HANDLE t = CreateThread(NULL, 0, InvThread, NULL, 0, NULL);
    if (t) CloseHandle(t);
    HANDLE k = CreateThread(NULL, 0, KeyThread, NULL, 0, NULL);
    if (k) CloseHandle(k);
    int tick = hf->size >= offsetof(HfApi, on_game_thread) + sizeof hf->on_game_thread ? hf->on_game_thread(mod, Tick, NULL) : 0;
    hf->log(mod, "Dedicated Melee Slot 1.5.0 loaded (weapon event %s, inventory thread %s, key thread %s, game tick %s)",
        ok ? "subscribed" : "REFUSED", t ? "started" : "FAILED", k ? "started" : "FAILED", tick ? "hooked" : "NOT hooked");
    return 0;
}
