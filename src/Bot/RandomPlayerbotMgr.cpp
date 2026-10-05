/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RandomPlayerbotMgr.h"
#include "PlayerbotsDatabase.h"
#include "AiFactory.h"
#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "Cell.h"
#include "CellImpl.h"
#include "ChannelMgr.h"
#include "DBCStores.h"
#include "DBCStructure.h"
#include "DatabaseEnv.h"
#include "Define.h"
#include "FleeManager.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "LFGMgr.h"
#include "MapMgr.h"
#include "NewRpgInfo.h"
#include "NewRpgStrategy.h"
#include "ObjectGuid.h"
#include "Opcodes.h"
#include "PerfMonitor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"
#include "Position.h"
#include "RaceMgr.h"
#include "Random.h"
#include "RandomPlayerbotFactory.h"
#include "ServerFacade.h"
#include "SharedDefines.h"
#include "TravelMgr.h"
#include "Unit.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSessionMgr.h"
#include <algorithm>
#include <boost/thread/thread.hpp>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <random>
#include <set>
#include <utility>

struct GuidClassRaceInfo
{
    ObjectGuid::LowType guid;
    uint32 rClass;
    uint32 rRace;
};

void PrintStatsThread() { sRandomPlayerbotMgr.PrintStats(); }

void activatePrintStatsThread()
{
    boost::thread t(PrintStatsThread);
    t.detach();
}

void CheckBgQueueThread() { sRandomPlayerbotMgr.CheckBgQueue(); }

void activateCheckBgQueueThread()
{
    boost::thread t(CheckBgQueueThread);
    t.detach();
}

void CheckLfgQueueThread() { sRandomPlayerbotMgr.CheckLfgQueue(); }

void activateCheckLfgQueueThread()
{
    boost::thread t(CheckLfgQueueThread);
    t.detach();
}

void CheckPlayersThread() { sRandomPlayerbotMgr.CheckPlayers(); }

void activateCheckPlayersThread()
{
    boost::thread t(CheckPlayersThread);
    t.detach();
}

class botPIDImpl
{
public:
    botPIDImpl(double dt, double max, double min, double Kp, double Ki, double Kd);
    ~botPIDImpl();
    double calculate(double setpoint, double pv);
    void adjust(double Kp, double Ki, double Kd)
    {
        _Kp = Kp;
        _Ki = Ki;
        _Kd = Kd;
    }
    void reset() { _integral = 0; }

private:
    double _dt;
    double _max;
    double _min;
    double _Kp;
    double _Ki;
    double _Kd;
    double _pre_error;
    double _integral;
};

botPID::botPID(double dt, double max, double min, double Kp, double Ki, double Kd)
{
    pimpl = new botPIDImpl(dt, max, min, Kp, Ki, Kd);
}
void botPID::adjust(double Kp, double Ki, double Kd) { pimpl->adjust(Kp, Ki, Kd); }
void botPID::reset() { pimpl->reset(); }
double botPID::calculate(double setpoint, double pv) { return pimpl->calculate(setpoint, pv); }
botPID::~botPID() { delete pimpl; }

/**
 * Implementation
 */
botPIDImpl::botPIDImpl(double dt, double max, double min, double Kp, double Ki, double Kd)
    : _dt(dt), _max(max), _min(min), _Kp(Kp), _Ki(Ki), _Kd(Kd), _pre_error(0), _integral(0)
{
}

double botPIDImpl::calculate(double setpoint, double pv)
{
    // Calculate error
    double error = setpoint - pv;

    // Proportional term
    double Pout = _Kp * error;

    // Integral term
    _integral += error * _dt;
    double Iout = _Ki * _integral;

    // Derivative term
    double derivative = (error - _pre_error) / _dt;
    double Dout = _Kd * derivative;

    // Calculate total output
    double output = Pout + Iout + Dout;

    // Restrict to max/min
    if (output > _max)
    {
        output = _max;
        _integral -= error * _dt;  // Stop integral buildup at max
    }
    else if (output < _min)
    {
        output = _min;
        _integral -= error * _dt;  // Stop integral buildup at min
    }

    // Save error to previous error
    _pre_error = error;

    return output;
}

botPIDImpl::~botPIDImpl() {}

uint32 RandomPlayerbotMgr::GetMaxAllowedBotCount() { return GetEventValue(0, "bot_count"); }

void RandomPlayerbotMgr::LogPlayerLocation()
{
    activeBots = 0;

    try
    {
        sPlayerbotAIConfig.openLog("player_location.csv", "w");

        if (sPlayerbotAIConfig.randomBotAutologin)
        {
            for (auto i : GetAllBots())
            {
                Player* bot = i.second;
                if (!bot)
                    continue;

                std::ostringstream out;
                out << sPlayerbotAIConfig.GetTimestampStr() << "+00,";
                out << "RND"
                    << ",";
                out << bot->GetName() << ",";
                out << std::fixed << std::setprecision(2);
                WorldPosition(bot).printWKT(out);
                out << bot->GetOrientation() << ",";
                out << std::to_string(bot->getRace()) << ",";
                out << std::to_string(bot->getClass()) << ",";
                out << bot->GetMapId() << ",";
                out << bot->GetLevel() << ",";
                out << bot->GetHealth() << ",";
                out << bot->GetPowerPct(bot->getPowerType()) << ",";
                out << bot->GetMoney() << ",";

                if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
                {
                    out << std::to_string(uint8(botAI->GetGrouperType())) << ",";
                    out << std::to_string(uint8(botAI->GetGuilderType())) << ",";
                    out << (botAI->AllowActivity(ALL_ACTIVITY) ? "active" : "inactive") << ",";
                    out << (botAI->IsActive() ? "active" : "delay") << ",";
                    out << botAI->HandleRemoteCommand("state") << ",";

                    if (botAI->AllowActivity(ALL_ACTIVITY))
                        activeBots++;
                }
                else
                {
                    out << 0 << "," << 0 << ",err,err,err,";
                }

                out << (bot->IsInCombat() ? "combat" : "safe") << ",";
                out << (bot->isDead() ? (bot->GetCorpse() ? "ghost" : "dead") : "alive");

                sPlayerbotAIConfig.log("player_location.csv", out.str().c_str());
            }

            for (auto i : GetPlayers())
            {
                Player* bot = i;
                if (!bot)
                    continue;

                std::ostringstream out;
                out << sPlayerbotAIConfig.GetTimestampStr() << "+00,";
                out << "PLR"
                    << ",";
                out << bot->GetName() << ",";
                out << std::fixed << std::setprecision(2);
                WorldPosition(bot).printWKT(out);
                out << bot->GetOrientation() << ",";
                out << std::to_string(bot->getRace()) << ",";
                out << std::to_string(bot->getClass()) << ",";
                out << bot->GetMapId() << ",";
                out << bot->GetLevel() << ",";
                out << bot->GetHealth() << ",";
                out << bot->GetPowerPct(bot->getPowerType()) << ",";
                out << bot->GetMoney() << ",";

                if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
                {
                    out << std::to_string(uint8(botAI->GetGrouperType())) << ",";
                    out << std::to_string(uint8(botAI->GetGuilderType())) << ",";
                    out << (botAI->AllowActivity(ALL_ACTIVITY) ? "active" : "inactive") << ",";
                    out << (botAI->IsActive() ? "active" : "delay") << ",";
                    out << botAI->HandleRemoteCommand("state") << ",";

                    if (botAI->AllowActivity(ALL_ACTIVITY))
                        activeBots++;
                }
                else
                {
                    out << 0 << "," << 0 << ",player,player,player,";
                }

                out << (bot->IsInCombat() ? "combat" : "safe") << ",";
                out << (bot->isDead() ? (bot->GetCorpse() ? "ghost" : "dead") : "alive");

                sPlayerbotAIConfig.log("player_location.csv", out.str().c_str());
            }
        }
    }
    catch (...)
    {
        return;
        // This is to prevent some thread-unsafeness. Crashes would happen if bots get added or removed.
        // We really don't care here. Just skip a log. Making this thread-safe is not worth the effort.
    }
}

void RandomPlayerbotMgr::UpdateAIInternal(uint32 /*elapsed*/, bool /*minimal*/)
{
    if (totalPmo)
        totalPmo->finish();

    totalPmo = sPerfMonitor.start(PERF_MON_TOTAL, "RandomPlayerbotMgr::FullTick");

    if (!sPlayerbotAIConfig.randomBotAutologin || !sPlayerbotAIConfig.enabled)
        return;

    /*if (sPlayerbotAIConfig.enablePrototypePerformanceDiff)
    {
        LOG_INFO("playerbots", "---------------------------------------");
        LOG_INFO("playerbots",
                 "PROTOTYPE: Playerbot performance enhancements are active. Issues and instability may occur.");
        LOG_INFO("playerbots", "---------------------------------------");
        ScaleBotActivity();
    }*/

    uint32 maxAllowedBotCount = GetEventValue(0, "bot_count");
    if (!maxAllowedBotCount || (maxAllowedBotCount < sPlayerbotAIConfig.minRandomBots ||
                                maxAllowedBotCount > sPlayerbotAIConfig.maxRandomBots))
    {
        maxAllowedBotCount = urand(sPlayerbotAIConfig.minRandomBots, sPlayerbotAIConfig.maxRandomBots);
        SetEventValue(0, "bot_count", maxAllowedBotCount,
                      urand(sPlayerbotAIConfig.randomBotCountChangeMinInterval,
                            sPlayerbotAIConfig.randomBotCountChangeMaxInterval));
    }

    GetBots();
    // Copied deliberately: ProcessBot() erases from currentBots while this is
    // being iterated below, so the loops must run over a snapshot.
    std::unordered_set<uint32> availableBots = currentBots;
    uint32 availableBotCount = availableBots.size();
    uint32 onlineBotCount = playerBots.size();

    uint32 onlineBotFocus = 75;
    if (onlineBotCount < (uint32)(sPlayerbotAIConfig.minRandomBots * 90 / 100))
        onlineBotFocus = 25;

    // only keep updating till initializing time has completed,
    // which prevents unneeded expensive GameTime calls.
    if (_isBotInitializing)
    {
        _isBotInitializing = GameTime::GetUptime().count() < sPlayerbotAIConfig.maxRandomBots * (0.11 + 0.4);
    }

    uint32 updateIntervalTurboBoost = _isBotInitializing ? 1 : sPlayerbotAIConfig.randomBotUpdateInterval;
    SetNextCheckDelay(updateIntervalTurboBoost * (onlineBotFocus + 25) * 10);

    // Queue autoscaling is allowed to reserve bots above the normal bot_count,
    // but never above QueueAutoScaleMaxBots. AddRandomBots() itself still uses
    // the normal bot_count and therefore cannot accidentally grow the normal
    // world population to the burst ceiling.
    uint32 effectiveAllowedBotCount = maxAllowedBotCount;
    if (sPlayerbotAIConfig.queueAutoScale && !queueBurstBots.empty())
    {
        effectiveAllowedBotCount = std::max<uint32>(effectiveAllowedBotCount,
            std::min<uint32>(sPlayerbotAIConfig.queueAutoScaleMaxBots, currentBots.size()));
    }

    PerfMonitorOperation* pmo = sPerfMonitor.start(
        PERF_MON_TOTAL,
        onlineBotCount < effectiveAllowedBotCount ? "RandomPlayerbotMgr::Login" : "RandomPlayerbotMgr::UpdateAIInternal");

    bool realPlayerIsLogged = false;
    if (sPlayerbotAIConfig.disabledWithoutRealPlayer)
    {
        if (sWorldSessionMgr->GetActiveAndQueuedSessionCount() > 0)
        {
            RealPlayerLastTimeSeen = time(nullptr);
            realPlayerIsLogged = true;

            if (DelayLoginBotsTimer == 0)
            {
                DelayLoginBotsTimer = time(nullptr) + sPlayerbotAIConfig.disabledWithoutRealPlayerLoginDelay;
            }
        }
        else
        {
            if (DelayLoginBotsTimer)
            {
                DelayLoginBotsTimer = 0;
            }

            if (RealPlayerLastTimeSeen != 0 && onlineBotCount > 0 &&
                time(nullptr) > RealPlayerLastTimeSeen + sPlayerbotAIConfig.disabledWithoutRealPlayerLogoutDelay)
            {
                LogoutAllBots();
                LOG_INFO("playerbots", "Logout all bots due no real player session.");
            }
        }

        if (availableBotCount < maxAllowedBotCount &&
            (sPlayerbotAIConfig.disabledWithoutRealPlayer == false ||
             (realPlayerIsLogged && DelayLoginBotsTimer != 0 && time(nullptr) >= DelayLoginBotsTimer)))
        {
            AddRandomBots();
        }
    }
    else if (availableBotCount < maxAllowedBotCount)
    {
        AddRandomBots();
    }

    if (sPlayerbotAIConfig.syncLevelWithPlayers && !players.empty())
    {
        if (time(nullptr) > (PlayersCheckTimer + 60))
            sRandomPlayerbotMgr.CheckPlayers();
    }

    if (sPlayerbotAIConfig.randomBotJoinBG ||
        (sPlayerbotAIConfig.queueAutoScale && sPlayerbotAIConfig.queueAutoScaleBg))
    {
        uint32 interval = sPlayerbotAIConfig.queueAutoScale && sPlayerbotAIConfig.queueAutoScaleBg
                              ? sPlayerbotAIConfig.queueAutoScaleCheckInterval
                              : 35;
        if (time(nullptr) > (BgCheckTimer + interval))
            sRandomPlayerbotMgr.CheckBgQueue();
    }

    if (sPlayerbotAIConfig.randomBotJoinLfg ||
        (sPlayerbotAIConfig.queueAutoScale && sPlayerbotAIConfig.queueAutoScaleLfg))
    {
        uint32 interval = sPlayerbotAIConfig.queueAutoScale && sPlayerbotAIConfig.queueAutoScaleLfg
                              ? sPlayerbotAIConfig.queueAutoScaleCheckInterval
                              : 30;
        if (time(nullptr) > (LfgCheckTimer + interval))
            sRandomPlayerbotMgr.CheckLfgQueue();
    }

    if (sPlayerbotAIConfig.queueAutoScale)
    {
        CleanupOrphanedQueueGroups();
        BalanceQueueBots();
    }

    if (sPlayerbotAIConfig.randomBotAutologin && sPlayerbotAIConfig.randomBotPrintStatsInterval &&
        time(nullptr) > (printStatsTimer + sPlayerbotAIConfig.randomBotPrintStatsInterval))
    {
        if (!printStatsTimer)
        {
            printStatsTimer = time(nullptr);
        }
        else
        {
            sRandomPlayerbotMgr.PrintStats();
            // activatePrintStatsThread();
        }
    }
    uint32 updateBots = sPlayerbotAIConfig.randomBotsPerInterval * onlineBotFocus / 100;
    uint32 maxNewBots =
        onlineBotCount < effectiveAllowedBotCount &&
                (sPlayerbotAIConfig.disabledWithoutRealPlayer == false ||
                 (realPlayerIsLogged && DelayLoginBotsTimer != 0 && time(nullptr) >= DelayLoginBotsTimer))
            ? effectiveAllowedBotCount - onlineBotCount
            : 0;
    uint32 loginBots = std::min(sPlayerbotAIConfig.randomBotsPerInterval - updateBots, maxNewBots);

    if (!availableBots.empty())
    {
        // Update bots
        for (auto bot : availableBots)
        {
            if (!GetPlayerBot(bot))
                continue;

            if (ProcessBot(bot))
            {
                updateBots--;
            }

            if (!updateBots)
                break;
        }

        if (loginBots && botLoading.empty())
        {
            loginBots += updateBots;
            loginBots = std::min(loginBots, maxNewBots);

            LOG_DEBUG("playerbots", "{} new bots prepared to login", loginBots);

            // Log in bots
            for (auto bot : availableBots)
            {
                if (GetPlayerBot(bot))
                    continue;

                if (ProcessBot(bot))
                {
                    loginBots--;
                }

                if (!loginBots)
                    break;
            }

            DelayLoginBotsTimer = 0;
        }
    }

    if (pmo)
        pmo->finish();

    if (sPlayerbotAIConfig.hasLog("player_location.csv"))
    {
        LogPlayerLocation();
    }
}

// void RandomPlayerbotMgr::ScaleBotActivity()
//{
//     float activityPercentage = getActivityPercentage();
//
//     // if (activityPercentage >= 100.0f || activityPercentage <= 0.0f) pid.reset(); //Stop integer buildup during
//     // max/min activity
//
//     //    % increase/decrease                   wanted diff                                         , avg diff
//     float activityPercentageMod = pid.calculate(
//         sRandomPlayerbotMgr.GetPlayers().empty() ? sPlayerbotAIConfig.diffEmpty :
//         sPlayerbotAIConfig.diffWithPlayer, sWorldUpdateTime.GetAverageUpdateTime());
//
//     activityPercentage = activityPercentageMod + 50;
//
//     // Cap the percentage between 0 and 100.
//     activityPercentage = std::max(0.0f, std::min(100.0f, activityPercentage));
//
//     setActivityPercentage(activityPercentage);
// }

