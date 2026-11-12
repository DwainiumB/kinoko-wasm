#include "ObjectBreakable.hh"

#include "game/field/ObjectDirector.hh"

#include "game/item/ItemDirector.hh"
#include "game/item/ItemInventory.hh"
#include "game/item/ItemSlotData.hh"

#include "game/kart/KartObject.hh"

#include "game/system/RaceConfig.hh"

namespace Kinoko::Field {

/// @addr{0x8076ED1C}
void ObjectBreakable::calc() {
    if (m_state == 2) {
        if (m_respawnTimer < 1) {
            return;
        }

        if (--m_respawnTimer == 0) {
            respawn();
        }
    } else if (m_state == 3) {
        calcFall();
    }
}

/// @addr{0x8076F0AC}
/// @details The mission counter and the rumble are left out.
Kart::Reaction ObjectBreakable::onCollision(Kart::KartObject *kartObj,
        Kart::Reaction reactionOnKart, Kart::Reaction reactionOnObj, EGG::Vector3f & /*hitDepth*/) {
    if (reactionOnObj == Kart::Reaction::UNK_3) {
        breakByKart(kartObj);
    } else if (reactionOnObj == Kart::Reaction::UNK_5) {
        breakByKartUntimed(kartObj);
    }

    return reactionOnKart;
}

/// @addr{0x8076F1DC}
/// @details The mission counter is left out.
s32 ObjectBreakable::onItemCollision(Item::ItemObj *item, s32 reactionOnItem, s32 reactionOnObj,
        EGG::Vector3f &hitDepth) {
    if (!item) {
        return reactionOnItem;
    }

    if (reactionOnObj == ItemHit::ObjHit) {
        breakByItem(item, hitDepth);
    } else if (reactionOnObj == ItemHit::ObjHitRaceOnly) {
        breakByItemUntimed(item, hitDepth);
    }

    return reactionOnItem;
}

/// @addr{0x8076EDE4}
/// @brief Breaks the object. With items on, it drops a random item (setting 2: the chance in
/// percent, setting 1: the item box setting) towards dir.
/// @details Hiding the model (+0x68) is left out.
void ObjectBreakable::breakObject(u8 playerIdx, const EGG::Vector3f &dir) {
    constexpr f32 PERCENT = 100.0f;
    constexpr f32 MAX_UP_SPEED = 3.0f;
    constexpr f32 MIN_UP_SPEED = 2.0f;
    constexpr f32 DIR_SCALE = 2.0f;
    constexpr f32 DROP_HEIGHT = 300.0f;

    using ItemMode = System::RaceConfig::Scenario::ItemMode;

    f32 dropChance = static_cast<f32>(static_cast<s16>(m_mapObj->setting(2)));
    auto &random = ObjectDirector::Instance()->random();
    bool itemsOn = System::RaceConfig::Instance()->raceScenario().itemMode != ItemMode::None;

    if (random.getF32(PERCENT) < dropChance && itemsOn) {
        EGG::Vector3f vel = dir * DIR_SCALE;
        vel.y = MIN_UP_SPEED + random.getF32(MAX_UP_SPEED - MIN_UP_SPEED);

        // The game turns the velocity about the object's x angle only.
        calcRotLock();
        EGG::Matrix34f mat = EGG::Matrix34f::ident;
        mat.makeR(EGG::Vector3f(rot().x, 0.0f, 0.0f));
        vel = mat.ps_multVector(vel);

        auto *itemDir = Item::ItemDirector::Instance();
        Item::ItemId id = Item::ItemSlotData::Instance()->decideItem(m_mapObj->setting(1), 1, true,
                true, nullptr);
        Item::ItemObjKind kind = Item::ItemInventory::Behaviour(id).objKind;
        EGG::Vector3f itemPos = pos() + EGG::Vector3f(0.0f, DROP_HEIGHT, 0.0f);
        itemDir->spawnFromObject(kind, itemPos, vel, playerIdx);
    }

    m_state = 2;
    disableCollision();
}

} // namespace Kinoko::Field
