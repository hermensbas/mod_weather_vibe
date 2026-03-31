#include "mod_wv_core.h"

#include "Config.h"
#include "DBCStores.h"        // sAreaTableStore — zone → mapId resolution
#include "GameTime.h"
#include "Log.h"
#include "MapMgr.h"
#include "MiscPackets.h"
#include "World.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <sstream>
#include <string>

// ============================================================
// Static member definitions
// ============================================================
std::array<WeatherState, 12> const WeatherVibeCore::kAcceptedStates =
{
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

// ============================================================
// Singleton accessor
// ============================================================
WeatherVibeCore& WeatherVibeCore::Instance()
{
    static WeatherVibeCore instance;
    return instance;
}

// ============================================================
// Lifecycle
// ============================================================
void WeatherVibeCore::OnStartup()
{
    m_enabled = sConfigMgr->GetOption<bool>("WeatherVibe.Enable", true);

    if (!m_enabled)
    {
        LOG_INFO("server.loading", "[WeatherVibe] disabled by config");
        return;
    }

    m_debug = sConfigMgr->GetOption<uint32>("WeatherVibe.Debug", 0) != 0;

    LoadDayPartConfig();
    LoadStateRanges();
    m_lastApplied.clear();
    m_zoneToMapId.clear();

    LOG_INFO("server.loading", "[WeatherVibe] started (packet mode, per-state ranges)");
}

// ============================================================
// Config loading
// ============================================================
void WeatherVibeCore::LoadDayPartConfig()
{
    m_dayPartMode = sConfigMgr->GetOption<std::string>("WeatherVibe.DayPart.Mode", "auto");
    m_seasonMode  = sConfigMgr->GetOption<std::string>("WeatherVibe.Season",       "auto");

    m_starts.morning   = ParseHHMM(sConfigMgr->GetOption<std::string>("WeatherVibe.DayPart.MORNING.Start",   "06:00"), 6  * 60);
    m_starts.afternoon = ParseHHMM(sConfigMgr->GetOption<std::string>("WeatherVibe.DayPart.AFTERNOON.Start", "12:00"), 12 * 60);
    m_starts.evening   = ParseHHMM(sConfigMgr->GetOption<std::string>("WeatherVibe.DayPart.EVENING.Start",   "18:00"), 18 * 60);
    m_starts.night     = ParseHHMM(sConfigMgr->GetOption<std::string>("WeatherVibe.DayPart.NIGHT.Start",     "22:00"), 22 * 60);

    ValidateDayPartStarts();
}

void WeatherVibeCore::LoadStateRanges()
{
    for (size_t i = 0; i < (size_t)DayPart::COUNT; ++i)
    {
        m_stateRanges[i].clear();
    }

    auto makeKey = [](DayPart dp, WeatherState ws)
    {
        std::ostringstream oss;
        oss << "WeatherVibe.Intensity.InternalRange."
            << DayPartTokenUpper(dp) << "."
            << ConfigStateToken(ws);
        return oss.str();
    };

    Range def{ 0.30f, 1.00f };

    for (DayPart dp : { DayPart::MORNING, DayPart::AFTERNOON, DayPart::EVENING, DayPart::NIGHT })
    {
        for (WeatherState ws : kAcceptedStates)
        {
            m_stateRanges[(size_t)dp][(uint32)ws] = ParseRangePair(makeKey(dp, ws), def);
        }
    }
}

// ============================================================
// Time helpers
// ============================================================
tm WeatherVibeCore::GetLocalTimeSafe()
{
    time_t now = GameTime::GetGameTime().count();
    tm out{};
#if defined(_WIN32) || defined(_WIN64)
    localtime_s(&out, &now);
#else
    localtime_r(&now, &out);
#endif
    return out;
}

int WeatherVibeCore::ParseHHMM(std::string const& s, int defMinutes)
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

int WeatherVibeCore::ClampMinutes(int v)
{
    return std::clamp(v, 0, 23 * 60 + 59);
}

void WeatherVibeCore::ValidateDayPartStarts()
{
    m_starts.morning   = ClampMinutes(m_starts.morning);
    m_starts.afternoon = std::max(ClampMinutes(m_starts.afternoon), m_starts.morning   + 1);
    m_starts.evening   = std::max(ClampMinutes(m_starts.evening),   m_starts.afternoon + 1);
    m_starts.night     = ClampMinutes(m_starts.night); // wrap handled in GetCurrentDayPart()
}

// ============================================================
// Day/Season resolution
// ============================================================
DayPart WeatherVibeCore::GetCurrentDayPart() const
{
    std::string mode = m_dayPartMode;
    std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char c) { return char(std::tolower(c)); });

    if (mode == "morning")   { return DayPart::MORNING;   }
    if (mode == "afternoon") { return DayPart::AFTERNOON; }
    if (mode == "evening")   { return DayPart::EVENING;   }
    if (mode == "night")     { return DayPart::NIGHT;     }

    tm lt      = GetLocalTimeSafe();
    int minutes = lt.tm_hour * 60 + lt.tm_min;

    if (minutes >= m_starts.night || minutes < m_starts.morning) { return DayPart::NIGHT;     }
    if (minutes >= m_starts.evening)                              { return DayPart::EVENING;   }
    if (minutes >= m_starts.afternoon)                            { return DayPart::AFTERNOON; }

    return DayPart::MORNING;
}

