/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "HallowsEndStrategy.h"

#include <algorithm>

#include "AiObjectContext.h"
#include "AreaDefines.h"
#include "CheckMountStateAction.h"
#include "ChooseTravelTargetAction.h"
#include "Creature.h"
#include "FollowActions.h"
#include "GameObject.h"
#include "Item.h"
#include "Map.h"
#include "MoveToRpgTargetAction.h"
#include "MoveToTravelTargetAction.h"
#include "NewRpgBaseAction.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Random.h"
#include "RpgAction.h"
#include "Spell.h"
#include "SpellMgr.h"
#include "Timer.h"
#include "TravelAction.h"
#include "WorldSession.h"

namespace
{
constexpr uint32 GO_WATER_BARREL = 186234;
constexpr uint32 ITEM_WATER_BUCKET = 32971;
constexpr uint32 NPC_FIRE_TRIGGER = 23686;
constexpr uint32 NPC_SHADE_OF_HORSEMAN = 23543;
constexpr uint32 SPELL_FIRE_AURA = 42074;
constexpr uint32 SPELL_HORSEMAN_MOUNT = 48025;
constexpr uint32 SPELL_THROW_BUCKET = 42340;
// The event covers 100 yards around the Horseman; bots can stand on the opposite edge.
constexpr float SEARCH_RADIUS = 200.0f;

bool CanFightFires(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    if (!sPlayerbotAIConfig.HallowsEndEnabled)
        return false;
    if (!bot->IsAlive())
        return false;
    if (bot->IsInCombat())
        return false;
    if (bot->IsFlying())
        return false;
    if (bot->IsInFlight())
        return false;
    if (bot->GetVehicle())
        return false;

    // Gate grid searches by zone, including the outskirts of each participating village.
    switch (bot->GetZoneId())
    {
        case AREA_ELWYNN_FOREST:
        case AREA_DUN_MOROGH:
        case AREA_AZUREMYST_ISLE:
        case AREA_DUROTAR:
        case AREA_TIRISFAL_GLADES:
        case AREA_EVERSONG_WOODS:
            return true;
        default:
            return false;
    }
}

bool HasBucketLineOfSight(Player* bot, Creature* fire, SpellInfo const* spellInfo, Position const& origin)
{
    if (spellInfo->HasAttribute(SPELL_ATTR2_IGNORE_LINE_OF_SIGHT) ||
        spellInfo->HasAttribute(SPELL_ATTR5_ALWAYS_AOE_LINE_OF_SIGHT))
        return true;

    // Match Spell::CheckCast for a destination, not the flame creature's hit sphere.
    float height = bot->GetCollisionHeight();
    return bot->GetMap()->isInLineOfSight(origin.GetPositionX(), origin.GetPositionY(), origin.GetPositionZ() + height,
                                          fire->GetPositionX(), fire->GetPositionY(), fire->GetPositionZ() + height,
                                          bot->GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::M2);
}

Creature* FindFire(Player* bot, bool randomSelection)
{
    std::list<Creature*> fires;
    bot->GetCreatureListWithEntryInGrid(fires, NPC_FIRE_TRIGGER, SEARCH_RADIUS);
    Creature* selected = nullptr;
    uint32 candidates = 0;
    for (Creature* fire : fires)
    {
        if (!fire->HasAura(SPELL_FIRE_AURA))
            continue;

        bool select = randomSelection ? urand(1, ++candidates) == 1
                                      : !selected || bot->GetExactDist(fire) < bot->GetExactDist(selected);
        if (select)
            selected = fire;
    }
    return selected;
}

void PrepareInteraction(Player* bot)
{
    bot->StopMoving();
    bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
    bot->SetStandState(UNIT_STAND_STATE_STAND);
}

class HallowsEndParticipationMultiplier : public Multiplier
{
public:
    HallowsEndParticipationMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "hallows end participation") {}

    float GetValue(Action* action) override
    {
        if (!dynamic_cast<FollowAction*>(action) && !dynamic_cast<CheckMountStateAction*>(action) &&
            !dynamic_cast<RpgAction*>(action) && !dynamic_cast<NewRpgBaseAction*>(action) &&
            !dynamic_cast<TravelAction*>(action) && !dynamic_cast<ChooseTravelTargetAction*>(action) &&
            !dynamic_cast<MoveToTravelTargetAction*>(action) && !dynamic_cast<MoveToRpgTargetAction*>(action) &&
            !dynamic_cast<MoveRandomAction*>(action))
            return 1.0f;

        return CanFightFires(botAI) && context->GetValue<bool>("hallows end active")->Get() ? 0.0f : 1.0f;
    }
};
}  // namespace

bool IsHallowsEndActive(PlayerbotAI* botAI)
{
    if (!sPlayerbotAIConfig.HallowsEndEnabled)
        return false;

    Player* bot = botAI->GetBot();
    if (!bot || !bot->GetSession() || !bot->IsInWorld() || bot->IsBeingTeleported() || bot->IsDuringRemoveFromWorld() ||
        bot->GetSession()->IsLoggingOut() || !botAI->GetAiObjectContext())
        return false;

    return CanFightFires(botAI) && botAI->GetAiObjectContext()->GetValue<bool>("hallows end active")->Get();
}

