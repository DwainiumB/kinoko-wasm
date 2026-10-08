#include "host/FrameState.hh"

#include <game/item/ItemDirector.hh>
#include <game/kart/KartMove.hh>
#include <game/kart/KartObjectManager.hh>
#include <game/system/CourseMap.hh>
#include <game/system/KPadDirector.hh>
#include <game/system/RaceManager.hh>

// fprintf() takes the f32 state values as doubles; the promotion is intended, so the project-wide -Werror is relaxed here.
#pragma GCC diagnostic ignored "-Wdouble-promotion"

namespace Kinoko::Host {

void WriteStateHeader(FILE *file, const char *prefix) {
    fputs(prefix, file);
    fputs("frame,stage,timerMs,raceFrame,"
          "px,py,pz,qx,qy,qz,qw,mqx,mqy,mqz,mqw,"
          "evx,evy,evz,ivx,ivy,ivz,"
          "speed,speedRatio,driftState,mtCharge,hopStickX,kclSpeedFactor,flags,"
          "stickX,stickY,buttons,trick,"
          "completion,checkpoint,lap,wrongWay,mushrooms",
            file);
    for (int i = 0; i < PATH_LOOKAHEAD; ++i) {
        fprintf(file, ",pmx%d,pmz%d,plx%d,plz%d,prx%d,prz%d", i, i, i, i, i, i);
    }
    fputs("\n", file);
}

void WriteStateRow(FILE *file, u32 frame, const char *prefix) {
    const auto *race = System::RaceManager::Instance();
    auto *object = Kart::KartObjectManager::Instance()->object(0);
    const auto *move = object->move();
    const auto &status = object->status();
    const auto &player = race->player();
    const auto &input = System::KPadDirector::Instance()->playerInput(0).currentState();
    const auto &timer = race->timerManager().currentTimer();

    using Kart::eStatus;
    u32 flags = 0;
    auto set = [&](u32 bit, bool on) { flags |= on ? bit : 0; };
    set(1 << 0, status.onBit(eStatus::Accelerate));
    set(1 << 1, status.onBit(eStatus::Brake));
    set(1 << 2, status.onBit(eStatus::TouchingGround));
    set(1 << 3, status.onBit(eStatus::Hop));
    set(1 << 4, status.onBit(eStatus::DriftManual, eStatus::DriftAuto));
    set(1 << 5, status.onBit(eStatus::Boost));
    set(1 << 6, status.onBit(eStatus::MushroomBoost));
    set(1 << 7, status.onBit(eStatus::Wheelie));
    set(1 << 8, status.onBit(eStatus::InATrick, eStatus::ZipperTrick));
    set(1 << 9, status.onBit(eStatus::AirtimeOver20));
    set(1 << 10, status.onBit(eStatus::WallCollision));
    set(1 << 11, status.onBit(eStatus::JumpPad));
    set(1 << 12, status.onBit(eStatus::ZipperBoost, eStatus::HalfPipeRamp));
    set(1 << 13, status.onBit(eStatus::InCannon));
    set(1 << 14, status.onBit(eStatus::BeforeRespawn, eStatus::InRespawn));
    set(1 << 15, status.onBit(eStatus::Burnout));
    set(1 << 16, status.onBit(eStatus::LargeFlipHit, eStatus::Shocked, eStatus::Crushed));
    set(1 << 17, move->ssmtCharged());

    const auto &pos = object->pos();
    const auto &rot = object->fullRot();
    const auto &mainRot = object->mainRot();
    const auto &ev = object->extVel();
    const auto &iv = object->intVel();
    int mushrooms = 0;
    if (auto *items = Item::ItemDirector::Instance()) {
        mushrooms = items->itemInventory(0).currentCount();
    }
    int timerMs = static_cast<int>(timer.min) * 60000 + static_cast<int>(timer.sec) * 1000 +
            static_cast<int>(timer.mil);

    fputs(prefix, file);
    fprintf(file,
            "%u,%d,%d,%d,"
            "%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,"
            "%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,"
            "%.9g,%.9g,%d,%d,%d,%.9g,%u,"
            "%.9g,%.9g,%d,%d,"
            "%.9g,%d,%d,%d,%d",
            frame, static_cast<int>(race->stage()), timerMs, static_cast<int>(race->timer()), pos.x,
            pos.y, pos.z, rot.v.x, rot.v.y, rot.v.z, rot.w, mainRot.v.x, mainRot.v.y, mainRot.v.z,
            mainRot.w, ev.x, ev.y, ev.z, iv.x, iv.y, iv.z, move->speed(), move->speedRatio(),
            static_cast<int>(move->driftState()), static_cast<int>(move->mtCharge()),
            static_cast<int>(move->hopStickX()), move->kclSpeedFactor(), flags, input.stick.x,
            input.stick.y, static_cast<int>(input.buttons), static_cast<int>(input.trick),
            player.raceCompletion(), static_cast<int>(player.checkpointId()),
            static_cast<int>(player.maxLap()), player.drivingWrongWay() ? 1 : 0, mushrooms);

    // Road ahead: the player's checkpoint and the next ones along the first branch. Past the last
    // checkpoint the final one is repeated so the columns are always filled.
    auto *map = System::CourseMap::Instance();
    System::MapdataCheckPoint *cp = map ? map->getCheckPoint(player.checkpointId()) : nullptr;
    System::MapdataCheckPoint *last = cp;
    for (int i = 0; i < PATH_LOOKAHEAD; ++i) {
        if (cp) {
            last = cp;
        }
        if (last) {
            fprintf(file, ",%.9g,%.9g,%.9g,%.9g,%.9g,%.9g", last->midpoint().x, last->midpoint().y,
                    last->left().x, last->left().y, last->right().x, last->right().y);
        } else {
            fputs(",0,0,0,0,0,0", file);
        }
        cp = (cp && cp->nextCount()) ? cp->nextPoint(0) : nullptr;
    }
    fputs("\n", file);
    fflush(file);
}

} // namespace Kinoko::Host
