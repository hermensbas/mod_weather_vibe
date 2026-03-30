// mod_weather_vibe.cpp — v4.4 (brace-enforced style)
// Packet mode (no playbooks/windows). Uses WeatherState + direct packet push (WorldPackets::Misc::Weather).
// Resends last-applied on login/zone-change. No WeatherMgr / Weather objects.

#include "ScriptMgr.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "Config.h"
#include "MapMgr.h"
#include "Player.h"
#include "World.h"
#include "WorldSession.h"
#include "Log.h"
#include "GameTime.h"
#include "MiscPackets.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <iomanip>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <ctime>
#include <cmath>
#include <array>
#include <vector>
#include <random>

using Acore::ChatCommands::ChatCommandTable;
using Acore::ChatCommands::Console;

// ===============================
// constants, enums, structs (top)
// ===============================
namespace
{
    constexpr float kMinGrade = 0.0001f;
    constexpr float kMaxGrade = 0.9999f;

    // Season awareness (auto-derived or forced via config)
    enum class Season : uint8
    {
        SPRING = 0,
        SUMMER,
        AUTUMN,
        WINTER
    };

    // DayPart awareness (auto-derived or forced via config)
    enum class DayPart : uint8
    {
        MORNING = 0,
        AFTERNOON,
        EVENING,
        NIGHT,
        COUNT
    };


    struct Range { float min = 0.0f; float max = 1.0f; };

    struct DayPartStarts
    {
        int morning = 6 * 60;     // 06:00
        int afternoon = 12 * 60;  // 12:00
        int evening = 18 * 60;    // 18:00
        int night = 22 * 60;      // 22:00
    };

    struct LastApplied
    {
        WeatherState state = WEATHER_STATE_FINE;
        float grade = 0.f;
        bool hasValue = false; // anti-spam
    };

    // engine globals
    bool   g_EnableModule = true;
    bool   g_Debug = false;
    DayPartStarts g_Starts;
    std::string g_DayPartMode = "auto";
    std::string g_SeasonMode = "auto";

    // Per-daypart per-WeatherState ranges (keyed by WeatherState value: 0,1,3,4,5,6,7,8,22,41,42,86)
    std::unordered_map<uint32, Range> g_StateRanges[(size_t)DayPart::COUNT];

    // per-zone last applied weather state and grade.
    std::unordered_map<uint32, LastApplied> g_LastApplied;
}

// ======================================
// Helpers (names)
// ======================================
static char const* WeatherStateName(WeatherState s)
{
    switch (s)
    {
        case WEATHER_STATE_FINE: return "fine";
        case WEATHER_STATE_FOG: return "fog";
        case WEATHER_STATE_LIGHT_RAIN: return "light_rain";
        case WEATHER_STATE_MEDIUM_RAIN: return "medium_rain";
        case WEATHER_STATE_HEAVY_RAIN: return "heavy_rain";
        case WEATHER_STATE_LIGHT_SNOW: return "light_snow";
        case WEATHER_STATE_MEDIUM_SNOW: return "medium_snow";
        case WEATHER_STATE_HEAVY_SNOW: return "heavy_snow";
        case WEATHER_STATE_LIGHT_SANDSTORM: return "light_sandstorm";
        case WEATHER_STATE_MEDIUM_SANDSTORM: return "medium_sandstorm";
        case WEATHER_STATE_HEAVY_SANDSTORM: return "heavy_sandstorm";
        case WEATHER_STATE_THUNDERS: return "thunders";
        case WEATHER_STATE_BLACKRAIN: return "blackrain";
        case WEATHER_STATE_BLACKSNOW: return "blacksnow";
        default: return "unknown";
    }
}

static char const* DayPartName(DayPart d)
{
    switch (d)
    {
        case DayPart::MORNING:   return "Morning";
        case DayPart::AFTERNOON: return "Afternoon";
        case DayPart::EVENING:   return "Evening";
        case DayPart::NIGHT:     return "Night";
        default:                 return "Unknown";
    }
}

