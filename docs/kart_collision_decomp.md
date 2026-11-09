# Kart-to-kart collision: decompilation notes (PAL RMCP01)

Annotated pseudo-C of the game's kart-vs-kart collision, written from Ghidra 12.1.3 output plus a hand read of the
assembly (capstone with paired singles). Every field is named by role; the raw offset is kept in the comment. These
notes are what `KartCollide::calcPlayerCollision` and its helpers in `source/game/kart/KartCollide.cc` were ported from.
No game bytes are reproduced here.

How the listing was produced (all outside the repo): `main.dol` sections plus `StaticR.rel` relocated in place
(sections at module base + file offset, base `0x805102E0`, `.text` at `0x805103B4`, `.bss` at `0x809BD6E0`; 114766
relocations applied), wrapped as an ELF, r2 = `0x8038EFA0`, r13 = `0x8038CC00`, names from em-eight's symbol lists.
Stock Ghidra has no Gekko language, so a scratch copy had its paired-single opcodes rewritten to the scalar op of the
same length (only 30 such instructions in the region, almost all register saves; the two inlined `Vector3f` adds were
read from the real disassembly). Ghidra's analysis wrongly marked `EGG::Vector3f::normalise` as non-returning, which
truncated `0x8056F7F0` to 0x37C bytes; clearing that flag and the call-site flow overrides recovered the full 0x1530.

Conventions below: `self` is the kart whose `KartSub::calcPass1` is running, `other` is the kart found by the box
search. `status bit N` is Kinoko's `eStatus` numbering: word `N / 32` at `KartState + 0x4 + 4 * (N / 32)`, value bit
`N % 32`. Constants are the floats in the rodata pool at `0x80891730` unless noted.

## Call graph

```
KartSub::calcPass1                                   0x80596CFC
  BoxColUnit::search(self.boxColUnit, 0x1F)          0x80786FA8   (bit 0 = drivers, 3 = objects, 4 = drivables, ...)
  while (other = BoxColManager::getNextPlayer())     0x80785D8C   (iterator at +0x428, flag bit 0 = driver)
      self.collide->checkPlayerCollision(other)      0x8056F7F0   <- this document
  if (status bits 82 | 83) skip object/item collision
  KartCollide::calcObjectCollision                   0x80571F10   (runs AFTER the kart-kart check)
  KartCollide::checkItemCollision                    0x8057257C   (out of scope)
  ...
  KartCollide::findCollision / calcFloorEffect       0x80572C20 / 0x80571634
  KartCollide::calcPlayerBumpTimer                   0x805713B8   (decrement, floor at 0)
  KartCollide::calcFloorMomentRate                   0x805713FC

KartObjectManager::calc                              0x8058FFE8
  for every kart: state.kartPairMask(+0x84) = 0, collide.tangentOff(+0x30) = 0, collide.movement(+0x3C) = 0
  for every kart: kart.calc()                        (pass 0 and pass 1, kart 0 first)

checkPlayerCollision                                 0x8056F7F0  (0x1530)
  checkPlayerHitboxOverlap                           0x8056EEF4  (0x290)   all hitbox pairs, push / contact boxes
  applyPlayerBump(+1, self -> other)                 0x80570DA8  (0x48C)   speed loss, force, wrench, spin kick
  applyPlayerBump(-1, other -> self)                 0x80570DA8
  KartMove::startWheelieBumpWobble (bikes)           0x80579F6C  (0x78)
  KartMove vtable +0x30  (kart: empty 0x80570D20,    bike: 0x80588FC0 -> cancelWheelie 0x805883C4)
KartMove::calc                                       0x805788DC
  KartMove::calcWheelieBumpWobble                    0x80579FE4  (0x158)   after KartJump::calc, before m_bumpTimer--
```

The six unnamed functions right after `0x8056F7F0` and the one before it were checked against every `b`/`bl` and every
pointer-sized word in the image: only `0x80570DA8` is called by it. `0x80571234` is called from the slipstream code
(`0x80586FA8`), `0x80571484` from `activateOob`, `0x80571570` from `0x80571484`; `0x8056F73C`, `0x80570D24` and
`0x805715F4` have no branch or pointer reference at all. `0x80570D20` (a lone `blr` just before `0x80570D24`) is the
kart variant of KartMove vtable slot `+0x30`. None of the others is part of kart-kart collision.

## Offsets used

| object | offset | field |
|---|---|---|
| KartAccessor | +0x00 KartSettings (player index u8 at +0x10), +0x04 KartState, +0x08 KartBody (+0x90 KartPhysics; physics +0x04 KartDynamics, +0x08 hitbox group, +0xE4 z axis), +0x14 driver model, +0x28 KartMove, +0x2C KartAction, +0x30 KartCollide, +0x38 per-kart stat counters | |
| KartState | +0x04..+0x14 status words, +0x84 `kartPairMask` u16 (bit i = pair with player i already handled this frame) | |
| KartCollide | +0x04 proxy, +0x14 `boundingRadius`, +0x18 `playerBumpTimer` s16, +0x1C `floorMomentRate`, +0x30 `tangentOff`, +0x3C `movement` | |
| KartMove | +0x20 `speed`, +0x38 `smoothedUp`, +0x5C `dir`, +0x10C boost timers, +0x118 boost active mask u16 (type t at bit 2t: 0x1 mini-turbo / start / respawn boost, 0x4 mushroom / boost panel, 0x10 trick / zipper; `& 4` was first misread as trick / zipper, corrected after the `test2` Dolphin recording), +0x23C `wheelieBumpWobbleTimer` s16, +0x23E `wheelieBumpWobblePhase` u16, +0x240 `wheelieBumpWobbleAngle` f32 | |
| KartDynamics | +0x68 `pos`, +0x74 `extVel`, +0xA4 `angVel0`, +0xE4 `angVel2`, total force via `addForce` 0x805B6388 | |
| KartAction | +0x30 `hitDepth`; vtable +0x0C = `start` 0x805675DC | |
| Stats | +0x08 `weightClass` (u32), +0x10 `weight`, +0x14 `bumpDeviationLevel` | |
| Hitbox (stride 0x30, array at group +0x8C, count u16 at group +0) | +0x04 radius, +0x0C world position | |

## Status bits read or written