bool HallowsEndActiveValue::Calculate()
{
    if (!CanFightFires(botAI))
        return false;

    if (FindFire(bot, false))
        return true;

    // Reset even when the event trigger is inactive and cannot queue the extinguish action.
    if (auto* action = dynamic_cast<HallowsEndExtinguishAction*>(context->GetAction("hallows end extinguish")))
        action->ResetFireSelection();

    // An empty wave does not end the event: the flying Horseman can ignite more buildings.
    Creature* horseman = bot->FindNearestCreature(NPC_SHADE_OF_HORSEMAN, SEARCH_RADIUS, true);
    if (horseman && horseman->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE) && horseman->HasAura(SPELL_HORSEMAN_MOUNT))
        return true;

    return false;
}

void HallowsEndStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    Strategy::InitTriggers(triggers);
    triggers.push_back(
        new TriggerNode("hallows end fires", {NextAction("hallows end water", ACTION_MOVE + 1.0f),
                                              NextAction("hallows end extinguish", ACTION_MOVE + 1.0f)}));
}

void HallowsEndStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    Strategy::InitMultipliers(multipliers);
    multipliers.push_back(new HallowsEndParticipationMultiplier(botAI));
}

bool HallowsEndTrigger::IsActive()
{
    return CanFightFires(botAI) && context->GetValue<bool>("hallows end active")->Get();
}

bool HallowsEndWaterAction::isUseful()
{
    return CanFightFires(botAI) && !bot->IsNonMeleeSpellCast(false) && !bot->HasItemCount(ITEM_WATER_BUCKET, 1);
}

bool HallowsEndWaterAction::isPossible()
{
    if (!isUseful() || !IsMovingAllowed() || !context->GetValue<bool>("hallows end active")->Get())
        return false;

    ItemPosCountVec destination;

    if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, destination, ITEM_WATER_BUCKET, 1) != EQUIP_ERR_OK)
        return false;

    GameObject* barrel = bot->FindNearestGameObject(GO_WATER_BARREL, SEARCH_RADIUS);

    if (!barrel || !barrel->isSpawned())
        return false;

    if (_barrel != barrel->GetGUID())
        _lastPositionSearch = 0;
    _barrel = barrel->GetGUID();

    return true;
}

bool HallowsEndWaterAction::Execute(Event /*event*/)
{
    GameObject* barrel = botAI->GetGameObject(_barrel);
    if (!isUseful() || !barrel || !barrel->isSpawned())
        return false;

    if (!barrel->IsWithinDistInMap(bot, barrel->GetInteractionDistance()))
    {
        if (IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL))
        {
            SetDuration(250);
            return true;
        }

        constexpr uint32 SEARCH_INTERVAL_MS = 2000;
        uint32 now = getMSTime();
        if (_lastPositionSearch && getMSTimeDiff(_lastPositionSearch, now) < SEARCH_INTERVAL_MS)
            return false;
        _lastPositionSearch = now;

        // A route around an obstacle does not require line of sight from the starting point.
        float interactionDistance = barrel->GetInteractionDistance();
        float startAngle = barrel->GetAngle(bot);
        constexpr uint32 POSITION_SAMPLES = 16;

        for (uint32 i = 0; i < POSITION_SAMPLES; ++i)
        {
            float angle = startAngle + i * static_cast<float>(2.0 * M_PI) / POSITION_SAMPLES;
            float radius = std::min(2.0f, interactionDistance * 0.5f);
            float x = barrel->GetPositionX() + std::cos(angle) * radius;
            float y = barrel->GetPositionY() + std::sin(angle) * radius;
            float z = barrel->GetPositionZ();
            bot->UpdateAllowedPositionZ(x, y, z);
            if (z <= INVALID_HEIGHT)
                continue;

            PathGenerator path(bot);
            if (!path.CalculatePath(x, y, z) || !(path.GetPathType() & PATHFIND_NORMAL) ||
                (path.GetPathType() & (PATHFIND_INCOMPLETE | PATHFIND_SHORTCUT | PATHFIND_NOPATH)))
                continue;

            auto const& end = path.GetActualEndPosition();
            if (barrel->GetExactDist(end.x, end.y, end.z) >= interactionDistance)
                continue;

            if (MoveTo(bot->GetMapId(), end.x, end.y, end.z, false, false, true, true))
            {
                SetDuration(250);
                return true;
            }
        }
        return false;
    }

    PrepareInteraction(bot);
    WorldPacket packet(CMSG_GAMEOBJ_USE, 8);
    packet << barrel->GetGUID();
    bot->GetSession()->HandleGameObjectUseOpcode(packet);

    SetDuration(sPlayerbotAIConfig.GlobalCoolDown);
    return true;
}

bool HallowsEndExtinguishAction::isUseful()
{
    return CanFightFires(botAI) && !bot->IsNonMeleeSpellCast(false) && !bot->HasSpellCooldown(SPELL_THROW_BUCKET) &&
           bot->HasItemCount(ITEM_WATER_BUCKET, 1);
}

