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
// Free command helpers
// ============================================================

// .wvibe set <zoneId> <state:uint> <percentage:0..100>
static bool HandleCommandPercent(ChatHandler* handler, uint32 zoneId, uint32 stateVal, float percentage)
{
    if (!sWeatherVibeCore.IsEnabled())
    {
        handler->SendSysMessage("|cff00ff00WeatherVibe:|r Module is disabled in config.");
        return false;
    }

    // Validate state via core helper (re-exposed through a thin check — see note below)
    // We rely on the core's push returning false for invalid states rather than duplicating
    // the validation table here; however, we do a direct guard to give a useful error first.
    // The accepted values are the same WeatherState enumerators documented in the header.
    // Forward to core:
    float pct01 = std::clamp(percentage, 0.0f, 100.0f) / 100.0f;
    DayPart dp  = sWeatherVibeCore.GetCurrentDayPart();
    float raw;

    if (stateVal == WEATHER_STATE_FINE)
    {
        raw = (1.0f - pct01) * 0.30f;
    }
    else
    {
        raw = sWeatherVibeCore.MapPercentToRawGrade(dp, static_cast<WeatherState>(stateVal), pct01);
    }

    return sWeatherVibeCore.PushWeatherToClient(zoneId, static_cast<WeatherState>(stateVal), raw);
}

// .wvibe setRaw <zoneId> <state:uint> <raw:0..1>
static bool HandleCommandRaw(ChatHandler* handler, uint32 zoneId, uint32 stateVal, float grade)
{
    if (!sWeatherVibeCore.IsEnabled())
    {
        handler->SendSysMessage("|cff00ff00WeatherVibe:|r Module is disabled in config.");
        return false;
    }

    float raw = std::clamp(grade, 0.0f, 1.0f);
    return sWeatherVibeCore.PushWeatherToClient(zoneId, static_cast<WeatherState>(stateVal), raw);
}

// ============================================================
// CommandScript class
// ============================================================
class WeatherVibe_CommandScript : public CommandScript
{
public:
    WeatherVibe_CommandScript() : CommandScript("WeatherVibe_CommandScript") {}

    // ----------------------------------------------------------
    // .wvibe reload
    // ----------------------------------------------------------
    static bool HandleWvibeReload(ChatHandler* handler)
    {
        if (!sWeatherVibeCore.IsEnabled())
        {
            handler->SendSysMessage("|cff00ff00WeatherVibe:|r Is disabled (WeatherVibe.Enable = 0).");
            return false;
        }

        sWeatherVibeCore.LoadDayPartConfig();
        sWeatherVibeCore.LoadStateRanges();

        handler->SendSysMessage("|cff00ff00WeatherVibe:|r Reloaded (per-state ranges/dayparts).");
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
        handler->SendSysMessage("  .wvibe where");
        handler->SendSysMessage("  .wvibe show");
        handler->SendSysMessage("  .wvibe reload");
        handler->SendSysMessage("States: 0 Fine | 1 Fog | 3 LRain | 4 MRain | 5 HRain | 6 LSnow | 7 MSnow | 8 HSnow | 22 LSand | 41 MSand | 42 HSand | 86 Thunder");
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
    // .wvibe show
    // ----------------------------------------------------------
    static bool HandleWvibeShow(ChatHandler* handler)
    {
        if (!sWeatherVibeCore.IsEnabled())
        {
            handler->SendSysMessage("|cff00ff00WeatherVibe:|r Is disabled (WeatherVibe.Enable = 0).");
            return false;
        }

        auto const& lastApplied = sWeatherVibeCore.GetLastApplied();

        if (lastApplied.empty())
        {
            handler->SendSysMessage("|cff00ff00WeatherVibe:|r No last-applied weather recorded yet. Use .wvibe set or setRaw to push weather.");
            return true;
        }

        DayPart dp = sWeatherVibeCore.GetCurrentDayPart();
        Season  s  = sWeatherVibeCore.GetCurrentSeason();

        std::ostringstream oss;
        oss << "|cff00ff00WeatherVibe:|r show | season=" << WeatherVibeCore::SeasonName(s)
            << " | daypart=" << WeatherVibeCore::DayPartName(dp) << "\n";

        for (auto const& kv : lastApplied)
        {
            uint32           zoneId = kv.first;
            LastApplied const& la   = kv.second;

            oss << "zone " << zoneId
                << " -> last state=" << WeatherVibeCore::WeatherStateName(la.state)
                << " raw=" << std::fixed << std::setprecision(2) << la.grade
                << (la.hasValue ? "" : " (unset)")
                << "\n";
        }

        handler->SendSysMessage(oss.str().c_str());
        return true;
    }

    // ----------------------------------------------------------
    // .wvibe set / setRaw
    // ----------------------------------------------------------
    static bool HandleWvibeSet(ChatHandler* handler, uint32 zoneId, uint32 stateVal, float percentage)
    {
        return HandleCommandPercent(handler, zoneId, stateVal, percentage);
    }

    static bool HandleWvibeSetRaw(ChatHandler* handler, uint32 zoneId, uint32 stateVal, float rawGrade)
    {
        return HandleCommandRaw(handler, zoneId, stateVal, rawGrade);
    }

    // ----------------------------------------------------------
    // Command table
    // ----------------------------------------------------------
    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable wvibeSet =
        {
            { "set",    HandleWvibeSet,    SEC_ADMINISTRATOR, Console::Yes },
            { "setRaw", HandleWvibeSetRaw, SEC_ADMINISTRATOR, Console::Yes },
            { "reload", HandleWvibeReload, SEC_ADMINISTRATOR, Console::Yes },
            { "where",  HandleWvibeWhere,  SEC_ADMINISTRATOR, Console::Yes },
            { "show",   HandleWvibeShow,   SEC_ADMINISTRATOR, Console::Yes },
            { "help",   HandleWvibeHelp,   SEC_ADMINISTRATOR, Console::Yes },
        };
        static ChatCommandTable root =
        {
            { "wvibe", wvibeSet }
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