// Assigns accounts as RNDbot accounts (type 1) based on MaxRandomBots and EnablePeriodicOnlineOffline and its ratio,
// and assigns accounts as AddClass accounts (type 2) based AddClassAccountPoolSize. Type 1 and 2 assignments are
// permenant, unless MaxRandomBots or AddClassAccountPoolSize are set to 0. If so, their associated accounts will
// be unassigned (type 0)
void RandomPlayerbotMgr::AssignAccountTypes()
{
    LOG_INFO("playerbots", "Assigning account types for random bot accounts...");

    // Clear existing filtered lists
    rndBotTypeAccounts.clear();
    addClassTypeAccounts.clear();

    // First, get ALL randombot accounts from the database
    std::vector<uint32> allRandomBotAccounts;
    QueryResult allAccounts = LoginDatabase.Query(
        "SELECT id FROM account WHERE username LIKE '{}%%' ORDER BY id",
        sPlayerbotAIConfig.randomBotAccountPrefix.c_str());

    if (allAccounts)
    {
        do
        {
            Field* fields = allAccounts->Fetch();
            uint32 accountId = fields[0].Get<uint32>();
            allRandomBotAccounts.push_back(accountId);
        } while (allAccounts->NextRow());
    }

    LOG_INFO("playerbots", "Found {} total randombot accounts in database", allRandomBotAccounts.size());

    // Check existing assignments
    PlayerbotsDatabasePreparedStatement* assignmentsStmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_SEL_ACCOUNT_TYPE);
    PreparedQueryResult existingAssignments = PlayerbotsDatabase.Query(assignmentsStmt);
    std::map<uint32, uint8> currentAssignments;

    if (existingAssignments)
    {
        do
        {
            Field* fields = existingAssignments->Fetch();
            uint32 accountId = fields[0].Get<uint32>();
            uint8 accountType = fields[1].Get<uint8>();
            currentAssignments[accountId] = accountType;
        } while (existingAssignments->NextRow());
    }

    // Mark ALL randombot accounts as unassigned if not already assigned
    for (uint32 accountId : allRandomBotAccounts)
    {
        if (currentAssignments.find(accountId) == currentAssignments.end())
        {
            PlayerbotsDatabasePreparedStatement* stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_INS_ACCOUNT_TYPE);
            stmt->SetData(0, accountId);
            stmt->SetData(1, uint8(0));
            PlayerbotsDatabase.DirectExecute(stmt);
            currentAssignments[accountId] = 0;
        }
    }

    // Calculate needed RNDbot accounts
    uint32 neededRndBotAccounts = 0;
    if (sPlayerbotAIConfig.maxRandomBots > 0)
    {
        int divisor = RandomPlayerbotFactory::CalculateAvailableCharsPerAccount();
        int maxBots = sPlayerbotAIConfig.maxRandomBots;

        // Take periodic online-offline into account
        if (sPlayerbotAIConfig.enablePeriodicOnlineOffline)
        {
            maxBots *= sPlayerbotAIConfig.periodicOnlineOfflineRatio;
        }

        // Calculate base accounts needed for RNDbots, ensuring round up for maxBots not cleanly divisible by the divisor
        neededRndBotAccounts = (maxBots + divisor - 1) / divisor;
    }

    // Count existing assigned accounts
    uint32 existingRndBotAccounts = 0;
    uint32 existingAddClassAccounts = 0;

    for (auto const& [accountId, accountType] : currentAssignments)
    {
        if (accountType == 1) existingRndBotAccounts++;
        else if (accountType == 2) existingAddClassAccounts++;
    }

    // Assign RNDbot accounts from lowest position if needed
    if (existingRndBotAccounts < neededRndBotAccounts)
    {
        uint32 toAssign = neededRndBotAccounts - existingRndBotAccounts;
        uint32 assigned = 0;

        for (uint32 i = 0; i < allRandomBotAccounts.size() && assigned < toAssign; i++)
        {
            uint32 accountId = allRandomBotAccounts[i];
            if (currentAssignments[accountId] == 0) // Unassigned
            {
                PlayerbotsDatabasePreparedStatement* stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_UPD_ACCOUNT_TYPE);
                stmt->SetData(0, uint8(1));
                stmt->SetData(1, accountId);
                PlayerbotsDatabase.DirectExecute(stmt);
                currentAssignments[accountId] = 1;
                assigned++;
            }
        }

        if (assigned < toAssign)
        {
            LOG_ERROR("playerbots", "Not enough unassigned accounts to fulfill RNDbot requirements. Need {} more accounts.", toAssign - assigned);
        }
    }

    // Assign AddClass accounts from highest position if needed
    uint32 neededAddClassAccounts = sPlayerbotAIConfig.addClassAccountPoolSize;

    if (existingAddClassAccounts < neededAddClassAccounts)
    {
        uint32 toAssign = neededAddClassAccounts - existingAddClassAccounts;
        uint32 assigned = 0;

        for (size_t idx = allRandomBotAccounts.size(); idx-- > 0 && assigned < toAssign;)
        {
            uint32 accountId = allRandomBotAccounts[idx];
            if (currentAssignments[accountId] == 0) // Unassigned
            {
                PlayerbotsDatabasePreparedStatement* stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_UPD_ACCOUNT_TYPE);
                stmt->SetData(0, uint8(2));
                stmt->SetData(1, accountId);
                PlayerbotsDatabase.DirectExecute(stmt);
                currentAssignments[accountId] = 2;
                assigned++;
            }
        }

        if (assigned < toAssign)
        {
            LOG_ERROR("playerbots", "Not enough unassigned accounts to fulfill AddClass requirements. Need {} more accounts.", toAssign - assigned);
        }
    }

    // Populate filtered account lists with ALL accounts of each type
    for (auto const& [accountId, accountType] : currentAssignments)
    {
        if (accountType == 1) rndBotTypeAccounts.push_back(accountId);
        else if (accountType == 2) addClassTypeAccounts.push_back(accountId);
    }

    LOG_INFO("playerbots", "Account type assignment complete: {} RNDbot accounts, {} AddClass accounts, {} unassigned",
             rndBotTypeAccounts.size(), addClassTypeAccounts.size(),
             currentAssignments.size() - rndBotTypeAccounts.size() - addClassTypeAccounts.size());
}

bool RandomPlayerbotMgr::IsAccountType(uint32 accountId, uint8 accountType)
{
    PlayerbotsDatabasePreparedStatement* stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_SEL_ACCOUNT_TYPE_BY_ACCOUNT_AND_TYPE);
    stmt->SetData(0, accountId);
    stmt->SetData(1, accountType);
    return PlayerbotsDatabase.Query(stmt) != nullptr;
}

bool RandomPlayerbotMgr::IsAddClassAccount(uint32 accountId) const
{
    return std::find(addClassTypeAccounts.begin(), addClassTypeAccounts.end(), accountId) !=
           addClassTypeAccounts.end();
}

// Logs-in bots in 4 phases. Phase 1 logs Alliance bots up to how much is expected according to the faction ratio,
// and Phase 2 logs-in the remainder Horde bots to reach the total maxAllowedBotCount. If maxAllowedBotCount is not
// reached after Phase 2, the function goes back to log-in Alliance bots and reach maxAllowedBotCount. This is done
// because not every account is guaranteed 5A/5H bots, so the true ratio might be skewed by few percentages. Finally,
// Phase 4 is reached if and only if the value of RandomBotAccountCount is lower than it should.
uint32 RandomPlayerbotMgr::AddRandomBots()
{
    uint32 maxAllowedBotCount = GetEventValue(0, "bot_count");
    static time_t missingBotsTimer = 0;

    if (currentBots.size() < maxAllowedBotCount)
    {
        // Calculate how many bots to add
        maxAllowedBotCount -= currentBots.size();
        maxAllowedBotCount = std::min(sPlayerbotAIConfig.randomBotsPerInterval, maxAllowedBotCount);

        // Single RNG instance for all shuffling
        std::mt19937 rng(std::chrono::steady_clock::now().time_since_epoch().count());

        // Only need to track the Alliance count, as it's in Phase 1
        uint32 totalRatio = sPlayerbotAIConfig.randomBotAllianceRatio + sPlayerbotAIConfig.randomBotHordeRatio;
        uint32 allowedAllianceCount = maxAllowedBotCount * (sPlayerbotAIConfig.randomBotAllianceRatio) / totalRatio;

        uint32 remainder = maxAllowedBotCount * (sPlayerbotAIConfig.randomBotAllianceRatio) % totalRatio;

        // Fix #1082: Randomly add one based on reminder
        if (remainder && urand(1, totalRatio) <= remainder)
        {
            allowedAllianceCount++;
        }

        // Determine which accounts to use based on EnablePeriodicOnlineOffline
        std::vector<uint32> accountsToUse;
        if (sPlayerbotAIConfig.enablePeriodicOnlineOffline)
        {

            // Calculate how many accounts can be used
            // With enablePeriodicOnlineOffline, don't use all of rndBotTypeAccounts right away. Fraction results are rounded up
            uint32 accountsToUseCount = (rndBotTypeAccounts.size() + sPlayerbotAIConfig.periodicOnlineOfflineRatio - 1)
                                        / sPlayerbotAIConfig.periodicOnlineOfflineRatio;

            // Randomly select accounts
            std::vector<uint32> shuffledAccounts = rndBotTypeAccounts;
            std::shuffle(shuffledAccounts.begin(), shuffledAccounts.end(), rng);

            for (uint32 i = 0; i < accountsToUseCount && i < shuffledAccounts.size(); i++)
            {
                accountsToUse.push_back(shuffledAccounts[i]);
            }
        }
        else
        {
            accountsToUse = rndBotTypeAccounts;
        }

        // Pre-map all characters from selected accounts
        struct CharacterInfo
        {
            uint32 guid;
            uint8 rClass;
            uint8 rRace;
            uint32 accountId;
        };
        std::vector<CharacterInfo> allCharacters;

        for (uint32 accountId : accountsToUse)
        {
            CharacterDatabasePreparedStatement* stmt =
                CharacterDatabase.GetPreparedStatement(CHAR_SEL_ACCOUNT_INFO_CHARS);
            stmt->SetData(0, accountId);
            PreparedQueryResult result = CharacterDatabase.Query(stmt);
            if (!result)
                continue;

            do
            {
                Field* fields = result->Fetch();
                CharacterInfo info;
                info.guid = fields[0].Get<uint32>();
                info.rRace = fields[3].Get<uint8>();
                info.rClass = fields[4].Get<uint8>();
                info.accountId = accountId;
                allCharacters.push_back(info);
            } while (result->NextRow());
        }

        // Shuffle for class balance
        std::shuffle(allCharacters.begin(), allCharacters.end(), rng);

        // Separate characters by faction for phased login
        std::vector<CharacterInfo> allianceChars;
        std::vector<CharacterInfo> hordeChars;

        for (auto const& charInfo : allCharacters)
        {
            if (IsAlliance(charInfo.rRace))
                allianceChars.push_back(charInfo);

            else
                hordeChars.push_back(charInfo);
        }

        // Lambda to handle bot login logic
        auto tryLoginBot = [&](CharacterInfo const& charInfo) -> bool
        {
            if (GetEventValue(charInfo.guid, "add") ||
                GetEventValue(charInfo.guid, "logout") ||
                GetPlayerBot(charInfo.guid) ||
                currentBots.contains(charInfo.guid) ||
                (sPlayerbotAIConfig.disableDeathKnightLogin && charInfo.rClass == CLASS_DEATH_KNIGHT))
            {
                return false;
            }

            uint32 add_time = sPlayerbotAIConfig.enablePeriodicOnlineOffline
                                ? urand(sPlayerbotAIConfig.minRandomBotInWorldTime,
                                        sPlayerbotAIConfig.maxRandomBotInWorldTime)
                                : sPlayerbotAIConfig.permanentlyInWorldTime;

            SetEventValue(charInfo.guid, "add", 1, add_time);
            SetEventValue(charInfo.guid, "logout", 0, 0);
            currentBots.insert(charInfo.guid);

            return true;
        };

        // PHASE 1: Log-in Alliance bots up to allowedAllianceCount
        for (auto const& charInfo : allianceChars)
        {
            if (!allowedAllianceCount)
                break;

            if (tryLoginBot(charInfo))
            {
                maxAllowedBotCount--;
                allowedAllianceCount--;
            }
        }

        // PHASE 2: Log-in Horde bots up to maxAllowedBotCount
        for (auto const& charInfo : hordeChars)
        {
            if (!maxAllowedBotCount)
                break;

            if (tryLoginBot(charInfo))
                maxAllowedBotCount--;
        }

        // PHASE 3: If maxAllowedBotCount wasn't reached, log-in more Alliance bots
        for (auto const& charInfo : allianceChars)
        {
            if (!maxAllowedBotCount)
                break;

            if (tryLoginBot(charInfo))
                maxAllowedBotCount--;
        }

        // PHASE 4: An error is given if maxAllowedBotCount is still not reached
        if (maxAllowedBotCount)
        {
            if (missingBotsTimer == 0)
                missingBotsTimer = time(nullptr);

            if (time(nullptr) - missingBotsTimer >= 10)
            {
                int divisor = RandomPlayerbotFactory::CalculateAvailableCharsPerAccount();
                uint32 moreAccountsNeeded = (maxAllowedBotCount + divisor - 1) / divisor;
                LOG_ERROR("playerbots",
                          "Can't log-in all the requested bots. Try increasing RandomBotAccountCount in your conf file.\n"
                          "{} more accounts needed.", moreAccountsNeeded);
                missingBotsTimer = 0;    // Reset timer so error is not spammed every tick
            }
        }
        else
        {
            missingBotsTimer = 0;       // Reset timer if logins for this interval were successful
        }
    }
    else
    {
        missingBotsTimer = 0;           // Reset timer if there's enough bots
    }

    return currentBots.size();
}

void RandomPlayerbotMgr::LoadBattleMastersCache()
{
    BattleMastersCache.clear();

    LOG_INFO("playerbots", "Loading Battlemasters Cache...");

    QueryResult result = WorldDatabase.Query("SELECT `entry`,`bg_template` FROM `battlemaster_entry`");

    uint32 count = 0;

    if (!result)
    {
        return;
    }

    do
    {
        ++count;

        Field* fields = result->Fetch();

        uint32 entry = fields[0].Get<uint32>();
        uint32 bgTypeId = fields[1].Get<uint32>();

        CreatureTemplate const* bmaster = sObjectMgr->GetCreatureTemplate(entry);
        if (!bmaster)
            continue;

        FactionTemplateEntry const* bmFaction = sFactionTemplateStore.LookupEntry(bmaster->faction);
        uint32 bmFactionId = bmFaction->faction;
        FactionEntry const* bmParentFaction = sFactionStore.LookupEntry(bmFactionId);
        uint32 bmParentTeam = bmParentFaction->team;
        TeamId bmTeam = TEAM_NEUTRAL;
        if (bmParentTeam == 891)
            bmTeam = TEAM_ALLIANCE;

        if (bmFactionId == 189)
            bmTeam = TEAM_ALLIANCE;

        if (bmParentTeam == 892)
            bmTeam = TEAM_HORDE;

        if (bmFactionId == 66)
            bmTeam = TEAM_HORDE;

        BattleMastersCache[bmTeam][BattlegroundTypeId(bgTypeId)].insert(
            BattleMastersCache[bmTeam][BattlegroundTypeId(bgTypeId)].end(), entry);
        LOG_DEBUG("playerbots", "Cached Battlemaster #{} for BG Type {} ({})", entry, bgTypeId,
                  bmTeam == TEAM_ALLIANCE ? "Alliance"
                  : bmTeam == TEAM_HORDE  ? "Horde"
                                          : "Neutral");

    } while (result->NextRow());

    LOG_INFO("playerbots", ">> Loaded {} battlemaster entries", count);
}

std::vector<uint32> parseBrackets(std::string const& str)
{
    std::vector<uint32> brackets;
    std::stringstream ss(str);
    std::string item;

    while (std::getline(ss, item, ','))
    {
        brackets.push_back(static_cast<uint32>(std::stoi(item)));
    }

    return brackets;
}

