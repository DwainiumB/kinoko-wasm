#pragma once

#include "host/Context.hh"
#include "host/KSystem.hh"

#include <egg/core/SceneManager.hh>

#include <game/system/RaceConfig.hh>

namespace Kinoko {

/// @brief Kinoko system for tree/beam searches driven by an external process.
/// @details Like KDriveSystem, but the controlling process can also save and restore the whole
/// simulation (Host::Context, a copy of the game heap), so a search can branch from any state
/// without replaying the race from the start. Line protocol over stdin/stdout:
///   stdout `COLS <csv header>` once at start, then one reply per command:
///   `STEP <buttons> <stickX> <stickY> <trick>`  simulate one frame; reply `ROW <csv row>`,
///   followed by
///                                              `END finished=1 timeMs=<ms> ...` once the race is
///                                              finished
///   `SAVE <id>`                                 snapshot the current state under id; reply `OK`
///   `LOAD <id>`                                 restore the snapshot; reply `OK`
///   `FREE <id>`                                 drop a snapshot; reply `OK`
///   `QUIT`                                      exit
/// Used by tools/beam_search.py.
class KSearchSystem : public KSystem {
public:
    void init() override;
    void calc() override;
    bool run() override;
    void parseOptions(int argc, char **argv) override;

    static KSearchSystem *CreateInstance();
    static void DestroyInstance();

    static KSearchSystem *Instance() {
        return static_cast<KSearchSystem *>(s_instance);
    }

    KSearchSystem();
    ~KSearchSystem() override;

private:
    KSearchSystem(const KSearchSystem &) = delete;
    KSearchSystem(KSearchSystem &&) = delete;

    void applyInput(int buttons, float stickX, float stickY, int trick);
    void reportEnd() const;

    static void OnInit(System::RaceConfig *config, void *arg);

    EGG::SceneManager *m_sceneMgr;
    s32 m_courseId;
    s32 m_characterId;
    s32 m_vehicleId;
    u32 m_frame;
};

} // namespace Kinoko