| bit | Kinoko name | role here |
|---|---|---|
| 18 | TouchingGround | wobble only on the ground |
| 26 | MushroomBoost | battle-only balloon steal (not ported, battle mode) |
| 29 | Wheelie | bike in a wheelie gets the wobble |
| 32 | InAction | no wobble while in an action |
| 33, 35, 36, 77, 78, 102 | TriggerRespawn, CannonStart, InCannon, InRespawn, AfterRespawn, AfterCannon | collision disabled |
| 40, 72, 82, 83, 89 | added as `NoKartCollision40` etc. | collision disabled; set by item/cannon/vanish code not in Kinoko |
| 80 && 81 | Crushed && `CrushAnimation` | collision disabled |
| 134 | added as `Ghost` | collision disabled (KartState+0x14 bit 6, `isGhost` 0x80590678) |
| 15 | AirtimeOver20 | bump force halved |
| 42 | OverZipper | bump force x0.2 |
| 56 | added as `FloorMomentRateReset` | set on the bumped kart (also set by the game's `startFloorMomentRate` 0x805713D8) |
| 63, 79, 91 | added as `InStar`, `InMega`, `InBullet` | item outcomes instead of a bump |
| 64, 65 | added as `SlipstreamCharge`, `SlipstreamBoost` | bump weight raised to the other kart's while boosting (see the slipstream section below) |
| 66 | added as `WheelieBumpWobble` | wobble active |
| 91 (other) | InBullet | no bump applied to a kart in a bullet |

## checkPlayerCollision 0x8056F7F0

```c
// Same predicate, inlined twice (self, then other): kart-kart collision is skipped while any of these holds.
static bool isPlayerCollisionDisabled(KartObjectProxy *kart) {
    u32 *s = &kart->state->status;                                    // +0x04 .. +0x14
    return (s[1] & 0x102) /* 33 TriggerRespawn, 40 */ || (s[2] & 0x020C6000) /* 77, 78, 82, 83, 89 */
        || kart->isGhost() /* 134 */ || (s[1] & 0x18) /* 35 CannonStart, 36 InCannon */ || (s[2] & 0x100) /* 72 */
        || ((s[2] & 0x10000) && (s[2] & 0x20000)) /* 80 Crushed && 81 */ || (s[3] & 0x40) /* 102 AfterCannon */;
}

void KartCollide::checkPlayerCollision(KartObjectProxy *other) {
    if (isPlayerCollisionDisabled(self)) return;
    if (isPlayerCollisionDisabled(other)) return;

    u8 otherIdx = other->playerIdx(), selfIdx = self->playerIdx();
    if (self->state->kartPairMask & (1 << otherIdx)) return;          // +0x84: pair already done this frame
    if (selfIdx == otherIdx) return;
    self->state->kartPairMask  |= 1 << otherIdx;                      // both sides marked: one pass per pair
    other->state->kartPairMask |= 1 << selfIdx;

    BoundBox3f pushBox = {0, 0};                                      // sp+0xA8: min/max of (dir to other * depth)
    BoundBox3f contactBox = {0, 0};                                   // sp+0x90: min/max of contact points
    Vector3f pushDir = Vector3f::zero;                                // sp+0x80, copied from the zero vector 0x802A4148
    if (!checkPlayerHitboxOverlap(&pushBox, &contactBox, other)) return;

    pushDir = pushBox.min + pushBox.max;                              // paired-single add (x,y), then z
    f32 depth = pushDir.normalise();                                  // f31

    Vector3f offset = pushDir * (-0.5f * depth);                      // 0x80891748 = -0.5; factor rounded first
    self->collide->tangentOff += offset;  self->collide->movement += offset;     // +0x30, +0x3C
    other->collide->tangentOff += -offset; other->collide->movement += -offset;

    // Visual only: spark effect at contactBox.min + contactBox.max + pushDir * (0.5 * -depth) (0x806A6428 on the
    // effect manager 0x809C21D0 +0x68 [selfIdx]), driver/kart model shakes (0x807D396C), sounds (0x807091AC).

    bool sameTeam = false;                                            // RaceConfig(0x809BD728)+0xB90 bit 1 = teams:
    if (raceConfig.modeFlags & 2) {                                   // players[i].team at +0xF4 + 0xF0 * i, 2 = none
        s32 ts = team(selfIdx), to = team(otherIdx);
        sameTeam = ts != 2 && to != 2 && ts == to;
    }
    bool battle = gameMode in {3, 9, 10};                             // +0xB70; used for battle scoring only

    bool selfStar = bit 63, otherStar, selfMega = bit 79, otherMega, selfCrushed = bit 80, otherCrushed,
         selfBullet = bit 91, otherBullet;
    bool doBump = false;
    if (sameTeam) {
        doBump = true;
    } else if (selfBullet != otherBullet) {
        if (otherBullet && !selfBullet && !selfStar) {
            self->action->hitDepth = -pushDir;                        // 0x80590D38: KartAction+0x30
            if (self->action->start(6, 1, 1, 0, otherIdx, 0)) { /* sound 0x110, stat counters 0x805918E0/BC */ }
        } else if (selfBullet && !otherBullet && !otherStar) {
            other->action->hitDepth = pushDir;
            if (other->action->start(6, ...)) { ... }
        } else doBump = true;
    } else if (selfStar != otherStar) {
        if (otherStar && !selfBullet && !selfStar) {
            self->action->hitDepth = -pushDir;
            if (self->action->start(3, 1, 1, 0, otherIdx, 0)) { /* driver anim 5 + rumble on other, battle score */ }
        } else if (selfStar && !otherBullet && !otherStar) {
            other->action->hitDepth = pushDir;
            if (other->action->start(3, ...)) { ... }
        } else doBump = true;
    } else if ((selfMega && !otherMega && !otherCrushed) || (otherMega && !selfMega && !selfCrushed)) {
        if (otherMega && !selfMega)      self->action->start(13, 1, 0, 0, otherIdx, 0);   // no hitDepth here
        else if (selfMega && !otherMega) other->action->start(13, ...);
        else doBump = true;
    } else {
        doBump = true;
    }
    if (!doBump) return;

    // Each kart's lateral axis: pose column 0 (Kinoko bodyForward) made perpendicular to KartMove::dir (+0x5C).
    Vector3f otherSide = other->bodyForward().perpInPlane(other->move->dir, true);
    Vector3f otherHitDir = pushDir.proj(otherSide);  otherHitDir.normalise();          // projUnit 0x805AEB88
    Vector3f selfSide = self->bodyForward().perpInPlane(self->move->dir, true);
    Vector3f selfHitDir = otherHitDir.proj(selfSide); selfHitDir.normalise();          // projects otherHitDir!

    if (!(other status bit 91 InBullet)) applyPlayerBump(+1.0f, self, other, otherHitDir, pushDir);
    if (!(self status bit 91 InBullet))  applyPlayerBump(-1.0f, other, self, selfHitDir, pushDir);

    // Kart model hit reaction (visual) for both karts.
    if (self->isBike() && self status bit 29 Wheelie)   self->move->startWheelieBumpWobble(otherIdx);
    if (other->isBike() && other status bit 29 Wheelie) other->move->startWheelieBumpWobble(selfIdx);

    // Battle only (bVar4 && !sameTeam): a kart in a mushroom boost (bit 26) that hits one that is not protected
    // (bits 33, 40, 77, 78, 82, 83, 89, 4, 135, 32, 35, 36, 63, 79, 91) steals a balloon (0x805819A8 on the
    // victim's KartMove) and scores. No effect outside battle.

    self->move->vtable[0x30]();    // kart: empty; bike: cancelWheelie()
    other->move->vtable[0x30]();
    // collision sound between the two karts (0x807091AC)
}
```

## checkPlayerHitboxOverlap 0x8056EEF4

```c
bool KartCollide::checkPlayerHitboxOverlap(BoundBox3f *pushBox, BoundBox3f *contactBox, KartObjectProxy *other) {
    bool found = false;
    CollisionGroup *mine = self->hitboxGroup(), *theirs = other->hitboxGroup();
    for (u16 i = 0; i < mine->count; ++i) {
        Hitbox &a = mine->hitbox(i);
        for (u16 j = 0; j < theirs->count; ++j) {
            Hitbox &b = theirs->hitbox(j);
            Vector3f delta = b.worldPos - a.worldPos;                  // VEC3_sub(out, b, a): points from self to other
            f32 radiusSum = a.radius + b.radius;
            f32 distSq = delta.z * delta.z + (delta.x * delta.x + delta.y * delta.y);   // exact add order
            f32 radiusSumSq = radiusSum * radiusSum;
            bool hit = false;
            f32 depth;
            if (radiusSumSq - distSq > 0.0f) {                         // rounded to single, then compared
                f32 dist = distSq > 0.0f ? distSq * FrSqrt(distSq) : 0.0f;          // EGG::Mathf::sqrt, inlined
                f32 radiusLen = radiusSumSq > 0.0f ? radiusSumSq * FrSqrt(radiusSumSq) : 0.0f;
                depth = radiusLen - dist;
                delta.normalise();
                hit = true;
            }
            if (hit) {
                Vector3f contact = a.worldPos + delta * a.radius;       // Vec3_scale, Vec3_add
                contactBox->min = min(contactBox->min, contact);        // VEC3Minimize / VEC3Maximize
                contactBox->max = max(contactBox->max, contact);
                delta *= depth;
                pushBox->min = min(pushBox->min, delta);
                pushBox->max = max(pushBox->max, delta);
                found = true;
            }
        }
    }
    return found;
}
```

Both boxes start at zero, not at +/-FLT_MAX, so `min` never exceeds 0 and `max` never drops below 0 per component;
the push direction is the sum of the most negative and most positive component over all overlapping pairs.

## applyPlayerBump 0x80570DA8

`owner` is always `self` (the kart running calcPass1): its weight class and bump deviation level are used for BOTH
calls, whichever kart is the target.

```c
void KartCollide::applyPlayerBump(f32 sign, KartObjectProxy *source, KartObjectProxy *target,
        const Vector3f &hitDir, const Vector3f &pushDir) {
    if (target->collide->playerBumpTimer > 0) return;                 // +0x18
    // item notify 0x8079A578(ItemDirector 0x809C3618, sourceIdx, targetIdx): trailing items, out of scope

    f32 wSource = source->stats.weight, wTarget = target->stats.weight;               // Stats+0x10
    if (source status bit 65 SlipstreamBoost && wSource < wTarget) wSource = wTarget;
    if (target status bit 65 SlipstreamBoost && wTarget < wSource) wTarget = wSource;
    if (source->move->boostActive(MushroomAndBoostPanel)) wSource *= 1.5f;             // +0x118 & 4
    if (target->move->boostActive(MushroomAndBoostPanel)) wSource *= 0.5f;             // sic: wSource again
    f32 weightRatio = wSource / wTarget;

    Vector3f dir = hitDir.perpInPlane(target->move->smoothedUp, true);                 // +0x38
    f32 strength = weightRatio * (3.0f + 0.11f * fabs(source->move->speed));           // +0x20
    if (!(100.0f >= strength)) strength = 100.0f;                                      // fcmpo 100, strength; bge

    f32 classFactor = 0.3f * (1.0f + (f32)owner->stats.weightClass);                    // u32 -> float, Stats+0x08
    f32 offsetScale = classFactor * (-sign * target->collide->boundingRadius);          // +0x14
    Vector3f point = target->pos + pushDir * offsetScale;

    f32 force = sign * strength;
    if (target status bit 42 OverZipper)          force *= 0.2f;
    else if (target status bit 15 AirtimeOver20)  force *= 0.5f;
    Vector3f push = (dir * force).rej(target->move->smoothedUp);                       // rejUnit 0x805AEBD0
    target->dynamics->addForce(push);                                                   // totalForce +=
    target->dynamics->applyWrenchScaled(point, push, owner->stats.bumpDeviationLevel);  // Stats+0x14

    f32 speed = target->move->speed, absSpeed = fabs(speed);
    f32 slowed = absSpeed - 0.5f * strength, floor = 0.5f * absSpeed;
    if (floor > slowed) slowed = floor;
    target->move->speed = slowed * (speed < 0.0f ? -1.0f : 1.0f);

    target->collide->playerBumpTimer = 10;
    target->collide->floorMomentRate = 0.01f;                          // +0x1C, 0x80891764
    target status bit 56 FloorMomentRateReset = 1;

    Vector3f axis = dir.cross(owner->componentZAxis());               // VEC3_fromCross(out, dir, ownerZ)
    f32 spinSign = axis.y < 0.0f ? 1.0f : -1.0f;
    target->dynamics->angVel0 += Vector3f(0.0f, 0.0f, spinSign * (0.05f * strength));  // +0xA4 (x, y += 0.0f)

    // CPU AI notify (Enemy::AI::handleBump 0x807263CC), driver model reaction (0x80865534, uses force),
    // controller rumble min(0.05 * strength, 1) (0x8059041C): no effect on the kart's physics.
}
```

## Bike wheelie wobble

```c
// 0x80579F6C, called on the bike that was in a wheelie (otherIdx = the kart it hit)
void KartMove::startWheelieBumpWobble(u8 otherIdx) {
    status bit 66 WheelieBumpWobble = 1;
    wheelieBumpWobbleTimer = 0;                                       // +0x23C
    wheelieBumpWobbleAngle = 0.0f;                                    // +0x240 (phase +0x23E is NOT reset)
    // KartItem(ItemDirector+0x14 [idx * 0x248]) 0x80798B28(item, 1, otherIdx): item side, out of scope
}

// 0x80579FE4, every frame from KartMove::calc right after KartJump::calc
void KartMove::calcWheelieBumpWobble() {
    if (!(status bit 66)) return;
    if (++wheelieBumpWobbleTimer > 25) { status bit 66 = 0; return; }   // s16 at 0x808B5BA4
    if (!(status bit 18 TouchingGround) || (status bit 32 InAction)) return;
    f32 speedFactor = speedRatioCapped / 0.4f;  if (!(1.0f >= speedFactor)) speedFactor = 1.0f;
    wheelieBumpWobblePhase += 5000;                                   // u16 wraps (0x808B5B9C)
    f32 wobble = speedFactor * (0.1f * SinFIdx((f32)wheelieBumpWobblePhase * (1.0f / 256.0f)));   // psq_l GQR3 = u16
    wheelieBumpWobbleAngle = wobble;
    dynamics->angVel2 += Vector3f(0.0f, wobble, 0.0f);                // +0xE4
}
```

Found with the `test2` Dolphin recording (`DOLPHIN_VERIFY_RESULT.md`): the wobble bit (66) is also read in two places
outside these functions.

```c
// KartMoveBike::calcWheelie 0x805883F4, not in a wheelie:
if (status bit 66 WheelieBumpWobble) wheelieRot -= 0.002f;          // 0x808B5AC4, instead of the rotDec ramp
else if (wheelieRot > 0.0f) { rotDec = max(rotDec - 0.001f, -0.03f); wheelieRot += rotDec; }

// KartMove::calcAcceleration 0x8057B9BC, right after speedLimit *= calcWallCollisionSpeedFactor():
if (status bit 66 WheelieBumpWobble) speedLimit *= 0.4f;            // 0x8057BF08, 0x808B5B98
if (status bit 131) speedLimit = max(speedLimit, (0x8059144C())->+0x84);   // a remote racer online: thirteenth pass
```

## Order of the karts' passes (`KartObjectManager::calc` 0x8058FFE8)

Three loops: reset every kart's pair mask, tangentOff and movement; `calcSub` (pass 0, 0x8058EEB4) for every kart;
`calc` (pass 1, 0x8058EEBC) for every kart. A push that kart A's pass 1 gives kart B reaches B's dynamics on the next
frame. (Kinoko ran both passes per kart in one loop until the `test1` recording showed the difference.)

## Not ported (no effect on kart physics, or needs systems Kinoko does not have)

- Effects, model shakes, driver animations, sounds, rumble, per-kart stat counters (accessor +0x38), CPU AI notify.
- Item hooks: `0x8079A578` (trailing items of the target) and `0x80798B28` (bike wobble). Items are not in Kinoko yet.
- Battle mode: balloon steal and score callbacks (game modes 3, 9, 10). Kinoko has no battle mode.
- Teams: RaceConfig team flag. Kinoko's RaceConfig has no teams, so `sameTeam` is always false.
- The star / mega / bullet branches are ported but cannot fire yet: nothing in Kinoko sets bits 63, 79 or 91, and
  Kinoko's `KartAction` handlers for actions 6 and 13 are stubs (`startStub`).
- Slipstream was missing at first; it is now ported too (section below).

# Other VS / multiplayer behaviour (second pass)

How these were found: every kart-code function that reads another racer (`KartObjectManager` lookups,
`PlayerHolder_getPlayer`), the game mode / engine class / course fields of RaceConfig (`+0xB68` course,
`+0xB6C` engine class, `+0xB70` game mode, `+0xB74`, `+0xB78`, `+0xB90` flags), and every test of the slipstream
status bits. Battle (modes 3, 9, 10), mission (mode 4), awards/live-view (modes 0xB, 0xC), rumble, camera, sound and
UI readers were set aside. What is left and affects kart physics in a VS race:

## Slipstream (`KartMove::calcSlipstream` 0x80586FA8)

Called from `KartMove::calc` as `InCannon ? calcCannon() : calcSlipstream()`. State: `KartMove+0xEC` timer (u32),
`+0xF0` factor (1.0; +0.005 per charging frame up to 1.1; 1.4 while boosting; nothing physical reads it, kept for
fidelity), status bit 64 charging, bit 65 boosting. Constants at `0x808B5B70..`.

```c
// RaceManager +0x34 = slot mask of 12 slots (0xFFF00000, set by 0x805506EC with 12, NOT the player count),
// +0x38 = current slot (0x80000000 at race init). Every frame from the countdown on (0x8055070C):
//     turn = (turn >> 1) & slots;  if (turn == 0) turn = 0x80000000;
void KartMove::calcSlipstream() {
    if (selfIdx == cntlzw(raceManager.turn) && !ghost && !(KartObjectManager.flags & 2)
            && !(status bit 91 InBullet || bit 65)) {
        bool found = false;
        if (!InAction && speedRatioCapped >= 0.5f) {
            for (each other racer j != self, not a ghost, speedRatioCapped >= 0.5f) {
                Vector3f behind = other.pos + other.move.dir * -1000.0f;
                if ((boundingRadius + 1000)^2 - |pos - behind|^2 <= 0) continue;    // 0x80571234
                Vector3f toOther = other.pos - pos;
                f32 cone = other.isBike ? 0.99f : 0.982f;
                if (other.move.dir . toOther > sqrt(|toOther|^2) * cone) { found = true; break; }
            }
        }
        if (found) bit 64 = 1; else { timer = 0; bit 64 = 0; factor = 1.0f; }
        return;
    }
    if (bit 64) {
        factor = min(factor + 0.005f, 1.1f);
        if (++timer >= 90 && !(bits 72, 82, 83, 91, 4 BeforeRespawn, 135, 32 InAction)) {
            bit 64 = 0; bit 65 = 1; timer = 0; factor = 1.4f;
        }
        return;
    }
    if ((bit 65 && ++timer > 135) || speedRatioCapped < 0.5f || InAction) { timer = 0; factor = 1.0f; bit 65 = 0; }
}
```

So a racer only searches on its own slot, once every 12 frames; between searches a found draft keeps charging.

Effects of the boost (bit 65):
- `KartMove::calcVehicleSpeed` 0x8057AB68, accelerating, not on moving water: acceleration = 0.7 (after a star /
  mega / thunder cloud's 3.0, before the half pipe's 5.0).
- `KartMove::calcAcceleration` 0x8057B9BC: the boost speed limit is raised to at least 115 (star 105, mega and thunder
  cloud 95 come first in the same max chain).
- Kart-kart bump: slipstreaming raises a kart's bump weight to the other kart's (`0x80570DA8`, already ported).

Reset (`0x80582488`, inlined in `clear`, `initOob`, `enterCannon` between the SSMT and offroad-invincibility resets):
timer 0, factor 1.0, bits 64 and 65 cleared.

## Engine class (`RaceConfig+0xB6C`)

`KartMove` constructor: `KartMove+0x10 = {0.8, 0.9, 1.0, 0.7}[engineClass]` (50cc, 100cc, 150cc, battle).
Used for: `baseSpeed = factor * stats.speed` (also the initial soft speed limits, `init` 0x805784D4); the boost speed
limit term in `calcAcceleration` (`boostLimit * scale * factor * kclSpeedFactor`); the jump pad check
`softSpeedLimit > 110 * factor` (`tryStartJumpPad` 0x8057FD18); and a bullet-bill speed not in Kinoko.
Course objects: `ObjPillar` / its collidable (0x807FED80, 0x807FEB68) take the fall start from setting 0 / 2 / 3 at
150cc / 100cc / 50cc; `ObjVolcanoPiece` (0x80817DE8) takes rest/shake times from settings (1,2) / (3,4) / (5,6),
always (1,2) on Block Plaza. A third object constructor (`0x806E3A8C`, same pattern) is not in Kinoko. The other
readers are sound (`0x806FAE60`), UI (`0x807F8994`), items, CPU AI and GP scoring.

## Start grid (`RaceManager::findKartStartPoint` 0x805362DC)

VS and GP use `RaceManager::Player+0x20` as the 1-based grid place and the player count; time trials and ghost
races use place 1 of 1. `+0x20` is set in the RaceManager constructor from the race config player's byte at
`+0x109` (written by the menus); battle uses index + 1. Kinoko now takes it from `RaceConfig::Player::startPlacement`
(0 = index + 1, the old behaviour).

## Checked and not needed for VS kart physics

- `KartObjectManager+0x28` flags: set only for course ids 0x12, 0x14, 0x23, game modes 0xB/0xC and missions.
- Teams (`+0xB90` bit 2) mark karts in team VS; Kinoko has no teams.
- Lakitu units (`JugemDirector`, one per local player in the game, one in Kinoko) and the camera only affect
  visuals: Kinoko's respawn physics reads the per-player respawn point from RaceManager.
- Not ported, separate systems: items (and everything item-triggered: star, mega, bullet, thunder cloud, ink,
  shock), mirror mode (mirrored course collision), battle, CPU racers' AI, online packet code.

# Item effects on karts (third pass)

Offsets in KartMove: `+0x10C..` boost timers, `+0x148` offroad invincibility, `+0x178` shock speed multiplier,
`+0x17C` mega scale, `+0x18A` star timer, `+0x18C` shock timer, `+0x18E` ink timer, `+0x190` ink pending (byte),
`+0x192` crush timer, `+0x194` mega timer, `+0x196` mega shrink started (byte), `+0x260` KartScale.
KartScale types: 0 `thunder_scale_up`, 1 `thunder_scale_down`, 2 `kinoko_scale_up`, 3 `kinoko_scale_down`.

```c
// KartMove::calc 0x805788DC, item part, in this order after the boosts:
//   mega (as 0x80580DC0), star timer (as 0x805803F0), shock (0x80580998), crush (0x80580F9C), ..., scale (0x8058160C),
//   ..., calcVehicleSpeed (vtable +0x44), ink (0x80581B1C), calcAcceleration (0x8057B9BC)
// calcScale: totalScale(+0x170) = s160 * s180 * shockMultiplier * megaScale (s160, s180 are 1 in a race)
// KartAction::start 0x805675DC(action, r5, hitsMega r6, out r7, item arg r8, r9) refuses when any of
//   bits 33, 40, 77, 78, 82, 83, 89 | BeforeRespawn 4 | 72 | (battle && stage >= 4) | InStar 63 | InCannon 36 |
//   InBullet 91, or InMega 79 when !hitsMega.
// Action table (start, calc, end): 6 = (0x80569024, 0x80568D34, 0x80568F68), 10 = 17 = (0x8056804C startRotation(2),
//   0x80568204, 0x8056837C), 13 = (0x80569818, 0x80569A1C, 0x80569AD0).
// Item use: KartItem +0xB4 use object 0x80791910 looks the held id up in the boot-time behaviour table (0x809C36A0,
//   stride 0x1C: +0x8 count, +0x10 timed, +0x14 object kind (0 = no object), +0x18 use function) and calls it.
// Lightning: 0x807B7C20 calls 0x80798728 on every other KartItem (team mates spared): not in a respawn/cannon state ->
//   KartMove::applyLightning 0x80580438 -> applyLightningEffect(table[rank + 11 - players] + 72, 0, 0).
// Rank: 0x805336D8, insertion sort by race completion from the previous order; finished = 0.01 * idx + 6000099 - ms.
```

# Item objects (fourth pass)

Green shell, banana, holding items behind the kart, throwing, hits and drops. Addresses PAL. Decompiled with a second
paired-single rewrite (`ps_merge*` -> `fmr`, `ps_sum*` / `ps_muls*` / `ps_madds*` -> scalar ops) on top of the first
one; "double" values in that pseudo-C are float pairs.

```c
// ItemDirector (0x809C3618): +0x14 KartItem[players] (0x248 each), +0x48 ItemObjHolder[15] (0x24 each), +0x42C total over 30.
// ItemDirector::calc 0x80799850: holders with active objects -> ItemObjHolder::calc 0x80796470 (offline: obj->calc for each
//   active, i.e. the course objects move BEFORE the karts' item buttons), total = sum(weight * (spawned + held)) - 30,
//   then KartItem::calc 0x80797928 for every player. Karts collide with items in KartSub::calcPass1, before all this.
// Object kinds (holder index): 0 green, 1 red, 2 banana, 3 mushroom, 4 star, 5 blue, 6 lightning, 7 fake item box, 8 mega,
//   9 bob-omb, 10 blooper, 11 POW, 12 golden, 13 bullet, 14 thunder cloud. Properties 0x809C2F48 (stride 0x74):
//   +0x04 limit, +0x08 pool, +0x14..+0x16 keep-own-state bytes, +0x18 initial state ptmf, +0x2C no box col,
//   +0x38 KCL radius, +0x3C held hit radius, +0x40 hit radius, +0x44 held scale, +0x48 scale, +0x4C/+0x50 growth,
//   +0x54 landing bounce, +0x58 survives hits, +0x59 breaks on kart hit, +0x5A reacts to kart hit, +0x5C weight.
// Item behaviour 0x809C36A0 (stride 0x1C): +0x04 object kind, +0x08 count, +0x10 timed, +0x14 use mode
//   (0 instant, 1 one held object, 2 triple chain (bananas), 3 triple orbit (shells)), +0x18 use function.
// Holder pool order: [0, landed) resting, [landed, active) on the course, [active, spawned) taken out (held), rest free.
//   spawn 0x80795E04 (evicts the oldest on the course when full), activate 0x807961F0 (into the active part, box col
//   unit with hit radius * scale, frame = 0), despawn 0x80795F68 (compacts, removes the unit).

KartItem::calc 0x80797928, flags +0x0C: 0x1000 held, 0x2000 pressed, 0x4000 released this frame, 0x400 lockout,
  0x200 unlocked this frame, 0x10 / 0x20 stick up / down (on at |y| > 0.6, off at |y| < 0.1), 0x40 press accepted,
  0x80 release accepted, 0x100 release armed (set by the held item every frame), 0x1/0x2/0x4 roulette / inventory /
  held when accepted (0x80798584: race stage, not in an action, not bit 50).
  Use when (flags & 0x242) == 0x242: ItemTrail::use 0x80791910 (KartItem +0xB4). Throw when (flags & 0x284) == 0x284,
  checked by the held item's calc (0x80792C68), i.e. one frame after the release arms.
  Also: +0x34 position delta, +0x40 throw pitch (0.05 towards 0.9 * lerp(zAxis.y, delta dir y, speed ratio)),
  +0x23C stick x smoothed by 1/15 per frame (aim of a forward throw).
ItemTrail (KartItem +0xB4): mode 1 init 0x80792B08 / calc 0x80792C68; position 0x80792618 (lags the kart's turns,
  0.1..0.27 follow rates, 40 + 0.9 * stats+0x170 behind, stats+0x178 up, KCL push-out blended 0.2); kart pose cache
  0x80791E5C; collisions 0x80791CC8 (box col unit once the scale reaches 0.8, radius = kart scale * held hit radius);
  drop countdown +0x54 = 10, one held item at 8, 4, 0 (0x8079231C) with 0x807A6F74 (up 25/35 + kart up speed);
  0x80798B28 starts it from the spin (0x80567D3C), action 2 (0x8056865C), action 5 (0x80568FA4, all modes) and the
  bike wheelie bump (0x80579F6C).
Throw direction: forward 0x80794E88 (speed + |delta|, forward * dirScale + delta, aim 20 * -speed ratio * stick x),
  backward 0x80795030 (speed - min(|delta|, base speed), -(forward * dirScale + delta.xz)), then 0x80794D1C (project on
  the floor on sticky roads, else keep KartItem +0x40 as the y and rescale x/z).
ItemObj::calc 0x8079EFEC: scale growth (+0x4C after a forward throw, else +0x50; dropped items 0.1 with a landing
  spring), released blend 0x807A25B0 for 44 frames (frames 0-1 follow the kart / hand, the state runs from frame 2),
  else the state ptmf (+0x168); vanish when off the floor too long (+0x15E 60/180), on fall boundaries, below y 0;
  owner immunity cleared after 30 frames (15 grounded with 0x40000, 7 with 0x80000).
Green shell: release 0x807AEB98 (forward 85 / 9 bounces unless stick down: backward / 4 bounces) -> 0x807B5178;
  state 0x807AEC64: 1500 frames, last bounce after 900, move 0x807B54F4 towards speed 105 (+0.01 per frame):
  KCL push (0x6CEBDFFF), floor following, 30 frames of flight off steep floors, wall reflection (y damped 0.7),
  bounce budget, gravity 2 (falling) / 4.2 (rising), floor pull 2.5.
Banana: release 0x807A3BE0 (dropped behind unless stick up) -> arc 0x807B7104 (random +-0.03 spread below speed 30),
  state 0x807A3E78 -> 0x807B7570 (gravity 1.8 / drag 0.97 dropped, 2.2 / 0.999 thrown), land 0x807B6910,
  break 0x807A40F4 (pop up to 30) -> shrink 0x807A3C80, 20 frames.
Kart hits: KartCollide::calcItemCollision 0x8057257C <- ItemDirector 0x80799CAC <- ItemObj 0x807A14D4 (bounding
  sphere, then the item as a point of radius r against the kart's convex hull, collider 0x808D198C); handlers
  0x808B5468: shells action 2, banana action 0 (none in a mega), FIB 2, blue / bob-omb 7 while exploding; KartAction
  start(action, 0, 0, 0, 12, 1). The item is knocked away (0x807A6738) and breaks (0x807A6560, 20 frames).
Inventory drop (hit by lightning, actions 3 / 4 / 6 / large flip / crush): 0x80798A30 -> 0x807BC610: 10-frame
  countdown, objects spawned at once with a random-angle spread (0x807BC350: 15 or 20 out, scale 0.05), one released
  at 7, 5, 3 (0x807BC170, + 0.7 * kart velocity).
```

## Item boxes and the roulette (fifth pass)

Random generators. `RaceManager` +0x04 and +0x08 are two `Util::Random`, seeded from the scenario's seed1 / seed2
(`RaceConfig` +0xB94 / +0xB98) in the constructor 0x805327A0. `RaceConfig::Scenario::initRng` 0x8052F924 (called at
the end of `Scenario::initRace` 0x8052FB90): game modes 2, 4, 5 seed1 = 0x74A1B095, seed2 = clock; tournament (mode
flag 4) seed1 = 0x92BC7D03; replays and online keep the seeds they were given (online: from the network); every other
offline mode takes both from `OSGetTick`. +0x04 is advanced once per frame at the end of `calcEnginesUnpaused`
0x80554AD4 and used by course objects. The item code picks one with 0x807BD718: +0x08 online or in a Time Trial,
+0x04 otherwise (offline GP / VS). The item box constructor draws `nextU32(180)` from +0x08 (model spin phase), which
is why the boxes are harmless to Time Trial ghosts.

Race flags 0x809C38B8 (`RaceGlobals_init` 0x807BD1D0, run from the player model manager's constructor 0x8078CA5C):
+0 player count, +1 online (modes 7..10), +2 offline VS (mode 1), +3 battle (3, 9, 10), +4 Time Trial (2, 5, 6, and
anything > 12), +5 teams (mode flag 2), +0x18 a per-course item scale, +0x1C course 0x1D flag.

Item box `ObjItemBox` (`itembox`, ctor 0x808285B0, vtable 0x808D7BC0, 200 bytes): +0xB0 active, +0xB4 timer, +0xB8
respawn frames (setting 5: 4/5 -> 600, 6 -> 900, else 150), +0xBC spin phase, +0xBE setting 3. Scale 0.9, position
+(0, 180 + setting 6, 0). calc 0x80828860: broken -> hidden and intangible (vtable +0x90, BoxColUnit flag 0x200) until
the timer passes the respawn time, then active; active -> intangible while the timer < 10 (0x808D79D4), else tangible
(+0x94). The rest of calc (grow-back scale 1.2, height 45, frames 12 / 6) only moves the model. onCollision (vtable
+0xC0) 0x80828CB8: kart -> rumble, `KartItem::startRoulette` 0x80798C38 (settings 1 / 2, short when setting 5 is 2..4),
sounds, records the box position for the CPU (0x809C21D0 +0x68[player] +0xAD / +0x100); no kart -> break effect.
Both: timer 0, inactive, intangible. Derived boxes: `f_itembox` 0x80828F44, `s_itembox` 0x80829208 (falls with
`checkSphereFullPush`), carried ones 0x8082876C (Hwanwan) and the heyho group 0x8082963C.

`KartItem::startRoulette` 0x80798C38: only with an empty inventory, not remote (+0x1A), not finished (RaceManager
player +0x38 bit 1); humans (+0x19, `PlayerModel_isHuman` 0x807BD5BC: not type 1/4/5) use setting 1, CPUs setting 2;
rank from 0x807BD374 (RaceManager player +0x20 outside battles).

Roulette (KartItem +0x54, 0x34 bytes): +0 KartItem, +4 state, +8 frame, +0xC duration, +0x10 speed, +0x14 progress,
+0x18 previous shown, +0x1C shown, +0x20 decided, +0x24 pending (an item held behind the kart with the forced-release
flag), +0x28 box setting u16, +0x2A rank, +0x2B setting was 0xFF, +0x2C short, +0x2D waiting for the network, +0x2E
replaced, +0x30 online roll count. start 0x807B9FB0: refused in a Time Trial (flag +4), unless idle, or with a pending
item; 184 frames at speed 1.1 (45 / 0.17 short); `decideItem`; offline, an item whose kind has no room (0x80799BE8:
holder +0x0C - in use < count) becomes a mushroom (a triple mushroom stays) with +0x2E set; first shown item drawn.
calc 0x807BA37C: frame++; while something is shown speed = speed * 0.9675 + 0.002 (fmuls, fadds), progress += speed;
spinning: progress >= 1 -> -1, next shown (0x807BB8D0); stays spinning while frame <= duration unless KartItem flags
0x241 (pressed while spinning, press accepted, unlocked) after frame 59 and not short; stopping: progress >= 1 ->
shows the decided item, and the time after that ends (returns 1). `KartItem::calc` then resets it (0x807BA2D8) and
either spawns a thunder cloud (0x8079A298) or calls `ItemInventory::setItem(id, +0x2E)` 0x807BC940. Hits (0x80798A30)
and `KartItem::clear` 0x80798848 cancel a spinning roulette (0x807BA5D8).

`ItemSlotData` 0x809C3670 (0x48 bytes, created in the ItemDirector constructor 0x807BA77C, loaded 0x807BAF48): +0x10
player table, +0x18 CPU table, +0x20 special table ({row count, u16 rows * 19}), +0x28 shown-item list, +0x2C per-rank
lists (12 * 0x50), +0x30 per-special-row lists, +0x34 lightning / +0x38 blue shell / +0x3C blooper / +0x40 POW
cooldowns, +0x44 player count. File: course `ItemSlotTable/ItemSlotTable.slt`, else Common `ItemSlot.bin`; a section
count, then sections {rank count, item count, bytes item-major}. Grand Prix (and Time Trial): sections 0, 1, 5; offline
VS: 2, 3, 5, then both tables remapped by player count (0x807BAD20, row map 0x808A6320 + count * 0x18, rows past the
count cleared); online VS: 4 player, 3 CPU, 5; battle 0x807BBB00. Parse 0x807BA9D8: chance = (s32)(factor *
(f32)(byte + offset)) per item category (behaviour +0x0C): frantic {0.25, 1, 4} / {3, 0, 0}, strategic {8, 0.25, 0} /
{0, 3, 0}, online battle of 5..8 {0.5, 1, 2} / {1, 0, 0}.

`decideItem` 0x807BB42C: row = rank - 1 of the player or CPU table, or setting - 1 of the special table; with a triple
item held (0x80798DBC) or the total item count over 30 (ItemDirector +0x42C), only items 0..15 (not in missions). An
item counts when its kind's cooldown is 0, not mega / thunder cloud in a mega, not thunder cloud in a star, usable, its
holder has room (+0x1C limit - spawned - held > 0), and not a bullet while anyone is in a bullet. Cumulative u16 sums at
0x809C3674; draw `nextU32(sum)`, first item whose sum is above it; nothing -> mushroom. Shown items 0x807BB8D0: a random
entry of the list, the next one when it equals the current. Cooldowns 0x807BB9EC (ItemDirector calc, first): set full
(1800, 1800, 900, 1200) when the countdown timer is 0, then decremented; reset by the lightning (0x807B7B7C), blue
shell (0x807BB9C8), blooper and POW uses.

Item route: ITPH (`parseItemPaths` 0x80513BFC) and ITPT (0x80513ADC) at CourseMap +0x14 / +0x18. Point links
0x80517E88 (prev at +4, next at +0xA, counts +0x10 / +0x11). Per kart (KartItem +0x44: point, moved, position): init
0x807B99D4 (point 0 when there are item paths), every frame at the end of `KartItem::calc` 0x807B8A9C (nearest of the
points up to two links away), and when 12000 or more away 0x807B8F30 (walk forward). Also reset at respawn
(0x80584334) and by area type 4 (0x80798D0C): not ported.

## Item objects, part 2 (sixth pass)

Red shell (vtable 0x808D2228): states at 0x808D2148, run by `calcRed` 0x807AA87C. Release 0x807AA688: stick down
-> backward slide, 0 bounces; with an item point -> `initRoute` 0x807B4878 (speed 75, 45 frames of direct steering,
the racer ahead as target); otherwise a forward slide that looks for a target. Route following is shell-base code
(the fields at +0x1DC..+0x27C belong to every shell): direct 0x807B4AE4, spline 0x807B4944 / 0x807B4D2C (Bezier
0x807B7D9C..0x807B8600), next point 0x807B91E4. Target search 0x807B3BAC / 0x807B381C (4000, about 18 degrees,
height -1500..1000), homing 0x807B2F54 (50, 0.2), lost 0x807B3EC0.

`initRoute` 0x807B4878 is called as (speed, obj, trail, frames, ?, ?, wallBreaks, target, branchSearch): the red shell
passes (75, 45, 1, 0, 1, target, 0), the blue shell (frames 20, ..., 0, target, 1). wallBreaks 0 gives
`initThrow` -1 bounces (no bounce limit). branchSearch makes 0x807B91E4 follow the target's own item point (KartItem
+0x44) forward for a third of the course's point count to pick the branch it is on; not ported yet (only the blue
shell uses it).