void RandomPlayerbotMgr::CheckBgQueue()
{
    if (!BgCheckTimer)
    {
        BgCheckTimer = time(nullptr);
        return;  // Exit immediately after initializing the timer
    }

    if (time(nullptr) < BgCheckTimer)
    {
        return;  // No need to proceed if the current time is less than the timer
    }

    // Update the timer to the current time
    BgCheckTimer = time(nullptr);

    LOG_DEBUG("playerbots", "Checking BG Queue...");

    // Initialize Battleground Data (do not clear here)

    for (int bracket = BG_BRACKET_ID_FIRST; bracket < MAX_BATTLEGROUND_BRACKETS; ++bracket)
    {
        for (int queueType = BATTLEGROUND_QUEUE_AV; queueType < MAX_BATTLEGROUND_QUEUE_TYPES; ++queueType)
        {
            BattlegroundData[queueType][bracket] = BattlegroundInfo();
        }
    }

    // Process real players and populate Battleground Data with player/queue count
    // Opens a queue for bots to join
    bool anyRealBgActivity = false;
    for (Player* player : players)
    {
        if (player && (player->InBattlegroundQueue() || player->InBattleground()))
            anyRealBgActivity = true;
        // Skip player if not currently in a queue
        if (!player->InBattlegroundQueue())
            continue;

        Battleground* bg = player->GetBattleground();
        if (bg && bg->GetStatus() == STATUS_WAIT_LEAVE)
            continue;

        TeamId teamId = player->GetTeamId();

        for (uint8 queueType = 0; queueType < PLAYER_MAX_BATTLEGROUND_QUEUES; ++queueType)
        {
            BattlegroundQueueTypeId queueTypeId = player->GetBattlegroundQueueTypeId(queueType);
            if (queueTypeId == BATTLEGROUND_QUEUE_NONE)
                continue;

            // Check if real player is able to create/join this queue
            BattlegroundTypeId bgTypeId = sBattlegroundMgr->BGTemplateId(queueTypeId);
            uint32 mapId = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId)->GetMapId();
            PvPDifficultyEntry const* pvpDiff = GetBattlegroundBracketByLevel(mapId, player->GetLevel());
            if (!pvpDiff)
                continue;

            // If player is allowed, populate the BattlegroundData with the appropriate level requirements
            BattlegroundBracketId bracketId = pvpDiff->GetBracketId();
            BattlegroundData[queueTypeId][bracketId].minLevel = pvpDiff->minLevel;
            BattlegroundData[queueTypeId][bracketId].maxLevel = pvpDiff->maxLevel;

            // Arena logic
            bool isRated = false;
            if (BattlegroundMgr::BGArenaType(queueTypeId))
            {
                BattlegroundQueue& bgQueue = sBattlegroundMgr->GetBattlegroundQueue(queueTypeId);
                GroupQueueInfo ginfo;

                if (bgQueue.GetPlayerGroupInfoData(player->GetGUID(), &ginfo))
                {
                    isRated = ginfo.IsRated;
                }

                if (bgQueue.IsPlayerInvitedToRatedArena(player->GetGUID()) ||
                    (player->InArena() && player->GetBattleground()->isRated()))
                    isRated = true;

                if (isRated)
                    BattlegroundData[queueTypeId][bracketId].ratedArenaPlayerCount++;
                else
                    BattlegroundData[queueTypeId][bracketId].skirmishArenaPlayerCount++;
            }
            // BG Logic
            else
            {
                if (teamId == TEAM_ALLIANCE)
                    BattlegroundData[queueTypeId][bracketId].bgAlliancePlayerCount++;
                else
                    BattlegroundData[queueTypeId][bracketId].bgHordePlayerCount++;

                // If a player has joined the BG, update the instance count in BattlegroundData (for consistency)
                if (player->InBattleground())
                {
                    std::vector<uint32>* instanceIds = nullptr;
                    uint32 instanceId = player->GetBattleground()->GetInstanceID();

                    instanceIds = &BattlegroundData[queueTypeId][bracketId].bgInstances;
                    if (instanceIds &&
                        std::find(instanceIds->begin(), instanceIds->end(), instanceId) == instanceIds->end())
                        instanceIds->push_back(instanceId);

                    BattlegroundData[queueTypeId][bracketId].bgInstanceCount = instanceIds->size();
                }
            }

            if (!player->IsInvitedForBattlegroundInstance() && !player->InBattleground())
            {
                if (BattlegroundMgr::BGArenaType(queueTypeId))
                {
                    if (isRated)
                        BattlegroundData[queueTypeId][bracketId].activeRatedArenaQueue = 1;
                    else
                        BattlegroundData[queueTypeId][bracketId].activeSkirmishArenaQueue = 1;
                }
                else
                {
                    BattlegroundData[queueTypeId][bracketId].activeBgQueue = 1;
                }
            }
        }
    }

    if (!anyRealBgActivity)
        CancelQueueBurst(false, true);

    // Process player bots
    for (auto& [guid, bot] : playerBots)
    {
        if (!bot || !bot->InBattlegroundQueue() || !bot->IsInWorld() || !IsRandomBot(bot))
            continue;

        Battleground* bg = bot->GetBattleground();
        if (bg && bg->GetStatus() == STATUS_WAIT_LEAVE)
            continue;

        TeamId teamId = bot->GetTeamId();

        for (uint8 queueType = 0; queueType < PLAYER_MAX_BATTLEGROUND_QUEUES; ++queueType)
        {
            BattlegroundQueueTypeId queueTypeId = bot->GetBattlegroundQueueTypeId(queueType);
            if (queueTypeId == BATTLEGROUND_QUEUE_NONE)
                continue;

            BattlegroundTypeId bgTypeId = sBattlegroundMgr->BGTemplateId(queueTypeId);
            uint32 mapId = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId)->GetMapId();
            PvPDifficultyEntry const* pvpDiff = GetBattlegroundBracketByLevel(mapId, bot->GetLevel());
            if (!pvpDiff)
                continue;

            BattlegroundBracketId bracketId = pvpDiff->GetBracketId();
            BattlegroundData[queueTypeId][bracketId].minLevel = pvpDiff->minLevel;
            BattlegroundData[queueTypeId][bracketId].maxLevel = pvpDiff->maxLevel;

            if (BattlegroundMgr::BGArenaType(queueTypeId))
            {
                bool isRated = false;
                BattlegroundQueue& bgQueue = sBattlegroundMgr->GetBattlegroundQueue(queueTypeId);
                GroupQueueInfo ginfo;

                if (bgQueue.GetPlayerGroupInfoData(guid, &ginfo))
                {
                    isRated = ginfo.IsRated;
                }

                if (bgQueue.IsPlayerInvitedToRatedArena(guid) || (bot->InArena() && bot->GetBattleground()->isRated()))
                    isRated = true;

                if (isRated)
                    BattlegroundData[queueTypeId][bracketId].ratedArenaBotCount++;
                else
                    BattlegroundData[queueTypeId][bracketId].skirmishArenaBotCount++;
            }
            else
            {
                if (teamId == TEAM_ALLIANCE)
                    BattlegroundData[queueTypeId][bracketId].bgAllianceBotCount++;
                else
                    BattlegroundData[queueTypeId][bracketId].bgHordeBotCount++;
            }

            if (bot->InBattleground())
            {
                std::vector<uint32>* instanceIds = nullptr;
                uint32 instanceId = bot->GetBattleground()->GetInstanceID();
                bool isArena = false;
                bool isRated = false;

                // Arena logic
                if (bot->InArena())
                {
                    isArena = true;
                    if (bot->GetBattleground()->isRated())
                    {
                        isRated = true;
                        instanceIds = &BattlegroundData[queueTypeId][bracketId].ratedArenaInstances;
                    }
                    else
                    {
                        instanceIds = &BattlegroundData[queueTypeId][bracketId].skirmishArenaInstances;
                    }
                }
                // BG Logic
                else
                {
                    instanceIds = &BattlegroundData[queueTypeId][bracketId].bgInstances;
                }

                if (instanceIds &&
                    std::find(instanceIds->begin(), instanceIds->end(), instanceId) == instanceIds->end())
                    instanceIds->push_back(instanceId);

                if (isArena)
                {
                    if (isRated)
                        BattlegroundData[queueTypeId][bracketId].ratedArenaInstanceCount = instanceIds->size();
                    else
                        BattlegroundData[queueTypeId][bracketId].skirmishArenaInstanceCount = instanceIds->size();
                }
                else
                {
                    BattlegroundData[queueTypeId][bracketId].bgInstanceCount = instanceIds->size();
                }
            }
        }
    }

    // If enabled, wait for all bots to have logged in before queueing for Arena's / BG's
    if (sPlayerbotAIConfig.randomBotAutoJoinBG && playerBots.size() >= GetMaxAllowedBotCount())
    {
        uint32 randomBotAutoJoinArenaBracket = sPlayerbotAIConfig.randomBotAutoJoinArenaBracket;
        uint32 randomBotAutoJoinBGRatedArena2v2Count = sPlayerbotAIConfig.randomBotAutoJoinBGRatedArena2v2Count;
        uint32 randomBotAutoJoinBGRatedArena3v3Count = sPlayerbotAIConfig.randomBotAutoJoinBGRatedArena3v3Count;
        uint32 randomBotAutoJoinBGRatedArena5v5Count = sPlayerbotAIConfig.randomBotAutoJoinBGRatedArena5v5Count;

        uint32 randomBotAutoJoinBGICCount = sPlayerbotAIConfig.randomBotAutoJoinBGICCount;
        uint32 randomBotAutoJoinBGEYCount = sPlayerbotAIConfig.randomBotAutoJoinBGEYCount;
        uint32 randomBotAutoJoinBGAVCount = sPlayerbotAIConfig.randomBotAutoJoinBGAVCount;
        uint32 randomBotAutoJoinBGABCount = sPlayerbotAIConfig.randomBotAutoJoinBGABCount;
        uint32 randomBotAutoJoinBGWSCount = sPlayerbotAIConfig.randomBotAutoJoinBGWSCount;

        std::vector<uint32> icBrackets = parseBrackets(sPlayerbotAIConfig.randomBotAutoJoinICBrackets);
        std::vector<uint32> eyBrackets = parseBrackets(sPlayerbotAIConfig.randomBotAutoJoinEYBrackets);
        std::vector<uint32> avBrackets = parseBrackets(sPlayerbotAIConfig.randomBotAutoJoinAVBrackets);
        std::vector<uint32> abBrackets = parseBrackets(sPlayerbotAIConfig.randomBotAutoJoinABBrackets);
        std::vector<uint32> wsBrackets = parseBrackets(sPlayerbotAIConfig.randomBotAutoJoinWSBrackets);

        // Check both bgInstanceCount / bgInstances.size
        // to help counter against potentional inconsistencies
        auto updateRatedArenaInstanceCount = [&](uint32 queueType, uint32 bracket, uint32 minCount)
        {
            if (BattlegroundData[queueType][bracket].activeRatedArenaQueue == 0 &&
                BattlegroundData[queueType][bracket].ratedArenaInstanceCount < minCount &&
                BattlegroundData[queueType][bracket].ratedArenaInstances.size() < minCount)
                BattlegroundData[queueType][bracket].activeRatedArenaQueue = 1;
        };

        auto updateBGInstanceCount = [&](uint32 queueType, std::vector<uint32> brackets, uint32 minCount)
        {
            for (uint32 bracket : brackets)
            {
                if (BattlegroundData[queueType][bracket].activeBgQueue == 0 &&
                    BattlegroundData[queueType][bracket].bgInstanceCount < minCount &&
                    BattlegroundData[queueType][bracket].bgInstances.size() < minCount)
                    BattlegroundData[queueType][bracket].activeBgQueue = 1;
            }
        };

        // Update rated arena instance counts
        updateRatedArenaInstanceCount(BATTLEGROUND_QUEUE_2v2, randomBotAutoJoinArenaBracket,
                                      randomBotAutoJoinBGRatedArena2v2Count);
        updateRatedArenaInstanceCount(BATTLEGROUND_QUEUE_3v3, randomBotAutoJoinArenaBracket,
                                      randomBotAutoJoinBGRatedArena3v3Count);
        updateRatedArenaInstanceCount(BATTLEGROUND_QUEUE_5v5, randomBotAutoJoinArenaBracket,
                                      randomBotAutoJoinBGRatedArena5v5Count);

        // Update battleground instance counts
        updateBGInstanceCount(BATTLEGROUND_QUEUE_IC, icBrackets, randomBotAutoJoinBGICCount);
        updateBGInstanceCount(BATTLEGROUND_QUEUE_EY, eyBrackets, randomBotAutoJoinBGEYCount);
        updateBGInstanceCount(BATTLEGROUND_QUEUE_AV, avBrackets, randomBotAutoJoinBGAVCount);
        updateBGInstanceCount(BATTLEGROUND_QUEUE_AB, abBrackets, randomBotAutoJoinBGABCount);
        updateBGInstanceCount(BATTLEGROUND_QUEUE_WS, wsBrackets, randomBotAutoJoinBGWSCount);
    }

    LogBattlegroundInfo();
}

void RandomPlayerbotMgr::LogBattlegroundInfo()
{
    for (auto const& queueTypePair : BattlegroundData)
    {
        uint8 queueType = queueTypePair.first;

        BattlegroundQueueTypeId queueTypeId = BattlegroundQueueTypeId(queueType);

        if (uint8 type = BattlegroundMgr::BGArenaType(queueTypeId))
        {
            for (auto const& bracketIdPair : queueTypePair.second)
            {
                auto& bgInfo = bracketIdPair.second;
                if (bgInfo.minLevel == 0)
                    continue;
                LOG_INFO("playerbots",
                         "ARENA:{} {}: Player (Skirmish:{}, Rated:{}) Bots (Skirmish:{}, Rated:{}) Total (Skirmish:{} "
                         "Rated:{}), Instances (Skirmish:{} Rated:{})",
                         type == ARENA_TYPE_2v2   ? "2v2"
                         : type == ARENA_TYPE_3v3 ? "3v3"
                                                  : "5v5",
                         std::to_string(bgInfo.minLevel) + "-" + std::to_string(bgInfo.maxLevel),
                         bgInfo.skirmishArenaPlayerCount, bgInfo.ratedArenaPlayerCount, bgInfo.skirmishArenaBotCount,
                         bgInfo.ratedArenaBotCount, bgInfo.skirmishArenaPlayerCount + bgInfo.skirmishArenaBotCount,
                         bgInfo.ratedArenaPlayerCount + bgInfo.ratedArenaBotCount, bgInfo.skirmishArenaInstanceCount,
                         bgInfo.ratedArenaInstanceCount);
            }
            continue;
        }

        BattlegroundTypeId bgTypeId = BattlegroundMgr::BGTemplateId(queueTypeId);
        std::string _bgType;
        switch (bgTypeId)
        {
            case BATTLEGROUND_AV:
                _bgType = "AV";
                break;
            case BATTLEGROUND_WS:
                _bgType = "WSG";
                break;
            case BATTLEGROUND_AB:
                _bgType = "AB";
                break;
            case BATTLEGROUND_EY:
                _bgType = "EotS";
                break;
            case BATTLEGROUND_RB:
                _bgType = "Random";
                break;
            case BATTLEGROUND_SA:
                _bgType = "SotA";
                break;
            case BATTLEGROUND_IC:
                _bgType = "IoC";
                break;
            default:
                _bgType = "Other";
                break;
        }

        for (auto const& bracketIdPair : queueTypePair.second)
        {
            auto& bgInfo = bracketIdPair.second;
            if (bgInfo.minLevel == 0)
                continue;

            LOG_INFO("playerbots",
                     "BG:{} {}: Player ({}:{}) Bot ({}:{}) Total (A:{} H:{}), Instances {}, Active Queue: {}", _bgType,
                     std::to_string(bgInfo.minLevel) + "-" + std::to_string(bgInfo.maxLevel),
                     bgInfo.bgAlliancePlayerCount, bgInfo.bgHordePlayerCount, bgInfo.bgAllianceBotCount,
                     bgInfo.bgHordeBotCount, bgInfo.bgAlliancePlayerCount + bgInfo.bgAllianceBotCount,
                     bgInfo.bgHordePlayerCount + bgInfo.bgHordeBotCount, bgInfo.bgInstanceCount, bgInfo.activeBgQueue);
        }
    }
    LOG_DEBUG("playerbots", "BG Queue check finished");

    if (!sPlayerbotAIConfig.queueAutoScale || !sPlayerbotAIConfig.queueAutoScaleBg)
        return;

    for (auto const& [queueTypeRaw, brackets] : BattlegroundData)
    {
        BattlegroundQueueTypeId queueTypeId = static_cast<BattlegroundQueueTypeId>(queueTypeRaw);
        if (BattlegroundMgr::BGArenaType(queueTypeId))
            continue;

        BattlegroundTypeId bgTypeId = sBattlegroundMgr->BGTemplateId(queueTypeId);
        Battleground* bgTemplate = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);
        if (!bgTemplate)
            continue;

        // Fill the real battleground capacity, not just the minimum start size.
        // AV for example is 40 players per team.
        uint32 targetPerTeam = bgTemplate->GetMaxPlayersPerTeam();
        if (!targetPerTeam)
            targetPerTeam = bgTemplate->GetMinPlayersPerTeam();
        if (!targetPerTeam)
            continue;

        for (auto const& [bracketRaw, info] : brackets)
        {
            // Keep autoscaling after the battleground has started. Once all
            // queued players are invited/inside, activeBgQueue becomes 0 even
            // though an under-filled instance is still running. BGJoinAction
            // already considers bgInstanceCount when deciding whether a bot
            // should join, so the population layer must do the same.
            if ((!info.activeBgQueue && !info.bgInstanceCount) || !info.minLevel || !info.maxLevel)
                continue;

            uint32 alliance = info.bgAlliancePlayerCount + info.bgAllianceBotCount;
            uint32 horde = info.bgHordePlayerCount + info.bgHordeBotCount;

            uint32 needAlliance = alliance < targetPerTeam ? targetPerTeam - alliance : 0;
            uint32 needHorde = horde < targetPerTeam ? targetPerTeam - horde : 0;

            if (!needAlliance && !needHorde)
                continue;

            uint32 room = 0;
            if (currentBots.size() < sPlayerbotAIConfig.queueAutoScaleMaxBots)
                room = sPlayerbotAIConfig.queueAutoScaleMaxBots - currentBots.size();

            uint32 totalMissing = needAlliance + needHorde;
            uint32 remaining = sPlayerbotAIConfig.queueAutoScaleBurstOnly
                                   ? std::min<uint32>(totalMissing, room)
                                   : std::min<uint32>(sPlayerbotAIConfig.queueAutoScaleLoginBatch, room);

            if (!remaining)
                continue;

            // Fair first-pass split. Previously Alliance was always processed
            // first and could consume the entire batch every cycle.
            uint32 budgetAlliance = 0;
            uint32 budgetHorde = 0;

            if (needAlliance && needHorde)
            {
                budgetAlliance = std::min<uint32>(needAlliance, (remaining + 1) / 2);
                budgetHorde = std::min<uint32>(needHorde, remaining / 2);
            }
            else if (needAlliance)
                budgetAlliance = std::min<uint32>(needAlliance, remaining);
            else
                budgetHorde = std::min<uint32>(needHorde, remaining);

            if (budgetAlliance)
                EnsureQueueBots(
                    TEAM_ALLIANCE, info.minLevel, info.maxLevel, lfg::PLAYER_ROLE_NONE,
                    budgetAlliance, false, true);

            if (budgetHorde)
                EnsureQueueBots(
                    TEAM_HORDE, info.minLevel, info.maxLevel, lfg::PLAYER_ROLE_NONE,
                    budgetHorde, false, true);
        }
    }
}

void RandomPlayerbotMgr::ProcessPendingBurstLfgJoins()
{
    for (auto& [guid, reservation] : queueBurstBots)
    {
        if (!reservation.lfg)
            continue;
        Player* bot = GetPlayerBot(guid);
        if (!bot || !bot->IsInWorld())
            continue;
        if (sLFGMgr->GetState(bot->GetGUID()) != lfg::LFG_STATE_NONE)
            continue;
        if (!sPlayerbotAIConfig.queueAutoScaleBurstOnly &&
            reservation.joinRequestedAt && time(nullptr) < reservation.joinRequestedAt + 15)
            continue;
        if (!QueueBurstBotLfgNow(bot, reservation) &&
            queueBurstLfgJoinWindow == time(nullptr) &&
            queueBurstLfgJoinCount >= sPlayerbotAIConfig.queueAutoScaleLfgJoinBatch)
            break;
    }
}

void RandomPlayerbotMgr::CancelQueueBurst(bool cancelLfg, bool cancelBg)
{
    for (auto it = queueBurstBots.begin(); it != queueBurstBots.end(); )
    {
        uint32 guid = it->first;
        QueueBotReservation const reservation = it->second;
        if ((reservation.lfg && !cancelLfg) || (reservation.bg && !cancelBg))
        {
            ++it;
            continue;
        }

        Player* bot = GetPlayerBot(guid);
        if (reservation.lfg && cancelLfg && bot)
        {
            lfg::LfgState state = sLFGMgr->GetState(bot->GetGUID());
            if (state >= lfg::LFG_STATE_DUNGEON && state != lfg::LFG_STATE_FINISHED_DUNGEON)
            {
                ++it;
                continue;
            }
            if (state != lfg::LFG_STATE_NONE)
            {
                sLFGMgr->LeaveLfg(bot->GetGUID());
                sLFGMgr->LeaveAllLfgQueues(bot->GetGUID(), true,
                    bot->GetGroup() ? bot->GetGroup()->GetGUID() : ObjectGuid::Empty);
            }
        }

        if (reservation.bg && cancelBg && bot)
        {
            if (bot->InBattleground())
            {
                ++it;
                continue;
            }
            for (uint8 slot = 0; slot < PLAYER_MAX_BATTLEGROUND_QUEUES; ++slot)
            {
                BattlegroundQueueTypeId q = bot->GetBattlegroundQueueTypeId(slot);
                if (q == BATTLEGROUND_QUEUE_NONE)
                    continue;
                bot->RemoveBattlegroundQueueId(q);
                sBattlegroundMgr->GetBattlegroundQueue(q).RemovePlayer(bot->GetGUID(), true);
            }
        }

        SetEventValue(guid, "add", 0, 0);
        SetEventValue(guid, "randomize", 0, 0);

        bool const restoreLfgLocks = reservation.lfg && bot;
        if (!bot)
            currentBots.erase(guid);

        it = queueBurstBots.erase(it);

        if (restoreLfgLocks)
            sLFGMgr->InitializeLockedDungeons(bot, bot->GetGroup());
    }
}

