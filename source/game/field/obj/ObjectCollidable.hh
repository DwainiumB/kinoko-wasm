#pragma once

#include "game/field/obj/ObjectBase.hh"

#include "game/field/ObjectCollisionBase.hh"

namespace Kinoko {

namespace Kart {

class KartObject;
enum class Reaction;

} // namespace Kart

namespace Item {

class ItemObj;

} // namespace Item

namespace Field {

/// @brief The values of the item hit tables: GeoHitTableItem.bin says what a course object does to
/// an item, GeoHitTableItemObj.bin what the item does to the object.
namespace ItemHit {

enum : s32 {
    None = 0,
    Bounce = 1,             ///< The item bounces off like off a wall.
    Break = 2,              ///< The item is knocked aside and breaks.
    ObjHit = 3,             ///< The object is hit.
    ObjHitHard = 4,         ///< The object is hit harder (some objects also get pushed).
    ObjHitRaceOnly = 5,     ///< ObjHit in offline races other than Time Trials (0x8081F778).
    ObjHitHardRaceOnly = 6, ///< ObjHitHard in offline races other than Time Trials.
    Pass = 7,               ///< The item never collides with the object.
};

} // namespace ItemHit

class ObjectCollidable : public ObjectBase {
public:
    ObjectCollidable(const System::MapdataGeoObj &params);
    ObjectCollidable(const char *name, const EGG::Vector3f &pos, const EGG::Vector3f &rot,
            const EGG::Vector3f &scale);
    ~ObjectCollidable() override;

    void load() override;
    void calcCollisionTransform() override;
    [[nodiscard]] f32 getCollisionRadius() const override;

    virtual void loadAABB(f32 maxSpeed);
    virtual void loadAABB(f32 radius, f32 maxSpeed);
    virtual void processKartReactions(Kart::KartObject *kartObj, Kart::Reaction &reactionOnKart,
            Kart::Reaction &reactionOnObj);

    /// @addr{0x8068179C}
    virtual Kart::Reaction onCollision(Kart::KartObject * /*kartObj*/,
            Kart::Reaction reactionOnKart, Kart::Reaction /*reactionOnObj*/,
            EGG::Vector3f & /*hitDepth*/) {
        return reactionOnKart;
    }

    virtual void processItemReactions(const Item::ItemObj *item, s32 &reactionOnItem,
            s32 &reactionOnObj);

    /// @addr{0x806817A4}
    /// @brief An item touched this object (vtable +0xC4): the object reacts to `reactionOnObj`.
    /// @param item Can be null in the game (the overrides check it); never null in Kinoko.
    /// @return What happens to the item.
    virtual s32 onItemCollision(Item::ItemObj * /*item*/, s32 reactionOnItem, s32 /*reactionOnObj*/,
            EGG::Vector3f & /*hitDepth*/) {
        return reactionOnItem;
    }

    virtual void onWallCollision(Kart::KartObject *, const EGG::Vector3f &) {}
    virtual void onObjectCollision(Kart::KartObject *) {}
    virtual bool checkCollision(ObjectCollisionBase *lhs, EGG::Vector3f &dist);

    /// @addr{0x8068173C}
    virtual const EGG::Vector3f &getCollisionTranslation() const {
        return EGG::Vector3f::zero;
    }

    /// @addr{0x80573518}
    [[nodiscard]] virtual ObjectCollisionBase *collision() const {
        return m_collision;
    }

protected:
    void createCollision() override;

    /// @addr{0x806816B8}
    [[nodiscard]] virtual const EGG::Vector3f &collisionCenter() const {
        return EGG::Vector3f::zero;
    }

    void registerManagedObject();

    ObjectCollisionBase *m_collision;
};

} // namespace Field

} // namespace Kinoko
