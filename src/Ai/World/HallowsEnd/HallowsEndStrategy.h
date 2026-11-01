/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_HALLOWSENDSTRATEGY_H
#define PLAYERBOTS_HALLOWSENDSTRATEGY_H

#include "MovementActions.h"
#include "Strategy.h"
#include "Value.h"

bool IsHallowsEndActive(PlayerbotAI* botAI);

class HallowsEndStrategy : public Strategy
{
public:
    HallowsEndStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}
    std::string const getName() override { return "hallows end"; }
    uint32 GetType() const override { return STRATEGY_TYPE_NONCOMBAT; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    void InitMultipliers(std::vector<Multiplier*>& multipliers) override;
};

class HallowsEndActiveValue : public CalculatedValue<bool>
{
public:
    HallowsEndActiveValue(PlayerbotAI* botAI) : CalculatedValue<bool>(botAI, "hallows end active", 2) {}

protected:
    bool Calculate() override;
};

class HallowsEndTrigger : public Trigger
{
public:
    HallowsEndTrigger(PlayerbotAI* botAI) : Trigger(botAI, "hallows end fires", 1) {}
    bool IsActive() override;
};

class HallowsEndWaterAction : public MovementAction
{
public:
    HallowsEndWaterAction(PlayerbotAI* botAI) : MovementAction(botAI, "hallows end water") {}
    bool isUseful() override;
    bool isPossible() override;
    bool Execute(Event event) override;

private:
    ObjectGuid _barrel;
    uint32 _lastPositionSearch = 0;
};

class HallowsEndExtinguishAction : public MovementAction
{
public:
    HallowsEndExtinguishAction(PlayerbotAI* botAI) : MovementAction(botAI, "hallows end extinguish") {}
    bool isUseful() override;
    bool isPossible() override;
    bool Execute(Event event) override;
    void ResetFireSelection();

private:
    ObjectGuid _fire;
    ObjectGuid _bucket;
    uint32 _bucketCount = 0;
    bool _bucketFireSelected = false;
    uint32 _lastPositionSearch = 0;
};

#endif
