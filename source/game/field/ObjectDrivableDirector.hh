#pragma once

#include "game/field/obj/ObjectCollidable.hh"
#include "game/field/obj/ObjectDrivable.hh"
#include "game/field/obj/ObjectObakeManager.hh"

#include <egg/core/Allocator.hh>

#include <vector>

namespace Kinoko {

namespace Host {

class Context;

} // namespace Host

namespace Field {

class ObjectDrivableDirector : EGG::Disposer {
    friend class Host::Context;

public:
    void init();
    void calc();
    void addObject(ObjectDrivable *obj);
    void createObakeManager(const System::MapdataGeoObj &params);

    [[nodiscard]] bool checkSpherePartial(f32 radius, const EGG::Vector3f &pos,
            const EGG::Vector3f &prevPos, KCLTypeMask mask, CollisionInfoPartial *info,
            KCLTypeMask *maskOut, u32 timeOffset);
    [[nodiscard]] bool checkSpherePartialPush(f32 radius, const EGG::Vector3f &pos,
            const EGG::Vector3f &prevPos, KCLTypeMask mask, CollisionInfoPartial *info,
            KCLTypeMask *maskOut, u32 timeOffset);
    [[nodiscard]] bool checkSphereFull(f32 radius, const EGG::Vector3f &pos,
            const EGG::Vector3f &prevPos, KCLTypeMask mask, CollisionInfo *info,
            KCLTypeMask *maskOut, u32 timeOffset);
    [[nodiscard]] bool checkSphereFullPush(f32 radius, const EGG::Vector3f &pos,
            const EGG::Vector3f &prevPos, KCLTypeMask mask, CollisionInfo *info,
            KCLTypeMask *maskOut, u32 timeOffset);
    [[nodiscard]] bool checkSphereCachedPartial(f32 radius, const EGG::Vector3f &pos,
            const EGG::Vector3f &prevPos, KCLTypeMask mask, CollisionInfoPartial *info,
            KCLTypeMask *maskOut, u32 timeOffset);
    [[nodiscard]] bool checkSphereCachedPartialPush(f32 radius, const EGG::Vector3f &pos,
            const EGG::Vector3f &prevPos, KCLTypeMask mask, CollisionInfoPartial *info,
            KCLTypeMask *maskOut, u32 timeOffset);
    [[nodiscard]] bool checkSphereCachedFullPush(f32 radius, const EGG::Vector3f &pos,
            const EGG::Vector3f &prevPos, KCLTypeMask mask, CollisionInfo *info,
            KCLTypeMask *maskOut, u32 timeOffset);
    void colNarScLocal(f32 radius, const EGG::Vector3f &pos, KCLTypeMask mask, u32 timeOffset);

    [[nodiscard]] ObjectObakeManager *obakeManager() const {
        return m_obakeManager;
    }

    /// @brief Drivable-platform objects (e.g. the Kinoko-family bouncy mushrooms), for the web
    /// renderer -- these register here instead of ObjectDirector::m_objects (they're driven ON,
    /// not just collided with), so kinoko_get_objects() (web_bridge.cc) needs both lists to show
    /// every Kinoko-simulated object's real position/scale, not just ObjectCollidable ones.
    [[nodiscard]] const fixed_vector<ObjectDrivable *> &objects() const {
        return m_objects;
    }

    static ObjectDrivableDirector *CreateInstance();
    static void DestroyInstance();

    [[nodiscard]] static ObjectDrivableDirector *Instance() {
        return s_instance;
    }

private:
    EGG_NEW_DELETE_FRIEND

    ObjectDrivableDirector();
    ~ObjectDrivableDirector() override;

    fixed_vector<ObjectDrivable *> m_objects;     ///< All objects live here
    fixed_vector<ObjectDrivable *> m_calcObjects; ///< Objects needing calc() live here too.
    ObjectObakeManager *m_obakeManager;           ///< Manages rGV2 blocks and spatial indexing.

    static constexpr size_t MAX_OBJECTS = 400; ///< Maximum number of objects in the vectors

    static ObjectDrivableDirector *s_instance;
};

} // namespace Field

} // namespace Kinoko