Triple items: `ItemTrail` use mode 2 (chain) init 0x807931DC / calc 0x8079356C / detach 0x80793F74, mode 3 (orbit)
init 0x8079455C / calc 0x80794694 / positions 0x807941F0 / detach 0x80794AB0; throw on KartItem flags 0x244.

Fake item box (vtable 0x808D1D60): release 0x807A7940 (70 / 45 / 10, 1 or 3 bounces), flight 0x807A79E0; kart hit
0x8057325C -> action 2 unless mega, star or bullet.

Bob-omb (vtable 0x808D1BE0): sub-states at 0x808D1B18 (flying 0x807A4B48, fuse 0x807A43FC, explode 0x807A45E0 /
0x807A4734), runner 0x807A4CDC, nearest racer 0x807A5D88; explosion scale 36 (+3.12 per frame), radius 36 x the
hit radius 29 = 1044; fuse caps 120 / 60 / 30 / 4 at 8 / 4 / 2 / 1 radii (`RaceConfig` +0xB6C: 180 at 50cc, 150 at
100cc); kart hit 0x805731C8 -> action 7, action 0 after explosion frame 30.

Thunder cloud (vtable 0x808D2480): spawned by the roulette 0x8079A298 (state = property +0x18, the update 0x807AF76C
through 0x807A3908), passed on in the bump 0x80570DA8 -> 0x8079A578 -> 0x807B0B4C / 0x807B1108; strikes at 600
(0x807987EC -> `KartMove` 0x805805EC: `applyLightningEffect(612, false, true)`), breaks after 690.

