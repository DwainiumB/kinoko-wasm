#include "ObjectItembox.hh"

#include "game/item/ItemDirector.hh"

#include "game/kart/KartObject.hh"
#include "game/kart/KartParam.hh"

#include "game/system/RaceManager.hh"

namespace Kinoko::Field {

/// @addr{0x808285B0}
/// @details The box floats 180 units plus its KMP setting 6 above its point, at scale 0.9. In a
/// split-screen battle its setting 3 becomes 1; Kinoko has one screen.
ObjectItembox::ObjectItembox(const System::MapdataGeoObj &params)
    : ObjectCollidable(params), m_active(true), m_timer(0) {
    constexpr f32 SCALE = 0.9f;
    constexpr f32 HEIGHT = 180.0f;
    constexpr u16 SPIN_RANGE = 180;
    constexpr s32 RESPAWN_FRAMES = 150;
    constexpr s32 RESPAWN_FRAMES_LONG = 600;      // 0x808D79D8
    constexpr s32 RESPAWN_FRAMES_VERY_LONG = 900; // 0x808D79D6

    m_screenMode = params.setting(3);
    setScale(EGG::Vector3f(SCALE, SCALE, SCALE));

    EGG::Vector3f raised = pos() + EGG::Vector3f(0.0f, HEIGHT, 0.0f);
    f32 height = static_cast<f32>(static_cast<s16>(params.setting(6)));
    setPos(raised + EGG::Vector3f::ey * height);

    m_spinPhase = System::RaceManager::Instance()->random2().getU32(SPIN_RANGE);

    u16 respawnMode = params.setting(5);
    if (static_cast<u16>(respawnMode - 4) < 2) {
        m_respawnFrames = RESPAWN_FRAMES_LONG;
    } else if (respawnMode == 6) {
        m_respawnFrames = RESPAWN_FRAMES_VERY_LONG;
    } else {
        m_respawnFrames = RESPAWN_FRAMES;
    }
}

ObjectItembox::~ObjectItembox() = default;

/// @addr{0x80828860}
/// @brief A broken box stays hidden for its respawn time; a box that came back can be driven
/// through again from its 10th frame.
void ObjectItembox::calc() {
    constexpr s32 COLLISION_DELAY = 10; // 0x808D79D4

    if (!m_active) {
        if (m_timer++ < m_respawnFrames) {
            disableCollision();
        } else {
            m_timer = 0;
            m_active = true;
        }
    } else {
        ++m_timer;
        if (m_timer < COLLISION_DELAY) {
            disableCollision();
        } else {
            enableCollision();
        }
    }
}

/// @addr{0x80828CB8}
/// @brief The box breaks and the racer's roulette starts. Boxes with KMP settings give the items
/// of the special table: setting 1 for humans, setting 2 for CPUs; setting 5 = 2, 3 or 4 makes a
/// short roulette. The controller rumble, the sounds and effects, the CPU's record of the box
/// (0x809C21D0 +0x68, +0xAD and +0x100) and the mission counter are not ported.
Kart::Reaction ObjectItembox::onCollision(Kart::KartObject *kartObj, Kart::Reaction reactionOnKart,
        Kart::Reaction /*reactionOnObj*/, EGG::Vector3f & /*hitDepth*/) {
    if (!m_active) {
        return reactionOnKart;
    }

    auto &kartItem = Item::ItemDirector::Instance()->kartItem(kartObj->param()->playerIdx());
    if (!m_mapObj) {
        kartItem.startRoulette(0, 0, false);
    } else {
        u16 respawnMode = m_mapObj->setting(5);
        bool isShort = static_cast<u16>(respawnMode - 2) <= 2;
        kartItem.startRoulette(m_mapObj->setting(1), m_mapObj->setting(2), isShort);
    }

    m_timer = 0;
    m_active = false;
    disableCollision();

    return reactionOnKart;
}

} // namespace Kinoko::Field