void HallowsEndExtinguishAction::ResetFireSelection()
{
    _fire.Clear();
    _bucketFireSelected = false;
    _lastPositionSearch = 0;
}

bool HallowsEndExtinguishAction::isPossible()
{
    if (!isUseful())
        return false;

    Item* bucket = bot->GetItemByEntry(ITEM_WATER_BUCKET);
    if (!bucket)
        return false;

    // A new item or a consumed stack charge starts a new bucket's target selection.
    if (bucket->GetGUID() != _bucket || bucket->GetCount() != _bucketCount)
    {
        _bucket = bucket->GetGUID();
        _bucketCount = bucket->GetCount();
        ResetFireSelection();
    }

    Creature* fire = botAI->GetCreature(_fire);
    if (fire && fire->HasAura(SPELL_FIRE_AURA))
        return bot->IsWithinDistInMap(fire, SEARCH_RADIUS);

    // Random for each bucket; nearest if its target goes out before the throw.
    fire = FindFire(bot, !_bucketFireSelected);
    if (!fire)
    {
        ResetFireSelection();
        return false;
    }

    _fire = fire->GetGUID();
    _bucketFireSelected = true;
    _lastPositionSearch = 0;

    return true;
}

bool HallowsEndExtinguishAction::Execute(Event /*event*/)
{
    Creature* fire = botAI->GetCreature(_fire);
    Item* bucket = bot->GetItemByEntry(ITEM_WATER_BUCKET);
    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(SPELL_THROW_BUCKET);
    if (!isUseful() || !fire || !fire->HasAura(SPELL_FIRE_AURA) || !bucket || !spellInfo)
        return false;

    float range = spellInfo->GetMaxRange(false, bot);
    if (bot->GetExactDist(fire) >= range || !HasBucketLineOfSight(bot, fire, spellInfo, *bot))
    {
        if (!IsMovingAllowed())
            return false;
        if (bot->isMoving() && IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL))
        {
            SetDuration(250);
            return true;
        }

        constexpr uint32 SEARCH_INTERVAL_MS = 2000;
        uint32 now = getMSTime();
        if (_lastPositionSearch && getMSTimeDiff(_lastPositionSearch, now) < SEARCH_INTERVAL_MS)
            return false;
        _lastPositionSearch = now;

        // Multiple ground rings cover both broad roofs and narrow approaches.
        std::vector<Position> positions;
        float angle = fire->GetAngle(bot);
        constexpr uint32 POSITION_SAMPLES = 16;
        for (float radiusFactor : {0.9f, 0.65f, 0.4f, 0.2f})
        {
            for (uint32 i = 0; i < POSITION_SAMPLES; ++i)
            {
                float offset = angle + i * static_cast<float>(2.0 * M_PI) / POSITION_SAMPLES;
                float x = fire->GetPositionX() + std::cos(offset) * range * radiusFactor;
                float y = fire->GetPositionY() + std::sin(offset) * range * radiusFactor;
                float z = bot->GetMapHeight(x, y, bot->GetPositionZ());
                if (z <= INVALID_HEIGHT)
                    continue;
                if (fire->GetExactDist(x, y, z) >= range)
                    continue;
                if (!HasBucketLineOfSight(bot, fire, spellInfo, Position(x, y, z)))
                    continue;

                positions.emplace_back(x, y, z);
            }
        }
        std::sort(positions.begin(), positions.end(), [this](Position const& left, Position const& right)
                  { return bot->GetExactDistSq(left) < bot->GetExactDistSq(right); });

        for (Position const& position : positions)
        {
            PathGenerator path(bot);
            if (!path.CalculatePath(position.GetPositionX(), position.GetPositionY(), position.GetPositionZ()) ||
                !(path.GetPathType() & PATHFIND_NORMAL) ||
                (path.GetPathType() & (PATHFIND_INCOMPLETE | PATHFIND_SHORTCUT | PATHFIND_NOPATH)))
                continue;

            auto const& end = path.GetActualEndPosition();
            if (fire->GetExactDist(end.x, end.y, end.z) >= range ||
                !HasBucketLineOfSight(bot, fire, spellInfo, Position(end.x, end.y, end.z)))
                continue;

            if (MoveTo(bot->GetMapId(), end.x, end.y, end.z, false, false, true, true))
                return true;
        }
        return false;
    }

    PrepareInteraction(bot);
    if (bot->CanUseItem(bucket) != EQUIP_ERR_OK)
        return false;

    SpellCastTargets targets;
    targets.SetDst(fire->GetPositionX(), fire->GetPositionY(), fire->GetPositionZ(), 0.0f);

    // Use the item opcode so charges, cooldowns and the bucket's consumption follow normal player rules.
    WorldPacket packet(CMSG_USE_ITEM);
    packet << bucket->GetBagSlot() << bucket->GetSlot() << uint8(1) << SPELL_THROW_BUCKET << bucket->GetGUID()
           << uint32(0) << uint8(0);
    targets.Write(packet);
    bot->GetSession()->HandleUseItemOpcode(packet);

    SetDuration(sPlayerbotAIConfig.GlobalCoolDown);
    return true;
}