Blue shell (vtable 0x808D23D8 over 0x808D2280, 0x338 bytes): states at 0x808D2308 (runner 0x807AD118): 0 slide
(130), 1 route (calcRoute 0.008 with the branch search; re-chooses its target each frame and locks on through
0x807B45B0, or gives up after 1200 frames), 2 homing (0x807B43DC -> core 0x807B3FDC: aim point = target + 300 x its
vel1Dir + radius x (cos side + sin dir), velocity blended 0.125 towards rate 0.5 x the offset, capped at the fly
speed, never reversing; until within 800), 3 one turn round the target (0x807B4400: angle step 1000 + 35 per frame,
radius shrinking 4 / 8; ends at angle 0x11F40), 4 dive (0x807B4430: towards 210 ahead of the target at a rising
height, 25 frames), 5 descent (box collision back, squash 0x8079FD84, explodes on the course), 6 explosion (as the
bob-omb's: 36 x 29 = 1044, +0x334 at 30, kart hit 0x805731B0 -> action 7 then 0). Target choice 0x807AC1A8: the best
racer still racing who is not the thrower; the thrower itself when it leads. KartMove +0x38 / +0x44 / +0x74 are
Kinoko's smoothedUp / up / vel1Dir.

POW (`PowManager` 0x809C3640): use 0x807B1DB4 (159-frame timer; targets: everyone ranked ahead of the user), update
0x807B22F8 (shakes at 143, 95, 45 left: 0x807B20BC sets KartAction +0xF8 = target, hops +6 / +10 unless airtime over
20 on the first two, and the last starts the global strike 0x805674FC: 15 frames at 0x809C1898). `KartMove`
0x805810A8 (in `KartMove::calc` before the mega / star / shock block): a trick-up input opens a 10-frame dodge window
(+0x152); strike && target && touching ground -> action 11 (start 0x80568064: rotation 3, or 1 when dodged, +20 up,
items lost; calc 0x805683B8: the action 1 spin, params 0.96 / 0.96 / 3 past 360 degrees; trick up in its first 8
frames: 0x80568558 switches to rotation 1 keeping the angle). The table at 0x808B62A0 that also reaches 0x805805EC /
0x80580648 is the online status replication (0x8058D644), not the offline path.

Blooper (`GessoManager` 0x809C3610): use 0x807A81B4 (timer from 1; target frame = the `start` CHR of `gesso.brres` +
30 = 100), update 0x807A92B0 (at the target frame 0x807A972C picks the racers ranked ahead; player i is inked at
148 + target + 3 x {3, 1, 4, 11, 5, 9, 2, 6, 10, 8, 7, 0}[i] through `KartMove` vtable +0x20 = 0x80581A58).

Bullet bill: use 0x807A9AFC -> 0x807986E4 -> `KartMove::activateBullet` 0x805858AC (hard speed limit 145, hitbox
scale = stats +0x184 (mega scale), the effect clear at vtable +0x2C 0x805837CC, dir from the main rotation, extVel /
dynamics +0xB0 / +0xC8 cleared, turn values cleared, top rate 0.3, `KartKiller` start, KartMove +0x180 = 1.5) ->
`KartKiller` (accessor +0x60; start 0x8059B7B8, calc 0x8059BC44, cancel 0x8059C118). `KartMove::calc` has its own
bullet path (top, end jump pad, pads, dirs, killer, sticky road, offroad, manual drift, scale, cannon, then ink and
acceleration; no rotation); `calcAcceleration` pins speed and soft limit to 145 x engine factor x killer +0x2C;
`calcTop` blends smoothedUp towards +0x19C at +0x198 while the killer flies. KartMove offsets: +0xF4 divingRot, +0x104
outsideDriftBonus, +0x158 realTurn, +0x15C weightedTurn, +0x288 rawTurn, +0x174 hitboxScale, +0x178 shock speed
multiplier, +0x17C megaScale, +0x180 bullet scale (total scale = +0x160 x +0x180 x (+0x178 x +0x17C)). KartDynamics:
+0x74 extVel, +0xB0 movingRoadVel, +0xE4 angVel2, +0xF0 mainRot, +0x170 forceUpright, +0x171 noGravity, +0x172
killExtVelY, +0x174 bike-as-kart. KartState +0x88 stick X.

## Item hand anchors and the kart body rumble (sixth pass, 2026-11-02)

Where a thrown or dropped item starts (`ItemObj::calcRelease` 0x807A25B0 → `KartModel` 0x807D171C):

```c
// KartModel (PlayerModel): +0x78 Matrix34f seat offset = makeT(0, dispY, dispZ) from kartDriverDispParam.bin
//   (36 vehicles x 48 characters x 14 floats, floats 0 and 1); +0x114 driver matrix; +0x6C0 throw anchor (x,y,z);
//   +0x6CC drop anchor; +0x104 skeleton node table (36 x 0x60: +0x00 parent, +0x04 child, +0x08 next, +0x14 slot
//   name, +0x18 ResNodeData, +0x24 Matrix34f, +0x54 translation); +0x108 root node; +0x6C PlayerModel's driver
//   resources (+0x28 nw4r::ut::List of the driver's animation slots: 0x15 "throw_f", 0x17 "throw_b", 0x1E ...).
// PlayerModel_init 0x807C8758: for slot 0x15 and 0x17, pose the skeleton (0x807CAE8C, below) with that CHR0 at frame
//   4.0 and copy node 13 ("wrist_r1") +0x54 into +0x6C0 / +0x6CC. Slot names: skl_root leg_l1 leg_l2 ankle_l1 leg_r1
//   leg_r2 ankle_r1 spin arm_l1 arm_l2 wrist_l1 arm_r1 arm_r2 wrist_r1 face_1 cap1 mouth_1 ca_mouth_1 yo_mouth_1
//   tang_1 tang_2 tang_3 tie_1 wing_l1 wing_r1 tail_1 tail_2 tail_3 skirt_1 d_hair_1 d_hair_2 p_hair_1 p_hair_2,
//   then three spare slots for other bones (0x807C7E7C). A global (bss 0x809C38D0, 0 so far) clamps +0x6C4 from below.
// 0x807CAE8C pose(frame, node, parentMtx, parentScale, &chr0): track = the CHR0 track named like the node's bone
//   (ResDic_GetIndex; the root falls back to track 0), result = ResAnmChr::GetAnmResult(frame, chr0, track)
//   (0x80055540: scale 0x80053D00, rotation 0x80054460 -> MTX34RotXYZFIdx(angles * 256/360), translation
//   0x80054ED0; keyframe formats 1/2/3 = hermite with fres(span) refined once, formats 4-6 per frame),
//   node matrix per flags 0x20 (no rotation) / 0x40 (no translation) (0x80055600), scale (1,1,1) if flag 8
//   (0x800555C0); with a parent: translation *= parent scale, mtx = parent * mtx; recurse child, then next sibling.
// 0x807D171C handPosition(model, forward, &pos, &kartPos, itemRadius*itemScale):
//   local = forward ? (0, +0x6C4, +0x6C8) : (0, +0x6D0 + d, +0x6D4 - d),
//   d = (1 - speedRatioCapped) * (itemRadius + KartCollide+0x14 boundingRadius) / max(1, 5 * (1 + zAxis.y));
//   local *= (scale.x, max(1, scale.y), scale.z); pos = PSMTXMultVec(+0x114, local); kartPos = pos().
// calcRelease: FromKart (forward throw) frames 0-1 -> handPosition(1); else frame 0 (= release frame 2) ->
//   handPosition(0); the object's +0xBC (prevPos) = kartPos.
// Driver matrix (PlayerModel_update 0x807CB360 / 0x807CB530): +0x114 = bodyRot * +0x78, the +0x78 translation
//   scaled by the kart scale when it is not (1,1,1); bodyRot = KartBody +0x1C, King Boo (character 0x13) uses pose().
// KartBody +0x1C (0x8056CE98, from PlayerSub_updateTransforms 0x8059783C, after calcPass1, before the model):
//   counter(u16 +0xA4) += BSP +0x258 rumbleSpeed; y = SinFIdx(counter * (1/256)) * BSP +0x254 rumbleHeight * scale.y;
//   +0x1C = makeQT(fullRot, pos) * makeT(0, y, 0). The counter is never reset; it has advanced 173 times at race
//   frame 0 (measured on test1-3, both racers). Kinoko: KartBody::calcMatrix, DriverItemAnchor, KartModel.
```

## Item-item collision and action 0 (seventh pass, 2026-11-02)

```c
// ItemObj_update 0x8079EFEC: if (boxColUnit && !Landed) { BoxColManager::search(unit, Item|Object|Drivable);
//   itemItem 0x807A1C94; itemObject 0x807A1ED8 } else if (kind == bob-omb) { search(unit, Object|Drivable); itemObject }
// 0x807A1C94 for each BoxColManager::getNextItem(): this not trailing -> 0x807A0E28(this, other) (skip self);
//   this trailing -> skip if !other.boxColUnit or (other.OwnerImmune && same owner); handle if !other.trailing ||
//   this.globalIdx <= other.globalIdx || other.online. Hit: spheres (ro + rt) now, or both spheres against the
//   other's previous position. Then r1 = response(this, other), r2 = response(other, this),
//   apply(this, r1, other.pos, other.vel); apply(other, r2, this.pos, this.vel before).
// 0x807A0E28 free vs free: skip if other trailing / no unit; skip if other not landed and other.idx < this.idx;
//   online pairs only when a response is 1.
// 0x807A139C response: table 0x808A4DFC [this kind][other kind] (holder order), see ItemObj.cc. Other exploding
//   (bob-omb sub-state 2 / blue state 6): (!weakened && !this.Dropped) ? 2 : 0, then team check (bss 0x809C38BD).
// 0x807A18FC apply(response, otherPos, otherVel): Breaking -> nothing; (!flags2 bit 6 && Dropped && !survivesHits)
//   -> vanish. 1: not trailing; delta = prevPos - otherPos; (!Landed && WallSlide) -> contact 0x807A1680; else
//   WallSet; vel . delta <= 0: normalise delta, onWall ptmf(otherVel, delta), !Landed && |pos - otherPos| <
//   hitRadius -> pos.xz = otherPos.xz + delta.xz * hitRadius; vel . delta > 0: same push after normalising.
//   2: trailing -> KartItem[owner] trail detach 0x807951AC; |otherVel| == 0 -> knockedAway(otherPos, otherVel,
//   <unset>, 0); else dir = normalise(pos - otherPos with y >= 0), knockedAway(otherPos, dir * 15, dir, 0); break.
// 0x807A1680 contact(otherPos, delta): Contact set -> normalise delta, delta . contactNrm > 0.5 -> vel.xz = 13 *
//   delta.xz. |pos - otherPos| < hitRadius -> WallSet; first contact and vel . delta < 0 -> u = Random.getU32(0x10000),
//   SinCosFIdx(u / 256) -> contactNrm.x = sin, .z = cos, vel.xz = 13 * contactNrm.xz, Contact. Contact ->
//   colInfo.wallDist > -FLT_MIN -> contactNrm.xz = normalise(wallNrm.xz) (0x807A1864).
// Kart action tables (ptmf, 12 bytes each): start 0x808B4D40, calc 0x808B4E18, end 0x808B4EF0. Action 0 start
//   0x80567F68 = startRotation(1) + rumble; calc 0x80568204 and end 0x8056837C are action 1's.
```

## Respawn (eighth pass, 2026-11-03)

```c
// MapdataJugemPoint (0x30 bytes, ctor 0x805183A8): +0 SData*, +4 rot in radians, +0x10 rotate(q, -ez),
//   +0x1C rotate(q, -ex) with q = setRPY(rot * DEG2RAD); +0x28 nearest ENPT ahead, +0x29 nearest ITPT ahead
//   (0x805184FC from CourseMap::init 0x80512AE8: forward/right/up = rotate(q, ez/ex/ey), tan = SinFIdx(10.6667) /
//   CosFIdx(10.6667) (15 deg); for each point: d = p - pos, a = fwd.d (skip if a < 0), r = a * tan, s = right.d,
//   u = up.d; keep if s*s + u*u < r*r and (none yet or a < best); none -> 0); +0x2A = range % 100 (99 -> -1),
//   +0x2C = range / 100 (range < 0: -1, 0); +0x2C > 0 -> respawn sound (+0x2C - 1).
// 0x8051896C getRespawnPos(out, playerIdx): gameMode 2 or 5 (TT, ghost) -> idx = 0xFF -> pos. Else table
//   0x8088FA08 s8 [12][2] = {-1,0},{1,0},{-3,0},{3,0},{-1,1},{1,1},{-3,1},{3,1},{-1,2},{1,2},{-3,2},{3,2}:
//   out = pos + (+0x1C) * (150 * t[idx][0]) + (+0x10) * (300 * t[idx][1]).
// 0x80584334 doRespawn: jugem = RaceManager::jugemPoint(idx) 0x8053621C; pos = getRespawnPos(idx); rot = (0,
//   SData.rot.y, 0); camera 0x806AC400(cameraMgr[809C21D0]->+0x68[idx], &pos); pos.y += 700;
//   setInitialPhysicsValues(pos, rot) 0x80584044; 0x80590E28 -> KartCamera::initPos; KartItem::clear 0x80798848;
//   KCL probe checkSpherePartialPush(r 250, pos, (pos.x, pos.y + 300, pos.z), 0x20E80DFF): closest entry attribute
//   >> 8 & 7 -> 0x80591924(kart, variant) -> light/shadow params (0x80560648) for accessor +0x44 and +0x48;
//   Lakitu 0x8071E734(jugemMgr[809C28B8], idx, jugem); status0 &= ~TriggerRespawn, status1 |= InRespawn;
//   cameraMgr obj +0x69 = 1; optional managers [809C4748]+0x4EC[idx] = 1, [809C496C]+0x1F68[idx] = 1;
//   Enemy::AI::onOutOfBounds(jugem) 0x807263B0; if getItemPoint(jugem+0x29): KartItem::setItemPoint(idx, true)
//   0x807B99D4; jugem+0x2C > 0 -> sound.
// Other jugem users: 0x80538FEC / 0x805391E4 group respawn points by +0x2A (battle), not ported.
```

## Item float rounding: normalise, cross product, trail blends (ninth pass, 2026-11-03)

Found chasing the green shell drift in `test4` (1 to 3 ulps in one position component, starting while held). All of it
is operation order; the formulas were right.

```c
// Normalise helpers shared by the item code (now source/game/item/ItemMath.hh):
//   0x807872C0 NormaliseInto(out, in): sq = fma(z, z, x*x) + y*y; sq > 0 -> out = in * FrSqrt(sq) (0x80085040,
//     frsqrte + one Newton step); returns whether it normalised.
//   0x80793F04 NormaliseLength(out, in): mag = PSVECMag(in) (0x8019AC68: the same square, then sqrt as
//     sq * FrSqrt(sq)); mag > 0 -> out = in * (1 / mag), a true division. Used by the shells and the chain links.
// PSVECNormalize 0x8019AC24: same square and the same Newton step inline, no zero check.
// PSVECCrossProduct 0x8019ACCC (a x b): x = fms(a.y, b.z, b.y*a.z); y = -fms(a.x, b.z, b.x*a.z);
//   z = -fms(a.y, b.x, b.y*a.x), each with one rounded product -> EGG::Vector3f::ps_cross.
// ItemTrail::Orthonormalise 0x807912E0: PSVECNormalize(up); right = PSVECCrossProduct(up, fwd);
//   PSVECNormalize(right); fwd = PSVECCrossProduct(right, up).
// ItemTrail::calcKartPose 0x80791E5C: up += (target - up) * 0.2 as ps_sub + ps_madds0 (fused); target = bodyUp
//   (pose column 1) when busy, else KartMove +0x38 (smoothedUp). fwd = pose column 2; Orthonormalise; old pose[3] kept
//   at +0x9C; pose[3] = getPos(); bob (+0xCC) decays (x 0.85) or, with status bit 29, eases to the wheelie lift;
//   ofs = trailUpOfs (+0xE0) * scale.z (+0xB8, the scale from before this call); pose[3].y += bob, then
//   pose[3] += fwd * ofs; posDelta (+0x9C) = pose[3] - old. The scale (+0xB0) is only updated after this, and the
//   constructor (0x80791760) and init (0x80791840) never set it, so it is 0 on the race's first use.
// ItemTrail::calcTrailPos 0x80792618 (single), calcChain 0x8079356C, calcOrbit 0x80794694:
//   wall offset on a sphere hit: ofs += (tangentOff - ofs) * 0.2, ps_sub + ps_madds0 (fused); no hit: ofs *= 0.9.
//   hover height: single / orbit kcl + ((lift - hop) + heightOfs * scale.y);
//                 chain (lift - hop) * liftScale + (kcl + heightOfs * scale.y).
//   chain swing scale: 110 * (speedRatio * speedRatio).
// ItemTrail::calcOrbitPositions 0x807941F0: slot constants cos120 = -0.5, sin120 = 0.8660254 (0x3F5DB3D7).
```

**Measured**: `test4` items 13,004 -> 15,387 of 15,400 bit-exact (the rest are 13 throw frames), `test3` items
10,366 -> 10,638 of 11,435, `test5` 10,777 of 10,777, no kart differences, 13/13 suite, 56/56 ghosts.

## The throw frame: where a thrown item starts (tenth pass, 2026-11-04)

```c
// ItemObjKoura::initThrow 0x807B5178 (speed, trail, bounces, forward, floats):
//   forward ? 0x80794E88 (speed, 1.0, 35) : 0x80795030 (30, 1.0, 40) -> +0x1A0 speed, +0x1A4 dir;
//   +0x1B0.. basis (dir, side = (-dir.z, 0, dir.x)); vel.x = dir.x * speed (+0x50), vel.z = dir.z * speed (+0x58);
//   if (scale +0x68 < 0.8) { scaleVec +0x5C..+0x64 = scale = 0.8; if (forward) pos +0x44 = KartObjectProxy::getPos() }
//   bounces +0x1C8 / limit +0x1CD, onWall table 0x808D25E0 (+0x174..), 0x807915A8 (air frames from status 41/42),
//   flags +0x78 |= 0x01000000 (Released).
// Banana / fake box throw 0x807B7104: the same getPos() copy (0x807B7374) for a forward throw, then the 0.8 floor.
// getPos is the kart's own position. The trail's m_pose[3] (+0x58 + 0x24, set by calcKartPose 0x80791E5C) is that
// position plus the wheelie bob and the item forward offset (stats +0x17C * scale.z, 3 for the Standard Kart): the
// base the held item hangs from, never what a throw starts from. Kinoko used it in both throws (the 13 throw frames of
// test4); fixed 2026-11-04.
```

## Item against course objects (eleventh pass, 2026-11-04)

```c
// Callers. ItemObj update 0x8079EFEC, after the state function, with a box collision unit (+0xB0):
//   not landed (+0x78 bit 3): BoxColManager search 0x80786774 (mask 0xE: item and both object bits),
//     item-item 0x807A1C94, then 0x807A1ED8;
//   landed and kind 9 (bob-omb): search with mask 0xC (objects only), then 0x807A1ED8.
// ItemTrail 0x80791CC8, per held item that has a unit (last to first): search 0xE, 0x807A1C94, 0x807A1ED8.
// Explosions (0x807A3648) also call the director search: these are course objects' blasts, not the
//   bob-omb's or blue shell's (fifteenth pass).

// ItemObj::calcObjectCollision 0x807A1ED8 (item = this):
//   if (!boxColUnit) return;
//   min = max = (0,0,0);                                          // 0x808A5200
//   n = ObjectDirector::checkItemObjectCollision(radius +0xB8, item, &pos +0x44);   // 0x8082ADBC
//   if (n && vanishesOnHit(item)) { startVanish(item, 0); return; }  // 0x8079FF14, 0x807A6C14
//   for i < n:
//     r = director +0x48[i]; d = director +0x44 + 12 * i;        // reaction on the item, hit depth
//     if (!(flags & 0x40 Breaking) && r) { VEC3Minimize(min, min, d); VEC3Maximize(max, max, d); }
//         // 0x800855C0 / 0x80085580: fsel on (b - a) / (a - b), so equal values keep the first operand
//     if (r == 1) {                                              // bounce
//       p = (prevPos.x - d.x, prevPos.y, prevPos.z - d.z);       // prevPos +0xBC
//       v = 0x8082B42C(i);  // object's collision ? its +0x10 movement (0x80573520) : zero (0x802A4100)
//       calcItemPairResponse(item, 1, p, v);                     // 0x807A18FC, the item-item response
//       flags &= ~0x20000;                                       // owner immunity off
//     } else if (r == 2) {                                       // knocked aside, breaks
//       s = (vel.z, 0, -vel.x); if (PSVECDot(s, d) > 0) s *= -1; s.y = 0;   // dot as ps_madd + ps_sum0
//       NormaliseInto(s, s);  p = s * 200 + pos;                 // 0x807872C0, 0x808A5268
//       if (vel.y > 0) vel.y = 0; vel.x *= -0.25; vel.z *= -0.25;   // 0x808A526C
//       calcItemPairResponse(item, 2, p, -vel);
//     }
//     if (!boxColUnit) break;
//   pos += min + max;

// ObjectDirector::checkItemObjectCollision 0x8082ADBC (radius f1, item r4, pos r5):
//   point collider +0x134 (vtable 0x808D7FF8; support = centre +0x154, radius +0x150) = (pos, radius);
//   while (obj = BoxColManager::getNextObject()) {               // 0x80785E5C
//     if (!obj->collision()) continue;                           // vt +0xB4
//     onItem = GeoHitTableItem   (+0x08) row[slot(obj->id())][item->kind];   // rows of 15 s16, header skipped
//     onObj  = GeoHitTableItemObj(+0x0C) row[slot(obj->id())][item->kind];
//     if (onObj && onItem != 7) { if (onItem == 1 && (item flags & 1 Trailing)) continue; }
//     else if (onItem == 0 || onItem == 7 || (item flags & 1)) continue;
//     obj->calcCollisionTransform();                             // vt +0x74
//     if (!ObjectCollisionBase::check(point, obj->collision(), &director +0x44[n])) continue;   // 0x80834348
//     obj->processItemReactions(item, &onItem, &onObj);          // vt +0xBC
//     if (!item || !vanishesOnHit(item))
//       director +0x48[n] = obj->onItemCollision(item, onItem, onObj, &depth[n]);   // vt +0xC4
//     director +0x40[n++] = obj;
//   }
//   director +0x4C = n;  (the kart search 0x8082AB04 uses the same three arrays; no y clamp here)
// Table constructor 0x807F9278: count, field count, then per row a header s16 and `fields` values; the row pointer
//   kept at +0x0C points past the header. Director constructor 0x8082A38C loads, in order, GeoHitTableItem.bin (+0x08),
//   GeoHitTableItemObj.bin (+0x0C), GeoHitTableKart.bin (+0x10), GeoHitTableKartObj.bin (+0x14).
// Values: on the item 0 none, 1 bounce, 2 break, 7 never collide; on the object 3 / 4 hit, 5 / 6 hit offline only.
//   On DS Delfino Square: item boxes are 0 / 0 for every kind (never collide); the wooden box breaks green and red
//   shells, bananas and bob-ombs and is broken by shells and bob-ombs (object reaction 5); the small tree 0x15B
//   bounces green shells and bananas and breaks red shells.

// ObjectCollidable defaults (vtable 0x808D6DD8): +0xBC processItemReactions 0x8081F778: director +0x54 (online) or
//   +0x55 (Time Trial / ghost race) -> unchanged, else onObj 5 -> 3, 6 -> 4 (unlike the kart one, nothing dropped).
//   +0xC4 onItemCollision 0x806817A4: return onItem.

// Overrides ported (object state already in Kinoko):
//   ObjectBreakable 0x8076F1DC (wooden boxes, both kinds): item && onObj 3 -> vt +0x10C breakByItem(item, depth),
//     onObj 5 -> vt +0x110 breakByItemUntimed; mission counter (RaceConfig +0xB70 == 4) left out; return onItem.
//     Base +0x10C / +0x110 empty (0x8076F2D8 / 0x8076F2DC).
//   ObjectWoodbox 0x8077EA6C: breakObject(vt +0xF8)(item flags & 0x8000 NoOwner ? 12 : owner +0x6C, -depth); effect
//     +0x100 and sound 0x808204FC visual. 0x8077EB08: the same unless director +0x55.
//   ObjectDokan 0x80778CC4: onObj 3 or 5 and !+0xB0 -> +0xB0 = 1, +0xB4 = ey * 100 (0x808D04C8); no rumble.
//   ObjectPylon 0x8082DFD8: state +0xB0 <= 1 -> +0xE4 = RaceManager timer, startHit with factor 0.5 (0x808AC8B8),
//     the same arithmetic as the kart's 0x8082E3F0 (vel = (-d.x, 0, -d.z) normalised, angVel = vel * 0.5 * 0.5,
//     vel.y = 0.85, vel *= 0.5 * 100, depth normalised); returns 0 for every onObj (the item passes through).
//   ObjectHeyhoBall 0x806D0978: onObj 3..6 and state != 3 -> next state 3 (explode).
//   ObjectFireSnakeV 0x806C18A8: onObj 3..6 and state != 5 -> next state 5 (despawning).
//   ObjectVolcanoBall 0x806E2D80: onObj 3..6 -> +0xF9 = 1. calcBurning 0x806E3324 then disables the collision
//     (vt +0x90) every frame; calcFalling 0x806E3034 re-enables it (vt +0x94) when the rail ends; init 0x806E2C4C and
//     enterState1 0x806E2F38 clear +0xF9.
//   ObjectKoopaFigure64 0x806C0824, ObjectOilSFC 0x806DDA54: item -> depth = 0, return onItem.
//   ObjectChoropuHoll 0x806B9674: item -> depth = 0, return 0. (ObjectChoropuGround 0x806B926C returns onItem.)

// Not ported: the object's VS hit state is not in Kinoko for karts either (kart reactions 3 / 4 are dropped in
//   Time Trials, which is all Kinoko ran before VS):
//   ObjectDossun* 0x80760118 / 0x80760AD4 (vt +0x108: anim state 5, a spin; base returns 1 while +0xB4 is 0),
//   ObjectPenguin 0x80775CE0 (vt +0x108 0x80775D2C: state 5), ObjectHwanwan 0x806E9D6C (state 2),
//   ObjectKuribo 0x806DBA98 (squashed, drops a mushroom through 0x80799FBC), ObjectCarA 0x806B802C,
//   ObjectCarTGE 0x806D7780, ObjectCow 0x806BC3F0, ObjectHanachan 0x806C6E40, ObjectHeyho 0x806CF198,
//   ObjectCrab 0x808862D4, ObjectPakkunF 0x8077502C, ObjectParasolR 0x80779A68, ObjectPoihana 0x80749504,
//   ObjectBoble 0x8075E28C, ObjectBasabasaDummy 0x806B6A18. Their item hits use the default (return onItem).
//   (All but the crab and the Cataquack are ported in the fourteenth pass.)

// The shells' wall callbacks (+0x174, called by calcItemPairResponse reaction 1, for item-item and item-object):
//   green / thrown 0x807B62B8 (set by initThrow 0x807B5178 from 0x808D25E0, thunk 0x807A3920):
//     n.y > 0 ? reflect(n) : reflect((n.x, 0, n.z)); vel = moveDir +0x1B0 * speed +0x1A0.
//     reflect is the KCL wall reflection of 0x807B54F4: proj = n * PSVECDot(dir, n) (0x807DE8B8); dir -= proj;
//     reflects (+0x1CE) ? dir -= proj; y damping 0.7 (0x808A6144); NormaliseInto; side = PSVECCrossProduct(dir, up);
//     moveDir = dir; plus a sound (0x807A2F18, id 0xF9). (Kinoko's side uses cross(), not ps_cross(); side is not
//     read before it is recomputed, so it has no effect.)
//   red shell on the route or homing 0x807AA5AC (enterStateRoute 0x807AAE58, enterStateHoming 0x807AB004, tables
//     0x808D220C / 0x808D2218, thunk 0x807A3928): 0x807B532C (free from velocity), 0x807B62B8, next state +0x2B8 = 1.
//   Kinoko had both left null, which also skipped the bounce in item-item collisions.
//   0x807B66F0 (a shell restarted from its velocity) also sets 0x807B62B8; it is not ported.
```

**Measured**: no item touches a course object in any of the five recordings, and all stay as before: no kart
differences, items 15,400 / 10,777 / 10,638 bit-exact, 13/13 suite, 56/56 ghosts. Self-check on DS Delfino Square
(`test5` inputs plus scripted items): a green shell thrown at race frame 6790 meets wooden box 1 at 6802 (reaction 2),
breaks, and the box breaks and drops a mushroom credited to the thrower at 6802 (the kart breaks it at 6808 without the
shell) and the kart drives through at full speed; a green shell bounces off a small tree (velocity (204, -2, -20) to
(171, 0, -113), pushed out by the hit depth); red shells break on trees.

## Race start: one draw per player from the second generator (twelfth pass, 2026-11-04)

```c
// RaceManager::init 0x80532F88 (called from RaceScene::initEngines 0x8055472C, after KartObjectManager::init):
//   flags +0x1C / +0x1D / +0x20 / +0x30, slipstream turn +0x38 = 0x80000000, KPadDirector::lockGhostProxies,
//   for (i = 0; i < RaceConfig player count (+0x24); ++i) Player::init(players +0x0C [i]);
//   then the game mode's vtable +0x1C.
// RaceManager::Player::init 0x80534194:
//   +0x22 = 0 (u16);
//   r = RaceManager +0x08 (the second generator)->getU32(10);   // 0x805555CC, at 0x805341C4
//   +0x4C = +0x04 = 1 << (r + 1);                                // one bit from 2 to 1024
//   then the checkpoint search (findSector 0x80511500 from the kart's position, jugem index), and
//   +0x2C = +0x30 = 0, +0x50 = 0 (u16), +0x52 = 13, +0x34 = 7, +0x38 |= 1.
// Nothing reads +0x04 or +0x4C: the only getter, 0x80534934 (popcount 0x800850B0, then the bit index, 0 when above
//   9), has no caller and no pointer to it anywhere in the image; no RaceManager::Player function loads either word.
// Of the code Kinoko has, the second generator's other offline draws are the item boxes' spin phases (constructor
//   0x808285B0, RaceScene::createEngines 0x80554208, so before the race init). After it, offline VS draws only from
//   the first generator (ObjectDirector 0x80782F74 and ItemRaceFlags 0x807BD718 use the second one online, and the
//   item code in Time Trials, where no recorded ghost breaks or drops an item).
```

**Second generator in the dumps** (the logger records its state, `rng["2"]` words 3 and 4): at race load the game
has drawn 30 times on DS Delfino Square (`test5`) and 434 times on the Luigi Circuit recordings (`test4`), then 2
more on each of two intro frames and about one every 100 frames through the countdown and race. Kinoko draws 20 and
22 (the item boxes and now the two players). The rest comes from objects Kinoko does not simulate (visual-only course
objects and effects), so Kinoko's second generator cannot follow the game's; offline nothing that moves a kart or an
item reads it, which is why every recording matches. Online it would matter (`ObjectDirector::random` and the item
code use it there).

**Measured**: Kinoko now makes the same draws in the same order (players 0 to n-1, each before its checkpoint
search) and keeps the value as `RaceManager::Player::m_startRandomBit`. All five recordings stay bit-exact (no
kart differences, items unchanged), 13/13 suite, 56/56 ghosts.

## Status bit 131: a racer driven by another console (thirteenth pass, 2026-11-04)

```c
// Writers of the word KartState +0x14 bit 0x8 (scan of the whole image for ori / rlwinm on that word): only the
// KartState constructor 0x805943B4. KartState::reset 0x80594594 wipes words 0 to 3 only, and nothing clears it, so it
// lasts the race.
// KartState constructor, word 4 (+0x14) from the race config player type (RaceConfig player +0x38):
//   type 0 (local) -> 0x02 (bit 129), 1 (CPU) -> 0x01 (bit 128), 3 (ghost) -> 0x40 (bit 134);
//   then 0x809C1950 ? 0x04 (bit 130) : 0x809C1951 ? 0x08 (bit 131).            // 0x80594490..0x805944C8
// KartObject creation 0x8058F5B4, per kart: initPlayerTypeTemporaries 0x80592FA8 clears both bytes; if RaceConfig
//   +0xB70 (game mode) is 7..10 (online): player type 0 -> 0x809C1950 = 1, type 4 (remote) -> 0x809C1951 = 1.
//   With 0x809C1951 the kart also gets the remote KartMove (0x80577FC4, 0x294 bytes, KartObject 0x80595E08) and a
//   remote data object (0x8058985C, 0xB4 bytes, KartObject +0x5C, read back through KartObjectProxy::kartNetReceiver
//   0x8059144C = accessor +0x40). (A static initialiser at 0x805F8A4C zeroes the bytes at boot.)
// So bit 129, which the recordings always show set and Kinoko does not have, is the local-player bit; bit 131 is set
// only online, for a racer driven by another console.

// KartMove::calcAcceleration 0x8057B9BC, after the wheelie-wobble factor (0x8057BF08):
//   if (status bit 131 && receiver +0x84 > speedLimit) speedLimit = receiver +0x84;    // 0x8057BF1C, fcmpo + ble
//   It raises the limit to a floor, not a cap (an earlier note here said min).
// receiver +0x84: the remote racer's soft speed limit as its console had it. RacedataFactory_packRacedataRecord
//   0x8058CB30 packs the sender's KartMove +0x20 (speed), +0x18 (soft speed limit) and +0x9C, each as a 4-bit
//   quantised float (0x8052CDC8); the receiver 0x80589F64 reads them back (0x8052CE58) in that order into +0x80,
//   +0x84, +0x88.
// Other readers of bit 131 (all remote-racer special cases, not ported; Kinoko cannot have a remote racer):
//   KartCollide::processFloor 0x8056EA04, the item hits 0x8057257C (0x80572684), activateOob 0x80573B00,
//   updateRespawn 0x80573ED4, respawn 0x805799AC, trick start 0x80575D7C, mini-turbo charge 0x8057EE50, KartState
//   input and collision updates 0x8059487C / 0x80594BD4, KartObject update 0x80596480, a dozen more in
//   0x80571D98..0x8059B7B8, and five in 0x8053B4CC..0x8053B7A4 (found by scanning the image for the bit test).
```

**Measured**: Kinoko now has the bit (`eStatus::OnlineRemote`), sets it in `KartState::init` under the same condition
(online game mode and player type 4, a new `RaceConfig::Player::Type::Remote`), keeps it through `reset`, and applies
the floor from `KartMove::m_receivedSoftSpeedLimit` (set by a host; Kinoko has no network). No race Kinoko can run has
a remote racer, so all five recordings stay bit-exact, 13/13 suite, 56/56 ghosts.

## Course objects hit by items and by special karts (fourteenth pass, 2026-11-04)

```c
// The kart search ObjectDirector 0x8082AB04 (Kinoko read column 0 only, "not in a star, mega or bullet"):
//   column = 0x8082B4E0(kart): status bit 63 (star) -> 1, else bit 79 (mega) -> 2, else bit 91 (bullet) -> 3, else 0;
//   onKart = GeoHitTableKart [slot(id)][column]; onObj = GeoHitTableKartObj [slot(id)][column];
//   skips objects whose vt +0xD8 is false while status bit 88 is set (default 0x80681734 returns 0; nothing in
//   Kinoko sets bit 88, its role is not identified: not ported);
//   processKartReactions (vt +0xB8), r = onCollision (vt +0xC0);
//   if !(status bit 32, in an action): r == 8 || r == 0x1A ? vt +0xCC (0x8081F5D8) : vt +0xC8 (0x8081F548)
//     (both default to an effect and a sound; Kinoko had them unconditional and empty);
//   obj +0xA8 = 1 (read only by the default effect); depth.y clamped at 0.
// For a kart in none of the three states, the KartObj column of every object below is 0 (no reaction), so these
// objects react only to items and to karts in a star, mega mushroom or bullet bill. Offline VS turns 5 into 3 and
// 6 into 4 (0x8081F66C, 0x8081F778); Time Trials and online leave 5 / 6 and drop 3 / 4.
// The "soko" lookups in onCollision overrides read the kart's column too: Dossun 0x8075FF98 (DossuncSoko),
// Press 0x807782D4 (PressSoko), CarTGE 0x806D7328 (its dummy id, both tables), BasabasaDummy 0x806B6874 (both).
// DAT_802a4130 in the decompiles is EGG::Vector3f::ey, 802a4100 the zero vector. 0x80590478 (kart, bool) is the
// star / mega rumble (visual).

// BasabasaDummy (bats): kart 0x806B6874, item 0x806B6A18 (onObj 3..6; null item: -base(2) * 80).
//   Hit (state != 1): next state 1, +0x11C = hitter pos (unused), +0x128 = hitter vel (kart 0x80590D08 velocity;
//   item vel, or (bat - item) normalised * 80 when |vel| < 30).
//   enterState1 0x806B6288: vt +0x8C unregisterCollision; dir = hitVel, dir.y = |hitVel.xz| * sin(20) / cos(20)
//   (FIdx 0.7111111 * 20), normalise2; +0xF4 = dir * 150 (+0x100). calcState1 0x806B652C: vel.y -= 2, pos += vel;
//   the +0x14E branch is dead (nothing sets it); after 130 frames vt +0xC init (which draws the race generator
//   3 times) and +0xF0 active = 0. Tumbling matrix and blinking visual.
// Penguin 0x80775C58 / 0x80775CE0 (onObj 3/5) -> vt +0x108: base 0x80775D2C, PenguinS 0x80776D90 (saves state
//   +0xD8): state 5, +0xC0 = 180, +0xC4 = 60. calc state 5: base 0x80775804 (rail, calcPos, 0x80775950, timer ->
//   enterWalk), PenguinS 0x807766E8 (calcRail 0x807768A0, calcPos, 0x80775950; at the end state 2/3 -> stand-up 4
//   with the anim's frame count, else enterWalk; +0xB4 = base(2)). 0x80775950: speed -= 1 (back up at < 12),
//   rot.y += speed * DEG2RAD, calcTransform. calcRail also accepts state 5 at settings 0 and 1.
// Kuribo (Goomba) kart 0x806DB764 (onObj 3/5), item 0x806DBA98 (3..6, null item allowed):
//   0x806DD128 knock direction: flat = v - tangent0(+0xF8) * dot; +0xEC = +-base2(+0xDC) by the side of
//   (railmid + flat) relative to the first-last rail line (0x806B38A8).
//   0x806DC460 squash: vt +0x90 disableCollision, +0x12C = +0x130 = 1, +0x134 = 0, +0x136 = 1, +0x138 = 0;
//   if +0x104 (!TT && item mode != none) 0x80799FBC(kind 3 mushroom, pos, +0xEC * 50, owner) (no random draw).
//   0x806DC5E0 squash (+0x136): scaleY = 1 + amp * SinFIdx(40.743664 * (2pi * t / 20)), amp 1 below full height
//   else 0.5; widen +0.1 up to 1.5; at < 0.1 -> hidden. 0x806DC790 hidden (+0x13C) until setting0 < t + squashT.
//   0x806DC9D8 regrow (+0x144): scaleY = 1.5 * Sin(.. t / 40); ends when t > 10 && scaleY <= 1: vt +0x94.
//   Every stage first calls calcAnim 0x806DCDDC again, so a hit Goomba walks twice per frame (as in the game).
// Hwanwan (Chain Chomp) 0x806E9CAC (onObj 0: speed ratio < 0.5 -> wall; 3/5 -> next state 2), 0x806E9D6C (3..6).
//   State 2 0x806EA288 / 0x806EA2FC: +0x404 = -1, +0x410 = 150 (50 on Mario Circuit with floor variant +0x49C == 1,
//   set by checkFloorCollision 0x806EA784 from the closest KCL entry); y += 150 - 2.5 t until landed (t > 9 on the
//   floor), then y += 0.6 * 150 - 2.5 (t - land); state 0 ten frames after landing. Spin and chain links visual.
// CarA 0x806B7E60 / 0x806B802C: launch (+0x110 = 0, +0x118 = 2*90/3, +0x130 = 390/+0x118, +0x114 = pos.y,
//   +0xF8 = ey); calc 0x806B82CC runs 0x806B89A8 instead of the follow-the-rail placement: y = base +
//   parabola(90, 3, t) (bounce parabola(27, 3, t - land) after the first touch below the rail from frame 6), pitch
//   +0x12C, matrix OrthonormalBasis(tangent) * makeRT(pitch); ends at the second landing (vt +0x94).
// CarTGE 0x806D7328 / 0x806D7780: per player +0x1B1 squash flags and the manager's per-player timers (+0x120,
//   0x806D50AC; Kinoko had one of each); launch 0x806D7EA4 (+0x208 = 200/3, +0x204 = 390/+0x208, disable
//   collision) and flight 0x806D827C (roll about the forward axis 0x806B3AC4, corners +-350, one bounce at 30%,
//   the static 0x808C5880 = 20 / 24 / 33 by the forward's slope; the landings take the model's matrix, Kinoko the
//   car's own). Bomb cars (type 2, 0x806D7F58, exploding through 0x8079A654 kind 9) are not in Kinoko (PANIC).
// Cow 0x806BC2AC / 0x806BC3F0 -> 0x806BCB28 (disable collision, timer +0x120 = 0); leader and follower calc tails
//   0x806BD388 / 0x806BE1A8: timer++, 0x806BCBA4: model height parabola(45, 3, t); from t > 5 a 50 sphere at
//   pos + h + 50 (0x64EBDFFF) ends it (enable collision). The other +0x120 reads guard animations.
// Hanachan (Wiggler) parts 0x806C6C94 / 0x806C6E40 (not bob-ombs) -> 0x806C723C (disable collision, +0xC8 = 46);
//   part calc 0x806C6C10 -> 0x806C72B8 (drawn hop parabola(70, 3, t), land (10 sphere, t > 5), second hop at half
//   speed, collision back at the second landing, +0xD1). Manager 0x806C9860 tail 0x806CBBC4: a part on its first
//   hop frame starts a wave; every 4 frames the next parts on both sides hop (+0xD2 on all parts blocks hits);
//   it ends when the far end part lands. Touch wobble +0xB0 / +0xB4 / +0xB8 and colour fades visual; +0xE0 / +0xE1
//   (WallSpark) are never set.
// Heyho (Shy Guy) 0x806CF024 / 0x806CF198 -> 0x806CFAC0; calc 0x806CEDF8 runs 0x806CFB38 before the up
//   interpolation: forward turned about up by -(40 t - 0.5 * (32/120) t^2) for 120 frames, then faster by 4 per
//   frame until within 0.97 (not written back then, except while jumping off the floor).
// PakkunF 0x80774F20 / 0x8077502C (null item too) -> 0x80774EB8: state 2, +0xBC = 180; calc state 2 back to wait.
// ParasolR 0x8077971C / 0x80779A68: onObj 3/5 toggle (visual), 4/6 hop: +0xB8 = ey * 100, calc 0x80779174:
//   vel.y -= 2, pos += vel, 0x80779400 bounce at a 100 sphere (0x20E80FFF): vel.y *= -0.2, stop below 0.2.
// Boble 0x8075E1F4 / 0x8075E28C (item, onObj 3) -> 0x8075E2E0: six ObjectDirector::random draws (nextF32 180,
//   nextF32 30, getU32 12 three times, getU32 3), speed 30 / 50 / 100 by |depth| + |hitter vel|; state 1
//   0x8075DEAC: gravity 2, tumble, back on the rail at a setting-1 point after 180 frames (+0xC4 is never cleared).
// Dossun 0x8075FF98 (onObj 3/5 -> vt +0x108), items 0x80760118 (stationary 0x80760AD4): Thwomps standing still
//   bounce every item; vt +0x108 Normal 0x80760A44 (anm state 5, +0xF8 = 60, +0x100, +0x124 = 60), Syuukai
//   0x8075FF08 (only while stomping); state 5 rises 30 per frame (0x807605EC, 0x8075F520); spin 0x80760634 /
//   0x8075F584 (Normal from its calc 0x807602E0, stops after 60 frames near +0xFC; Syuukai in the last 30 cycle
//   frames). Base / YokoMove / Dossunc 0x807642F8 (+0x124 = 2 movement state) and Tsuibi: not ported.

// Not ported:
//   Crab 0x808861AC / 0x808862D4: the hit puts the crab in state 2 (inert: both handlers return 0 then) inside the
//     1272-line walk state machine 0x80883B98 (flight, KCL checks, resurfacing state 3), which Kinoko simplified for
//     Time Trials. Needs a full port of 0x80883B98.
//   Poihana (Cataquack) 0x80749378 / 0x80749504: its hit flags (+0x348 grabbed a kart, +0x349 knocked) feed the VS
//     chase AI (states 2 to 7, target search 0x80747BFC), all stubs in Kinoko.
//   PakkunF's VS targeting (0x807743E4 sets the timed wait +0xB8 only in TT and online; offline it attacks the
//     nearest racer within 2000, 0x80782C14, and turns its head, +0xDC): Kinoko has only the timed wait.
//   Moonview Highway bomb cars (and the manager's blast launches 0x806D52E0): Kinoko PANICs on them.
```

**Measured**: none of the five recordings has a kart in a star or mega touching an object, or an item hitting one
of these objects, so all stay as before: no kart differences, items 15,400 / 10,777 / 10,638 bit-exact, 13/13 suite,
56/56 ghosts. Self-check with a temporary hook (removed) forcing a star-kart hit on every object at frame 700 on 11
courses (Mushroom Gorge, Moo Moo Meadows, Maple Treeway, Coconut Mall, DK Summit, Bowser's Castle, N64 Sherbet
Land, GCN Mario Circuit, Dry Dry Ruins, Mario Circuit, Grumble Volcano): no crash; Goombas lose their collision and
get it back after about 630 frames, cows after their jump (under 30 frames), Wiggler parts after about 80, cars fly
about 1350 up and land, Thwomps and penguins spin.

## Blasts of course objects (fifteenth pass, 2026-11-04)

```c
// 0x807A3648 is not the bob-omb or blue shell blast. Its only caller is the thunk 0x8079A654
//   (r3 = ItemDirector + 0x2A4, then b 0x807A3648), called by exploding course objects:
//   ObjectKoopaBall calcExploding 0x80771324 (Bowser's fireballs): while timer > 20 after its decrement,
//     radius 870 * 2 (0x808A3EA4 * 0x808CF3F4), pos +0x30, kind 9;
//   ObjectHeyhoBall calcExploding 0x806D0F24 (the Shy Guy ship's cannonballs): every frame before the end of
//     the blast, radius 1500 (0x808C4BB8), kind 9;
//   CarTGE bomb cars 0x806D8B5C (kind 9; not in Kinoko);
//   Hanachan 0x806CC934, behind the manager's vt +0x104, which is 0x806C9BB8 (returns 0) in the only vtable: dead.
// Bob-omb and blue shell blasts reach course objects through the item's own pass (0x8079EFEC: landed bob-ombs
//   search with mask 0xC, others 0xE, then 0x807A1ED8 with the grown hit radius), ported in the eleventh pass.
//
// The stand-in item at ItemDirector +0x2A4: built by the ItemObj constructor 0x8079D8BC, then 0x807A3618:
//   +0x74 = 0, +0x78 = 0, +0xB0 = null (no box unit), +0x7C = 0x40 (so 0x8079FF14 "vanishes on hit" is false),
//   vel +0x50 = zero. Owner +0x6C stays 0, so a wooden box it breaks credits player 0.
// ItemDirector::explodeAt 0x807A3648 (radius f1, stand-in r3, pos r4, kind r5):
//   stand-in kind +0x04 = kind, pos +0x44 = pos, hit radius +0xB8 = radius;
//   BoxColManager search 0x80786B14 (pos, radius, mask 0xE);
//   while (item = getNextItem 0x80785DF4):
//     if (PSVECSquareDistance(stand-in pos, item pos) < (radius + item hit radius)^2)
//       calcItemPairResponse 0x807A18FC(item, kind 9 or 5 ? 2 : table 0x808A4DFC [item kind][kind],
//                                       stand-in pos, stand-in vel (zero));
//   ObjectDirector::checkItemObjectCollision 0x8082ADBC(radius, stand-in, stand-in pos): the objects react as to a
//     bob-omb (its GeoHitTableItemObj column); what the hits would do to the stand-in is not used.
```

**Measured**: no change in the five recordings (no kart differences, items 15,400 / 10,777 / 10,638 bit-exact) or
the 13 suite cases, and all 56 ghosts still sync. The ghosts cover all 32 courses, Bowser's Castle and GBA Shy Guy
Beach among them, so the new blasts already run in those Time Trials. In a VS race on those two courses (`test1`
inputs) the blasts run on 918 and 560 frames and never reach an item or a reacting object. A blast forced onto
DS Delfino Square's first wooden box (temporary hook, removed) broke both boxes there, and each dropped its item
credited to player 0.

## Maple Treeway's leaf piles (sixteenth pass, 2026-11-09)

Found by `test8` (Maple Treeway, Dolphin): at race frame 1131 P1 drove through a pile of dry leaves and the game
spawned a mushroom item object 380 units above it, credited to P1. Kinoko created the piles as `ObjectNoImpl`, so the
mushroom and the pile's random draws were missing, and the next item roll (1473) decided a banana instead of the
game's green shell. Object 0x176 is "karehayama" (ObjFlow.bin; 0x175 "leaf_effect", 0x2ED / 0x2F1 "EnvKareha*" are
its visual neighbours). It is an `ObjectBreakable`, the wooden boxes' base class.