void RandomPlayerbotMgr::CheckLfgQueue()
{
    if (!LfgCheckTimer || time(nullptr) > (LfgCheckTimer + 30))
        LfgCheckTimer = time(nullptr);

    LOG_DEBUG("playerbots", "Checking LFG Queue...");

    // Clear LFG list
    LfgDungeons[TEAM_ALLIANCE].clear();
    LfgDungeons[TEAM_HORDE].clear();

    bool const allowCrossFactionLfg =
        sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_GROUP) &&
        sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_CHAT);
    bool anyRealLfgQueue = false;

    struct TeamDemand
    {
        uint32 total = 0;
        uint32 tanks = 0;
        uint32 heals = 0;
        uint32 dps = 0;
        uint8 minLevel = 0;
        uint8 maxLevel = 0;
        uint8 raidLevel = 0;
        uint32 targetPlayers = 0;
        uint32 targetTanks = 0;
        uint32 targetHeals = 0;
        uint32 targetDps = 0;
    };

    // LFG demand must be tracked per activity, not only per faction.
    // Otherwise two simultaneous raids of the same faction are collapsed into
    // one target and bots queued for one raid are counted for the other.
    struct LfgDemandKey
    {
        TeamId team = TEAM_NEUTRAL;
        uint32 dungeonId = 0;

        bool operator<(LfgDemandKey const& other) const
        {
            return team < other.team || (team == other.team && dungeonId < other.dungeonId);
        }
    };

    std::map<LfgDemandKey, TeamDemand> demand;

    for (std::vector<Player*>::iterator i = players.begin(); i != players.end(); ++i)
    {
        Player* player = *i;
        if (!player || !player->IsInWorld())
            continue;

        Group* group = player->GetGroup();
        ObjectGuid guid = group ? group->GetGUID() : player->GetGUID();

        lfg::LfgState gState = sLFGMgr->GetState(guid);
        if (gState != lfg::LFG_STATE_NONE && gState < lfg::LFG_STATE_DUNGEON)
        {
            anyRealLfgQueue = true;
            uint8 roles = sLFGMgr->GetRoles(player->GetGUID());

            lfg::LfgDungeonSet const& dList = sLFGMgr->GetSelectedDungeons(player->GetGUID());

            for (lfg::LfgDungeonSet::const_iterator itr = dList.begin(); itr != dList.end(); ++itr)
            {
                lfg::LFGDungeonData const* dungeon = sLFGMgr->GetLFGDungeon(*itr);
                if (!dungeon)
                    continue;

                TeamId demandTeam = allowCrossFactionLfg ? TEAM_NEUTRAL : player->GetTeamId();
                LfgDemandKey key{demandTeam, dungeon->id};
                TeamDemand& td = demand[key];

                ++td.total;
                td.minLevel = td.minLevel ? std::min<uint8>(td.minLevel, player->GetLevel()) : player->GetLevel();
                td.maxLevel = std::max<uint8>(td.maxLevel, player->GetLevel());

                if (roles & lfg::PLAYER_ROLE_TANK)
                    ++td.tanks;
                else if (roles & lfg::PLAYER_ROLE_HEALER)
                    ++td.heals;
                else
                    ++td.dps;

                // Ask AzerothCore for the requirements of this exact activity.
                // Using the player's whole selected set here would merge
                // independent raids back together.
                lfg::LfgDungeonSet oneDungeon;
                oneDungeon.insert(dungeon->id);
                lfg::LfgRoleRequirements requirements = sLFGMgr->GetLfgRoleRequirements(oneDungeon);
                if (requirements.players)
                {
                    td.targetPlayers = requirements.players;
                    td.targetTanks = requirements.tanks;
                    td.targetHeals = requirements.healers;
                    td.targetDps = requirements.dps;
                }

                if (dungeon->type == lfg::LFG_TYPE_RAID)
                {
                    uint8 raidLevel = 60;
                    if (dungeon->expansion == 1)
                        raidLevel = 70;
                    else if (dungeon->expansion >= 2)
                        raidLevel = 80;

                    td.raidLevel = raidLevel;
                    td.minLevel = raidLevel;
                    td.maxLevel = raidLevel;
                }
                else
                {
                    uint8 dungeonMin = dungeon->minlevel ? dungeon->minlevel : player->GetLevel();
                    uint8 dungeonMax = dungeon->maxlevel ? dungeon->maxlevel : player->GetLevel();
                    td.minLevel = td.minLevel ? std::min<uint8>(td.minLevel, dungeonMin) : dungeonMin;
                    td.maxLevel = std::max<uint8>(td.maxLevel, dungeonMax);
                }

                // Keep the legacy list populated for non-burst code paths.
                if (allowCrossFactionLfg)
                {
                    if (std::find(LfgDungeons[TEAM_ALLIANCE].begin(), LfgDungeons[TEAM_ALLIANCE].end(), dungeon->id) ==
                        LfgDungeons[TEAM_ALLIANCE].end())
                        LfgDungeons[TEAM_ALLIANCE].push_back(dungeon->id);
                    if (std::find(LfgDungeons[TEAM_HORDE].begin(), LfgDungeons[TEAM_HORDE].end(), dungeon->id) ==
                        LfgDungeons[TEAM_HORDE].end())
                        LfgDungeons[TEAM_HORDE].push_back(dungeon->id);
                }
                else
                {
                    std::vector<uint32>& teamDungeons = LfgDungeons[player->GetTeamId()];
                    if (std::find(teamDungeons.begin(), teamDungeons.end(), dungeon->id) == teamDungeons.end())
                        teamDungeons.push_back(dungeon->id);
                }
            }
        }
    }

    if (!anyRealLfgQueue)
    {
        CancelQueueBurst(true, false);
        return;
    }

    // Count already queued random bots so a new check does not reserve another
    // tank/healer while the previous burst bot is already waiting in LFG.
    for (auto const& [guid, bot] : playerBots)
    {
        (void)guid;
        if (!bot || !bot->IsInWorld() || !IsRandomBot(bot))
            continue;

        if (sPlayerbotAIConfig.queueAutoScaleBurstOnly &&
            !IsQueueBurstBot(bot, true, false))
            continue;

        lfg::LfgState state = sLFGMgr->GetState(bot->GetGUID());
        if (state == lfg::LFG_STATE_NONE || state >= lfg::LFG_STATE_DUNGEON)
            continue;

        // A queued bot may satisfy exactly one autoscale demand. Burst bots are
        // pinned to the activity stored in their reservation so another raid
        // cannot accidentally count them as part of its population.
        lfg::LfgDungeonSet const& botDungeons = sLFGMgr->GetSelectedDungeons(bot->GetGUID());
        TeamId demandTeam = allowCrossFactionLfg ? TEAM_NEUTRAL : bot->GetTeamId();
        uint32 demandDungeonId = 0;

        auto reservationItr = queueBurstBots.find(guid.GetCounter());
        if (reservationItr != queueBurstBots.end() && reservationItr->second.lfg &&
            reservationItr->second.lfgDungeonId)
        {
            uint32 reservedDungeonId = reservationItr->second.lfgDungeonId;
            if (botDungeons.find(reservedDungeonId) != botDungeons.end() &&
                demand.find(LfgDemandKey{demandTeam, reservedDungeonId}) != demand.end())
                demandDungeonId = reservedDungeonId;
        }

        if (!demandDungeonId)
        {
            for (uint32 dungeonId : botDungeons)
            {
                if (demand.find(LfgDemandKey{demandTeam, dungeonId}) != demand.end())
                {
                    demandDungeonId = dungeonId;
                    break;
                }
            }
        }

        if (!demandDungeonId)
            continue;

        TeamDemand& td = demand[LfgDemandKey{demandTeam, demandDungeonId}];

        if (td.raidLevel && bot->GetLevel() != td.raidLevel)
            continue;

        uint8 roles = sLFGMgr->GetRoles(bot->GetGUID());
        if (!roles)
        {
            PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
            if (ai && ai->IsTank(bot, false))
                roles = lfg::PLAYER_ROLE_TANK;
            else if (ai && ai->IsHeal(bot, false))
                roles = lfg::PLAYER_ROLE_HEALER;
            else
                roles = lfg::PLAYER_ROLE_DAMAGE;
        }

        if (roles & lfg::PLAYER_ROLE_TANK)
            ++td.tanks;
        else if (roles & lfg::PLAYER_ROLE_HEALER)
            ++td.heals;
        else
            ++td.dps;
    }

    // Refresh only reservations whose exact activity is still requested.
    // Previously every LFG reservation was kept alive while any real LFG
    // player existed, leaving stale raid reservations around indefinitely.
    if (anyRealLfgQueue)
    {
        time_t now = time(nullptr);
        for (auto& [guid, reservation] : queueBurstBots)
        {
            if (!reservation.lfg)
                continue;

            TeamId reservationTeam = allowCrossFactionLfg ? TEAM_NEUTRAL : reservation.team;
            if (reservation.lfgDungeonId &&
                demand.find(LfgDemandKey{reservationTeam, reservation.lfgDungeonId}) != demand.end())
            {
                reservation.lastNeeded = now;
            }
        }
    }

    if (sPlayerbotAIConfig.queueAutoScale && sPlayerbotAIConfig.queueAutoScaleLfg)
    {
        ProcessPendingBurstLfgJoins();
        uint32 room = currentBots.size() < sPlayerbotAIConfig.queueAutoScaleMaxBots
                          ? sPlayerbotAIConfig.queueAutoScaleMaxBots - currentBots.size()
                          : 0;
        uint32 remaining = sPlayerbotAIConfig.queueAutoScaleBurstOnly
                               ? room
                               : std::min<uint32>(sPlayerbotAIConfig.queueAutoScaleLoginBatch, room);
        for (auto& [key, td] : demand)
        {
            if (!remaining || !td.total || !td.targetPlayers)
                continue;

            TeamId team = key.team;
            uint32 dungeonId = key.dungeonId;

            uint32 needTank = td.targetTanks > td.tanks ? td.targetTanks - td.tanks : 0;
            uint32 needHeal = td.targetHeals > td.heals ? td.targetHeals - td.heals : 0;
            uint32 needDps = td.targetDps > td.dps ? td.targetDps - td.dps : 0;
            uint8 minLevel = td.minLevel ? td.minLevel : sPlayerbotAIConfig.randomBotMinLevel;
            uint8 maxLevel = td.maxLevel ? td.maxLevel : sPlayerbotAIConfig.randomBotMaxLevel;

            if (needTank && remaining)
            {
                uint32 added = EnsureQueueBots(team, minLevel, maxLevel, lfg::PLAYER_ROLE_TANK,
                    std::min(needTank, remaining), true, false, dungeonId);
                remaining -= std::min(remaining, added);
            }

            if (needHeal && remaining)
            {
                uint32 added = EnsureQueueBots(team, minLevel, maxLevel, lfg::PLAYER_ROLE_HEALER,
                    std::min(needHeal, remaining), true, false, dungeonId);
                remaining -= std::min(remaining, added);
            }

            if (needDps && remaining)
            {
                uint32 added = EnsureQueueBots(team, minLevel, maxLevel, lfg::PLAYER_ROLE_DAMAGE,
                    std::min(needDps, remaining), true, false, dungeonId);
                remaining -= std::min(remaining, added);
            }
        }
    }
}

bool RandomPlayerbotMgr::CanClassFillQueueRole(uint8 cls, uint8 role) const
{
    if (role == lfg::PLAYER_ROLE_NONE || role == lfg::PLAYER_ROLE_DAMAGE)
        return true;

    if (role == lfg::PLAYER_ROLE_TANK)
    {
        return cls == CLASS_WARRIOR || cls == CLASS_PALADIN || cls == CLASS_DRUID ||
               cls == CLASS_DEATH_KNIGHT;
    }

    if (role == lfg::PLAYER_ROLE_HEALER)
    {
        return cls == CLASS_PRIEST || cls == CLASS_PALADIN || cls == CLASS_DRUID || cls == CLASS_SHAMAN;
    }

    return false;
}

uint8 RandomPlayerbotMgr::GetSpecTabForQueueRole(uint8 cls, uint8 role) const
{
    if (role == lfg::PLAYER_ROLE_TANK)
    {
        switch (cls)
        {
            case CLASS_WARRIOR: return 2;       // Protection
            case CLASS_PALADIN: return 1;       // Protection
            case CLASS_DRUID: return 1;         // Feral
            case CLASS_DEATH_KNIGHT: return 0;  // Blood
            default: return 255;
        }
    }

    if (role == lfg::PLAYER_ROLE_HEALER)
    {
        switch (cls)
        {
            case CLASS_PRIEST: return 0;  // Discipline
            case CLASS_PALADIN: return 0; // Holy
            case CLASS_DRUID: return 2;   // Restoration
            case CLASS_SHAMAN: return 2;  // Restoration
            default: return 255;
        }
    }

    if (role == lfg::PLAYER_ROLE_DAMAGE)
    {
        switch (cls)
        {
            case CLASS_PRIEST: return 2;        // Shadow
            case CLASS_PALADIN: return 2;       // Retribution
            case CLASS_DEATH_KNIGHT: return 1;  // Frost
            default: return 0;
        }
    }

    return 255;
}

uint32 RandomPlayerbotMgr::EnsureQueueBots(TeamId team, uint8 minLevel, uint8 maxLevel, uint8 role, uint32 count,
                                           bool forLfg, bool forBg, uint32 lfgDungeonId)
{
    if (!count || currentBots.size() >= sPlayerbotAIConfig.queueAutoScaleMaxBots)
        return 0;

    bool const allowAnyFaction =
        forLfg && sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_GROUP) &&
        sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_CHAT);

    // Do not reserve duplicates while a previously selected burst bot is still
    // loading and has not entered LFG/BG yet. Once it is actually queued the
    // caller already sees it in the normal queue counts.
    uint32 pending = 0;
    time_t const now = time(nullptr);
    for (auto itr = queueBurstBots.begin(); itr != queueBurstBots.end(); )
    {
        uint32 guid = itr->first;
        QueueBotReservation& reservation = itr->second;

        if (reservation.team != team || reservation.role != role || reservation.lfg != forLfg ||
            reservation.bg != forBg ||
            (forLfg && reservation.lfgDungeonId != lfgDungeonId))
        {
            ++itr;
            continue;
        }

        Player* pendingBot = GetPlayerBot(guid);
        bool enteredQueue = false;
        if (pendingBot)
        {
            if (forBg)
                enteredQueue = pendingBot->InBattlegroundQueue() || pendingBot->InBattleground();
            else if (forLfg)
            {
                lfg::LfgState state = sLFGMgr->GetState(pendingBot->GetGUID());
                enteredQueue = state != lfg::LFG_STATE_NONE;
            }
        }

        if (enteredQueue)
        {
            ++itr;
            continue;
        }

        // A reservation that never reaches the queue must not block this role
        // forever. This can happen after a failed login, failed LFG join,
        // role mismatch or AI action that never fires.
        if (reservation.reservedAt &&
            now >= reservation.reservedAt + sPlayerbotAIConfig.queueAutoScalePendingSeconds)
        {
            SetEventValue(guid, "add", 0, 0);
            SetEventValue(guid, "randomize", 0, 0);

            bool const restoreLfgLocks = reservation.lfg && pendingBot;
            if (!pendingBot)
                currentBots.erase(guid);

            itr = queueBurstBots.erase(itr);

            if (restoreLfgLocks)
                sLFGMgr->InitializeLockedDungeons(pendingBot, pendingBot->GetGroup());

            continue;
        }

        ++pending;
        ++itr;
    }

    if (pending >= count)
        return 0;

    count -= pending;

    if (!forLfg)
    {
        minLevel = std::max<uint8>(minLevel, sPlayerbotAIConfig.randomBotMinLevel);
        maxLevel = std::min<uint8>(maxLevel, sPlayerbotAIConfig.randomBotMaxLevel);
        if (minLevel > maxLevel)
            std::swap(minLevel, maxLevel);
    }

    struct Candidate
    {
        uint32 guid;
        uint8 race;
        uint8 cls;
        uint8 level;
    };

    std::vector<Candidate> exactCandidates;
    std::vector<Candidate> fallbackCandidates;

    // Normal population management intentionally uses the RNDbot account
    // subset. Queue burst population is different: it must be able to use any
    // random-bot character when the preferred subset cannot supply the needed
    // faction/bracket/role. This is especially important for 40v40 BGs and
    // raid tanks/healers, where the RNDbot subset can be exhausted quickly.
    std::vector<uint32> accountsToScan = rndBotTypeAccounts;
    if (forBg || forLfg)
    {
        for (uint32 accountId : sPlayerbotAIConfig.randomBotAccounts)
        {
            if (std::find(accountsToScan.begin(), accountsToScan.end(), accountId) == accountsToScan.end())
                accountsToScan.push_back(accountId);
        }
    }

    for (uint32 accountId : accountsToScan)
    {
        QueryResult result = CharacterDatabase.Query(
            "SELECT guid, race, class, level FROM characters WHERE account = {} AND deleteDate IS NULL",
            accountId);
        if (!result)
            continue;

        do
        {
            Field* fields = result->Fetch();
            Candidate c{fields[0].Get<uint32>(), fields[1].Get<uint8>(), fields[2].Get<uint8>(), fields[3].Get<uint8>()};

            if (currentBots.contains(c.guid) || queueBurstBots.contains(c.guid) || GetPlayerBot(c.guid))
                continue;
            if (GetEventValue(c.guid, "add"))
                continue;
            if (sPlayerbotAIConfig.disableDeathKnightLogin && c.cls == CLASS_DEATH_KNIGHT)
                continue;
            if (!allowAnyFaction && (team == TEAM_ALLIANCE) != IsAlliance(c.race))
                continue;
            bool const levelMatches = c.level >= minLevel && c.level <= maxLevel;
            if (!levelMatches)
            {
                // Both BG and LFG burst bots can be rebuilt to the requested
                // target level after login. Normal/non-queue callers still
                // require an exact level match.
                if (!forBg && !forLfg)
                    continue;
            }
            if (!CanClassFillQueueRole(c.cls, role))
                continue;

            if (levelMatches)
                exactCandidates.push_back(c);
            else
                fallbackCandidates.push_back(c);
        } while (result->NextRow());
    }

    if (exactCandidates.empty() && fallbackCandidates.empty())
        return 0;

    std::mt19937 rng(std::chrono::steady_clock::now().time_since_epoch().count());
    std::shuffle(exactCandidates.begin(), exactCandidates.end(), rng);
    std::shuffle(fallbackCandidates.begin(), fallbackCandidates.end(), rng);

    std::vector<Candidate> candidates;
    candidates.reserve(exactCandidates.size() + fallbackCandidates.size());
    candidates.insert(candidates.end(), exactCandidates.begin(), exactCandidates.end());
    candidates.insert(candidates.end(), fallbackCandidates.begin(), fallbackCandidates.end());

    uint32 room = sPlayerbotAIConfig.queueAutoScaleMaxBots - currentBots.size();
    uint32 wanted = std::min<uint32>(count, room);
    uint32 added = 0;
    uint8 targetLevel = static_cast<uint8>((static_cast<uint32>(minLevel) + maxLevel) / 2);

    if (forLfg && minLevel == maxLevel)
        targetLevel = minLevel;

    for (Candidate const& c : candidates)
    {
        if (added >= wanted)
            break;

        SetEventValue(c.guid, "add", 1, sPlayerbotAIConfig.permanentlyInWorldTime);
        SetEventValue(c.guid, "logout", 0, 0);
        // Keep the selected role/spec stable for the lifetime of the burst bot.
        SetEventValue(c.guid, "randomize", 1, sPlayerbotAIConfig.permanentlyInWorldTime);
        currentBots.insert(c.guid);

        QueueBotReservation reservation;
        reservation.team = team;
        reservation.role = role;
        reservation.minLevel = minLevel;
        reservation.maxLevel = maxLevel;
        reservation.targetLevel = targetLevel;
        reservation.lfgDungeonId = forLfg ? lfgDungeonId : 0;
        reservation.lastNeeded = time(nullptr);
        reservation.reservedAt = reservation.lastNeeded;
        reservation.lfg = forLfg;
        reservation.bg = forBg;
        queueBurstBots[c.guid] = reservation;
        ++added;
    }

    return added;
}

