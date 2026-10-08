#pragma once

#include "host/KSystem.hh"

#include <egg/core/SceneManager.hh>

#include <game/system/RaceConfig.hh>

namespace Kinoko {

/// @brief Kinoko system for headless input-sequence evaluation.
/// @details Drives a Local player from a pre-recorded input sequence (a "task file", written by
/// an external search/optimizer -- see tools/brute_force.py) instead of a human or ghost, runs
/// the race to completion or a frame cap, and prints the outcome to stdout as a single
/// `RESULT ...` line for the driving process to parse. One race per process invocation, matching
/// KReplaySystem; batching many evaluations into one process (resetting the scene between them)
/// is a possible future speed-up once this baseline is validated.
class KBruteSystem : public KSystem {
public:
    void init() override;
    void calc() override;
    bool run() override;
    void parseOptions(int argc, char **argv) override;

    static KBruteSystem *CreateInstance();
    static void DestroyInstance();

    static KBruteSystem *Instance() {
        return static_cast<KBruteSystem *>(s_instance);
    }

    KBruteSystem();
    ~KBruteSystem() override;

private:
    KBruteSystem(const KBruteSystem &) = delete;
    KBruteSystem(KBruteSystem &&) = delete;

    /// One recorded controller state for a single frame. Matches KPadHostController::setInputs.
    struct InputFrame {
        u16 buttons;
        u8 trick;
        f32 stickX;
        f32 stickY;
    };

    void loadTask(const char *path);
    [[nodiscard]] InputFrame frameAt(size_t i) const;
    [[nodiscard]] bool calcEnd() const;
    void reportResult() const;

    static void OnInit(System::RaceConfig *config, void *arg);

    EGG::SceneManager *m_sceneMgr;

    const u8 *m_taskBuffer;
    s32 m_courseId;
    s32 m_characterId;
    s32 m_vehicleId;
    size_t m_frameCount;
    u32 m_maxFrames;
    size_t m_frameIdx;
    s32 m_goFrame; ///< The frame index at which RaceManager::Stage first became Race (the "GO!"
                   ///< moment inputs actually start mattering for a rocket start / start slide),
                   ///< or -1 if the race never left Intro/Countdown. See reportResult.
};

} // namespace Kinoko