```
// ObjectKarehayama, vtable 0x808CE020 (constructor 0x80766F4C: base constructor 0x8076EBE0, then the vtable;
// destructor 0x80766F88). Shared with the base: init 0x80766FE0 (base init 0x8076EC68 + effects), loadFlags
// 0x807677E4 (1), calc 0x8076ED1C, onCollision 0x8076F0AC, onItemCollision 0x8076F1DC, enableCollision 0x8076ED70.
// +0xF4 respawn 0x80767250: base respawn 0x8076EDD4 (enableCollision, state 1) + model / effect calls (visual).
// +0xF8 breakObject 0x807672A4 (player r4, dir r5):
//   r = ObjectDirector::random()->nextF32(10.0);                   // 0x80782F74, 0x805556A8, 10 at 0x808CE018
//   m.setAxisRotation((r - 10 * 0.5) * 0.017453292, ey);           // 0x802303BC, 0.5 at 0x808A3B50, 0x808A3B54
//   ObjectBreakable::breakObject(player, PSMTXMultVec(m, dir));      // 0x8076EDE4: drop chance draw, decideItem,
//                                                                    //   speed draw, spawnFromObject, state 2
// +0xFC calcFall 0x807677E0: nothing.  +0x100 0x8076724C: nothing.
// +0x104 breakByKart 0x8076734C (object reaction 3):
//   if (kart->speedRatioCapped() <= 0.4) { model off (+0x68); state = 2; disableCollision (+0x90); }   // 0x808A3B58
//   else breakObject(kart->playerIdx(), kart->velocity());          // 0x80590A5C, 0x80590D08
//   effects +0x118 (0x80767114) / +0x11C (0x807671DC); respawnTimer +0xB4 = 138;   // 0x808CE008
// +0x108 breakByKartUntimed 0x80767448 (object reaction 5): returns at once for a ghost kart outside a Time Trial
//   replay; otherwise as +0x104, but also scatters without a drop when director +0x55 (Time Trial / ghost race).
// +0x10C breakByItem 0x8076757C: breakObject(item flags +0x78 & 0x8000 ? 12 : owner +0x6C, -hitDepth); +0x11C;
//   respawnTimer = 138; sound 0x2E7.
// +0x110 breakByItemUntimed 0x80767648: model off; state 2; disableCollision; +0x11C; respawnTimer = 138; sound.
//   (Offline outside Time Trials processItemReactions 0x8081F778 turns object reaction 5 into 3, so an item breaks
//   a pile with a drop there.)
```