bool RandomPlayerbotMgr::QueueBurstBotLfgNow(Player* bot, QueueBotReservation& reservation)
{
    if (!bot || !bot->GetSession() || !reservation.lfg || reservation.role == lfg::PLAYER_ROLE_NONE)
        return false;

    if (sLFGMgr->GetState(bot->GetGUID()) != lfg::LFG_STATE_NONE)
        return true;

    time_t const now = time(nullptr);
    if (queueBurstLfgJoinWindow != now)
    {
        queueBurstLfgJoinWindow = now;
        queueBurstLfgJoinCount = 0;
    }
    if (queueBurstLfgJoinCount >= sPlayerbotAIConfig.queueAutoScaleLfgJoinBatch)
        return false;

    lfg::LfgDungeonSet list;

    // Burst reservations are pinned to one exact LFG activity. This prevents a
    // bot reserved for AQ20 from joining AQ40 (or vice versa) simply because
    // both raids are currently present in the faction-wide LfgDungeons list.
    if (reservation.lfgDungeonId)
    {
        LFGDungeonEntry const* dungeon = sLFGDungeonStore.LookupEntry(reservation.lfgDungeonId);
        if (dungeon &&
            (dungeon->TypeID == lfg::LFG_TYPE_RANDOM ||
             dungeon->TypeID == lfg::LFG_TYPE_DUNGEON ||
             dungeon->TypeID == lfg::LFG_TYPE_HEROIC ||
             dungeon->TypeID == lfg::LFG_TYPE_RAID))
        {
            uint8 level = bot->GetLevel();
            bool levelOk = !dungeon->MinLevel || (level >= dungeon->MinLevel && level <= dungeon->MaxLevel);
            bool oldDungeonTooHigh =
                dungeon->TypeID == lfg::LFG_TYPE_DUNGEON && level > dungeon->MinLevel + 10;

            if (levelOk && !oldDungeonTooHigh)
                list.insert(dungeon->ID);
        }
    }
    else
    {
        // Compatibility fallback for reservations created before this patch.
        std::vector<uint32> const& configuredDungeons = LfgDungeons[bot->GetTeamId()];
        for (uint32 dungeonId : configuredDungeons)
        {
            LFGDungeonEntry const* dungeon = sLFGDungeonStore.LookupEntry(dungeonId);
            if (!dungeon)
                continue;
            if (dungeon->TypeID != lfg::LFG_TYPE_RANDOM && dungeon->TypeID != lfg::LFG_TYPE_DUNGEON &&
                dungeon->TypeID != lfg::LFG_TYPE_HEROIC && dungeon->TypeID != lfg::LFG_TYPE_RAID)
                continue;

            uint8 level = bot->GetLevel();
            if (dungeon->MinLevel && (level < dungeon->MinLevel || level > dungeon->MaxLevel))
                continue;
            if (level > dungeon->MinLevel + 10 && dungeon->TypeID == lfg::LFG_TYPE_DUNGEON)
                continue;
            list.insert(dungeon->ID);
        }
    }

    if (list.empty())
        return false;

    PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
    if (!ai)
        return false;

    uint32 roleMask = reservation.role;
    std::string const gearScore = std::to_string(ai->GetEquipGearScore(bot));

    // Rebuild the cached LFG lock map immediately before joining. The
    // PlayerbotsGlobalScript hook removes only quest/item/achievement locks
    // while this character has an active burst-LFG reservation.
    sLFGMgr->InitializeLockedDungeons(bot, bot->GetGroup());

    if (sPlayerbotAIConfig.queueAutoScaleBurstOnly)
    {
        reservation.joinRequestedAt = now;
        ++queueBurstLfgJoinCount;

        sLFGMgr->JoinLfg(bot, uint8(roleMask), list, gearScore);
        bot->UpdateLFGChannel();

        lfg::LfgState state = sLFGMgr->GetState(bot->GetGUID());
        if (state == lfg::LFG_STATE_NONE)
            return false;

        return true;
    }

    WorldPacket* data = new WorldPacket(CMSG_LFG_JOIN);
    *data << roleMask;
    *data << bool(false);
    *data << bool(false);
    *data << uint8(list.size());
    for (uint32 dungeon : list)
        *data << dungeon;
    *data << uint8(3) << uint8(0) << uint8(0) << uint8(0);
    *data << gearScore;

    bot->GetSession()->QueuePacket(data);
    reservation.joinRequestedAt = now;
    ++queueBurstLfgJoinCount;

    return true;
}

bool RandomPlayerbotMgr::IsQueueBurstLfgJoinPending(Player const* bot) const
{
    if (!bot)
        return false;

    auto itr = queueBurstBots.find(bot->GetGUID().GetCounter());
    if (itr == queueBurstBots.end() || !itr->second.lfg || !itr->second.joinRequestedAt)
        return false;

    if (sLFGMgr->GetState(bot->GetGUID()) != lfg::LFG_STATE_NONE)
        return false;

    return time(nullptr) < itr->second.joinRequestedAt + 15;
}

bool RandomPlayerbotMgr::IsQueueBotBusy(Player* bot) const
{
    if (!bot)
        return false;

    if (bot->IsInCombat() || bot->InBattleground() || bot->InArena() || bot->InBattlegroundQueue())
        return true;

    if (bot->GetGroup())
        return true;

    lfg::LfgState state = sLFGMgr->GetState(bot->GetGUID());
    return state != lfg::LFG_STATE_NONE && state != lfg::LFG_STATE_FINISHED_DUNGEON;
}

bool RandomPlayerbotMgr::IsQueueBurstBot(Player const* bot, bool forLfg, bool forBg) const
{
    if (!bot)
        return false;

    auto itr = queueBurstBots.find(bot->GetGUID().GetCounter());
    if (itr == queueBurstBots.end())
        return false;

    if (forLfg && !itr->second.lfg)
        return false;
    if (forBg && !itr->second.bg)
        return false;

    return true;
}

void RandomPlayerbotMgr::CleanupOrphanedQueueGroups()
{
    // Queue-created groups can survive after every real player has left.
    // Bots then keep group/instance slots occupied because grouped bots are
    // considered busy by the autoscale cleanup.
    std::unordered_set<uint32> processedGroups;

    for (auto const& [guid, bot] : playerBots)
    {
        (void)guid;

        if (!bot || !bot->IsInWorld())
            continue;

        Group* group = bot->GetGroup();
        if (!group)
            continue;

        bool const isLfgGroup = group->isLFGGroup();
        bool const isBgGroup = group->isBGGroup() || group->isBFGroup();
        if (!isLfgGroup && !isBgGroup)
            continue;

        uint32 const groupId = group->GetGUID().GetCounter();
        if (!processedGroups.insert(groupId).second)
            continue;

        bool hasRealPlayer = false;
        std::vector<Player*> botsToRemove;

        for (GroupReference* ref = group->GetFirstMember(); ref != nullptr; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member)
                continue;

            // Anything managed by RandomPlayerbotMgr is a bot. Any connected
            // member outside playerBots is a real player and keeps the group alive.
            if (GetPlayerBot(member->GetGUID().GetCounter()))
                botsToRemove.push_back(member);
            else
            {
                hasRealPlayer = true;
                break;
            }
        }

        if (hasRealPlayer || botsToRemove.empty())
            continue;

        ObjectGuid const groupGuid = group->GetGUID();

        LOG_INFO("playerbots",
                 "Queue autoscale cleanup: queue group {} has no real players; removing {} bots (LFG={}, BG={})",
                 groupGuid.ToString(), botsToRemove.size(), isLfgGroup, isBgGroup);

        // Do not mutate GroupReference while iterating it. Both the LFG/BG
        // leave paths and RemoveFromGroup() may change or disband the group.
        for (Player* member : botsToRemove)
        {
            if (!member)
                continue;

            uint32 const botGuid = member->GetGUID().GetCounter();

            if (isLfgGroup)
            {
                lfg::LfgState const state = sLFGMgr->GetState(member->GetGUID());
                if (state != lfg::LFG_STATE_NONE)
                    sLFGMgr->LeaveLfg(member->GetGUID());

                sLFGMgr->LeaveAllLfgQueues(member->GetGUID(), true, groupGuid);
            }

            if (isBgGroup)
            {
                if (member->InBattleground())
                {
                    // Frees the live battleground slot and removes the player
                    // from the battleground raid.
                    member->LeaveBattleground();
                }
                else
                {
                    // The bot may still only be queued or invited.
                    for (uint8 slot = 0; slot < PLAYER_MAX_BATTLEGROUND_QUEUES; ++slot)
                    {
                        BattlegroundQueueTypeId queueTypeId = member->GetBattlegroundQueueTypeId(slot);
                        if (queueTypeId == BATTLEGROUND_QUEUE_NONE)
                            continue;

                        member->RemoveBattlegroundQueueId(queueTypeId);
                        sBattlegroundMgr->GetBattlegroundQueue(queueTypeId)
                            .RemovePlayer(member->GetGUID(), true);
                    }
                }
            }

            // If the activity-specific leave path did not already detach the
            // bot, explicitly remove it from the remaining group.
            if (member->GetGroup())
                member->RemoveFromGroup(GROUP_REMOVEMETHOD_LEAVE);

            auto reservationItr = queueBurstBots.find(botGuid);
            if (reservationItr != queueBurstBots.end())
            {
                bool const restoreLfgLocks = reservationItr->second.lfg;

                SetEventValue(botGuid, "add", 0, 0);
                SetEventValue(botGuid, "randomize", 0, 0);
                queueBurstBots.erase(reservationItr);

                if (restoreLfgLocks)
                    sLFGMgr->InitializeLockedDungeons(member, member->GetGroup());
            }
        }
    }
}

void RandomPlayerbotMgr::BalanceQueueBots()
{
    if (queueBurstBots.empty())
        return;

    // Never trim ordinary population here. Only bots created by queue autoscale
    // are candidates, and only while total reserved population is over MaxRandomBots.
    if (currentBots.size() <= sPlayerbotAIConfig.maxRandomBots)
        return;

    uint32 excess = currentBots.size() - sPlayerbotAIConfig.maxRandomBots;
    uint32 remaining = std::min<uint32>(excess, sPlayerbotAIConfig.queueAutoScaleLogoutBatch);
    time_t now = time(nullptr);

    for (auto it = queueBurstBots.begin(); it != queueBurstBots.end() && remaining; )
    {
        uint32 guid = it->first;
        QueueBotReservation const& reservation = it->second;
        Player* bot = GetPlayerBot(guid);

        if (bot && IsQueueBotBusy(bot))
        {
            it->second.lastNeeded = now;
            ++it;
            continue;
        }

        if (now < reservation.lastNeeded + sPlayerbotAIConfig.queueAutoScaleIdleSeconds)
        {
            ++it;
            continue;
        }

        SetEventValue(guid, "add", 0, 0);
        SetEventValue(guid, "randomize", 0, 0);

        bool const restoreLfgLocks = reservation.lfg && bot;

        // Offline reservations can be removed immediately. Online bots are
        // logged out by ProcessBot() on the next manager pass after add expires.
        if (!bot)
            currentBots.erase(guid);

        it = queueBurstBots.erase(it);

        if (restoreLfgLocks)
            sLFGMgr->InitializeLockedDungeons(bot, bot->GetGroup());

        --remaining;
    }
}

void RandomPlayerbotMgr::CheckPlayers()
{
    if (!PlayersCheckTimer || time(nullptr) > (PlayersCheckTimer + 60))
        PlayersCheckTimer = time(nullptr);

    LOG_INFO("playerbots", "Checking Players...");

    if (!playersLevel)
        playersLevel = sPlayerbotAIConfig.randombotStartingLevel;

    for (std::vector<Player*>::iterator i = players.begin(); i != players.end(); ++i)
    {
        Player* player = *i;

        if (player->IsGameMaster())
            continue;

        // if (player->GetSession()->GetSecurity() > SEC_PLAYER)
        //     continue;

        if (player->GetLevel() > playersLevel)
            playersLevel = player->GetLevel() + 3;
    }

    LOG_INFO("playerbots", "Max player level is {}, max bot level set to {}", playersLevel - 3, playersLevel);
}

void RandomPlayerbotMgr::ScheduleRandomize(uint32 bot, uint32 time) { SetEventValue(bot, "randomize", 1, time); }

void RandomPlayerbotMgr::ScheduleTeleport(uint32 bot, uint32 time)
{
    if (!time)
        time = 60 + urand(sPlayerbotAIConfig.randomBotUpdateInterval, sPlayerbotAIConfig.randomBotUpdateInterval * 3);

    SetEventValue(bot, "teleport", 1, time);
}

void RandomPlayerbotMgr::ScheduleChangeStrategy(uint32 bot, uint32 time)
{
    if (!time)
        time = urand(sPlayerbotAIConfig.minRandomBotChangeStrategyTime,
                     sPlayerbotAIConfig.maxRandomBotChangeStrategyTime);

    SetEventValue(bot, "change_strategy", 1, time);
}

bool RandomPlayerbotMgr::ProcessBot(uint32 bot)
{
    ObjectGuid botGUID = ObjectGuid::Create<HighGuid::Player>(bot);
    Player* player = GetPlayerBot(botGUID);
    PlayerbotAI* botAI = player ? GET_PLAYERBOT_AI(player) : nullptr;

    uint32 isValid = GetEventValue(bot, "add");
    if (!isValid)
    {
        if (!player || !player->GetGroup())
        {
            if (player)
                LOG_DEBUG("playerbots", "Bot #{} {}:{} <{}>: log out", bot, IsAlliance(player->getRace()) ? "A" : "H",
                          player->GetLevel(), player->GetName().c_str());
            else
                LOG_DEBUG("playerbots", "Bot #{}: log out", bot);

            SetEventValue(bot, "add", 0, 0);
            currentBots.erase(bot);

            if (player)
                LogoutPlayerBot(botGUID);
        }

        return false;
    }

    uint32 randomTime;
    if (!player)
    {
        AddPlayerBot(botGUID, 0);
        randomTime = urand(1, 2);

        uint32 randomBotUpdateInterval = _isBotInitializing ? 1 : sPlayerbotAIConfig.randomBotUpdateInterval;
        randomTime = urand(std::max(5, static_cast<int>(randomBotUpdateInterval * 0.5)),
                           std::max(12, static_cast<int>(randomBotUpdateInterval * 2)));
        SetEventValue(bot, "update", 1, randomTime);

        // do not randomize or teleport immediately after server start (prevent lagging)
        if (!GetEventValue(bot, "randomize"))
        {
            randomTime = urand(3, std::max(4, static_cast<int>(randomBotUpdateInterval * 0.4)));
            ScheduleRandomize(bot, randomTime);
        }
        if (!GetEventValue(bot, "teleport"))
        {
            randomTime = urand(std::max(7, static_cast<int>(randomBotUpdateInterval * 0.7)),
                               std::max(14, static_cast<int>(randomBotUpdateInterval * 1.4)));
            ScheduleTeleport(bot, randomTime);
        }

        return true;
    }

    if (!player->IsInWorld())
        return false;

    if (player->GetGroup() || player->HasUnitState(UNIT_STATE_IN_FLIGHT))
        return false;

    uint32 update = GetEventValue(bot, "update");
    if (!update)
    {
        if (botAI)
            botAI->GetAiObjectContext()->GetValue<bool>("random bot update")->Set(true);

        bool update = true;
        if (botAI)
        {
            // botAI->GetAiObjectContext()->GetValue<bool>("random bot update")->Set(true);
            if (!sRandomPlayerbotMgr.IsRandomBot(player))
                update = false;

            if (player->GetGroup() && botAI->GetGroupLeader())
            {
                PlayerbotAI* groupLeaderBotAI = GET_PLAYERBOT_AI(botAI->GetGroupLeader());
                if (!groupLeaderBotAI || IsSelfBot(botAI->GetGroupLeader()))
                    update = false;
            }

            // if (botAI->HasPlayerNearby(sPlayerbotAIConfig.grindDistance))
            //     update = false;
        }

        if (update)
            ProcessBot(player);

        randomTime = urand(sPlayerbotAIConfig.minRandomBotReviveTime, sPlayerbotAIConfig.maxRandomBotReviveTime);
        SetEventValue(bot, "update", 1, randomTime);

        return true;
    }

    uint32 logout = GetEventValue(bot, "logout");
    if (player && !logout && !isValid)
    {
        LOG_DEBUG("playerbots", "Bot #{} {}:{} <{}>: log out", bot, IsAlliance(player->getRace()) ? "A" : "H",
                  player->GetLevel(), player->GetName().c_str());
        LogoutPlayerBot(botGUID);
        currentBots.erase(bot);
        SetEventValue(bot, "logout", 1,
                      urand(sPlayerbotAIConfig.minRandomBotInWorldTime, sPlayerbotAIConfig.maxRandomBotInWorldTime));
        return true;
    }

    return false;
}

bool RandomPlayerbotMgr::ProcessBot(Player* bot)
{

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return false;

    if (bot->InBattleground())
        return false;

    if (bot->InBattlegroundQueue())
        return false;

     uint32 botId = bot->GetGUID().GetCounter();

    // if death revive
    if (bot->isDead())
    {
        if (!GetEventValue(botId, "dead"))
        {
            uint32 randomTime =
                urand(sPlayerbotAIConfig.minRandomBotReviveTime, sPlayerbotAIConfig.maxRandomBotReviveTime);
            LOG_DEBUG("playerbots", "Mark bot {} as dead, will be revived in {}s.", bot->GetName().c_str(),
                      randomTime);
            SetEventValue(botId, "dead", 1, sPlayerbotAIConfig.maxRandomBotInWorldTime);
            SetEventValue(botId, "revive", 1, randomTime);
            return false;
        }

        if (!GetEventValue(botId, "revive"))
        {
            Revive(bot);
            return true;
        }

        return false;
    }

    // leave group if leader is rndbot
    Group* group = bot->GetGroup();
    if (group && !group->isLFGGroup() && IsRandomBot(group->GetLeader()))
    {
        botAI->LeaveOrDisbandGroup();
        LOG_INFO("playerbots", "Bot {} remove from group since leader is random bot.", bot->GetName().c_str());
    }

    // only randomize and teleport idle bots
    bool idleBot = false;
    if (TravelTarget* target = botAI->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get())
    {
        if (target->getTravelState() == TravelState::TRAVEL_STATE_IDLE)
        {
            idleBot = true;
        }
    }
    else
    {
        idleBot = true;
    }

    if (idleBot)
    {
        // randomize
        uint32 randomize = GetEventValue(botId, "randomize");
        if (!randomize)
        {
            // bool randomiser = true;
            // if (player->GetGuildId())
            // {
            //     if (Guild* guild = sGuildMgr->GetGuildById(player->GetGuildId()))
            //     {
            //         if (guild->GetLeaderGUID() == player->GetGUID())
            //         {
            //             for (std::vector<Player*>::iterator i = players.begin(); i != players.end(); ++i)
            //                 GuildTaskMgr::instance().Update(*i, player);
            //         }

            //         uint32 accountId = sCharacterCache->GetCharacterAccountIdByGuid(guild->GetLeaderGUID());
            //         if (!sPlayerbotAIConfig.IsInRandomAccountList(accountId))
            //         {
            //             uint8 rank = player->GetRank();
            //             randomiser = rank < 4 ? false : true;
            //         }
            //     }
            // }
            // if (randomiser)
            // {
            Randomize(bot);
            LOG_DEBUG("playerbots", "Bot #{} {}:{} <{}>: randomized", botId,
                      bot->GetTeamId() == TEAM_ALLIANCE ? "A" : "H", bot->GetLevel(), bot->GetName());
            uint32 randomTime =
                urand(sPlayerbotAIConfig.minRandomBotRandomizeTime, sPlayerbotAIConfig.maxRandomBotRandomizeTime);
            ScheduleRandomize(botId, randomTime);
            return true;
        }

        // uint32 changeStrategy = GetEventValue(bot, "change_strategy");
        // if (!changeStrategy)
        // {
        //     LOG_INFO("playerbots", "Changing strategy for bot  #{} <{}>", bot, player->GetName().c_str());
        //     ChangeStrategy(player);
        //     return true;
        // }

        uint32 teleport = GetEventValue(botId, "teleport");
        if (!teleport)
        {
            LOG_DEBUG("playerbots", "Bot #{} <{}>: teleport for level and refresh", botId, bot->GetName());
            Refresh(bot);
            RandomTeleportForLevel(bot);
            uint32 time = urand(sPlayerbotAIConfig.minRandomBotTeleportInterval,
                                sPlayerbotAIConfig.maxRandomBotTeleportInterval);
            ScheduleTeleport(botId, time);
            return true;
        }
    }

    return false;
}