static char const* DayPartTokenUpper(DayPart d)
{
    switch (d)
    {
        case DayPart::MORNING:   return "MORNING";
        case DayPart::AFTERNOON: return "AFTERNOON";
        case DayPart::EVENING:   return "EVENING";
        case DayPart::NIGHT:     return "NIGHT";
        default:                 return "UNKNOWN";
    }
}

static char const* ConfigStateToken(WeatherState s)
{
    switch (s)
    {
        case WEATHER_STATE_FINE:               return "Fine";
        case WEATHER_STATE_FOG:                return "Fog";
        case WEATHER_STATE_LIGHT_RAIN:         return "LightRain";
        case WEATHER_STATE_MEDIUM_RAIN:        return "MediumRain";
        case WEATHER_STATE_HEAVY_RAIN:         return "HeavyRain";
        case WEATHER_STATE_LIGHT_SNOW:         return "LightSnow";
        case WEATHER_STATE_MEDIUM_SNOW:        return "MediumSnow";
        case WEATHER_STATE_HEAVY_SNOW:         return "HeavySnow";
        case WEATHER_STATE_LIGHT_SANDSTORM:    return "LightSandstorm";
        case WEATHER_STATE_MEDIUM_SANDSTORM:   return "MediumSandstorm";
        case WEATHER_STATE_HEAVY_SANDSTORM:    return "HeavySandstorm";
        case WEATHER_STATE_THUNDERS:           return "Thunders";
        default:                               return "Unknown";
    }
}

static bool IsValidWeatherState(uint32 value)
{
    switch (value)
    {
        case WEATHER_STATE_FINE:
        case WEATHER_STATE_FOG:
        case WEATHER_STATE_LIGHT_RAIN:
        case WEATHER_STATE_MEDIUM_RAIN:
        case WEATHER_STATE_HEAVY_RAIN:
        case WEATHER_STATE_LIGHT_SNOW:
        case WEATHER_STATE_MEDIUM_SNOW:
        case WEATHER_STATE_HEAVY_SNOW:
        case WEATHER_STATE_LIGHT_SANDSTORM:
        case WEATHER_STATE_MEDIUM_SANDSTORM:
        case WEATHER_STATE_HEAVY_SANDSTORM:
        case WEATHER_STATE_THUNDERS:
            return true;
        default:
            return false;
    }
}

// Season naming helper
static char const* SeasonName(Season s)
{
    switch (s)
    {
        case Season::SPRING: return "Spring";
        case Season::SUMMER: return "Summer";
        case Season::AUTUMN: return "Autumn";
        case Season::WINTER: return "Winter";
        default:             return "Unknown";
    }
}

// ======================================
// Time helpers
// ======================================
static tm GetLocalTimeSafe()
{
    time_t now = GameTime::GetGameTime().count(); // unix seconds
    tm out{};
#if defined(_WIN32) || defined(_WIN64)
    localtime_s(&out, &now);
#else
    localtime_r(&now, &out);
#endif
    return out;
}

static int ParseHHMM(std::string const& s, int defMinutes)
{
    int h = 0, m = 0; char colon = 0;
    std::string t = s;
    t.erase(std::remove_if(t.begin(), t.end(), [](unsigned char c) { return std::isspace(c); }), t.end());

    if (std::sscanf(t.c_str(), "%d%c%d", &h, &colon, &m) == 3 && colon == ':' && h >= 0 && h < 24 && m >= 0 && m < 60)
    {
        return h * 60 + m;
    }

    if (std::sscanf(t.c_str(), "%d", &h) == 1 && h >= 0 && h < 24)
    {
        return h * 60;
    }

    return defMinutes;
}

static inline int ClampMinutes(int v)
{
    return std::clamp(v, 0, 23 * 60 + 59);
}

static void ValidateDayPartStarts()
{
    g_Starts.morning = ClampMinutes(g_Starts.morning);
    g_Starts.afternoon = std::max(ClampMinutes(g_Starts.afternoon), g_Starts.morning + 1);
    g_Starts.evening = std::max(ClampMinutes(g_Starts.evening), g_Starts.afternoon + 1);
    g_Starts.night = ClampMinutes(g_Starts.night); // wrap handled by GetCurrentDayPart()
}