Ported as `ObjectKarehayama` (effects, model and sounds left out: nothing in the race reads them). With it `test8`
is identical: 19,460 kart-frames, 33,368 item-object frames bit-exact.

## Stars, megas, Thwomps, cars and Bowser's fireball (seventeenth pass, 2026-11-09)

Found by `test10` (Bowser's Castle, P1 forced to a star by the Gecko test code) and `test11` (Coconut Mall, forced
mega). Both diverged on the first frame P1 steered after the power-up started; the object diff (new
`course_object_logger.lua`) showed the rest.

```
// KartMove::calcRotation 0x8057C69C, the turn's speed shaping and multipliers (not drifting, going forwards):
//   s = (status +0xC & 0x8000 InMega) ? 1.25 : 1.0;                 // 0x808B59CC
//   low = 20 * s;                                                    // 0x808B5974
//   if (speed >= low) turn = (speed >= 70)                          // 0x808B5978, NOT scaled
//       ? turn * 0.5 : turn * 0.5 + (1 - (speed - low) / (70 * s - low)) * (turn * (1 - 0.5));
//   else turn = turn * 0.4 + (speed / low) * (turn * (1 - 0.4));    // 0x808B5970
//   if (!forwards) turn = -turn;
//   if ((InStar 63 || InMega 79 || ThunderCloud 93) && !DriftManual 3) turn *= 1.1;   // 0x808B59C8
//   if (ZipperBoost && !DriftManual) turn *= 2.0;                   // 0x808B5B44 (already ported)

// ObjectDossunNormal::init 0x8076023C: base init 0x8075EEA8; stomp state 0; rot from the matrix if not locked;
//   restRot +0xFC = rot.y; if (pi (0x808A38F0) <= restRot) restRot -= 2 pi (0x808A38EC).
//   Kinoko had `<=` the other way round: every Thwomp below pi rested at angle - 2 pi. Same direction, but
//   sin / cos (table lookups, 0x80085110) of -0.785398 and -7.068583 fall on different table steps:
//   cos 0x3F3504F8 / sin 0x3F3504F5 in the game, 0x3F3504F7 for both in Kinoko.

// ObjectCarA launch 0x806B89A8 (a car hit by a mega, star, bullet or item):
//   ... parabola above the rail, then a 30% bounce once below it (constants 90, 3, 0.3, 30 at 0x808C2204,
//   0x808C2208, 0x8089F1E0, 0x8089F1E4) ...
//   on the landing frame: baseY = rail y; landed = 1; landFrame = timer; f31 (the height) = 0.0 (0x806B8AC0,
//   loaded for the landing sound 0x2AE), so the car sits at the rail's height that frame.
//   mat = basis(tangent) * makeRT((0, 0, angle * DEG2RAD)): the car rolls about its own z axis (Kinoko had x).

// ObjectKoopaBall (Bowser's fireball): init 0x807703D0 copies the transform it just built from the rail
//   tangent (+0x58) to +0xF0 (0x807704A4). Each frame of flight (0x80770F4C):
//   angle +0x120 += -angSpeed * DEG2RAD; transform = makeR((angle, 0, 0)) * base(+0xF0); translation = pos.
//   Kinoko multiplied the current transform on the left, so the turns compounded.
// calcIntangible 0x80771248: when the cooldown +0xE0 runs out, fire; cooldown = director +0x55 (Time Trial /
//   ghost race) ? 210 (0x808CF3EC) : 270 (0x808CF3E8). Kinoko always used 210.

// KartSub::calcMovingObj 0x80597A88 (ported, not exercised by any recording yet):
//   if ((status +0x8 & 0x4000000, bit 58) || InBullet || !movingObjCount) decay(0.7, airtime < 20 ? 1 : 0.9, floor);
//   else { objVel /= floorCount;
//          if (KartCollide +0x2C & 0x8000 MovingRoad && (InStar || InMega || InBullet || ThunderCloud)) {
//              objVel = proj(objVel, KartMove +0x68 lastDir); if (dot(objVel, lastDir) < 0) objVel = 0; }
//          else if (CPU) { ... split along lastDir, forward part scaled by KartSub +0x5C ... }
//          setMovingRoadVel(0.2, objVel); }
//   KartCollide::processFloor 0x8056EA04 sets +0x2C 0x8000 when the floor mask has KCL type 0x15 (moving road),
//   at 0x8056EDF4; calcFloorEffect 0x80571634 clears it (reset mask 0xFFFE04AC).
```