void RandomPlayerbotMgr::Revive(Player* player)
{
    uint32 bot = player->GetGUID().GetCounter();

    // LOG_INFO("playerbots", "Bot {} revived", player->GetName().c_str());
    SetEventValue(bot, "dead", 0, 0);
    SetEventValue(bot, "revive", 0, 0);

    Refresh(player);
    RandomTeleportGrindForLevel(player);
}

void RandomPlayerbotMgr::RandomTeleport(Player* bot, std::vector<WorldLocation>& locs, bool hearth)
{
    // ignore when alrdy teleported or not in the world yet.
    if (bot->IsBeingTeleported() || !bot->IsInWorld())
        return;

    // no teleport / movement update when rooted.
    if (bot->IsRooted())
        return;

    // ignore when in queue for battle grounds.
    if (bot->InBattlegroundQueue())
        return;

    // ignore when in battle grounds or arena.
    if (bot->InBattleground() || bot->InArena())
        return;

    // ignore when in group (e.g. world, dungeons, raids) and leader is not a player.
    if (bot->GetGroup() && !bot->GetGroup()->IsLeader(bot->GetGUID()))
        return;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (botAI)
    {
        // ignore when in when taxi with boat/zeppelin and has players nearby
        if (bot->HasUnitMovementFlag(MOVEMENTFLAG_ONTRANSPORT) && bot->HasUnitState(UNIT_STATE_IGNORE_PATHFINDING) &&
            botAI->HasPlayerNearby())
            return;
    }

    // if (sPlayerbotAIConfig.randomBotRpgChance < 0)
    //     return;

    if (locs.empty())
    {
        LOG_DEBUG("playerbots", "Cannot teleport bot {} - no locations available", bot->GetName().c_str());
        return;
    }

    std::vector<WorldPosition> tlocs;
    for (auto& loc : locs)
        tlocs.push_back(WorldPosition(loc));
    // Do not teleport to maps disabled in config
    tlocs.erase(std::remove_if(tlocs.begin(), tlocs.end(),
                               [](WorldPosition l)
                               {
                                   std::vector<uint32>::iterator i =
                                       find(sPlayerbotAIConfig.randomBotMaps.begin(),
                                            sPlayerbotAIConfig.randomBotMaps.end(), l.GetMapId());
                                   return i == sPlayerbotAIConfig.randomBotMaps.end();
                               }),
                tlocs.end());
    if (tlocs.empty())
    {
        LOG_DEBUG("playerbots", "Cannot teleport bot {} - all locations removed by filter", bot->GetName().c_str());
        return;
    }

    PerfMonitorOperation* pmo = sPerfMonitor.start(PERF_MON_RNDBOT, "RandomTeleportByLocations");

    std::shuffle(std::begin(tlocs), std::end(tlocs), RandomEngine::Instance());
    for (uint32 i = 0; i < tlocs.size(); i++)
    {
        WorldLocation loc = tlocs[i];

        float x = loc.GetPositionX();  // + (attemtps > 0 ? urand(0, sPlayerbotAIConfig.grindDistance) -
                                       // sPlayerbotAIConfig.grindDistance / 2 : 0);
        float y = loc.GetPositionY();  // + (attemtps > 0 ? urand(0, sPlayerbotAIConfig.grindDistance) -
                                       // sPlayerbotAIConfig.grindDistance / 2 : 0);
        float z = loc.GetPositionZ();

        Map* map = sMapMgr->FindMap(loc.GetMapId(), 0);
        if (!map)
            continue;

        AreaTableEntry const* zone = sAreaTableStore.LookupEntry(map->GetZoneId(bot->GetPhaseMask(), x, y, z));
        if (!zone)
            continue;

        AreaTableEntry const* area = sAreaTableStore.LookupEntry(map->GetAreaId(bot->GetPhaseMask(), x, y, z));
        if (!area)
            continue;

        // Do not teleport to enemy zones if level is low
        if (zone->team == 4 && bot->GetTeamId() == TEAM_ALLIANCE)
            continue;

        if (zone->team == 2 && bot->GetTeamId() == TEAM_HORDE)
            continue;

        if (map->IsInWater(bot->GetPhaseMask(), x, y, z, bot->GetCollisionHeight()))
            continue;

        float ground = map->GetHeight(bot->GetPhaseMask(), x, y, z + 0.5f);
        if (ground <= INVALID_HEIGHT)
            continue;

        z = 0.05f + ground;

        if (!botAI->StarterLevelDistanceCheck(bot, loc, true))
            continue;

        LocaleConstant const& locale = sWorld->GetDefaultDbcLocale();
        LOG_DEBUG("playerbots",
                  "Random teleporting bot {} (level {}) to Map: {} ({}) Zone: {} ({}) Area: {} ({}) ZoneLevel: {} "
                  "AreaLevel: {} {},{},{} ({}/{} "
                  "locations)",
                  bot->GetName().c_str(), bot->GetLevel(), map->GetId(), map->GetMapName(), zone->ID,
                  zone->area_name[locale], area->ID, area->area_name[locale], zone->area_level, area->area_level, x, y,
                  z, i + 1, tlocs.size());

        if (hearth)
        {
            bot->SetHomebind(loc, zone->ID);
        }

        // Prevent blink to be detected by visible real players
        if (botAI->HasPlayerNearby(150.0f))
        {
            break;
        }

        bot->GetMotionMaster()->Clear();
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (botAI)
            botAI->Reset(true);
        bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
        bot->TeleportTo(loc.GetMapId(), x, y, z, 0);
        bot->SendMovementFlagUpdate();

        if (pmo)
            pmo->finish();

        return;
    }

    if (pmo)
        pmo->finish();

    // LOG_ERROR("playerbots", "Cannot teleport bot {} - no locations available ({} locations)", bot->GetName().c_str(),
    //           tlocs.size());
}

void RandomPlayerbotMgr::PrepareAddclassCache()
{
    // Using accounts marked as type 2 (AddClass)
    int32 collected = 0;

    for (uint32 accountId : addClassTypeAccounts)
    {
        for (uint8 claz = CLASS_WARRIOR; claz <= CLASS_DRUID; claz++)
        {
            if (claz == 10)
                continue;

            QueryResult results = CharacterDatabase.Query(
                "SELECT guid, race FROM characters "
                "WHERE account = {} AND class = '{}' AND online = 0",
                accountId, claz);

            if (results)
            {
                do
                {
                    Field* fields = results->Fetch();
                    ObjectGuid guid = ObjectGuid(HighGuid::Player, fields[0].Get<uint32>());
                    uint32 race = fields[1].Get<uint32>();
                    bool isAlliance = race == 1 || race == 3 || race == 4 || race == 7 || race == 11;
                    addclassCache[GetTeamClassIdx(isAlliance, claz)].insert(guid);
                    collected++;
                } while (results->NextRow());
            }
        }
    }

    LOG_INFO("playerbots", ">> {} characters collected for addclass command from {} AddClass accounts.", collected, addClassTypeAccounts.size());
}

void RandomPlayerbotMgr::Init()
{
    if (sPlayerbotAIConfig.addClassCommand)
        sRandomPlayerbotMgr.PrepareAddclassCache();

    if (sPlayerbotAIConfig.randomBotJoinBG)
        sRandomPlayerbotMgr.LoadBattleMastersCache();

    PlayerbotsDatabasePreparedStatement* stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_DEL_RANDOM_BOTS_BY_EVENT);
    stmt->SetData(0, std::string("add"));
    PlayerbotsDatabase.DirectExecute(stmt);
}

void RandomPlayerbotMgr::InitArenaTeams()
{
    if (sPlayerbotAIConfig.deleteRandomBotArenaTeams)
    {
        RandomPlayerbotFactory::DeleteBotArenaTeams();
        return;
    }

    RandomPlayerbotFactory::LoadArenaTeamData();

    LOG_INFO("playerbots", "Bot arena teams: 2v2={}/{}, 3v3={}/{}, 5v5={}/{}",
             RandomPlayerbotFactory::GetBotArenaTeamCount(ARENA_TYPE_2v2), sPlayerbotAIConfig.randomBotArenaTeam2v2Count,
             RandomPlayerbotFactory::GetBotArenaTeamCount(ARENA_TYPE_3v3), sPlayerbotAIConfig.randomBotArenaTeam3v3Count,
             RandomPlayerbotFactory::GetBotArenaTeamCount(ARENA_TYPE_5v5), sPlayerbotAIConfig.randomBotArenaTeam5v5Count);
}

void RandomPlayerbotMgr::RandomTeleportForLevel(Player* bot)
{
    if (bot->InBattleground())
        return;

    if (bot->GetLevel() >= 10 && urand(0, 100) < sPlayerbotAIConfig.probTeleToBankers * 100)
    {
        std::vector<WorldLocation> locs = sTravelMgr.GetCityLocations(bot);
        if (!locs.empty())
        {
            RandomTeleport(bot, locs, true);
            return;
        }
    }
    std::vector<WorldLocation> locs = sTravelMgr.GetTeleportLocations(bot);

    if (sPlayerbotAIConfig.randomBotConcentrateInPlayerZone && !locs.empty())
    {
        std::vector<WorldLocation> playerZoneLocs = GetPlayerZoneTeleportLocations(locs, bot);
        if (!playerZoneLocs.empty())
            locs = std::move(playerZoneLocs);
    }

    if (!locs.empty())
    {
        RandomTeleport(bot, locs, false);
        return;
    }
}

// Returns the subset of teleport locations that lie in a zone currently occupied by a real
// (non-GM) player, so random bots can be gathered where players actually are. Returns an empty
// vector when no player is online or none of the locations match, letting the caller fall back
// to the default world-wide behaviour.
std::vector<WorldLocation> RandomPlayerbotMgr::GetPlayerZoneTeleportLocations(std::vector<WorldLocation> const& locs,
                                                                              Player* bot)
{
    std::set<uint32> playerMaps;
    std::set<std::pair<uint32, uint32>> playerMapZones;

    // players only ever holds real (non random bot) players and is maintained on login/logout, so
    // this is a pass over the online player list, not a world-wide scan.
    for (Player* player : players)
    {
        if (!player || !player->IsInWorld() || player->IsGameMaster())
            continue;

        Map* map = player->GetMap();
        if (!map)
            continue;

        // Instanceable maps (dungeons, raids, battlegrounds, arenas) are never valid targets: a
        // WorldLocation carries no instance id, so a bot would be sent to another instance of the
        // same map rather than to the player.
        if (map->Instanceable())
            continue;

        // Resolve the player zone the same way as the candidate locations below (unphased terrain),
        // so a player standing in a phased area still matches its underlying zone.
        uint32 zoneId = map->GetZoneId(PHASEMASK_NORMAL, player->GetPositionX(), player->GetPositionY(),
                                       player->GetPositionZ());
        playerMaps.insert(map->GetId());
        playerMapZones.insert(std::make_pair(map->GetId(), zoneId));
    }

    std::vector<WorldLocation> filtered;
    if (playerMapZones.empty())
        return filtered;

    for (WorldLocation const& loc : locs)
    {
        if (playerMaps.find(loc.GetMapId()) == playerMaps.end())
            continue;

        uint32 zoneId = sMapMgr->GetZoneId(PHASEMASK_NORMAL, loc);
        if (playerMapZones.find(std::make_pair(loc.GetMapId(), zoneId)) == playerMapZones.end())
            continue;

        // Skip enemy-faction zones, matching the check in RandomTeleport. This has to be done here:
        // the caller only falls back to normal teleporting when the filtered set is empty, so keeping
        // a hostile location would let the downstream team check drain the set and strand the bot.
        if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(zoneId))
        {
            if (zone->team == 4 && bot->GetTeamId() == TEAM_ALLIANCE)
                continue;

            if (zone->team == 2 && bot->GetTeamId() == TEAM_HORDE)
                continue;
        }

        filtered.push_back(loc);
    }

    return filtered;
}

void RandomPlayerbotMgr::RandomTeleportGrindForLevel(Player* bot)
{
    if (bot->InBattleground())
        return;

    std::vector<WorldLocation> locs = sTravelMgr.GetTeleportLocations(bot);
    LOG_DEBUG("playerbots", "Random teleporting bot {} for level {} ({} locations available)", bot->GetName().c_str(),
              bot->GetLevel(), locs.size());

    RandomTeleport(bot, locs);
}

void RandomPlayerbotMgr::RandomTeleport(Player* bot)
{
    if (bot->InBattleground())
        return;

    PerfMonitorOperation* pmo = sPerfMonitor.start(PERF_MON_RNDBOT, "RandomTeleport");
    std::vector<WorldLocation> locs;

    std::list<Unit*> targets;
    float range = sPlayerbotAIConfig.randomBotTeleportDistance;
    Acore::AnyUnitInObjectRangeCheck u_check(bot, range);
    Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(bot, targets, u_check);
    Cell::VisitObjects(bot, searcher, range);

    if (!targets.empty())
    {
        for (Unit* unit : targets)
        {
            bot->UpdatePosition(*unit);
            FleeManager manager(bot, sPlayerbotAIConfig.sightDistance, 0, true);
            float rx, ry, rz;
            if (manager.CalculateDestination(&rx, &ry, &rz))
            {
                WorldLocation loc(bot->GetMapId(), rx, ry, rz);
                locs.push_back(loc);
            }
        }
    }
    else
    {
        RandomTeleportForLevel(bot);
    }

    if (pmo)
        pmo->finish();

    Refresh(bot);
}

void RandomPlayerbotMgr::Randomize(Player* bot)
{
    if (bot->InBattleground())
        return;

    if (bot->GetLevel() < 3 || (bot->GetLevel() < 56 && bot->getClass() == CLASS_DEATH_KNIGHT))
    {
        RandomizeFirst(bot);
    }
    else if (bot->GetLevel() < sPlayerbotAIConfig.randomBotMaxLevel || !sPlayerbotAIConfig.downgradeMaxLevelBot)
    {
        uint8 level = bot->GetLevel();
        PlayerbotFactory factory(bot, level);
        factory.Randomize(true);
        // IncreaseLevel(bot);
    }
    else
    {
        RandomizeFirst(bot);
    }
}

void RandomPlayerbotMgr::IncreaseLevel(Player* bot)
{
    uint32 maxLevel = sPlayerbotAIConfig.randomBotMaxLevel;
    if (maxLevel > sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL))
        maxLevel = sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL);

    PerfMonitorOperation* pmo = sPerfMonitor.start(PERF_MON_RNDBOT, "IncreaseLevel");
    uint32 lastLevel = GetValue(bot, "level");
    uint8 level = bot->GetLevel() + 1;
    if (level > maxLevel)
    {
        level = maxLevel;
    }
    if (lastLevel != level)
    {
        PlayerbotFactory factory(bot, level);
        factory.Randomize(true);
    }

    if (pmo)
        pmo->finish();
}

void RandomPlayerbotMgr::RandomizeFirst(Player* bot)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return;

    uint32 maxLevel = sPlayerbotAIConfig.randomBotMaxLevel;
    if (maxLevel > sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL))
        maxLevel = sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL);

    // if lvl sync is enabled, max level is limited by online players lvl
    if (sPlayerbotAIConfig.syncLevelWithPlayers)
        maxLevel = std::max(sPlayerbotAIConfig.randomBotMinLevel,
                            std::min(playersLevel, sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL)));

    uint32 minLevel = sPlayerbotAIConfig.randomBotMinLevel;
    if (bot->getClass() == CLASS_DEATH_KNIGHT)
    {
        maxLevel = std::max(maxLevel, sWorld->getIntConfig(CONFIG_START_HEROIC_PLAYER_LEVEL));
        minLevel = std::max(minLevel, sWorld->getIntConfig(CONFIG_START_HEROIC_PLAYER_LEVEL));
    }

    PerfMonitorOperation* pmo = sPerfMonitor.start(PERF_MON_RNDBOT, "RandomizeFirst");

    uint32 level;

    if (sPlayerbotAIConfig.downgradeMaxLevelBot && bot->GetLevel() >= sPlayerbotAIConfig.randomBotMaxLevel)
    {
        if (bot->getClass() == CLASS_DEATH_KNIGHT)
        {
            level = sWorld->getIntConfig(CONFIG_START_HEROIC_PLAYER_LEVEL);
        }
        else
        {
            level = sPlayerbotAIConfig.randomBotMinLevel;
        }
    }
    else
    {
        uint32 roll = urand(1, 100);
        if (roll <= 100 * sPlayerbotAIConfig.randomBotMaxLevelChance)
        {
            level = maxLevel;
        }
        else if (roll <=
                 (100 * (sPlayerbotAIConfig.randomBotMaxLevelChance + sPlayerbotAIConfig.randomBotMinLevelChance)))
        {
            level = minLevel;
        }
        else
        {
            level = urand(minLevel, maxLevel);
        }
    }

    if (sPlayerbotAIConfig.disableRandomLevels)
    {
        level = bot->getClass() == CLASS_DEATH_KNIGHT ? std::max(sPlayerbotAIConfig.randombotStartingLevel,
                                                                 sWorld->getIntConfig(CONFIG_START_HEROIC_PLAYER_LEVEL))
                                                      : sPlayerbotAIConfig.randombotStartingLevel;
    }

    SetValue(bot, "level", level);
    PlayerbotFactory factory(bot, level);
    factory.Randomize(false);

    uint32 randomTime =
        urand(sPlayerbotAIConfig.minRandomBotRandomizeTime, sPlayerbotAIConfig.maxRandomBotRandomizeTime);
    uint32 inworldTime =
        urand(sPlayerbotAIConfig.minRandomBotInWorldTime, sPlayerbotAIConfig.maxRandomBotInWorldTime);

    PlayerbotsDatabasePreparedStatement* stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_UPD_RANDOM_BOTS);
    stmt->SetData(0, randomTime);
    stmt->SetData(1, "bot_delete");
    stmt->SetData(2, bot->GetGUID().GetCounter());
    PlayerbotsDatabase.Execute(stmt);

    stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_UPD_RANDOM_BOTS);
    stmt->SetData(0, inworldTime);
    stmt->SetData(1, "logout");
    stmt->SetData(2, bot->GetGUID().GetCounter());
    PlayerbotsDatabase.Execute(stmt);

    // teleport to a random inn for bot level
    botAI->Reset(true);

    if (bot->GetGroup())
        botAI->LeaveOrDisbandGroup();

    if (pmo)
        pmo->finish();

    RandomTeleportForLevel(bot);
}

void RandomPlayerbotMgr::RandomizeMin(Player* bot)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return;

    PerfMonitorOperation* pmo = sPerfMonitor.start(PERF_MON_RNDBOT, "RandomizeMin");
    uint32 level = sPlayerbotAIConfig.randomBotMinLevel;
    SetValue(bot, "level", level);
    PlayerbotFactory factory(bot, level);
    factory.Randomize(false);

    uint32 randomTime =
        urand(sPlayerbotAIConfig.minRandomBotRandomizeTime, sPlayerbotAIConfig.maxRandomBotRandomizeTime);
    uint32 inworldTime =
        urand(sPlayerbotAIConfig.minRandomBotInWorldTime, sPlayerbotAIConfig.maxRandomBotInWorldTime);

    PlayerbotsDatabasePreparedStatement* stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_UPD_RANDOM_BOTS);
    stmt->SetData(0, randomTime);
    stmt->SetData(1, "bot_delete");
    stmt->SetData(2, bot->GetGUID().GetCounter());
    PlayerbotsDatabase.Execute(stmt);

    stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_UPD_RANDOM_BOTS);
    stmt->SetData(0, inworldTime);
    stmt->SetData(1, "logout");
    stmt->SetData(2, bot->GetGUID().GetCounter());
    PlayerbotsDatabase.Execute(stmt);

    // teleport to a random inn for bot level
    botAI->Reset(true);

    if (bot->GetGroup())
        botAI->LeaveOrDisbandGroup();

    if (pmo)
        pmo->finish();
}