Season WeatherVibeCore::GetCurrentSeason() const
{
    std::string m = m_seasonMode;
    std::transform(m.begin(), m.end(), m.begin(), [](unsigned char c) { return char(std::tolower(c)); });

    if (m == "spring") { return Season::SPRING; }
    if (m == "summer") { return Season::SUMMER; }
    if (m == "autumn") { return Season::AUTUMN; }
    if (m == "winter") { return Season::WINTER; }

    // Auto: derive from day-of-year; anchor Spring around Mar 20 (~day 79)
    tm lt = GetLocalTimeSafe();
    int yday = lt.tm_yday;
    uint32 seasonIndex = ((yday - 78 + 365) / 91) % 4;

    switch (seasonIndex)
    {
        default:
        case 0: return Season::SPRING;
        case 1: return Season::SUMMER;
        case 2: return Season::AUTUMN;
        case 3: return Season::WINTER;
    }
}

// ============================================================
// Intensity mapping
// ============================================================
float WeatherVibeCore::ClampToCoreBounds(float g)
{
    if (g < 0.0f)  { return kMinGrade; }
    if (g >= 1.0f) { return kMaxGrade; }
    return g;
}

float WeatherVibeCore::MapPercentToRawGrade(DayPart dp, WeatherState state, float percent01) const
{
    percent01 = std::clamp(percent01, 0.0f, 1.0f);

    auto const& table = m_stateRanges[(size_t)dp];
    auto it = table.find((uint32)state);
    Range r = (it != table.end()) ? it->second : Range{ 0.30f, 1.00f };

    return r.min + percent01 * (r.max - r.min);
}

Range WeatherVibeCore::ParseRangePair(std::string const& key, Range def)
{
    std::string v = sConfigMgr->GetOption<std::string>(key, "");
    if (!v.empty())
    {
        float a = def.min, b = def.max;
        if (std::sscanf(v.c_str(), " %f , %f ", &a, &b) == 2)
        {
            if (b < a) { std::swap(a, b); }
            return { std::clamp(a, 0.0f, 1.0f), std::clamp(b, 0.0f, 1.0f) };
        }
    }

    return def;
}

// ============================================================
// Map resolution
// ============================================================
Map* WeatherVibeCore::GetMap(uint32 zoneId)
{
    // try_emplace: single lookup — inserts with 0 only on first call for this zoneId.
    auto [it, inserted] = m_zoneToMapId.try_emplace(zoneId, 0u);

    if (inserted)
    {
        AreaTableEntry const* area = sAreaTableStore.LookupEntry(zoneId);
        it->second = area ? area->mapid : 0u;
    }

    return sMapMgr->FindBaseNonInstanceMap(it->second);
}

// ============================================================
// Broadcast helpers
// ============================================================
bool WeatherVibeCore::BroadcastZonePacket(uint32 zoneId, WorldPacket const* packet)
{
    Map* map = GetMap(zoneId);
    if (!map)
    {
        return false;
    }

    return map->SendZoneMessage(zoneId, packet);
}

void WeatherVibeCore::BroadcastZoneText(uint32 zoneId, char const* text)
{
    Map* map = GetMap(zoneId);
    if (!map)
    {
        return;
    }

    map->SendZoneText(zoneId, text);
}

