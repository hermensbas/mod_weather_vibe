#include "mod_wv_core.h"

#include "Chat.h"
#include "ChatCommand.h"
#include "Player.h"
#include "ScriptMgr.h"

#include <iomanip>
#include <sstream>

using Acore::ChatCommands::ChatCommandTable;
using Acore::ChatCommands::Console;

// ============================================================
// CommandScript class
// Each handler owns its validation, feedback and dispatch.
// Core provides: AssertEnabled, PushWeatherPercent, ReloadConfig,
//                GetLastApplied, GetCurrentDayPart, GetCurrentSeason,
//                and the static name helpers.
// ============================================================
class WeatherVibe_CommandScript : public CommandScript
{
public:
    WeatherVibe_CommandScript() : CommandScript("WeatherVibe_CommandScript") {}

    // ----------------------------------------------------------
    // .wvibe set <zoneId> <state:uint> <percentage:0..100>
    // ----------------------------------------------------------
    static bool HandleWvibeSet(ChatHandler* handler, uint32 zoneId, uint32 stateVal, float percentage)
    {
        if (!sWeatherVibeCore.IsEnabled())
        {
            handler->SendSysMessage("|cff00ff00WeatherVibe:|r Module is disabled in config.");
            return false;
        }

        bool ok = sWeatherVibeCore.PushWeatherPercent(zoneId, static_cast<WeatherState>(stateVal), percentage);
        if (!ok)
        {
            handler->SendSysMessage("|cff00ff00WeatherVibe:|r Failed to push weather (zone not found or no players).");
        }

        return ok;
    }

    // ----------------------------------------------------------
    // .wvibe setRaw <zoneId> <state:uint> <raw:0..1>
    // ----------------------------------------------------------
    static bool HandleWvibeSetRaw(ChatHandler* handler, uint32 zoneId, uint32 stateVal, float rawGrade)
    {
        if (!sWeatherVibeCore.IsEnabled())
        {
            handler->SendSysMessage("|cff00ff00WeatherVibe:|r Module is disabled in config.");
            return false;
        }

        bool ok = sWeatherVibeCore.PushWeatherDebug(zoneId, static_cast<WeatherState>(stateVal), rawGrade);
        if (!ok)
        {
            handler->SendSysMessage("|cff00ff00WeatherVibe:|r Failed to push weather (zone not found or no players).");
        }

        return ok;
    }

    // ----------------------------------------------------------
    // .wvibe show
    // ----------------------------------------------------------
    static bool HandleWvibeShow(ChatHandler* handler)
    {
        if (!sWeatherVibeCore.IsEnabled())
        {
            handler->SendSysMessage("|cff00ff00WeatherVibe:|r Module is disabled in config.");
            return false;
        }

        auto const& lastApplied = sWeatherVibeCore.GetLastApplied();

        if (lastApplied.empty())
        {
            handler->SendSysMessage("|cff00ff00WeatherVibe:|r No weather pushed yet. Use .wvibe set or setRaw first.");
            return true;
        }

        DayPart dp = sWeatherVibeCore.GetCurrentDayPart();
        Season  s  = sWeatherVibeCore.GetCurrentSeason();

        std::ostringstream oss;
        oss << "|cff00ff00WeatherVibe:|r show | season=" << WeatherVibeCore::SeasonName(s)
            << " | daypart=" << WeatherVibeCore::DayPartName(dp) << "\n";

        for (auto const& kv : lastApplied)
        {
            LastApplied const& la = kv.second;
            oss << "zone " << kv.first
                << " -> state=" << WeatherVibeCore::WeatherStateName(la.state)
                << " raw="      << std::fixed << std::setprecision(2) << la.grade
                << "\n";
        }

        handler->SendSysMessage(oss.str().c_str());
        return true;
    }

    // ----------------------------------------------------------
    // .wvibe reload
    // ----------------------------------------------------------
    static bool HandleWvibeReload(ChatHandler* handler)
    {
        if (!sWeatherVibeCore.IsEnabled())
        {
            handler->SendSysMessage("|cff00ff00WeatherVibe:|r Module is disabled in config.");
            return false;
        }

        sWeatherVibeCore.ReloadConfig();

        handler->SendSysMessage("|cff00ff00WeatherVibe:|r Reloaded (daypart config + intensity ranges).");
        return true;
    }

    // ----------------------------------------------------------
    // .wvibe where
    // ----------------------------------------------------------
    static bool HandleWvibeWhere(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();
        if (!player)
        {
            handler->SendSysMessage("|cff00ff00WeatherVibe:|r No player found (use in-game only).");
            return false;
        }

        std::ostringstream oss;
        oss << "|cff00ff00WeatherVibe:|r where | zoneId=" << player->GetZoneId();
        handler->SendSysMessage(oss.str().c_str());
        return true;
    }

    // ----------------------------------------------------------
    // .wvibe help
    // ----------------------------------------------------------
    static bool HandleWvibeHelp(ChatHandler* handler)
    {
        handler->SendSysMessage("|cff00ff00WeatherVibe:|r commands:");
        handler->SendSysMessage("  .wvibe set [zoneId] [state] [pct:0..100]");
        handler->SendSysMessage("  .wvibe setRaw [zoneId] [state] [raw:0..1]");
        handler->SendSysMessage("  .wvibe where (shows current zoneId)");
        handler->SendSysMessage("  .wvibe show (weather per zone overview)");
        handler->SendSysMessage("  .wvibe reload (reload config)");
        handler->SendSysMessage("|cff00ff00WeatherVibe:|r States:");
        handler->SendSysMessage("0 Fine | 1 Fog | 3 LightRain | 4 MediumRain | 5 HeavyRain");
        handler->SendSysMessage("6 LightSnow | 7 MediumSnow | 8 HeavySnow");
        handler->SendSysMessage("22 LightSand | 41 MediumSand | 42 HeavySand | 86 Thunder");
        return true;
    }

    // ----------------------------------------------------------
    // Command table
    // ----------------------------------------------------------
    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable wvibeTable =
        {
            { "set",    HandleWvibeSet,    SEC_ADMINISTRATOR, Console::Yes },
            { "setRaw", HandleWvibeSetRaw, SEC_ADMINISTRATOR, Console::Yes },
            { "show",   HandleWvibeShow,   SEC_ADMINISTRATOR, Console::Yes },
            { "reload", HandleWvibeReload, SEC_ADMINISTRATOR, Console::Yes },
            { "where",  HandleWvibeWhere,  SEC_ADMINISTRATOR, Console::Yes },
            { "help",   HandleWvibeHelp,   SEC_ADMINISTRATOR, Console::Yes },
        };
        static ChatCommandTable root =
        {
            { "wvibe", wvibeTable }
        };
        return root;
    }
};

// ============================================================
// Registration — called from Addmod_weather_vibeScripts()
// ============================================================
void RegisterWeatherVibeCommands()
{
    new WeatherVibe_CommandScript();
}