void RandomPlayerbotMgr::Clear(Player* bot)
{
    PlayerbotFactory factory(bot, bot->GetLevel());
    factory.ClearEverything();
}

uint32 RandomPlayerbotMgr::GetZoneLevel(uint16 mapId, float teleX, float teleY, float /*teleZ*/)
{
    uint32 maxLevel = sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL);

    uint32 level = 0;
    QueryResult results = WorldDatabase.Query(
        "SELECT AVG(t.minlevel) minlevel, AVG(t.maxlevel) maxlevel FROM creature c "
        "INNER JOIN creature_template t ON c.id = t.entry WHERE map = {} AND minlevel > 1 AND ABS(position_x - {}) < "
        "{} AND ABS(position_y - {}) < {}",
        mapId, teleX, sPlayerbotAIConfig.randomBotTeleportDistance / 2, teleY,
        sPlayerbotAIConfig.randomBotTeleportDistance / 2);

    if (results)
    {
        Field* fields = results->Fetch();
        uint8 minLevel = fields[0].Get<uint8>();
        uint8 maxLevel = fields[1].Get<uint8>();
        level = urand(minLevel, maxLevel);
        if (level > maxLevel)
            level = maxLevel;
    }
    else
    {
        level = urand(1, maxLevel);
    }

    return level;
}

void RandomPlayerbotMgr::Refresh(Player* bot)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return;

    if (bot->isDead())
    {
        bot->ResurrectPlayer(1.0f);
        bot->SpawnCorpseBones();
        botAI->ResetStrategies(false);
    }

    // if (sPlayerbotAIConfig.disableRandomLevels)
    //     return;

    if (bot->InBattleground())
        return;

    LOG_DEBUG("playerbots", "Refreshing bot {} <{}>", bot->GetGUID().ToString().c_str(), bot->GetName().c_str());

    PerfMonitorOperation* pmo = sPerfMonitor.start(PERF_MON_RNDBOT, "Refresh");

    botAI->Reset();

    bot->DurabilityRepairAll(false, 1.0f, false);
    bot->SetFullHealth();
    bot->SetPvP(sWorld->IsPvPRealm());
    PlayerbotFactory factory(bot, bot->GetLevel());
    factory.Refresh();

    if (bot->GetMaxPower(POWER_MANA) > 0)
        bot->SetPower(POWER_MANA, bot->GetMaxPower(POWER_MANA));

    if (bot->GetMaxPower(POWER_ENERGY) > 0)
        bot->SetPower(POWER_ENERGY, bot->GetMaxPower(POWER_ENERGY));

    uint32 money = bot->GetMoney();
    bot->SetMoney(money + 500 * sqrt(urand(1, bot->GetLevel() * 5)));

    if (bot->GetGroup())
        botAI->LeaveOrDisbandGroup();

    if (pmo)
        pmo->finish();
}

bool RandomPlayerbotMgr::IsRandomBot(Player* bot)
{
    if (bot && GET_PLAYERBOT_AI(bot))
    {
        if (IsSelfBot(bot))
            return false;
    }
    if (bot)
    {
        return IsRandomBot(bot->GetGUID().GetCounter());
    }

    return false;
}

bool RandomPlayerbotMgr::IsRandomBot(ObjectGuid::LowType bot)
{
    ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(bot);
    if (!sPlayerbotAIConfig.IsInRandomAccountList(sCharacterCache->GetCharacterAccountIdByGuid(guid)))
        return false;

    return currentBots.contains(bot);
}

bool RandomPlayerbotMgr::IsAddclassBot(Player* bot)
{
    if (bot && GET_PLAYERBOT_AI(bot))
    {
        if (IsSelfBot(bot))
            return false;
    }
    if (bot)
    {
        return IsAddclassBot(bot->GetGUID().GetCounter());
    }

    return false;
}

bool RandomPlayerbotMgr::IsAddclassBot(ObjectGuid::LowType bot)
{
    ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(bot);

    // Check the cache with faction considerations
    for (uint8 claz = CLASS_WARRIOR; claz <= CLASS_DRUID; claz++)
    {
        if (claz == 10)
            continue;

        for (uint8 isAlliance = 0; isAlliance <= 1; isAlliance++)
        {
            if (addclassCache[GetTeamClassIdx(isAlliance, claz)].find(guid) !=
                addclassCache[GetTeamClassIdx(isAlliance, claz)].end())
            {
                return true;
            }
        }
    }

    // If not in cache, check the account type
    uint32 accountId = sCharacterCache->GetCharacterAccountIdByGuid(guid);
    if (accountId && IsAccountType(accountId, 2)) // Type 2 = AddClass
    {
        return true;
    }

    return false;
}

void RandomPlayerbotMgr::GetBots()
{
    if (!currentBots.empty())
        return;

    PlayerbotsDatabasePreparedStatement* stmt =
        PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_SEL_RANDOM_BOTS_BY_OWNER_AND_EVENT);
    stmt->SetData(0, 0);
    stmt->SetData(1, "add");
    uint32 maxAllowedBotCount = GetEventValue(0, "bot_count");
    if (PreparedQueryResult result = PlayerbotsDatabase.Query(stmt))
    {
        do
        {
            Field* fields = result->Fetch();
            uint32 bot = fields[0].Get<uint32>();
            if (GetEventValue(bot, "add"))
                currentBots.insert(bot);

            if (currentBots.size() >= maxAllowedBotCount)
                break;
        } while (result->NextRow());
    }
}

std::vector<uint32> RandomPlayerbotMgr::GetBgBots(uint32 bracket)
{
    // if (!currentBgBots.empty()) return currentBgBots;

    std::vector<uint32> BgBots;

    PlayerbotsDatabasePreparedStatement* stmt =
        PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_SEL_RANDOM_BOTS_BY_EVENT_AND_VALUE);
    stmt->SetData(0, "bg");
    stmt->SetData(1, bracket);
    if (PreparedQueryResult result = PlayerbotsDatabase.Query(stmt))
    {
        do
        {
            Field* fields = result->Fetch();
            uint32 bot = fields[0].Get<uint32>();
            BgBots.push_back(bot);
        } while (result->NextRow());
    }

    return BgBots;
}

CachedEvent* RandomPlayerbotMgr::FindEvent(uint32 bot, std::string const& event)
{
    BotEventCache& cache = eventCache[bot];

    // Load once
    if (!cache.loaded)
    {
        cache.events.clear();

        PlayerbotsDatabasePreparedStatement* stmt =
            PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_SEL_RANDOM_BOTS_BY_OWNER_AND_BOT);
        stmt->SetData(0, 0);
        stmt->SetData(1, bot);

        if (PreparedQueryResult result = PlayerbotsDatabase.Query(stmt))
        {
            do
            {
                Field* fields = result->Fetch();

                CachedEvent e;
                e.value = fields[1].Get<uint32>();
                e.lastChangeTime = fields[2].Get<uint32>();
                e.validIn = fields[3].Get<uint32>();
                e.data = fields[4].Get<std::string>();

                cache.events.emplace(fields[0].Get<std::string>(), std::move(e));
            } while (result->NextRow());
        }

        cache.loaded = true;
    }

    auto it = cache.events.find(event);
    if (it == cache.events.end())
        return nullptr;

    CachedEvent& e = it->second;

    // remove expired events
    if (e.validIn && (NowSeconds() - e.lastChangeTime) >= e.validIn && event != "specNo" && event != "specLink")
    {
        cache.events.erase(it);
        return nullptr;
    }

    return &e;
}

bool RandomPlayerbotMgr::IsSpecPvp(uint32 bot, uint8 cls)
{
    uint32 stored = GetValue(bot, "specNo");
    if (!stored)
        return false;
    uint32 specIndex = stored - 1;
    std::string const& name = sPlayerbotAIConfig.premadeSpecName[cls][specIndex];
    return !name.empty() && name.find("pvp") != std::string::npos;
}

uint32 RandomPlayerbotMgr::GetEventValue(uint32 bot, std::string const& event)
{
    if (CachedEvent* e = FindEvent(bot, event))
        return e->value;

    return 0;
}

std::string RandomPlayerbotMgr::GetEventData(uint32 bot, std::string const& event)
{
    if (CachedEvent* e = FindEvent(bot, event))
        return e->data;

    return "";
}

uint32 RandomPlayerbotMgr::SetEventValue(uint32 bot, std::string const& event, uint32 value, uint32 validIn,
                                         std::string const& data)
{
    PlayerbotsDatabaseTransaction trans = PlayerbotsDatabase.BeginTransaction();

    PlayerbotsDatabasePreparedStatement* stmt =
        PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_DEL_RANDOM_BOTS_BY_OWNER_AND_EVENT);
    stmt->SetData(0, 0);
    stmt->SetData(1, bot);
    stmt->SetData(2, event.c_str());
    trans->Append(stmt);

    if (value)
    {
        stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_INS_RANDOM_BOTS);
        stmt->SetData(0, 0);
        stmt->SetData(1, bot);
        stmt->SetData(2, NowSeconds());
        stmt->SetData(3, validIn);
        stmt->SetData(4, event.c_str());
        stmt->SetData(5, value);

        if (!data.empty())
            stmt->SetData(6, data.c_str());
        else
            stmt->SetData(6);  // NULL

        trans->Append(stmt);
    }

    PlayerbotsDatabase.CommitTransaction(trans);

    // Update in-memory cache
    BotEventCache& cache = eventCache[bot];
    cache.loaded = true;

    if (!value)
    {
        cache.events.erase(event);
        return 0;
    }

    CachedEvent& e = cache.events[event];  // create-on-write is OK here
    e.value = value;
    e.lastChangeTime = NowSeconds();
    e.validIn = validIn;
    e.data = data;

    return value;
}

uint32 RandomPlayerbotMgr::GetValue(uint32 bot, std::string const& type) { return GetEventValue(bot, type); }

uint32 RandomPlayerbotMgr::GetValue(Player* bot, std::string const& type)
{
    return GetValue(bot->GetGUID().GetCounter(), type);
}

std::string RandomPlayerbotMgr::GetData(uint32 bot, std::string const& type) { return GetEventData(bot, type); }

void RandomPlayerbotMgr::SetValue(uint32 bot, std::string const& type, uint32 value, std::string const& data)
{
    SetEventValue(bot, type, value, sPlayerbotAIConfig.maxRandomBotInWorldTime, data);
}

void RandomPlayerbotMgr::SetValue(Player* bot, std::string const& type, uint32 value, std::string const& data)
{
    SetValue(bot->GetGUID().GetCounter(), type, value, data);
}

bool RandomPlayerbotMgr::HandlePlayerbotConsoleCommand(ChatHandler* /*handler*/, char const* args)
{
    if (!sPlayerbotAIConfig.enabled)
    {
        LOG_ERROR("playerbots", "Playerbots system is currently disabled!");
        return false;
    }

    if (!args || !*args)
    {
        LOG_ERROR("playerbots", "Usage: rndbot stats/update/reset/init/refresh/add/remove");
        return false;
    }

    std::string const cmd = args;

    if (cmd == "reset")
    {
        PlayerbotsDatabase.Execute(PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_DEL_RANDOM_BOTS));
        sRandomPlayerbotMgr.eventCache.clear();
        LOG_INFO("playerbots", "Random bots were reset for all players. Please restart the Server.");
        return true;
    }

    if (cmd == "stats")
    {
        sRandomPlayerbotMgr.PrintStats();
        // activatePrintStatsThread();
        return true;
    }

    if (cmd == "reload")
    {
        sPlayerbotAIConfig.Initialize();
        return true;
    }

    if (cmd == "update")
    {
        sRandomPlayerbotMgr.UpdateAIInternal(0);
        return true;
    }

    std::map<std::string, ConsoleCommandHandler> handlers;
    // handlers["initmin"] = &RandomPlayerbotMgr::RandomizeMin;
    handlers["init"] = &RandomPlayerbotMgr::RandomizeFirst;
    handlers["clear"] = &RandomPlayerbotMgr::Clear;
    handlers["levelup"] = handlers["level"] = &RandomPlayerbotMgr::IncreaseLevel;
    handlers["refresh"] = &RandomPlayerbotMgr::Refresh;
    handlers["teleport"] = &RandomPlayerbotMgr::RandomTeleportForLevel;
    // handlers["rpg"] = &RandomPlayerbotMgr::RandomTeleportForRpg;
    handlers["revive"] = &RandomPlayerbotMgr::Revive;
    handlers["grind"] = &RandomPlayerbotMgr::RandomTeleport;
    handlers["change_strategy"] = &RandomPlayerbotMgr::ChangeStrategy;

    for (std::map<std::string, ConsoleCommandHandler>::iterator j = handlers.begin(); j != handlers.end(); ++j)
    {
        std::string const prefix = j->first;
        if (cmd.find(prefix) != 0)
            continue;

        std::string const name = cmd.size() > prefix.size() + 1 ? cmd.substr(1 + prefix.size()) : "%";

        std::vector<uint32> botIds;
        for (std::vector<uint32>::iterator i = sPlayerbotAIConfig.randomBotAccounts.begin();
             i != sPlayerbotAIConfig.randomBotAccounts.end(); ++i)
        {
            uint32 account = *i;
            if (QueryResult results = CharacterDatabase.Query(
                    "SELECT guid FROM characters WHERE account = {} AND name like '{}'", account, name.c_str()))
            {
                do
                {
                    Field* fields = results->Fetch();

                    uint32 botId = fields[0].Get<uint32>();
                    ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(botId);
                    if (!sRandomPlayerbotMgr.IsRandomBot(guid.GetCounter()))
                    {
                        continue;
                    }
                    Player* bot = ObjectAccessor::FindPlayer(guid);
                    if (!bot)
                        continue;

                    botIds.push_back(botId);
                } while (results->NextRow());
            }
        }

        if (botIds.empty())
        {
            LOG_INFO("playerbots", "Nothing to do");
            return false;
        }

        uint32 processed = 0;
        for (std::vector<uint32>::iterator i = botIds.begin(); i != botIds.end(); ++i)
        {
            ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(*i);
            Player* bot = ObjectAccessor::FindPlayer(guid);
            if (!bot)
                continue;

            LOG_INFO("playerbots", "[{}/{}] Processing command {} for bot {}", processed++, botIds.size(), cmd.c_str(),
                     bot->GetName().c_str());

            ConsoleCommandHandler handler = j->second;
            (sRandomPlayerbotMgr.*handler)(bot);
        }

        return true;
    }

    // std::vector<std::string> messages = sRandomPlayerbotMgr.HandlePlayerbotCommand(args);
    // for (std::vector<std::string>::iterator i = messages.begin(); i != messages.end(); ++i)
    // {
    //     LOG_INFO("playerbots", "{}", i->c_str());
    // }
    return true;
}

void RandomPlayerbotMgr::HandleCommand(uint32 type, std::string const text, Player* fromPlayer, std::string channelName)
{
    for (PlayerBotMap::const_iterator it = GetPlayerBotsBegin(); it != GetPlayerBotsEnd(); ++it)
    {
        Player* const bot = it->second;
        if (!bot)
            continue;

        if (!channelName.empty())
        {
            if (ChannelMgr* cMgr = ChannelMgr::forTeam(bot->GetTeamId()))
            {
                Channel* chn = cMgr->GetChannel(channelName, bot);
                if (!chn)
                    continue;
            }
        }

        if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
            botAI->HandleCommand(type, text, fromPlayer);
    }
}

void RandomPlayerbotMgr::OnPlayerLogout(Player* player)
{
    DisablePlayerBot(player->GetGUID());

    for (PlayerBotMap::const_iterator it = GetPlayerBotsBegin(); it != GetPlayerBotsEnd(); ++it)
    {
        Player* const bot = it->second;
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (botAI && player == botAI->GetMaster())
        {
            botAI->SetMaster(nullptr);
            if (!bot->InBattleground())
            {
                botAI->ResetStrategies();
            }
        }
    }

    std::vector<Player*>::iterator i = std::find(players.begin(), players.end(), player);
    if (i != players.end())
        players.erase(i);
}

void RandomPlayerbotMgr::OnBotLoginInternal(Player* const bot)
{
    if (_isBotLogging)
    {
        LOG_INFO("playerbots", "{}/{} Bot {} logged in", playerBots.size(),
                 sRandomPlayerbotMgr.GetMaxAllowedBotCount(), bot->GetName().c_str());

        if (playerBots.size() == sRandomPlayerbotMgr.GetMaxAllowedBotCount())
        {
            _isBotLogging = false;
        }
    }

    // Run guild recovery/assignment at login to handle empty guild tables after restart.
    if (sPlayerbotAIConfig.randomBotGuildCount > 0)
    {
        PlayerbotFactory factory(bot, bot->GetLevel());
        factory.InitGuild();
    }

    RandomPlayerbotFactory::AssignBotToArenaTeam(bot);

    if (sPlayerbotAIConfig.randomBotFixedLevel)
    {
        bot->SetPlayerFlag(PLAYER_FLAGS_NO_XP_GAIN);
    }
    else
    {
        bot->RemovePlayerFlag(PLAYER_FLAGS_NO_XP_GAIN);
    }

    auto qitr = queueBurstBots.find(bot->GetGUID().GetCounter());

    if (qitr != queueBurstBots.end() && qitr->second.targetLevel &&
        bot->GetLevel() != qitr->second.targetLevel)
    {
        uint8 targetLevel = qitr->second.targetLevel;

        SetValue(bot->GetGUID().GetCounter(), "level", targetLevel);

        PlayerbotFactory levelFactory(bot, targetLevel);
        levelFactory.Randomize(false);

        if (PlayerbotAI* ai = GET_PLAYERBOT_AI(bot))
            ai->Reset();
    }

    // A LFG burst reservation carries an explicit role. Force the primary
    // talent tab so LfgJoinAction::GetRoles() and the combat AI agree on the
    // requested tank/healer/DPS role.
    if (qitr != queueBurstBots.end() && qitr->second.lfg && qitr->second.role != lfg::PLAYER_ROLE_NONE)
    {
        uint8 specTab = GetSpecTabForQueueRole(bot->getClass(), qitr->second.role);
        if (specTab != 255)
        {
            uint32 specNo = sPlayerbotAIConfig.randomClassSpecIndex[bot->getClass()][specTab];

            PlayerbotFactory::InitTalentsBySpecNo(bot, specNo, true);

            PlayerbotFactory factory(bot, bot->GetLevel());
            factory.InitEquipment(false, true);
            factory.InitGlyphs(false);
            bot->SendTalentsInfoData(false);

            if (PlayerbotAI* ai = GET_PLAYERBOT_AI(bot))
                ai->Reset();
        }
    }

    if (qitr != queueBurstBots.end() && qitr->second.lfg)
        QueueBurstBotLfgNow(bot, qitr->second);
}