Ported: the turn multipliers, the Thwomp rest angle, the car roll axis and landing frame, the fireball's base
matrix and VS cooldown, and the moving-road branch. With them `test10` and `test11` are identical for every kart
frame, and every course object is bit-exact except Coconut Mall's two escalator groups, whose own matrix the game
keeps at the KMP rotation while Kinoko leaves it identity (the escalators themselves are right; nothing reads the
group's matrix).

## Red shells: the staggered target search (eighteenth pass, 2026-11-09)

Found by `test12` (Dry Dry Ruins, P1 forced to triple red shells): a red shell following the item route locked on to
P2 (parked at the start line) one frame earlier in Kinoko, slowed down a frame early and missed the hit the game made.

```
// ItemObjRedShell::searchTarget 0x807B3BAC (fromOwnerPlace r4, roundRobin r5), called by the free, search and route
// states once the state is 5 frames old (roundRobin = 0) and from 17 frames on (roundRobin = 1):
//   for (i = 0; i < playerCount (0x809C38B8); ++i) {
//     if (roundRobin && !(raceFrame (0x809C38C0) < 1 || (i & 3) == raceFrameMod4 (0x809C38C8))) continue;
//     if (tryTarget(i, &bestDistSq, fromOwnerPlace)) found = 1;   // 0x807B381C
//   }
// ItemRaceFlags frame copy 0x807BD340, the first call of ItemDirector::calc 0x80799850:
//   +0x08 = RaceManager +0x20 (the race frame); +0x0C = frame & 1; +0x10 = frame & 3; +0x14 = frame & 7.
```

So after its first 17 frames a red shell looks at racer i only on race frames where frame & 3 == i & 3: P2 (index 1)
on frames 1, 5, 9, ... Kinoko looked at every racer every frame. Ported (`ItemRaceFlags::CalcRaceFrame`, the
round-robin skip in `searchTarget`); `test12` then matches every kart frame up to 7126 (was 2555). The remaining
difference starts at race frame 2979 with 24 race-generator draws Kinoko does not make (caller not identified yet;
see `DOLPHIN_VERIFY_RESULT.md`).
