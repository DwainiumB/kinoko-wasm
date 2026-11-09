#pragma once

#include "host/KSystem.hh"

#include <egg/core/SceneManager.hh>

#include <game/system/RaceConfig.hh>

#include <array>
#include <cstdio>
#include <vector>

namespace Kinoko {

/// @brief Kinoko system that drives a multi-kart VS race from recorded inputs and logs every kart.
/// @details For checking kart-kart collision frame by frame against a Dolphin recording.
///   kinoko_host versus --course <id> --kart <character>:<vehicle>[:auto] --kart ...
///       --inputs <file> --out <file> [--frames <n>] [--cc 50|100|150] [--start <p0>,<p1>,...]
///       [--seed <seed1>[:<seed2>]] [--item-mode 0..3] [--human <i>,<j>,...]
///       [--items <file>] [--objects <file>] [--force-item <player>:<item id>]
/// --start gives each kart's 1-based place on the starting grid (default: player index + 1).
/// --seed sets RaceManager's two random seeds (hex or decimal; default 0x74A1B095:0): every client
/// of a lockstep race must use the same ones, they decide the items. --item-mode is balanced,
/// frantic, strategic or none. --human lists the players that get the player odds (default 0; the
/// others get the CPU odds).
/// Input file: one line per input change, `<race frame> <player> <buttons> <stickX> <stickY>
/// <trick>`, applied before the calc that brings RaceManager::timer() to that race frame; a player
/// keeps its last input until a new line for it appears. tools/kart_collision_diff.py writes it
/// from a Dolphin logger dump. A line `item <race frame> <player> <item id>` gives that player the
/// item (game ids, e.g. 9 star, 11 mega, 8 lightning) directly. Item boxes on the course give
/// items through the roulette. Output: CSV with one row per frame and kart (position, full
/// rotation, speed, kart-kart bump timer, ..., the roulette's state and shown item).
class KVersusSystem : public KSystem {
public:
    void init() override;
    void calc() override;
    bool run() override;
    void parseOptions(int argc, char **argv) override;

    static KVersusSystem *CreateInstance();
    static void DestroyInstance();

    static KVersusSystem *Instance() {
        return static_cast<KVersusSystem *>(s_instance);
    }

    KVersusSystem();
    ~KVersusSystem() override;

private:
    struct KartChoice {
        s32 character;
        s32 vehicle;
        bool driftIsAuto;
    };

    struct Input {
        u32 frame;
        u32 player;
        u16 buttons;
        f32 stickX;
        f32 stickY;
        s32 trick;
        s32 item; ///< -1 for an input line, else the item id this line gives.
    };

    KVersusSystem(const KVersusSystem &) = delete;
    KVersusSystem(KVersusSystem &&) = delete;

    void loadInputs();
    void applyInputs();
    void writeRows(FILE *out) const;
    void writeItemRows(FILE *out) const;
    void writeObjectRows(FILE *out);

    static void OnInit(System::RaceConfig *config, void *arg);

    EGG::SceneManager *m_sceneMgr;
    s32 m_courseId;
    System::RaceConfig::Scenario::EngineClass m_engineClass;
    std::vector<KartChoice> m_karts;
    std::vector<u8> m_startPlacements;
    std::vector<u8> m_humans;
    u32 m_seed1;
    u32 m_seed2;
    /// Extra draws on the first generator after loading, standing in for the game's CPU AI draws
    /// (DOLPHIN_VERIFY_RESULT.md).
    u32 m_rng1Skip = 0;
    System::RaceConfig::Scenario::ItemMode m_itemMode;
    const char *m_inputPath;
    const char *m_outPath;
    const char *m_itemsPath; ///< --items: every item object on the course, per frame.
    /// --objects: every course object's position and transform, a row whenever they change.
    const char *m_objectsPath;
    std::vector<std::array<f32, 15>> m_lastObjectRows; ///< Per object, the values last written.
    u32 m_maxFrames;
    u32 m_frameIdx;
    std::vector<Input> m_inputs;
    size_t m_nextInput;
};

} // namespace Kinoko
