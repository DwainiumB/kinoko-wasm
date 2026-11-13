#include "ObjectItemboxFloor.hh"

#include "game/field/CollisionDirector.hh"
#include "game/field/ObjectDirector.hh"
#include "game/field/ObjectDrivableDirector.hh"

namespace Kinoko::Field {

/// @addr{0x80829280}
/// @brief Drops the box from its raised point in steps of 50 until its sphere (the radius of its
/// flow entry) touches the floor, pushes it out, and lets the floor keep it if the floor moves.
void ObjectItemboxFloor::init() {
    constexpr KCLTypeMask MASK = 0x6CEBDFFF;
    constexpr f32 STEP = 50.0f;  ///< 0x808AC710
    constexpr f32 LOWEST = 0.0f; ///< 0x808AC6E0

    const auto &flowTable = ObjectDirector::Instance()->flowTable();
    f32 radius =
            static_cast<f32>(parse<s16>(flowTable.set(flowTable.slot(id()))->params.sphere.radius));

    // The wavy road, if the course has one (Bowser's Castle).
    ObjectTwistedWay *road = nullptr;
    for (auto *obj : ObjectDrivableDirector::Instance()->objects()) {
        if (obj->id() == ObjectId::TwistedWay) {
            road = static_cast<ObjectTwistedWay *>(obj);
            break;
        }
    }

    auto *colDir = CollisionDirector::Instance();
    while (pos().y > LOWEST) {
        CollisionInfo info;
        info.bbox.setZero();
        KCLTypeMask maskOut = KCL_NONE;
        EGG::Vector3f probe = pos();

        if (colDir->checkSphereFullPush(radius, probe, EGG::Vector3f::inf, MASK, &info, &maskOut,
                    0)) {
            setPos(pos() - info.tangentOff);

            // The floor that answered the check keeps the box (vtable +0x104 / +0x110).
            CollisionInfo roadInfo;
            roadInfo.bbox.setZero();
            KCLTypeMask roadMask = KCL_NONE;
            if (road &&
                    road->checkSphereFull(radius, probe, EGG::Vector3f::inf, MASK, &roadInfo,
                            &roadMask, 0)) {
                m_road = road;
                m_road->attachPoint(m_point, pos());
            }

            break;
        }

        setPos(pos() - EGG::Vector3f::ey * STEP);
    }
}

/// @addr{0x8082945C}
/// @brief The item box's calc, then the box follows its floor, raised by KMP setting 6.
void ObjectItemboxFloor::calc() {
    ObjectItembox::calc();

    if (!m_road) {
        return;
    }

    EGG::Vector3f carried;
    EGG::Vector3f up;
    m_road->followPoint(m_point, carried, up);

    // + the vector at 0x809C4324, (0, 180, 0) (the item box's float height; matched against the
    // game's positions on Bowser's Castle), + ey * setting 6.
    constexpr EGG::Vector3f FLOAT_HEIGHT = EGG::Vector3f(0.0f, 180.0f, 0.0f);
    f32 height = static_cast<f32>(static_cast<s16>(m_mapObj->setting(6)));
    setPos((carried + FLOAT_HEIGHT) + EGG::Vector3f::ey * height);
}

} // namespace Kinoko::Field
