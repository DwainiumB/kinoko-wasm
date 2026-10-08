#pragma once

#include "host/KSystem.hh"

#include <egg/core/SceneManager.hh>

#include <game/system/RaceConfig.hh>

namespace Kinoko {

/// @brief Kinoko system for closed-loop control by an external process.
/// @details Line protocol over stdin/stdout, one race per process:
///   1. stdout: `COLS <csv header>` once, before the first frame.
///   2. For every frame the process reads `<buttons> <stickX> <stickY> <trick>` from stdin, applies
///      it to the host controller, simulates one frame and writes `ROW <csv row>` (the columns of
///      Host::WriteStateRow) to stdout.
///   3. When the race finishes, or `--maxframes` is reached, it writes
///      `END finished=<0|1> frames=<n> timeMs=<ms or -1> completion=<x10000>` and exits.
/// Other output on stdout (engine logging) is ignored by the reader because it lacks these tags.
/// Used by tools/drive_policy.py.
class KDriveSystem : public KSystem {
public:
    void init() override;
    void calc() override;
    bool run() override;
    void parseOptions(int argc, char **argv) override;

    static KDriveSystem *CreateInstance();
    static void DestroyInstance();

    static KDriveSystem *Instance() {
        return static_cast<KDriveSystem *>(s_instance);
    }

    KDriveSystem();
    ~KDriveSystem() override;

private:
    KDriveSystem(const KDriveSystem &) = delete;
    KDriveSystem(KDriveSystem &&) = delete;

    bool readInput();
    bool calcEnd() const;
    void reportEnd() const;

    static void OnInit(System::RaceConfig *config, void *arg);

    EGG::SceneManager *m_sceneMgr;
    s32 m_courseId;
    s32 m_characterId;
    s32 m_vehicleId;
    u32 m_maxFrames;
    u32 m_frameIdx;
};

} // namespace Kinoko