void RandomPlayerbotMgr::OnPlayerLogin(Player* player)
{
    uint32 botsNearby = 0;

    for (PlayerBotMap::const_iterator it = GetPlayerBotsBegin(); it != GetPlayerBotsEnd(); ++it)
    {
        Player* const bot = it->second;
        if (player == bot /* || GET_PLAYERBOT_AI(player)*/)  // TEST
            continue;

        Cell playerCell(player->GetPositionX(), player->GetPositionY());
        Cell botCell(bot->GetPositionX(), bot->GetPositionY());

        // if (playerCell == botCell)
        // botsNearby++;

        Group* group = bot->GetGroup();
        if (!group)
            continue;

        for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
        {
            Player* member = gref->GetSource();
            PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
            if (botAI && member == player && (!botAI->GetMaster() || GET_PLAYERBOT_AI(botAI->GetMaster())))
            {
                if (!bot->InBattleground())
                {
                    botAI->SetMaster(player);
                    botAI->ResetStrategies();
                    botAI->TellMaster(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                        "hello", "Hello", {}));
                }

                break;
            }
        }
    }

    if (botsNearby > 100 && false)
    {
        WorldPosition botPos(player);

        // botPos.GetReachableRandomPointOnGround(player, sPlayerbotAIConfig.reactDistance * 2, true);

        // player->TeleportTo(botPos);
        // player->Relocate(botPos.coord_x, botPos.coord_y, botPos.coord_z, botPos.orientation);

        if (!player->GetFactionTemplateEntry())
        {
            botPos.GetReachableRandomPointOnGround(player, sPlayerbotAIConfig.reactDistance * 2, true);
        }
        else
        {
            std::vector<TravelDestination*> dests = TravelMgr::instance().getRpgTravelDestinations(player, true, true, 200000.0f);

            do
            {
                RpgTravelDestination* dest = (RpgTravelDestination*)dests[urand(0, dests.size() - 1)];
                CreatureTemplate const* cInfo = dest->GetCreatureTemplate();
                if (!cInfo)
                    continue;

                FactionTemplateEntry const* factionEntry = sFactionTemplateStore.LookupEntry(cInfo->faction);
                ReputationRank reaction = Unit::GetFactionReactionTo(player->GetFactionTemplateEntry(), factionEntry);

                if (reaction > REP_NEUTRAL && dest->nearestPoint(&botPos)->GetMapId() == player->GetMapId())
                {
                    botPos = *dest->nearestPoint(&botPos);
                    break;
                }
            } while (true);
        }

        player->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
        player->TeleportTo(botPos);

        // player->Relocate(botPos.getX(), botPos.getY(), botPos.getZ(), botPos.getO());
    }

    if (IsRandomBot(player))
    {
        // ObjectGuid::LowType guid = player->GetGUID().GetCounter(); //not used, conditional could be rewritten for
        // simplicity. line marked for removal.
        player->SetPvP(sWorld->IsPvPRealm());
    }
    else
    {
        players.push_back(player);
        LOG_DEBUG("playerbots", "Including non-random bot player {} into random bot update", player->GetName().c_str());
    }
}

void RandomPlayerbotMgr::OnPlayerLoginError(uint32 bot)
{
    SetEventValue(bot, "add", 0, 0);
    currentBots.erase(bot);
    queueBurstBots.erase(bot);
}

Player* RandomPlayerbotMgr::GetRandomPlayer()
{
    if (players.empty())
        return nullptr;

    uint32 index = urand(0, players.size() - 1);
    return players[index];
}

void RandomPlayerbotMgr::PrintStats()
{
    printStatsTimer = time(nullptr);
    LOG_INFO("playerbots", "Random Bots Stats: {} online", playerBots.size());

    std::map<uint8, uint32> alliance, horde;
    for (uint32 i = 0; i < 10; ++i)
    {
        alliance[i] = 0;
        horde[i] = 0;
    }

    std::map<uint8, uint32> perRace;
    std::map<uint8, uint32> perClass;

    std::map<uint8, uint32> lvlPerRace;
    std::map<uint8, uint32> lvlPerClass;
    for (uint8 race = RACE_HUMAN; race < sRaceMgr->GetMaxRaces(); ++race)
    {
        perRace[race] = 0;
        lvlPerRace[race] = 0;
    }

    for (uint8 cls = CLASS_WARRIOR; cls < MAX_CLASSES; ++cls)
    {
        perClass[cls] = 0;
        lvlPerClass[cls] = 0;
    }

    uint32 dps = 0;
    uint32 heal = 0;
    uint32 tank = 0;
    uint32 active = 0;
/*    uint32 update = 0;
    uint32 randomize = 0;
    uint32 teleport = 0;
    uint32 changeStrategy = 0;*/
    uint32 dead = 0;
    uint32 combat = 0;
    // uint32 revive = 0; //not used, line marked for removal.
    uint32 inFlight = 0;
    uint32 moving = 0;
    uint32 mounted = 0;
    uint32 inBg = 0;
    uint32 rest = 0;
    uint32 engine_noncombat = 0;
    uint32 engine_combat = 0;
    uint32 engine_dead = 0;
    std::unordered_map<NewRpgStatus, int> rpgStatusCount;
    // static NewRpgStatistic rpgStasticTotal;
    std::unordered_map<uint32, int> zoneCount;
    uint8 maxBotLevel = 0;
    for (PlayerBotMap::iterator i = playerBots.begin(); i != playerBots.end(); ++i)
    {
        Player* bot = i->second;
        if (IsAlliance(bot->getRace()))
            ++alliance[bot->GetLevel()];
        else
            ++horde[bot->GetLevel()];
        maxBotLevel = std::max(maxBotLevel, bot->GetLevel());

        ++perRace[bot->getRace()];
        ++perClass[bot->getClass()];

        lvlPerClass[bot->getClass()] += bot->GetLevel();
        lvlPerRace[bot->getRace()] += bot->GetLevel();

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (!botAI)
        {
            LOG_ERROR("playerbots", "Player/Bot {} is registered in sRandomPlayerbotMgr playerBots and has no bot AI!", bot->GetName().c_str());
            continue;
        }

        if (botAI->IsActivityAllowedCached())
            ++active;
        /* TODO: Review statistics on rpg merge
        if (botAI->GetAiObjectContext()->GetValue<bool>("random bot update")->Get())
            ++update;

        uint32 botId = bot->GetGUID().GetCounter();
        if (!GetEventValue(botId, "randomize"))
            ++randomize;

        if (!GetEventValue(botId, "teleport"))
            ++teleport;

        if (!GetEventValue(botId, "change_strategy"))
            ++changeStrategy;
        */
        if (bot->isDead())
        {
            ++dead;
            // if (!GetEventValue(botId, "dead"))
            //++revive;
        }
        if (bot->IsInCombat())
            ++combat;

        if (bot->isMoving())
            ++moving;

        if (bot->IsInFlight())
            ++inFlight;

        if (bot->IsMounted())
            ++mounted;

        if (bot->InBattleground() || bot->InArena())
            ++inBg;

        if (bot->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_RESTING))
            ++rest;

        if (botAI->GetState() == BOT_STATE_NON_COMBAT)
            ++engine_noncombat;

        else if (botAI->GetState() == BOT_STATE_COMBAT)
            ++engine_combat;

        else
            ++engine_dead;

        if (botAI->IsHeal(bot, false))
            ++heal;

        else if (botAI->IsTank(bot, false))
            ++tank;

        else
            ++dps;

        zoneCount[bot->GetZoneId()]++;

        if (sPlayerbotAIConfig.enableNewRpgStrategy)
        {
            rpgStatusCount[botAI->rpgInfo.GetStatus()]++;
            rpgStasticTotal += botAI->rpgStatistic;
            botAI->rpgStatistic = NewRpgStatistic();
        }
    }

    LOG_INFO("playerbots", "Bots level:");
    // uint32 maxLevel = sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL);
    uint32_t currentAlliance = 0, currentHorde = 0;
    uint32_t step = std::max(1, static_cast<int>((maxBotLevel + 4) / 8));
    uint32_t from = 1;

    for (uint8 i = 1; i <= maxBotLevel; ++i)
    {
        currentAlliance += alliance[i];
        currentHorde += horde[i];

        if (((i + 1) % step == 0) || i == maxBotLevel)
        {
            if (currentAlliance || currentHorde)
                LOG_INFO("playerbots", "    {}..{}: {} alliance, {} horde", from, i, currentAlliance, currentHorde);
            currentAlliance = 0;
            currentHorde = 0;
            from = i + 1;
        }
    }

    LOG_INFO("playerbots", "Bots race:");
    for (uint8 race = RACE_HUMAN; race < sRaceMgr->GetMaxRaces(); ++race)
    {
        if (perRace[race])
        {
            uint32 lvl = lvlPerRace[race] * 10 / perRace[race];
            float flvl = lvl / 10.0f;
            LOG_INFO("playerbots", "    {}: {}, avg lvl: {}", ChatHelper::FormatRace(race).c_str(), perRace[race],
                     flvl);
        }
    }

    LOG_INFO("playerbots", "Bots class:");
    for (uint8 cls = CLASS_WARRIOR; cls < MAX_CLASSES; ++cls)
    {
        if (perClass[cls])
        {
            uint32 lvl = lvlPerClass[cls] * 10 / perClass[cls];
            float flvl = lvl / 10.0f;
            LOG_INFO("playerbots", "    {}: {}, avg lvl: {}", ChatHelper::FormatClass(cls).c_str(), perClass[cls],
                     flvl);
        }
    }

    LOG_INFO("playerbots", "Bots role:");
    LOG_INFO("playerbots", "    tank: {}, heal: {}, dps: {}", tank, heal, dps);

    LOG_INFO("playerbots", "Bots status:");
    LOG_INFO("playerbots", "    Active: {}", active);
    LOG_INFO("playerbots", "    Moving: {}", moving);

    // LOG_INFO("playerbots", "Bots to:");
    // LOG_INFO("playerbots", "    update: {}", update);
    // LOG_INFO("playerbots", "    randomize: {}", randomize);
    // LOG_INFO("playerbots", "    teleport: {}", teleport);
    // LOG_INFO("playerbots", "    change_strategy: {}", changeStrategy);
    // LOG_INFO("playerbots", "    revive: {}", revive);

    LOG_INFO("playerbots", "    In flight: {}", inFlight);
    LOG_INFO("playerbots", "    On mount: {}", mounted);
    LOG_INFO("playerbots", "    In combat: {}", combat);
    LOG_INFO("playerbots", "    In BG: {}", inBg);
    LOG_INFO("playerbots", "    In Rest: {}", rest);
    LOG_INFO("playerbots", "    Dead: {}", dead);

    if (sPlayerbotAIConfig.enableNewRpgStrategy)
    {
        LOG_INFO("playerbots", "Bots rpg status:");
        LOG_INFO("playerbots",
                 "    Idle: {}, Rest: {}, GoGrind: {}, GoCamp: {}, MoveRandom: {}, MoveNpc: {}, DoQuest: {}, "
                 "TravelFlight: {}, OutdoorPvP: {}",
                 rpgStatusCount[RPG_IDLE], rpgStatusCount[RPG_REST], rpgStatusCount[RPG_GO_GRIND],
                 rpgStatusCount[RPG_GO_CAMP], rpgStatusCount[RPG_WANDER_RANDOM], rpgStatusCount[RPG_WANDER_NPC],
                 rpgStatusCount[RPG_DO_QUEST], rpgStatusCount[RPG_TRAVEL_FLIGHT], rpgStatusCount[RPG_OUTDOOR_PVP]);

        LOG_INFO("playerbots", "Bots total quests:");
        LOG_INFO("playerbots", "    Accepted: {}, Rewarded: {}, Dropped: {}", rpgStasticTotal.questAccepted,
                 rpgStasticTotal.questRewarded, rpgStasticTotal.questDropped);
    }

    LOG_INFO("playerbots", "Bots engine:", dead);
    LOG_INFO("playerbots", "    Non-combat: {}, Combat: {}, Dead: {}", engine_noncombat, engine_combat, engine_dead);
}

double RandomPlayerbotMgr::GetBuyMultiplier(Player* bot)
{
    uint32 id = bot->GetGUID().GetCounter();
    uint32 value = GetEventValue(id, "buymultiplier");
    if (!value)
    {
        value = urand(50, 120);
        uint32 validIn = urand(sPlayerbotAIConfig.minRandomBotsPriceChangeInterval,
                               sPlayerbotAIConfig.maxRandomBotsPriceChangeInterval);
        SetEventValue(id, "buymultiplier", value, validIn);
    }

    return (double)value / 100.0;
}

double RandomPlayerbotMgr::GetSellMultiplier(Player* bot)
{
    uint32 id = bot->GetGUID().GetCounter();
    uint32 value = GetEventValue(id, "sellmultiplier");
    if (!value)
    {
        value = urand(80, 250);
        uint32 validIn = urand(sPlayerbotAIConfig.minRandomBotsPriceChangeInterval,
                               sPlayerbotAIConfig.maxRandomBotsPriceChangeInterval);
        SetEventValue(id, "sellmultiplier", value, validIn);
    }

    return (double)value / 100.0;
}

void RandomPlayerbotMgr::AddTradeDiscount(Player* bot, Player* master, int32 value)
{
    if (!master)
        return;

    uint32 discount = GetTradeDiscount(bot, master);
    int32 result = (int32)discount + value;
    discount = (result < 0 ? 0 : result);

    SetTradeDiscount(bot, master, discount);
}

void RandomPlayerbotMgr::SetTradeDiscount(Player* bot, Player* master, uint32 value)
{
    if (!master)
        return;

    uint32 botId = bot->GetGUID().GetCounter();
    uint32 masterId = master->GetGUID().GetCounter();

    std::ostringstream name;
    name << "trade_discount_" << masterId;
    SetEventValue(botId, name.str(), value, sPlayerbotAIConfig.maxRandomBotInWorldTime);
}

uint32 RandomPlayerbotMgr::GetTradeDiscount(Player* bot, Player* master)
{
    if (!master)
        return 0;

    uint32 botId = bot->GetGUID().GetCounter();
    uint32 masterId = master->GetGUID().GetCounter();

    std::ostringstream name;
    name << "trade_discount_" << masterId;
    return GetEventValue(botId, name.str());
}

std::string const RandomPlayerbotMgr::HandleRemoteCommand(std::string const request)
{
    std::string::const_iterator pos = std::find(request.begin(), request.end(), ',');
    if (pos == request.end())
    {
        std::ostringstream out;
        out << "invalid request: " << request;
        return out.str();
    }

    std::string const command = std::string(request.begin(), pos);
    ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(atoi(std::string(pos + 1, request.end()).c_str()));
    Player* bot = GetPlayerBot(guid);
    if (!bot)
        return "invalid guid";

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return "invalid guid";

    return botAI->HandleRemoteCommand(command);
}

void RandomPlayerbotMgr::ChangeStrategy(Player* player)
{
    uint32 bot = player->GetGUID().GetCounter();

    if (frand(0.f, 100.f) > sPlayerbotAIConfig.randomBotRpgChance)
    {
        LOG_INFO("playerbots", "Bot #{} <{}>: sent to grind spot", bot, player->GetName().c_str());
        ScheduleTeleport(bot, 30);
    }
    else
    {
        LOG_INFO("playerbots", "Changing strategy for bot #{} <{}> to RPG", bot, player->GetName().c_str());
        LOG_INFO("playerbots", "Bot #{} <{}>: sent to inn", bot, player->GetName().c_str());
        RandomTeleportForLevel(player);
        SetEventValue(bot, "teleport", 1, sPlayerbotAIConfig.maxRandomBotInWorldTime);
    }

    ScheduleChangeStrategy(bot);
}

void RandomPlayerbotMgr::ChangeStrategyOnce(Player* player)
{
    uint32 bot = player->GetGUID().GetCounter();

    if (frand(0.f, 100.f) > sPlayerbotAIConfig.randomBotRpgChance)  // select grind / pvp
    {
        LOG_INFO("playerbots", "Bot #{} <{}>: sent to grind spot", bot, player->GetName().c_str());
        RandomTeleportForLevel(player);
        Refresh(player);
    }
    else
    {
        LOG_INFO("playerbots", "Bot #{} <{}>: sent to inn", bot, player->GetName().c_str());
        RandomTeleportForLevel(player);
    }
}

void RandomPlayerbotMgr::RandomTeleportForRpg(Player* bot)
{
    uint32 race = bot->getRace();
    uint32 level = bot->GetLevel();
    LOG_DEBUG("playerbots", "Random teleporting bot {} for RPG ({} locations available)", bot->GetName().c_str(),
              rpgLocsCacheLevel[race].size());
    RandomTeleport(bot, rpgLocsCacheLevel[race][level], true);
}

void RandomPlayerbotMgr::Remove(Player* bot)
{
    ObjectGuid owner = bot->GetGUID();

    PlayerbotsDatabasePreparedStatement* stmt =
        PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_DEL_RANDOM_BOTS_BY_OWNER);
    stmt->SetData(0, 0);
    stmt->SetData(1, owner.GetCounter());
    PlayerbotsDatabase.Execute(stmt);

    uint32 botId = owner.GetCounter();
    eventCache.erase(botId);

    LogoutPlayerBot(owner);
}

CreatureData const* RandomPlayerbotMgr::GetCreatureDataByEntry(uint32 entry)
{
    if (entry != 0)
    {
        for (auto const& itr : sObjectMgr->GetAllCreatureData())
            if (itr.second.id == entry)
                return &itr.second;
    }

    return nullptr;
}

ObjectGuid RandomPlayerbotMgr::GetBattleMasterGUID(Player* bot, BattlegroundTypeId bgTypeId)
{
    ObjectGuid battleMasterGUID = ObjectGuid::Empty;

    TeamId team = bot->GetTeamId();
    std::vector<uint32> Bms;

    for (auto i = std::begin(BattleMastersCache[team][bgTypeId]); i != std::end(BattleMastersCache[team][bgTypeId]);
         ++i)
    {
        Bms.insert(Bms.end(), *i);
    }

    for (auto i = std::begin(BattleMastersCache[TEAM_NEUTRAL][bgTypeId]);
         i != std::end(BattleMastersCache[TEAM_NEUTRAL][bgTypeId]); ++i)
    {
        Bms.insert(Bms.end(), *i);
    }

    if (Bms.empty())
        return battleMasterGUID;

    float dist1 = FLT_MAX;

    for (auto i = begin(Bms); i != end(Bms); ++i)
    {
        CreatureData const* data = sRandomPlayerbotMgr.GetCreatureDataByEntry(*i);
        if (!data)
            continue;

        Unit* Bm = PlayerbotAI::GetUnit(data);
        if (!Bm)
            continue;

        if (bot->GetMapId() != Bm->GetMapId())
            continue;

        // return first available guid on map if queue from anywhere
        if (!BattlegroundMgr::IsArenaType(bgTypeId))
        {
            battleMasterGUID = Bm->GetGUID();
            break;
        }

        AreaTableEntry const* zone = sAreaTableStore.LookupEntry(Bm->GetZoneId());
        if (!zone)
            continue;

        if (zone->team == 4 && bot->GetTeamId() == TEAM_ALLIANCE)
            continue;

        if (zone->team == 2 && bot->GetTeamId() == TEAM_HORDE)
            continue;

        if (Bm->getDeathState() == DeathState::Dead)
            continue;

        float dist2 = ServerFacade::instance().GetDistance2d(bot, data->posX, data->posY);
        if (dist2 < dist1)
        {
            dist1 = dist2;
            battleMasterGUID = Bm->GetGUID();
        }
    }

    return battleMasterGUID;
}