// ======================================
// Intensity mapping helpers
// ======================================
static float ClampToCoreBounds(float g)
{
    if (g < 0.0f)
    {
        return kMinGrade;
    }
    if (g >= 1.0f)
    {
        return kMaxGrade;
    }

    return g;
}

static void LoadDayPartConfig()
{
    // Read modes (fallback to auto)
    g_DayPartMode = sConfigMgr->GetOption<std::string>("WeatherVibe.DayPart.Mode", "auto");
    g_SeasonMode = sConfigMgr->GetOption<std::string>("WeatherVibe.Season", "auto");

    // Only start times are configurable for boundaries
    g_Starts.morning = ParseHHMM(sConfigMgr->GetOption<std::string>("WeatherVibe.DayPart.MORNING.Start", "06:00"), 6 * 60);
    g_Starts.afternoon = ParseHHMM(sConfigMgr->GetOption<std::string>("WeatherVibe.DayPart.AFTERNOON.Start", "12:00"), 12 * 60);
    g_Starts.evening = ParseHHMM(sConfigMgr->GetOption<std::string>("WeatherVibe.DayPart.EVENING.Start", "18:00"), 18 * 60);
    g_Starts.night = ParseHHMM(sConfigMgr->GetOption<std::string>("WeatherVibe.DayPart.NIGHT.Start", "22:00"), 22 * 60);

    ValidateDayPartStarts();
}

static Range ParseRangePair(std::string const& key, Range def)
{
    std::string v = sConfigMgr->GetOption<std::string>(key, "");
    if (!v.empty())
    {
        float a = def.min, b = def.max;
        if (std::sscanf(v.c_str(), " %f , %f ", &a, &b) == 2)
        {
            if (b < a)
            {
                std::swap(a, b);
            }
            return { std::clamp(a,0.0f,1.0f), std::clamp(b,0.0f,1.0f) };
        }
    }

    return def;
}

static std::array<WeatherState, 12> const kAcceptedStates = {
    WEATHER_STATE_FINE,
    WEATHER_STATE_FOG,
    WEATHER_STATE_LIGHT_RAIN,
    WEATHER_STATE_MEDIUM_RAIN,
    WEATHER_STATE_HEAVY_RAIN,
    WEATHER_STATE_LIGHT_SNOW,
    WEATHER_STATE_MEDIUM_SNOW,
    WEATHER_STATE_HEAVY_SNOW,
    WEATHER_STATE_LIGHT_SANDSTORM,
    WEATHER_STATE_MEDIUM_SANDSTORM,
    WEATHER_STATE_HEAVY_SANDSTORM,
    WEATHER_STATE_THUNDERS
};

static void LoadStateRanges()
{
    for (size_t i = 0; i < (size_t)DayPart::COUNT; ++i)
    {
        g_StateRanges[i].clear();
    }

    auto makeKey = [](DayPart dp, WeatherState ws)
        {
            std::ostringstream oss;
            oss << "WeatherVibe.Intensity.InternalRange."
                << DayPartTokenUpper(dp) << "."
                << ConfigStateToken(ws);
            return oss.str();
        };

    // unified defaults for all states/dayparts
    Range def{ 0.30f, 1.00f };

    for (DayPart dp : { DayPart::MORNING, DayPart::AFTERNOON, DayPart::EVENING, DayPart::NIGHT })
    {
        for (WeatherState ws : kAcceptedStates)
        {
            g_StateRanges[(size_t)dp][(uint32)ws] = ParseRangePair(makeKey(dp, ws), def);
        }
    }
}

// Converts profile percent (0..1) to raw grade (per-WeatherState range)
static float MapPercentToRawGrade(DayPart dp, WeatherState state, float percent01)
{
    percent01 = std::clamp(percent01, 0.0f, 1.0f);
    auto const& table = g_StateRanges[(size_t)dp];
    auto it = table.find((uint32)state);
    Range r = (it != table.end()) ? it->second : Range{ 0.30f, 1.00f };

    return r.min + percent01 * (r.max - r.min);
}

