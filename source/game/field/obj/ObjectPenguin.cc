#include "ObjectPenguin.hh"

#include "game/item/ItemObj.hh"

#include "game/kart/KartCollide.hh"

namespace Kinoko::Field {

/// @addr{0x80775624} @addr{0x8077708C}
ObjectPenguin::ObjectPenguin(const System::MapdataGeoObj &params)
    : ObjectCollidable(params), m_hitTimer(0), m_spinSpeed(0.0f) {}

/// @addr{0x80775670} @addr{0x807774A4}
ObjectPenguin::~ObjectPenguin() = default;

/// @addr{0x807756B0}
void ObjectPenguin::init() {
    m_railInterpolator->init(0.0f, 0);
    m_state = State::Walk;
    m_basis = EGG::Vector3f::ez;
    m_hitTimer = 0;
}

/// @addr{0x80775B1C}
void ObjectPenguin::calcRot() {
    constexpr f32 INTERP_RATE = 0.2f;

    m_basis = Interpolate(INTERP_RATE, m_basis, m_railInterpolator->curTangentDir());
    m_basis.normalise();
    setMatrixFromOrthonormalBasisAndPos(m_basis);
}

/// @addr{0x8077588C}
void ObjectPenguin::enterWalk() {
    m_state = State::Walk;
    calcTransform();
    setRotNoFlag(transform().base(2));
}

/// @addr{0x80775C58}
/// @brief A kart in a star, mega mushroom or bullet bill knocks the penguin into a spin (the
/// rumble is left out).
Kart::Reaction ObjectPenguin::onCollision(Kart::KartObject * /*kartObj*/,
        Kart::Reaction reactionOnKart, Kart::Reaction reactionOnObj, EGG::Vector3f & /*hitDepth*/) {
    if (reactionOnObj == Kart::Reaction::UNK_3 || reactionOnObj == Kart::Reaction::UNK_5) {
        startHit();
    }

    return reactionOnKart;
}

/// @addr{0x80775CE0}
s32 ObjectPenguin::onItemCollision(Item::ItemObj * /*item*/, s32 reactionOnItem, s32 reactionOnObj,
        EGG::Vector3f & /*hitDepth*/) {
    if (reactionOnObj == ItemHit::ObjHit || reactionOnObj == ItemHit::ObjHitRaceOnly) {
        startHit();
    }

    return reactionOnItem;
}

/// @addr{0x80775D2C}
/// @brief Starts the 180-frame spin (the animation, effect and sound are left out).
void ObjectPenguin::startHit() {
    if (m_state == State::Hit) {
        return;
    }

    m_state = State::Hit;
    m_hitTimer = HIT_DURATION;
    m_spinSpeed = HIT_SPIN_SPEED;
}

/// @addr{0x80775804}
/// @brief Keeps walking the rail while spinning, then walks normally again.
void ObjectPenguin::calcHit() {
    m_railInterpolator->calc();
    calcPos();
    calcHitSpin();

    if (--m_hitTimer == 0) {
        enterWalk();
    }
}

/// @addr{0x80775950}
/// @brief Turns the penguin about its y axis, slowing by 1 degree per frame down to 12.
void ObjectPenguin::calcHitSpin() {
    constexpr f32 MIN_SPIN_SPEED = 12.0f;

    f32 speed = m_spinSpeed - 1.0f;
    m_spinSpeed = speed;
    if (speed < MIN_SPIN_SPEED) {
        m_spinSpeed = speed + 1.0f;
    }

    calcRotLock();
    EGG::Vector3f rot = this->rot();
    rot.y += m_spinSpeed * DEG2RAD;
    setRot(rot);
    calcTransform();
}

/// @addr{0x80775E60}
ObjectPenguinS::ObjectPenguinS(const System::MapdataGeoObj &params)
    : ObjectPenguin(params), m_stateBeforeHit(State::Walk) {}

/// @addr{0x80776070}
ObjectPenguinS::~ObjectPenguinS() = default;

/// @addr{0x807760B0}
void ObjectPenguinS::init() {
    m_anmTimer = 0;
    m_railInterpolator->init(0.0f, 0);
    m_basis = m_railInterpolator->curTangentDir();
    m_state = State::Walk;
}

/// @addr{0x80776198}
void ObjectPenguinS::calc() {
    switch (m_state) {
    case State::Walk:
        calcWalk();
        break;
    case State::Dive:
        calcDive();
        break;
    case State::Slider:
        calcSlider();
        break;
    case State::SliderSlow:
        calcWalk();
        calcTransform();
        break;
    case State::StandUp:
        calcStandUp();
        break;
    case State::Hit:
        calcHit();
        break;
    }
}

/// @addr{0x80776D90}
/// @brief As the base penguin's, but remembers the state it was hit in.
void ObjectPenguinS::startHit() {
    if (m_state == State::Hit) {
        return;
    }

    m_stateBeforeHit = m_state;
    m_state = State::Hit;
    m_hitTimer = HIT_DURATION;
    m_spinSpeed = HIT_SPIN_SPEED;
}

/// @addr{0x807766E8}
/// @brief Spins along the rail; at the end a penguin hit while sliding stands up, any other walks.
void ObjectPenguinS::calcHit() {
    calcRail();
    calcPos();
    calcHitSpin();

    if (--m_hitTimer != 0) {
        return;
    }

    if (m_stateBeforeHit == State::Slider || m_stateBeforeHit == State::SliderSlow) {
        auto *anmMgr = m_drawMdl->anmMgr();
        anmMgr->playAnim(0.0f, 1.0f, 3);
        m_state = State::StandUp;
        m_anmTimer = anmMgr->activeAnim(Render::AnmType::Chr)->frameCount();
    } else {
        enterWalk();
    }

    calcTransform();
    m_basis = transform().base(2);
}

/// TODO: Maybe not needed
void ObjectPenguinS::loadAnims() {
    std::array<const char *, 4> names = {{
            "walk",
            "dive",
            "slider",
            "stand_up",
    }};

    std::array<Render::AnmType, 4> types = {{
            Render::AnmType::Chr,
            Render::AnmType::Chr,
            Render::AnmType::Chr,
            Render::AnmType::Chr,
    }};

    linkAnims(names, types);
}

/// @addr{0x80776498}
void ObjectPenguinS::calcSlider() {
    calcRail();
    calcPos();
    calcRot();

    if (--m_anmTimer == 0) {
        m_state = State::SliderSlow;
    }
}

/// @addr{0x80776670}
void ObjectPenguinS::calcStandUp() {
    calcRail();
    calcPos();
    calcRot();

    if (--m_anmTimer == 0) {
        enterWalk();
    }
}

/// @addr{0x807768A0}
void ObjectPenguinS::calcRail() {
    if (m_railInterpolator->calc() != RailInterpolator::Status::SegmentEnd) {
        return;
    }

    const auto &curPoint = m_railInterpolator->curPoint();
    u16 setting = curPoint.setting[0];

    if (setting == 0 && (m_state == State::SliderSlow || m_state == State::Hit)) {
        auto *anmMgr = m_drawMdl->anmMgr();
        anmMgr->playAnim(0.0f, 1.0f, 3);
        m_state = State::StandUp;
        m_anmTimer = anmMgr->activeAnim(Render::AnmType::Chr)->frameCount();
    } else if (setting == 1 &&
            (m_state == State::Walk || m_state == State::Dive || m_state == State::Hit)) {
        // Added for Kinoko web: the Dive state (the run before a slide, entered at a setting-2 rail
        // point) never ended, so after the first lap of its rail a penguin ran forever and never
        // slid again. Each rail runs walk -> run (setting 2) -> slide (setting 1) -> stand up
        // (setting 0) -> walk ...
        auto *anmMgr = m_drawMdl->anmMgr();
        anmMgr->playAnim(0.0f, 1.0f, 1);
        m_state = State::Slider;
        m_anmTimer = anmMgr->activeAnim(Render::AnmType::Chr)->frameCount();
    } else if (setting == 2 && m_state == State::Walk) {
        m_state = State::Dive;
    }

    m_railInterpolator->setCurrVel(static_cast<f32>(curPoint.setting[1]));
}

} // namespace Kinoko::Field