// ============================================================
// Weather dispatch
// ============================================================
bool WeatherVibeCore::PushWeatherToClient(uint32 zoneId, WeatherState state, float rawGrade)
{
    float normalizedGrade = ClampToCoreBounds(rawGrade);
    LastApplied& entry    = m_lastApplied[zoneId];

    WorldPackets::Misc::Weather weatherPackage(state, normalizedGrade);
    WorldPacket const* weatherPacket = weatherPackage.Write();
    bool isApplied = BroadcastZonePacket(zoneId, weatherPacket);

    if (isApplied)
    {
        entry.state    = state;
        entry.grade    = normalizedGrade;
        entry.hasValue = true;
    }

    if (m_debug)
    {
        DayPart dp = GetCurrentDayPart();
        Season  s  = GetCurrentSeason();
        std::ostringstream zmsg;
        zmsg << "|cff00ff00WeatherVibe:|r [DEBUG] season=" << SeasonName(s)
             << " | day="    << DayPartName(dp)
             << " | state="  << WeatherStateName(state)
             << " | grade="  << std::fixed << std::setprecision(2) << normalizedGrade
             << " | pushed=" << (isApplied ? "true" : "false");

        BroadcastZoneText(zoneId, zmsg.str().c_str());
    }

    return true; // treat as success even if no players online
}

void WeatherVibeCore::PushLastAppliedWeatherToClient(uint32 zoneId, Player* player)
{
    auto it = m_lastApplied.find(zoneId);

    WeatherState state;
    float        grade;

    if (it != m_lastApplied.end() && it->second.hasValue)
    {
        state = it->second.state;
        grade = it->second.grade;
    }
    else
    {
        state = WEATHER_STATE_FINE;
        grade = 0.0f;
    }

    WorldPackets::Misc::Weather weatherPackage(state, grade);
    player->SendDirectMessage(weatherPackage.Write());
}

// ============================================================
// Name helpers
// ============================================================
char const* WeatherVibeCore::WeatherStateName(WeatherState s)
{
    switch (s)
    {
        case WEATHER_STATE_FINE:             return "fine";
        case WEATHER_STATE_FOG:              return "fog";
        case WEATHER_STATE_LIGHT_RAIN:       return "light_rain";
        case WEATHER_STATE_MEDIUM_RAIN:      return "medium_rain";
        case WEATHER_STATE_HEAVY_RAIN:       return "heavy_rain";
        case WEATHER_STATE_LIGHT_SNOW:       return "light_snow";
        case WEATHER_STATE_MEDIUM_SNOW:      return "medium_snow";
        case WEATHER_STATE_HEAVY_SNOW:       return "heavy_snow";
        case WEATHER_STATE_LIGHT_SANDSTORM:  return "light_sandstorm";
        case WEATHER_STATE_MEDIUM_SANDSTORM: return "medium_sandstorm";
        case WEATHER_STATE_HEAVY_SANDSTORM:  return "heavy_sandstorm";
        case WEATHER_STATE_THUNDERS:         return "thunders";
        case WEATHER_STATE_BLACKRAIN:        return "blackrain";
        case WEATHER_STATE_BLACKSNOW:        return "blacksnow";
        default:                             return "unknown";
    }
}

char const* WeatherVibeCore::DayPartName(DayPart d)
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

char const* WeatherVibeCore::SeasonName(Season s)
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

// ============================================================
// Internal token helpers (used by LoadStateRanges)
// ============================================================
char const* WeatherVibeCore::DayPartTokenUpper(DayPart d)
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

char const* WeatherVibeCore::ConfigStateToken(WeatherState s)
{
    switch (s)
    {
        case WEATHER_STATE_FINE:             return "Fine";
        case WEATHER_STATE_FOG:              return "Fog";
        case WEATHER_STATE_LIGHT_RAIN:       return "LightRain";
        case WEATHER_STATE_MEDIUM_RAIN:      return "MediumRain";
        case WEATHER_STATE_HEAVY_RAIN:       return "HeavyRain";
        case WEATHER_STATE_LIGHT_SNOW:       return "LightSnow";
        case WEATHER_STATE_MEDIUM_SNOW:      return "MediumSnow";
        case WEATHER_STATE_HEAVY_SNOW:       return "HeavySnow";
        case WEATHER_STATE_LIGHT_SANDSTORM:  return "LightSandstorm";
        case WEATHER_STATE_MEDIUM_SANDSTORM: return "MediumSandstorm";
        case WEATHER_STATE_HEAVY_SANDSTORM:  return "HeavySandstorm";
        case WEATHER_STATE_THUNDERS:         return "Thunders";
        default:                             return "Unknown";
    }
}

bool WeatherVibeCore::IsValidWeatherState(uint32 value)
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