// ======================================
// Day/Season helpers used by debug/show
// ======================================
static DayPart GetCurrentDayPart()
{
    // Honor config override if not auto
    std::string mode = g_DayPartMode;
    std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (mode == "morning")
    {
        return DayPart::MORNING;
    }
    if (mode == "afternoon")
    {
        return DayPart::AFTERNOON;
    }
    if (mode == "evening")
    {
        return DayPart::EVENING;
    }
    if (mode == "night")
    {
        return DayPart::NIGHT;
    }

    // Auto: derive by time and configured boundaries
    tm lt = GetLocalTimeSafe();
    int minutes = lt.tm_hour * 60 + lt.tm_min;

    if (minutes >= g_Starts.night || minutes < g_Starts.morning)
    {
        return DayPart::NIGHT;
    }
    if (minutes >= g_Starts.evening)
    {
        return DayPart::EVENING;
    }
    if (minutes >= g_Starts.afternoon)
    {
        return DayPart::AFTERNOON;
    }

    return DayPart::MORNING;
}

static Season GetCurrentSeason()
{
    // Honor config override if not auto
    std::string m = g_SeasonMode;
    std::transform(m.begin(), m.end(), m.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (m == "spring")
    {
        return Season::SPRING;
    }
    if (m == "summer")
    {
        return Season::SUMMER;
    }
    if (m == "autumn")
    {
        return Season::AUTUMN;
    }
    if (m == "winter")
    {
        return Season::WINTER;
    }

    // Auto: derive from day-of-year; anchor Spring around Mar 20 (~day 79)
    tm lt = GetLocalTimeSafe();
    int yday = lt.tm_yday; // 0..365
    uint32 seasonIndex = ((yday - 78 + 365) / 91) % 4; // 0:Spring,1:Summer,2:Autumn,3:Winter
    switch (seasonIndex)
    {
    default:
    case 0: return Season::SPRING;
    case 1: return Season::SUMMER;
    case 2: return Season::AUTUMN;
    case 3: return Season::WINTER;
    }
}

// ======================================
// Broadcast weather package to zone
// ======================================
static bool BroadcastZonePacket(uint32 zoneId, WorldPacket const* packet)
{
    bool delivered = false;
    sMapMgr->DoForAllMaps([&](Map* map) -> void
        {
            delivered = map->SendZoneMessage(zoneId, packet) || delivered;
        });

    return delivered;
}

// ======================================
// Broadcast text to zone
// ======================================
static void BroadcastZoneText(uint32 zoneId, char const* text)
{
    sMapMgr->DoForAllMaps([&](Map* map) -> void
        {
            map->SendZoneText(zoneId, text);
        });
}

// ======================================
// Push weather to client and registers is lastApplied cache.
// ======================================
static bool PushWeatherToClient(uint32 zoneId, WeatherState state, float rawGrade)
{
    float normalizedGrade = ClampToCoreBounds(rawGrade);
    LastApplied& lastAppliedPtr = g_LastApplied[zoneId];

    WorldPackets::Misc::Weather weatherPackage(state, normalizedGrade);
    WorldPacket const* weatherPacket = weatherPackage.Write();
    bool isApplied = BroadcastZonePacket(zoneId, weatherPacket);
    if (isApplied)
    {
        lastAppliedPtr.state = state;
        lastAppliedPtr.grade = normalizedGrade;
        lastAppliedPtr.hasValue = true;
    }

    if (g_Debug)
    {
        DayPart d = GetCurrentDayPart();
        Season s = GetCurrentSeason();
        std::ostringstream zmsg;
        zmsg << "|cff00ff00WeatherVibe:|r [DEBUG] season=" << SeasonName(s)
            << " | day=" << DayPartName(d)
            << " | state=" << WeatherStateName(state)
            << " | grade=" << std::fixed << std::setprecision(2) << normalizedGrade
            << " | pushed=" << (isApplied ? "true" : "false");

        std::string debugText = zmsg.str();
        BroadcastZoneText(zoneId, debugText.c_str());
    }

    return true; // treat as success even if no players
}

// ======================================
// Push weather to client with last applied of the zone, otherwise weatherState FINE.
// ======================================
static void PushLastAppliedWeatherToClient(uint32 zoneId, Player* player)
{
    auto it = g_LastApplied.find(zoneId);
    WeatherState state;
    float grade;

    // check is lastApplied record exist
    if (it != g_LastApplied.end() && it->second.hasValue)
    {
        state = it->second.state;
        grade = it->second.grade;
    }
    else
    {
        // if not reset to default
        state = WEATHER_STATE_FINE;
        grade = 0.0f;
    }

    WorldPackets::Misc::Weather weatherPackage(state, grade);
    player->SendDirectMessage(weatherPackage.Write());
}

// ======================================
// Commands
// ======================================
// .wvibe set <zoneId> <state:uint> <percentage:0..100>
static bool HandleCommandPercent(ChatHandler* handler, uint32 zoneId, uint32 stateVal, float percentage)
{
    if (!g_EnableModule)
    {
        handler->SendSysMessage("|cff00ff00WeatherVibe:|r Module is disabled in config.");
        return false;
    }
    if (!IsValidWeatherState(stateVal))
    {
        handler->SendSysMessage("|cff00ff00WeatherVibe:|r Invalid state. Examples: 0=Fine, 1=Fog, 3=LightRain, 4=MediumRain, 5=HeavyRain, 6=LightSnow, 7=MediumSnow, 8=HeavySnow, 22=LightSandstorm, 41=MediumSandstorm, 42=HeavySandstorm, 86=Thunders.");
        handler->SendSysMessage("Usage: .wvibe set [zoneId] [state:uint] [percentage:0..100]");
        return false;
    }

    float pct01 = std::clamp(percentage, 0.0f, 100.0f) / 100.0f;
    DayPart dp = GetCurrentDayPart();
    float raw;

    if (stateVal == WEATHER_STATE_FINE)
    {
        // invert the value
        raw = (1.0f - pct01) * 0.30f;
    }
    else
    {
        raw = MapPercentToRawGrade(dp, static_cast<WeatherState>(stateVal), pct01);
    }

    return PushWeatherToClient(zoneId, static_cast<WeatherState>(stateVal), raw);
}

// .wvibe setRaw <zoneId> <state:uint> <raw:0..1>
static bool HandleCommandRaw(ChatHandler* handler, uint32 zoneId, uint32 stateVal, float grade)
{
    if (!g_EnableModule)
    {
        handler->SendSysMessage("|cff00ff00WeatherVibe:|r Module is disabled in config.");
        return false;
    }
    if (!IsValidWeatherState(stateVal))
    {
        handler->SendSysMessage("|cff00ff00WeatherVibe:|r Invalid state. Examples: 0=Fine, 1=Fog, 3=LightRain, 4=MediumRain, 5=HeavyRain, 6=LightSnow, 7=MediumSnow, 8=HeavySnow, 22=LightSandstorm, 41=MediumSandstorm, 42=HeavySandstorm, 86=Thunders.");
        handler->SendSysMessage("Usage: .wvibe setRaw [zoneId] [state:uint] [raw:0..1]");
        return false;
    }

    float raw = std::clamp(grade, 0.0f, 1.0f);
    return PushWeatherToClient(zoneId, static_cast<WeatherState>(stateVal), raw);
}

class WeatherVibe_CommandScript : public CommandScript
{
public:
    WeatherVibe_CommandScript() : CommandScript("WeatherVibe_CommandScript") {}

    static bool HandleWvibeReload(ChatHandler* handler)
    {
        if (!g_EnableModule)
        {
            handler->SendSysMessage("|cff00ff00WeatherVibe:|r Is disabled (WeatherVibe.Enable = 0).");
            return false;
        }

        LoadDayPartConfig();
        LoadStateRanges();

        handler->SendSysMessage("|cff00ff00WeatherVibe:|r Reloaded (per-state ranges/dayparts).");
        return true;
    }

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

    static bool HandleWvibeShow(ChatHandler* handler)
    {
        if (!g_EnableModule)
        {
            handler->SendSysMessage("|cff00ff00WeatherVibe:|r Is disabled (WeatherVibe.Enable = 0).");
            return false;
        }

        if (g_LastApplied.empty())
        {
            handler->SendSysMessage("|cff00ff00WeatherVibe:|r No last-applied weather recorded yet. Use .wvibe set or setRaw to push weather.");
            return true;
        }

        DayPart d = GetCurrentDayPart();
        Season s = GetCurrentSeason();

        std::ostringstream oss;
        oss << "|cff00ff00WeatherVibe:|r show | season=" << SeasonName(s) << " | daypart=" << DayPartName(d) << "\n";

        for (auto const& kv : g_LastApplied)
        {
            uint32 zoneId = kv.first;
            LastApplied const& la = kv.second;
            oss << "zone " << zoneId
                << " -> last state=" << WeatherStateName(la.state)
                << " raw=" << std::fixed << std::setprecision(2) << la.grade
                << (la.hasValue ? "" : " (unset)")
                << "\n";
        }

        handler->SendSysMessage(oss.str().c_str());
        return true;
    }

    static bool HandleWvibeSet(ChatHandler* handler, uint32 zoneId, uint32 stateVal, float percentage)
    {
        return HandleCommandPercent(handler, zoneId, stateVal, percentage);
    }

    static bool HandleWvibeSetRaw(ChatHandler* handler, uint32 zoneId, uint32 stateVal, float rawGrade)
    {
        return HandleCommandRaw(handler, zoneId, stateVal, rawGrade);
    }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable wvibeSet =
        {
            { "set",    HandleWvibeSet,     SEC_ADMINISTRATOR, Console::Yes },
            { "setRaw", HandleWvibeSetRaw,  SEC_ADMINISTRATOR, Console::Yes },
            { "reload", HandleWvibeReload,  SEC_ADMINISTRATOR, Console::Yes },
            { "where",  HandleWvibeWhere,   SEC_ADMINISTRATOR, Console::Yes },
            { "show",   HandleWvibeShow,    SEC_ADMINISTRATOR, Console::Yes },
            { "help",   HandleWvibeHelp,    SEC_ADMINISTRATOR, Console::Yes },
        };
        static ChatCommandTable root =
        {
            { "wvibe", wvibeSet }
        };
        return root;
    }
};

