#include <Common.hh>
#include <cmath>
#include <emscripten/emscripten.h>
#include <new>

#include "host/Context.hh"
#include "host/SceneCreatorDynamic.hh"
#include <egg/core/ExpHeap.hh>
#include <egg/core/SceneManager.hh>
#include <game/field/ObjectDirector.hh>
#include <game/field/ObjectDrivableDirector.hh>
#include <game/item/ItemDirector.hh>
#include <game/kart/KartMove.hh>
#include <game/kart/KartObjectManager.hh>
#include <game/kart/KartSuspensionPhysics.hh>
#include <game/render/KartCamera.hh>
#include <game/system/CourseMap.hh>
#include <game/system/KPadDirector.hh>
#include <game/system/RaceConfig.hh>
#include <game/system/RaceManager.hh>

using namespace Kinoko;

static void *s_memorySpace = nullptr;
static EGG::Heap *s_rootHeap = nullptr;
static EGG::SceneManager *s_sceneMgr = nullptr;

static s32 s_courseId = 0x08;
/// Which player the per-kart getters (position, state, wheels, timing, input, ...) read: 0 is the
/// local player, 1+ are CPU racers.
static size_t s_focus = 0;
/// VS races: the CPU racers' characters and vehicles (players 1..cpuCount); the local player is
/// player 0.
static int s_cpuCount = 0;
static int s_cpuCharacter[11], s_cpuVehicle[11];
static s32 s_characterId = 0x00;
static s32 s_vehicleId = 0x08;
/// Drift mode of the local player (false = manual). kinoko_init_tas sets it from the ghost header.
static bool s_driftIsAuto = false;

/// A ghost's .rkg bytes, zero-padded: Kinoko's RawGhostFile reads a full 0x2800 bytes even when the
/// file is compressed and shorter. When set, the race replays this ghost instead of a local player.
static u8 s_ghostFile[sizeof(System::RawGhostFile)];
static bool s_ghostMode = false;

/// VS race options from kinoko_set_vs_options: engine class (0 = 50cc, 1 = 100cc, 2 = 150cc) and
/// each player's 1-based starting grid place (0 = player index + 1).
static int s_engineClass = 2;
static u8 s_startPlacement[12];

/// Item options from kinoko_set_item_options: RaceManager's two random seeds (they decide the
/// items: every client of a lockstep race must use the same ones), the item mode, and which
/// players get the player odds (bit i = player i; the others get the CPU odds).
static u32 s_seed1 = 0x74A1B095;
static u32 s_seed2 = 0;
static int s_itemMode = 0;
static u32 s_humanMask = 1;

static void OnRaceConfigInit(System::RaceConfig *config, void *) {
    if (s_ghostMode) {
        // Same setup as KReplaySystem: the course, character, vehicle and inputs come from the file
        config->setGhost(s_ghostFile);
        config->raceScenario().players[0].type = System::RaceConfig::Player::Type::Ghost;
        config->raceScenario().gameMode = System::RaceConfig::Scenario::GameMode::Time_Trial;
        return;
    }

    auto &scenario = config->raceScenario();
    // A race without CPUs is a Time Trial (starting triple mushrooms, no item roulette).
    scenario.gameMode = s_cpuCount > 0 ? System::RaceConfig::Scenario::GameMode::Versus :
                                         System::RaceConfig::Scenario::GameMode::Time_Trial;
    scenario.itemMode = static_cast<System::RaceConfig::Scenario::ItemMode>(s_itemMode);
    scenario.seed1 = s_seed1;
    scenario.seed2 = s_seed2;
    scenario.course = static_cast<Course>(s_courseId);
    scenario.playerCount = static_cast<u8>(1 + s_cpuCount);

    auto &player = scenario.players[0];
    player.type = System::RaceConfig::Player::Type::Local;
    player.character = static_cast<Character>(s_characterId);
    player.vehicle = static_cast<Vehicle>(s_vehicleId);
    player.driftIsAuto = s_driftIsAuto;

    for (int i = 0; i < s_cpuCount; ++i) {
        auto &cpu = scenario.players[1 + i];
        cpu.type = (s_humanMask >> (1 + i)) & 1 ? System::RaceConfig::Player::Type::Local :
                                                  System::RaceConfig::Player::Type::Cpu;
        cpu.character = static_cast<Character>(s_cpuCharacter[i]);
        cpu.vehicle = static_cast<Vehicle>(s_cpuVehicle[i]);
        cpu.driftIsAuto = false;
    }

    scenario.engineClass = static_cast<System::RaceConfig::Scenario::EngineClass>(s_engineClass);
    for (size_t i = 0; i < scenario.players.size(); ++i) {
        scenario.players[i].startPlacement = s_startPlacement[i];
    }
}

