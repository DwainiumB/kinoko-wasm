#include "KBruteSystem.hh"

#include "host/Option.hh"
#include "host/SceneCreatorDynamic.hh"

#include <abstract/File.hh>

#include <game/kart/KartMove.hh>
#include <game/kart/KartObjectManager.hh>
#include <game/kart/KartState.hh>
#include <game/kart/Status.hh>
#include <game/system/KPadDirector.hh>
#include <game/system/RaceManager.hh>

#include <cstdio>
#include <cstring>

namespace Kinoko {

namespace {

/// Layout of a brute-force task file (all multi-byte fields little-endian, host native order --
/// this is a local format between tools/brute_force.py and this binary, not a Wii/RKG format):
/// Offset  | Size | Description
///-------- | ---- | -----------
/// 0x00 | 4 bytes | "KBRT" magic
/// 0x04 | u32 | format version (1)
/// 0x08 | s32 | course id
/// 0x0C | s32 | character id
/// 0x10 | s32 | vehicle id
/// 0x14 | u32 | frame count (length of the recorded input sequence)
/// 0x18 | u32 | max frames (safety cap; the race is cut short if it runs this long)
/// 0x1C | ... | frameCount input frames, 12 bytes each: u16 buttons, u8 trick, u8 pad, f32 stickX,
/// f32 stickY
constexpr size_t TASK_HEADER_SIZE = 0x1C;
constexpr size_t TASK_FRAME_SIZE = 12;

u32 ReadU32(const u8 *p) {
    u32 v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

s32 ReadS32(const u8 *p) {
    s32 v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

u16 ReadU16(const u8 *p) {
    u16 v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

f32 ReadF32(const u8 *p) {
    f32 v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

} // namespace

/// @brief Initializes the system.
void KBruteSystem::init() {
    ASSERT(m_taskBuffer);

    auto *sceneCreator = EGG::egg_new<Host::SceneCreatorDynamic>();
    m_sceneMgr = EGG::egg_new<EGG::SceneManager>(sceneCreator);

    System::RaceConfig::RegisterInitCallback(OnInit, nullptr);
    m_sceneMgr->changeScene(0);
}

/// @brief Executes a frame, feeding the next recorded input to the host controller first.
void KBruteSystem::calc() {
    auto *padDir = System::KPadDirector::Instance();
    auto *host = padDir ? padDir->hostController() : nullptr;

    if (host) {
        if (m_frameIdx < m_frameCount) {
            const InputFrame f = frameAt(m_frameIdx);
            host->setInputs(f.buttons, f.stickX, f.stickY, static_cast<System::Trick>(f.trick));
        } else {
            // Recorded sequence exhausted before the race ended (crashed run, or a candidate
            // that's simply shorter than the track) -- coast neutral until calcEnd() cuts it off.
            host->setInputs(0, 0.0f, 0.0f, System::Trick::None);
        }
    }

    m_sceneMgr->calc();

    if (m_goFrame < 0 &&
            System::RaceManager::Instance()->stage() == System::RaceManager::Stage::Race) {
        m_goFrame = static_cast<s32>(m_frameIdx);
    }

    ++m_frameIdx;
}

/// @brief Executes a run: replays the task's input sequence to completion or the frame cap, then
/// reports the outcome. Always returns true -- a candidate that doesn't finish is a normal search
/// outcome, not a process failure.
bool KBruteSystem::run() {
    while (!calcEnd()) {
        calc();
    }

    reportResult();
    return true;
}

/// @brief Parses non-generic command line options. The only accepted option is the task flag.
void KBruteSystem::parseOptions(int argc, char **argv) {
    if (argc < 2) {
        PANIC("Expected task argument!");
    }

    for (int i = 0; i < argc; ++i) {
        std::optional<Host::EOption> flag = Host::Option::CheckFlag(argv[i]);
        if (!flag || *flag == Host::EOption::Invalid) {
            WARN("Expected a flag! Got: %s", argv[i]);
            continue;
        }

        switch (*flag) {
        case Host::EOption::Task:
            ASSERT(i + 1 < argc);
            loadTask(argv[++i]);
            break;
        default:
            PANIC("Invalid flag for brute mode!");
            break;
        }
    }
}

KBruteSystem *KBruteSystem::CreateInstance() {
    ASSERT(!s_instance);
    s_instance = EGG::egg_new<KBruteSystem>();
    return static_cast<KBruteSystem *>(s_instance);
}

void KBruteSystem::DestroyInstance() {
    ASSERT(s_instance);
    auto *instance = s_instance;
    s_instance = nullptr;
    EGG::egg_delete(instance);
}

KBruteSystem::KBruteSystem()
    : m_sceneMgr(nullptr), m_taskBuffer(nullptr), m_courseId(0), m_characterId(0), m_vehicleId(0),
      m_frameCount(0), m_maxFrames(0), m_frameIdx(0), m_goFrame(-1) {}

KBruteSystem::~KBruteSystem() {
    if (s_instance) {
        s_instance = nullptr;
        WARN("KBruteSystem instance not explicitly handled!");
    }

    EGG::egg_delete(m_sceneMgr);
    delete[] m_taskBuffer;
}

/// @brief Loads and validates a task file, and caches the scenario fields OnInit will need. The
/// frame data itself is read lazily out of the retained buffer (see frameAt).
void KBruteSystem::loadTask(const char *path) {
    size_t size = 0;
    const u8 *buffer = Abstract::File::LoadHost(path, size);

    if (!buffer || size < TASK_HEADER_SIZE || std::memcmp(buffer, "KBRT", 4) != 0) {
        PANIC("File is not a valid brute-force task!");
    }

    m_courseId = ReadS32(buffer + 0x08);
    m_characterId = ReadS32(buffer + 0x0C);
    m_vehicleId = ReadS32(buffer + 0x10);
    m_frameCount = ReadU32(buffer + 0x14);
    m_maxFrames = ReadU32(buffer + 0x18);

    if (size < TASK_HEADER_SIZE + m_frameCount * TASK_FRAME_SIZE) {
        PANIC("Task file is smaller than its declared frame count!");
    }

    m_taskBuffer = buffer;
}

KBruteSystem::InputFrame KBruteSystem::frameAt(size_t i) const {
    const u8 *p = m_taskBuffer + TASK_HEADER_SIZE + i * TASK_FRAME_SIZE;

    InputFrame f;
    f.buttons = ReadU16(p);
    f.trick = p[2];
    f.stickX = ReadF32(p + 4);
    f.stickY = ReadF32(p + 8);
    return f;
}

/// @brief The race ends when it's won or the safety frame cap is hit -- there is no player to
/// desync against here, unlike KReplaySystem.
bool KBruteSystem::calcEnd() const {
    const auto *raceManager = System::RaceManager::Instance();

    if (raceManager->stage() == System::RaceManager::Stage::FinishGlobal) {
        return true;
    }

    if (m_frameIdx >= m_maxFrames) {
        return true;
    }

    return false;
}

/// @brief Prints the race outcome as a single line the driving process can parse: whether it
/// finished, the frame count it ran for, the finish time (ms, -1 if unfinished), race completion
/// (10000 per lap, so up to 30000 for a 3-lap race), whether it was driving the wrong way, each
/// lap split (ms, -1 if not reached), goFrame -- the frame index at which the race left Countdown
/// for Race, i.e. the "GO!" moment (frames before it are Intro/Countdown; -1 if the frame cap was
/// hit before the race ever started) -- speed, the kart's current forward speed -- and posX/posY/
/// posZ, its world-space position (Wii/Kinoko convention: Y is up).
/// @details raceCompletion is checkpoint-based: over a short post-start window (a few hundred
/// frames) it can be identical for very different runs simply because none of them have reached
/// the next checkpoint yet, well before it distinguishes a good start from a bad one. speed
/// changes every frame and is the metric TAS players actually judge a start slide / rocket start
/// by, so it's the right fitness signal for a short window even though raceCompletion is right for
/// a full lap or race.
void KBruteSystem::reportResult() const {
    const auto *race = System::RaceManager::Instance();
    const auto &player = race->player();
    const bool finished = race->stage() == System::RaceManager::Stage::FinishGlobal;

    auto toMs = [](const System::Timer &t) {
        return static_cast<int>(t.min) * 60000 + static_cast<int>(t.sec) * 1000 +
                static_cast<int>(t.mil);
    };

    const auto *kart = Kart::KartObjectManager::Instance()->object(0);
    const f32 speed = kart ? kart->move()->speed() : 0.0f;
    const EGG::Vector3f pos = kart ? kart->pos() : EGG::Vector3f::zero;
    // Ground truth for whether a start boost actually triggered and at what tier: the engine's own
    // charge value (see KartState::calcStartBoost/calcHandleStartBoost) and its own resulting boost
    // multiplier (>1.0 while a start/mushroom/trick boost of any kind is active, 1.0 otherwise) --
    // not a re-derivation on the Python side, the literal values Kinoko computed.
    const f32 boostCharge = kart ? kart->state()->startBoostCharge() : 0.0f;
    const f32 boostMultiplier = kart ? kart->move()->boost().multiplier() : 1.0f;
    // The kart's actual orientation quaternion (KartObjectProxy::fullRot() -- the combination of
    // all rotation sources: base rotation, tricks, stunts). Reported raw (not a derived facing-vs-
    // velocity angle) so the Python side can compute whatever it needs from it -- e.g. the forward
    // vector, to check how far the kart's facing direction has diverged from its direction of
    // travel, which raw speed alone can't distinguish from "correctly oriented but just slow".
    const EGG::Quatf rot = kart ? kart->fullRot() : EGG::Quatf(1.0f, 0.0f, 0.0f, 0.0f);
    // The kart's actual facing vector (KartObjectProxy::bodyFront(), the third column of the
    // rotation matrix) -- the same value the game's own movement code uses for m_dir. XZ gives a
    // ground-plane heading (Y is up in this engine's convention) without needing to reconstruct one
    // from the raw quaternion on the Python side.
    const EGG::Vector3f front = kart ? kart->bodyFront() : EGG::Vector3f::ez;
    // Whether a ramp/zipper stunt (KartHalfPipe) has actually been entered right now, not just
    // attempted -- see KartHalfPipe::isInStunt's docstring. Lets the Python side check a
    // candidate's real trick-state entry at a specific frame instead of inferring it from raw
    // stick/trick input alone, which only shows the button press, not whether the engine actually
    // committed to it.
    const bool inStunt = kart && kart->halfPipe() && kart->halfPipe()->isInStunt();
    // The real "is the kart currently executing a jump-trick right now" flag (Status.hh's
    // eStatus::InATrick), as opposed to isInStunt() above (KartHalfPipe, specifically for zipper/
    // half-pipe ramp objects) or merely having pressed a trick-direction input on some frame (which
    // only shows intent, not whether the engine actually entered the trick state).
    const bool inTrick = kart && kart->state()->status().onBit(Kart::eStatus::InATrick);

    std::printf(
            "RESULT finished=%d frames=%zu timeMs=%d completion=%d wrongWay=%d lap1=%d "
            "lap2=%d lap3=%d goFrame=%d speed=%.6f posX=%.6f posY=%.6f posZ=%.6f "
            "boostCharge=%.6f boostMultiplier=%.6f rotW=%.6f rotX=%.6f rotY=%.6f rotZ=%.6f "
            "frontX=%.6f frontY=%.6f frontZ=%.6f inStunt=%d inTrick=%d\n",
            finished ? 1 : 0, m_frameIdx, finished ? toMs(player.raceTimer()) : -1,
            static_cast<int>(player.raceCompletion() * 10000.0f), player.drivingWrongWay() ? 1 : 0,
            player.lapTimer(0).valid ? toMs(player.lapTimer(0)) : -1,
            player.lapTimer(1).valid ? toMs(player.lapTimer(1)) : -1,
            player.lapTimer(2).valid ? toMs(player.lapTimer(2)) : -1, m_goFrame,
            static_cast<double>(speed), static_cast<double>(pos.x), static_cast<double>(pos.y),
            static_cast<double>(pos.z), static_cast<double>(boostCharge),
            static_cast<double>(boostMultiplier), static_cast<double>(rot.w),
            static_cast<double>(rot.v.x), static_cast<double>(rot.v.y),
            static_cast<double>(rot.v.z), static_cast<double>(front.x),
            static_cast<double>(front.y), static_cast<double>(front.z), inStunt ? 1 : 0,
            inTrick ? 1 : 0);
}

/// @brief Initializes the race configuration for a single Local player driven by our task inputs.
void KBruteSystem::OnInit(System::RaceConfig *config, void * /* arg */) {
    auto *self = Instance();
    auto &scenario = config->raceScenario();
    scenario.course = static_cast<Course>(self->m_courseId);
    scenario.playerCount = 1;

    auto &player = scenario.players[0];
    player.type = System::RaceConfig::Player::Type::Local;
    player.character = static_cast<Character>(self->m_characterId);
    player.vehicle = static_cast<Vehicle>(self->m_vehicleId);
    player.driftIsAuto = false;
}

} // namespace Kinoko