// ==========================
// Player hooks @see PLayerScript.h
// ==========================
class WeatherVibe_PlayerScript : public PlayerScript
{
public:
    WeatherVibe_PlayerScript() : PlayerScript("WeatherVibe_PlayerScript") {}

    void OnPlayerLogin(Player* player) override
    {
        if (!g_EnableModule)
        {
            return;
        }

        ChatHandler(player->GetSession()).SendSysMessage("|cff00ff00WeatherVibe:|r enabled");

        // push weather to client with last applied of the zone, otherwise weatherState FINE.
        PushLastAppliedWeatherToClient(player->GetZoneId(), player);

    }

    void OnPlayerUpdateZone(Player* player, uint32 newZone, uint32 /*newArea*/) override
    {
        if (!g_EnableModule)
        {
            return;
        }

        // push weather to client with last applied of the zone, otherwise weatherState FINE.
        PushLastAppliedWeatherToClient(newZone, player);
    }
};

// ==========================
// World hooks @see WorldScript.h
// ==========================
class WeatherVibe_WorldScript : public WorldScript
{
public:
    WeatherVibe_WorldScript() : WorldScript("WeatherVibe_WorldScript") {}

    void OnStartup() override
    {
        g_EnableModule = sConfigMgr->GetOption<bool>("WeatherVibe.Enable", true);
        if (!g_EnableModule)
        {
            LOG_INFO("server.loading", "[WeatherVibe] disabled by config");
            return;
        }

        g_Debug = sConfigMgr->GetOption<uint32>("WeatherVibe.Debug", 0) != 0;
        LoadDayPartConfig();
        LoadStateRanges();
        g_LastApplied.clear();

        LOG_INFO("server.loading", "[WeatherVibe] started (packet mode, per-state ranges)");
    }
};

// ==================
// Module entry point
// ==================
void Addmod_weather_vibeScripts()
{
    new WeatherVibe_CommandScript();
    new WeatherVibe_PlayerScript();
    new WeatherVibe_WorldScript();
}