extern "C" {

void kinoko_set_vs_options(int engineClass, const int *placements, int count);

static void StartScene() {
    if (!s_rootHeap) {
        s_memorySpace = malloc(MEMORY_SPACE_SIZE);
        s_rootHeap = EGG::ExpHeap::create(s_memorySpace, MEMORY_SPACE_SIZE, DEFAULT_OPT);
        s_rootHeap->becomeCurrentHeap();
        EGG::SceneManager::SetRootHeap(s_rootHeap);
    }

    auto *sceneCreator = EGG::egg_new<Host::SceneCreatorDynamic>();
    s_sceneMgr = EGG::egg_new<EGG::SceneManager>(sceneCreator);

    System::RaceConfig::RegisterInitCallback(OnRaceConfigInit, nullptr);
    s_sceneMgr->changeScene(0);
}

EMSCRIPTEN_KEEPALIVE
void kinoko_init(int courseId, int characterId, int vehicleId) {
    kinoko_set_vs_options(2, nullptr, 0);
    s_cpuCount = 0;
    s_focus = 0;
    s_courseId = courseId;
    s_characterId = characterId;
    s_vehicleId = vehicleId;
    s_ghostMode = false;
    s_driftIsAuto = false;
    StartScene();
}

/// Same as kinoko_init, but the drift mode is passed through (1 = automatic, 0 = manual). TAS mode
/// uses the drift flag of the ghost it was loaded from (RKG byte 0xD), otherwise nothing matches.
EMSCRIPTEN_KEEPALIVE
void kinoko_init_tas(int courseId, int characterId, int vehicleId, int driftIsAuto) {
    kinoko_set_vs_options(2, nullptr, 0);
    s_cpuCount = 0;
    s_focus = 0;
    s_courseId = courseId;
    s_characterId = characterId;
    s_vehicleId = vehicleId;
    s_ghostMode = false;
    s_driftIsAuto = driftIsAuto != 0;
    StartScene();
}

/// Starts a VS race: the local player (player 0) plus cpuCount CPU racers (players 1..cpuCount, at
/// most 11) whose characters and vehicles are read from the two int arrays. CPU racers take their
/// inputs from kinoko_set_player_input.
EMSCRIPTEN_KEEPALIVE
void kinoko_init_vs(int courseId, int characterId, int vehicleId, int cpuCount,
        const int *cpuCharacters, const int *cpuVehicles) {
    s_cpuCount = cpuCount < 0 ? 0 : (cpuCount > 11 ? 11 : cpuCount);
    for (int i = 0; i < s_cpuCount; ++i) {
        s_cpuCharacter[i] = cpuCharacters[i];
        s_cpuVehicle[i] = cpuVehicles[i];
    }
    s_focus = 0;
    s_courseId = courseId;
    s_characterId = characterId;
    s_vehicleId = vehicleId;
    s_ghostMode = false;
    s_driftIsAuto = false;
    StartScene();
}

/// Sets the options of the next kinoko_init_vs race: engine class (0 = 50cc, 1 = 100cc,
/// 2 = 150cc) and, when placements is not null, each of count players' 1-based starting grid place
/// (0 = player index + 1). Time Trials always run at 150cc from index places.
EMSCRIPTEN_KEEPALIVE
void kinoko_set_vs_options(int engineClass, const int *placements, int count) {
    s_engineClass = engineClass < 0 ? 0 : (engineClass > 2 ? 2 : engineClass);
    for (int i = 0; i < 12; ++i) {
        s_startPlacement[i] = placements && i < count ? static_cast<u8>(placements[i]) : 0;
    }
}

/// Gives player idx an item (game ids: 0 green shell, 2 banana, 4 mushroom, 8 lightning, 9 star,
/// 10 golden mushroom, 11 mega mushroom, ...), as an item box would. Green shells and bananas are
/// held on the item button and thrown on release; see ITEM_OBJECTS_RESULT.md.
EMSCRIPTEN_KEEPALIVE
void kinoko_set_player_item(int idx, int itemId) {
    auto *itemDir = Item::ItemDirector::Instance();
    if (!itemDir || idx < 0 || idx > s_cpuCount || itemId < 0 || itemId > 0x12) {
        return;
    }

    itemDir->kartItem(static_cast<size_t>(idx))
            .inventory()
            .setItem(static_cast<Item::ItemId>(itemId));
}

/// Sets the item options of the next kinoko_init_vs race: RaceManager's random seeds, the item
/// mode (0 balanced, 1 frantic, 2 strategic, 3 none) and the players with player odds (bit i =
/// player i, default 1). Player 0 always gets the player odds.
EMSCRIPTEN_KEEPALIVE
void kinoko_set_item_options(u32 seed1, u32 seed2, int itemMode, u32 humanMask) {
    s_seed1 = seed1;
    s_seed2 = seed2;
    s_itemMode = itemMode < 0 ? 0 : (itemMode > 3 ? 3 : itemMode);
    s_humanMask = humanMask | 1;
}

/// The item player idx holds: (item id << 8) | count (item 20 = none). -1 for a bad index.
EMSCRIPTEN_KEEPALIVE
int kinoko_get_player_item(int idx) {
    auto *itemDir = Item::ItemDirector::Instance();
    if (!itemDir || idx < 0 || idx > s_cpuCount) {
        return -1;
    }

    const auto &inventory = itemDir->kartItem(static_cast<size_t>(idx)).inventory();
    return (static_cast<int>(inventory.id()) << 8) | (inventory.currentCount() & 0xFF);
}

/// The item roulette of player idx: (state << 8) | shown item (state 0 idle, 1 spinning,
/// 2 stopping; item 20 = none). -1 for a bad index.
EMSCRIPTEN_KEEPALIVE
int kinoko_get_roulette(int idx) {
    auto *itemDir = Item::ItemDirector::Instance();
    if (!itemDir || idx < 0 || idx > s_cpuCount) {
        return -1;
    }

    const auto &roulette = itemDir->kartItem(static_cast<size_t>(idx)).roulette();
    return (static_cast<int>(roulette.state()) << 8) | static_cast<int>(roulette.shownItem());
}

/// Writes up to max item objects (shells, bananas: the ported kinds) as 8 floats each: kind
/// (0 green shell, 2 banana), owner, held behind the owner (1/0), x, y, z, scale, hit radius.
/// Returns how many were written.
EMSCRIPTEN_KEEPALIVE
int kinoko_get_item_objects(float *out, int max) {
    auto *itemDir = Item::ItemDirector::Instance();
    if (!itemDir || !out) {
        return 0;
    }

    int n = 0;
    for (size_t k = 0; k < static_cast<size_t>(Item::ItemObjKind::Count); ++k) {
        auto &holder = itemDir->holder(static_cast<Item::ItemObjKind>(k));
        for (s32 i = 0; i < holder.spawnedCount() && n < max; ++i, ++n) {
            const Item::ItemObj *obj = holder.obj(i);
            float *o = out + n * 8;
            o[0] = static_cast<float>(k);
            o[1] = static_cast<float>(obj->owner());
            o[2] = obj->isTrailing() ? 1.0f : 0.0f;
            o[3] = obj->pos().x;
            o[4] = obj->pos().y;
            o[5] = obj->pos().z;
            o[6] = obj->scale();
            o[7] = obj->hitRadius();
        }
    }

    return n;
}

/// The number of racers (1 in Time Trials).
EMSCRIPTEN_KEEPALIVE
int kinoko_get_player_count() {
    return 1 + s_cpuCount;
}

/// Selects the player the per-kart getters read (position, rotation, state, wheels, floor, timing,
/// input). The camera always follows the local player.
EMSCRIPTEN_KEEPALIVE
void kinoko_set_focus(int idx) {
    s_focus = static_cast<size_t>(idx < 0 ? 0 : (idx > s_cpuCount ? s_cpuCount : idx));
}

/// Writes the inputs of a player driven by the host (the local player 0, or a CPU racer).
EMSCRIPTEN_KEEPALIVE
void kinoko_set_player_input(int idx, int buttons, float stickX, float stickY, int trick) {
    auto *padDir = System::KPadDirector::Instance();
    if (idx < 0 || idx > s_cpuCount || !padDir || !padDir->hostController(idx)) {
        return;
    }
    padDir->hostController(idx)->setInputs(static_cast<u16>(buttons), EGG::Vector2f(stickX, stickY),
            static_cast<System::Trick>(trick));
}

/// Starts a race that replays a ghost (.rkg bytes at rkg, size bytes). The caller must have checked
/// that the data starts with "RKGD"; Kinoko panics on a malformed header.
EMSCRIPTEN_KEEPALIVE
void kinoko_init_ghost(const u8 *rkg, int size) {
    memset(s_ghostFile, 0, sizeof(s_ghostFile));
    memcpy(s_ghostFile, rkg,
            size < static_cast<int>(sizeof(s_ghostFile)) ? size : sizeof(s_ghostFile));
    s_ghostMode = true;
    s_cpuCount = 0;
    s_focus = 0;
    StartScene();
}

EMSCRIPTEN_KEEPALIVE
void kinoko_set_input(int buttons, float stickX, float stickY, int trick) {
    auto *padDir = System::KPadDirector::Instance();
    if (!padDir || !padDir->hostController()) {
        return;
    }

    EGG::Vector2f stick(stickX, stickY);
    padDir->hostController()->setInputs(static_cast<u16>(buttons), stick,
            static_cast<System::Trick>(trick));
}

/// Snapshot slots (TAS mode keyframes and user save states). The table must live outside the game
/// heap: operator new is the EGG heap that a restore overwrites, so each Context is malloc'd with
/// placement new (Context itself mallocs its copy) and the table is static storage.
static constexpr int MAX_STATES = 1024;
static constexpr int DEFAULT_STATE_BYTES = 1 << 20;
/// A VS race (several karts, items) dirties more of the heap than a single-kart race: about 1.05 MiB at
/// most over 5400 frames of 12 karts on the 32 courses, so it gets a larger default.
static constexpr int DEFAULT_VS_STATE_BYTES = 4 << 20;
static Host::Context *s_states[MAX_STATES];

static void ReleaseState(int id) {
    if (s_states[id]) {
        s_states[id]->~Context();
        free(s_states[id]);
        s_states[id] = nullptr;
    }
}

/// Snapshots the live game into slot id (0..1023), keeping the first `bytes` of the game heap
/// (bytes <= 0: the default, 1 MiB; at most the whole 64 MiB heap). Replaces what the slot held.
/// Returns 1, or 0 on a bad id or when there is no race.
EMSCRIPTEN_KEEPALIVE
int kinoko_save_state(int id, int bytes) {
    if (id < 0 || id >= MAX_STATES || !s_sceneMgr || !System::RaceManager::Instance() ||
            !s_rootHeap) {
        return 0;
    }

    size_t def = static_cast<size_t>(s_cpuCount > 0 ? DEFAULT_VS_STATE_BYTES : DEFAULT_STATE_BYTES);
    size_t n = bytes <= 0 ? def : static_cast<size_t>(bytes);
    if (n > MEMORY_SPACE_SIZE) {
        n = MEMORY_SPACE_SIZE;
    }

    ReleaseState(id);
    void *mem = malloc(sizeof(Host::Context));
    if (!mem) {
        return 0;
    }
    s_states[id] = new (mem) Host::Context(n);
    return 1;
}

/// Restores slot id (Host::Context::SetActiveContext). Returns 1, or 0 for an empty / bad slot.
EMSCRIPTEN_KEEPALIVE
int kinoko_load_state(int id) {
    if (id < 0 || id >= MAX_STATES || !s_states[id]) {
        return 0;
    }

    Host::Context::SetActiveContext(*s_states[id]);
    return 1;
}

EMSCRIPTEN_KEEPALIVE
void kinoko_free_state(int id) {
    if (id >= 0 && id < MAX_STATES) {
        ReleaseState(id);
    }
}

EMSCRIPTEN_KEEPALIVE
void kinoko_free_all_states() {
    for (int i = 0; i < MAX_STATES; ++i) {
        ReleaseState(i);
    }
}

/// Debug (like the host's DIRTY command): compares the live game heap with slot id and writes
/// {differing 4 KiB pages, differing bytes, highest differing offset} to out. Returns 1, or 0 for
/// an empty slot. Only the bytes the snapshot kept are compared.
EMSCRIPTEN_KEEPALIVE
int kinoko_state_dirty(int id, int *out) {
    if (id < 0 || id >= MAX_STATES || !s_states[id]) {
        return 0;
    }

    static size_t perMiB[MEMORY_SPACE_SIZE / (1 << 20)];
    size_t bytes = 0, last = 0;
    size_t pages = s_states[id]->CountDifferingPages(perMiB, &bytes, &last);
    out[0] = static_cast<int>(pages);
    out[1] = static_cast<int>(bytes);
    out[2] = static_cast<int>(last);
    return 1;
}

EMSCRIPTEN_KEEPALIVE
void kinoko_step() {
    if (s_sceneMgr) {
        s_sceneMgr->calc();
    }
}

/// The game's own race camera, from Kinoko's KartCamera: writes {eye x/y/z, look-at x/y/z, fov
/// (degrees)} to out and returns 1 (0 when there is no camera yet).
EMSCRIPTEN_KEEPALIVE
int kinoko_get_camera(float *out) {
    auto *cam = Render::KartCamera::Instance();
    if (!cam || !cam->camParams()) {
        return 0;
    }
    const auto &p = cam->forwardPos();
    const auto &t = cam->forwardTarget();
    out[0] = p.x;
    out[1] = p.y;
    out[2] = p.z;
    out[3] = t.x;
    out[4] = t.y + cam->targetOffsetY();
    out[5] = t.z;
    out[6] = cam->fov();
    cam->debugState(out +
            7); // 7..12: pitch, drift yaw, hop pos y, m_1c, big-air height, big-air fall pitch
    return 1;
}

EMSCRIPTEN_KEEPALIVE
void kinoko_get_kart_pos(float *outPos) {
    auto *mgr = Kart::KartObjectManager::Instance();
    if (!mgr || !mgr->object(s_focus)) {
        return;
    }

    const auto &pos = mgr->object(s_focus)->pos();
    outPos[0] = pos.x;
    outPos[1] = pos.y;
    outPos[2] = pos.z;
}

EMSCRIPTEN_KEEPALIVE
void kinoko_get_kart_rot(float *outQuat) {
    auto *mgr = Kart::KartObjectManager::Instance();
    if (!mgr || !mgr->object(s_focus)) {
        return;
    }

    const auto &rot = mgr->object(s_focus)->fullRot();
    outQuat[0] = rot.v.x;
    outQuat[1] = rot.v.y;
    outQuat[2] = rot.v.z;
    outQuat[3] = rot.w;
}

/// Kart state for sounds and animation. Writes {speed, speedRatio, driftState, mtCharge,
/// hopStickX (drift direction: -1, 0 or 1)} to outMotion and
/// returns a bitfield of the flags below. The page diffs it every physics step to fire sounds.
enum KartSoundFlag : u32 {
    SND_ACCELERATE = 1 << 0,
    SND_BRAKE = 1 << 1,
    SND_GROUND = 1 << 2,
    SND_HOP = 1 << 3,
    SND_DRIFT = 1 << 4,
    SND_BOOST = 1 << 5,
    SND_MUSHROOM = 1 << 6,
    SND_WHEELIE = 1 << 7,
    SND_TRICK = 1 << 8,
    SND_LONG_AIR = 1 << 9,
    SND_WALL = 1 << 10,
    SND_JUMP_PAD = 1 << 11,
    SND_ZIPPER = 1 << 12,
    SND_CANNON = 1 << 13,
    SND_RESPAWN = 1 << 14,
    SND_BURNOUT = 1 << 15,
    SND_HIT = 1 << 16,
    SND_OFFROAD = 1 << 17,
    SND_SSMT = 1 << 18,
};

EMSCRIPTEN_KEEPALIVE
u32 kinoko_get_kart_state(float *outMotion) {
    auto *mgr = Kart::KartObjectManager::Instance();
    if (!mgr || !mgr->object(s_focus)) {
        return 0;
    }

    using Kart::eStatus;
    auto *kart = mgr->object(s_focus);
    const auto *move = kart->move();
    const auto &status = kart->status();

    outMotion[0] = move->speed();
    outMotion[1] = move->speedRatio();
    outMotion[2] = static_cast<float>(move->driftState());
    outMotion[3] = static_cast<float>(move->mtCharge());
    outMotion[4] = static_cast<float>(move->hopStickX());

    u32 flags = 0;
    auto set = [&](u32 bit, bool on) {
        if (on) {
            flags |= bit;
        }
    };
    set(SND_ACCELERATE, status.onBit(eStatus::Accelerate));
    set(SND_BRAKE, status.onBit(eStatus::Brake));
    set(SND_GROUND, status.onBit(eStatus::TouchingGround));
    set(SND_HOP, status.onBit(eStatus::Hop));
    set(SND_DRIFT, status.onBit(eStatus::DriftManual, eStatus::DriftAuto));
    set(SND_BOOST, status.onBit(eStatus::Boost));
    set(SND_MUSHROOM, status.onBit(eStatus::MushroomBoost));
    set(SND_WHEELIE, status.onBit(eStatus::Wheelie));
    set(SND_TRICK, status.onBit(eStatus::InATrick, eStatus::ZipperTrick));
    set(SND_LONG_AIR, status.onBit(eStatus::AirtimeOver20));
    set(SND_WALL, status.onBit(eStatus::WallCollision));
    set(SND_JUMP_PAD, status.onBit(eStatus::JumpPad));
    set(SND_ZIPPER, status.onBit(eStatus::ZipperBoost, eStatus::HalfPipeRamp));
    set(SND_CANNON, status.onBit(eStatus::InCannon));
    set(SND_RESPAWN, status.onBit(eStatus::BeforeRespawn, eStatus::InRespawn));
    set(SND_BURNOUT, status.onBit(eStatus::Burnout));
    set(SND_HIT, status.onBit(eStatus::LargeFlipHit, eStatus::Shocked, eStatus::Crushed));
    // Offroad surfaces slow the kart via the KCL speed factor (grass, sand, etc.)
    set(SND_OFFROAD, move->kclSpeedFactor() < 1.0f);
    set(SND_SSMT, move->ssmtCharged());
    return flags;
}

/// The current controller input (from the host, or from the ghost file when replaying one):
/// writes {stickX, stickY, buttons, trick} to out.
EMSCRIPTEN_KEEPALIVE
void kinoko_get_input(float *out) {
    auto *pad = System::KPadDirector::Instance();
    if (!pad) {
        return;
    }

    const auto &state = pad->playerInput(s_focus).currentState();
    out[0] = state.stick.x;
    out[1] = state.stick.y;
    out[2] = static_cast<float>(state.buttons);
    out[3] = static_cast<float>(static_cast<int>(state.trick));
}

/// Writes each wheel's world-space centre and radius (x, y, z, r per tire) to out and returns
/// the tire count (4 for karts, 2 for bikes). Order: front, front mirrored, rear, rear mirrored.
EMSCRIPTEN_KEEPALIVE
int kinoko_get_wheels(float *out) {
    auto *mgr = Kart::KartObjectManager::Instance();
    if (!mgr || !mgr->object(s_focus)) {
        return 0;
    }

    auto *kart = mgr->object(s_focus);
    u16 count = kart->tireCount();
    for (u16 i = 0; i < count; ++i) {
        const auto &p = kart->wheelPos(i);
        out[4 * i] = p.x;
        out[4 * i + 1] = p.y;
        out[4 * i + 2] = p.z;
        out[4 * i + 3] = kart->tirePhysics(i)->effectiveRadius();
    }
    return count;
}

/// The kind of ground under each wheel: writes {KCL base type, variant} per tire (-1, 0 when a
/// wheel has no floor), tire order as in kinoko_get_wheels, and returns the tire count. The base
/// type and variant decide the terrain effects (dirt, sand, grass, snow, ...), see the KCL flag
/// table.
EMSCRIPTEN_KEEPALIVE
int kinoko_get_floor(int *out) {
    auto *mgr = Kart::KartObjectManager::Instance();
    if (!mgr || !mgr->object(s_focus)) {
        return 0;
    }

    auto *kart = mgr->object(s_focus);
    u16 count = kart->tireCount();
    for (u16 i = 0; i < count; ++i) {
        const auto &cd = kart->collisionData(i);
        int type = -1;
        if (cd.bFloor && cd.closestFloorFlags) {
            type = 0;
            u32 mask = static_cast<u32>(cd.closestFloorFlags);
            while (!(mask & 1)) {
                mask >>= 1;
                ++type;
            }
        }
        out[2 * i] = type;
        out[2 * i + 1] = type >= 0 ? static_cast<int>(cd.closestFloorSettings) : 0;
    }
    return count;
}

/// Race progress for music: writes {stage, maxLap, countdownFrames} to out.
/// stage: 0 Intro, 1 Countdown, 2 Race, 3+ Finished.
EMSCRIPTEN_KEEPALIVE
void kinoko_get_race_state(int *out) {
    auto *race = System::RaceManager::Instance();
    if (!race) {
        return;
    }

    out[0] = static_cast<int>(race->stage());
    out[1] = race->player(s_focus).maxLap();
    out[2] = race->getCountdownTimer();
    out[3] = static_cast<int>(race->timer()); // the game's frame timer (what timed objects like the
                                              // volcano pieces run on)
}

/// Race timing for the HUD. Writes 8 ints to out:
///   [0] race time in ms, [1] highest lap reached (1-3), [2] stage (as kinoko_get_race_state),
///   [3] driving wrong way (0/1), [4..6] cumulative time in ms at which laps 1-3 were finished
///   (-1 while not finished), [7] race completion in 1/10000ths of the whole race (0-30000).
EMSCRIPTEN_KEEPALIVE
void kinoko_get_timing(int *out) {
    auto *race = System::RaceManager::Instance();
    if (!race) {
        return;
    }

    auto toMs = [](const System::Timer &t) {
        return static_cast<int>(t.min) * 60000 + static_cast<int>(t.sec) * 1000 +
                static_cast<int>(t.mil);
    };
    const auto &player = race->player(s_focus);
    out[0] = toMs(race->timerManager().currentTimer());
    out[1] = player.maxLap();
    out[2] = static_cast<int>(race->stage());
    out[3] = player.drivingWrongWay() ? 1 : 0;
    for (size_t i = 0; i < 3; ++i) {
        const auto &t = player.lapTimer(i);
        out[4 + i] = t.valid ? toMs(t) : -1;
    }
    out[7] = static_cast<int>(player.raceCompletion() * 10000.0f);
    if (auto *items = Item::ItemDirector::Instance()) {
        out[8] = items->itemInventory(static_cast<s16>(s_focus)).currentCount(); // mushrooms left
    }
}

/// Writes the world x/z centre of the track `lookAhead` checkpoints ahead of the kart to out and
/// returns 1, or returns 0 if there is none. Used to steer test drivers and, later, CPU/ghost aids.
EMSCRIPTEN_KEEPALIVE
int kinoko_get_guide_point(float *out, int lookAhead) {
    auto *race = System::RaceManager::Instance();
    auto *map = System::CourseMap::Instance();
    if (!race || !map) {
        return 0;
    }

    System::MapdataCheckPoint *cp = map->getCheckPoint(race->player(s_focus).checkpointId());
    for (int i = 0; cp && i < lookAhead; ++i) {
        cp = cp->nextCount() ? cp->nextPoint(0) : nullptr;
    }
    if (!cp) {
        return 0;
    }

    out[0] = cp->midpoint().x;
    out[1] = cp->midpoint().y; // Vector2f y is world z
    return 1;
}

/// The road ahead of the focused racer: for up to `count` checkpoints starting at its current one
/// (following the first branch), writes {mid x, mid z, left x, left z, right x, right z} to out and
/// returns how many were written. Used by the CPU driver.
EMSCRIPTEN_KEEPALIVE
int kinoko_get_path(float *out, int count) {
    auto *race = System::RaceManager::Instance();
    auto *map = System::CourseMap::Instance();
    if (!race || !map) {
        return 0;
    }

    System::MapdataCheckPoint *cp = map->getCheckPoint(race->player(s_focus).checkpointId());
    int n = 0;
    for (; cp && n < count; ++n) {
        float *o = out + 6 * n;
        o[0] = cp->midpoint().x;
        o[1] = cp->midpoint().y;
        o[2] = cp->left().x;
        o[3] = cp->left().y;
        o[4] = cp->right().x;
        o[5] = cp->right().y;
        cp = cp->nextCount() ? cp->nextPoint(0) : nullptr;
    }
    return n;
}

EMSCRIPTEN_KEEPALIVE
int kinoko_get_object_count() {
    auto *objDir = Field::ObjectDirector::Instance();
    if (!objDir) {
        return 0;
    }
    return static_cast<int>(objDir->objects().size());
}

/// Rotation-matrix -> quaternion (Shepperd's method) off the 3x3 part of a row-major Matrix34f
/// (m[row, col]; see ObjectBase::transform() / base(), where base(0..2) are the X/Y/Z basis
/// columns). Used only for objects whose orientation is a full matrix rather than a plain Euler
/// rot() -- see ObjectBase::usesMatrixTransform().
static EGG::Quatf matrixToQuat(const EGG::Matrix34f &m) {
    f32 m00 = m[0, 0], m01 = m[0, 1], m02 = m[0, 2];
    f32 m10 = m[1, 0], m11 = m[1, 1], m12 = m[1, 2];
    f32 m20 = m[2, 0], m21 = m[2, 1], m22 = m[2, 2];
    f32 trace = m00 + m11 + m22;
    EGG::Quatf q;
    if (trace > 0.0f) {
        f32 s = 0.5f / std::sqrt(trace + 1.0f);
        q.w = 0.25f / s;
        q.v.x = (m21 - m12) * s;
        q.v.y = (m02 - m20) * s;
        q.v.z = (m10 - m01) * s;
    } else if (m00 > m11 && m00 > m22) {
        f32 s = 2.0f * std::sqrt(1.0f + m00 - m11 - m22);
        q.w = (m21 - m12) / s;
        q.v.x = 0.25f * s;
        q.v.y = (m01 + m10) / s;
        q.v.z = (m02 + m20) / s;
    } else if (m11 > m22) {
        f32 s = 2.0f * std::sqrt(1.0f + m11 - m00 - m22);
        q.w = (m02 - m20) / s;
        q.v.x = (m01 + m10) / s;
        q.v.y = 0.25f * s;
        q.v.z = (m12 + m21) / s;
    } else {
        f32 s = 2.0f * std::sqrt(1.0f + m22 - m00 - m11);
        q.w = (m10 - m01) / s;
        q.v.x = (m02 + m20) / s;
        q.v.y = (m12 + m21) / s;
        q.v.z = 0.25f * s;
    }
    return q;
}

/// Writes up to capacity objects to outData, 15 floats each (id, position x/y/z, rotation x/y/z,
/// scale x/y/z, animState, orientation quaternion x/y/z/w), and returns how many were written
/// (never more than capacity). The quaternion is only meaningful (and only non-zero) for objects
/// whose orientation is a full matrix rather than the plain Euler rotation x/y/z already written
/// (ObjectBase::usesMatrixTransform(), e.g. ObjectHeyho tilting to the floor normal it rides over,
/// which the x/y/z rotation fields can't express) -- a page reading this buffer should fall back
/// to rotation x/y/z whenever x=y=z=w=0.
EMSCRIPTEN_KEEPALIVE
int kinoko_get_objects(float *outData, int capacity) {
    size_t offset = 0;
    size_t remaining = static_cast<size_t>(capacity);

    const auto writeOne = [&](const Field::ObjectBase *obj) {
        outData[offset++] = static_cast<float>(static_cast<u32>(obj->id()));

        const auto &p = obj->pos();
        outData[offset++] = p.x;
        outData[offset++] = p.y;
        outData[offset++] = p.z;

        const auto &r = obj->rot();
        outData[offset++] = r.x;
        outData[offset++] = r.y;
        outData[offset++] = r.z;

        const auto &sc = obj->scale();
        outData[offset++] = sc.x;
        outData[offset++] = sc.y;
        outData[offset++] = sc.z;

        outData[offset++] = static_cast<float>(obj->animState());

        if (obj->usesMatrixTransform()) {
            EGG::Quatf q = matrixToQuat(obj->transform());
            outData[offset++] = q.v.x;
            outData[offset++] = q.v.y;
            outData[offset++] = q.v.z;
            outData[offset++] = q.w;
        } else {
            outData[offset++] = 0.0f;
            outData[offset++] = 0.0f;
            outData[offset++] = 0.0f;
            outData[offset++] = 0.0f;
        }
        --remaining;
    };

    if (auto *objDir = Field::ObjectDirector::Instance()) {
        for (auto *obj : objDir->objects()) {
            if (remaining == 0) {
                break;
            }
            // Skip pure coordinator objects with no model of their own (e.g. ObjectBird, whose
            // leader/follower sub-objects carry the real seagull models and share its id) -- the
            // base game never draws these, so exporting their static spawn pos/rot as if they were
            // a real visible object left a phantom, never-moving duplicate on screen.
            if (!obj->hasModel() || !obj->isVisible()) {
                continue;
            }
            writeOne(obj);
        }
    }

    // Kinoko-family bouncy mushrooms and other drivable platforms register with
    // ObjectDrivableDirector instead of ObjectDirector -- see its objects() comment.
    if (auto *drivableDir = Field::ObjectDrivableDirector::Instance()) {
        for (auto *obj : drivableDir->objects()) {
            if (remaining == 0) {
                break;
            }
            if (!obj->hasModel() || !obj->isVisible()) {
                continue;
            }
            writeOne(obj);
        }
    }

    // 15 floats per object written by writeOne() above (id, pos x3, rot x3, scale x3, animState,
    // quat x4) -- this was 11 before the animState/quat fields were added and never updated, so the
    // page was being told the wrong object count and reading past the real data for every track.
    return static_cast<int>(offset / 15);
}

} // extern "C"

int main() {
    return 0;
}
