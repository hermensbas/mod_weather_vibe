// mod_weather_vibe
// 
//  - Auto-rotation engine with profiles (weights, percent bands, windows, tweening)
//  - Zone parent mapping (capitals/starter zones inherit parent zone climate)
//  - Sprinkle: temporary override spikes (with duration)
//  - Richer .show and new .wvibe auto/* admin commands
//  - 0% clear for FINE, baseline caps through config ranges
//
// Notes:
//  * The auto engine is optional (WeatherVibe.Auto.Enable).
//  * Profiles maps are configured via strings (see config comment block below).
//  * We use direct Weather packets (no WeatherMgr/objects). Last-applied is kept.
//
// ===================== CONFIG KEYS (add to .conf) =====================
// Toggle module
//   WeatherVibe.Enable = 1
// Debug broadcast to zone on pushes
//   WeatherVibe.Debug = 1
// Season + dayparts 
//   WeatherVibe.Season = auto|spring|summer|autumn|winter
//   WeatherVibe.DayPart.Mode = auto|morning|afternoon|evening|night
//   WeatherVibe.DayPart.MORNING.Start   = 06:00
//   WeatherVibe.DayPart.AFTERNOON.Start = 12:00
//   WeatherVibe.DayPart.EVENING.Start   = 18:00
//   WeatherVibe.DayPart.NIGHT.Start     = 22:00
// Per-state InternalRange per daypart per weather effect.
//
// --- (Auto engine) ---
//   WeatherVibe.Auto.Enable = 0            # master switch
//   WeatherVibe.Auto.TickMs = 1000         # engine tick granularity (ms)
//   WeatherVibe.Auto.MinWindowSec = 180    # min seconds a picked state should live before new pick
//   WeatherVibe.Auto.MaxWindowSec = 480    # max seconds a picked state should live
//   WeatherVibe.Auto.TweenSec = 20         # seconds to ramp toward target each change
//   WeatherVibe.Auto.TinyNudge = 0.01      # raw delta under which we skip sending
//
// Profiles config:
//   WeatherVibe.Profile.Names = Temperate,Tundra,Desert
//   WeatherVibe.Profile.Temperate.Weights = 0=45,1=5,3=20,4=15,5=5,6=0,7=0,8=0,22=0,41=0,42=0,86=10
//   WeatherVibe.Profile.Temperate.Percent.Min = 5
//   WeatherVibe.Profile.Temperate.Percent.Max = 55
//   WeatherVibe.Profile.Tundra.Weights   = 0=35,1=8,3=8,4=6,5=3,6=15,7=15,8=5,22=0,41=0,42=0,86=5
//   WeatherVibe.Profile.Tundra.Percent.Min = 5
//   WeatherVibe.Profile.Tundra.Percent.Max = 60
//   WeatherVibe.Profile.Desert.Weights   = 0=60,1=0,3=3,4=2,5=0,6=0,7=0,8=0,22=15,41=10,42=5,86=5
//   WeatherVibe.Profile.Desert.Percent.Min = 5
//   WeatherVibe.Profile.Desert.Percent.Max = 55
//
// Assign profiles to zones (controller zones only; children inherit via ZoneParent):
//   WeatherVibe.ZoneProfile.Map = 1=Temperate,3=Temperate,8=Tundra,10=Desert
//
// =====================================================================

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

    enum class DayPart : uint8
    {
        MORNING = 0,
        AFTERNOON,
        EVENING,
        NIGHT,
        COUNT
    };

    enum class Season : uint8
    {
        SPRING = 0,
        SUMMER,
        AUTUMN,
        WINTER,
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

    // ================= Auto engine =================
    struct Profile
    {
        std::string name;
        // weights per state id (0..). Absent or zero weight means not used.
        std::unordered_map<uint32, uint32> weights;
        float pctMin = 5.0f; // percent 0..100
        float pctMax = 55.0f;
    };

    struct Sprinkle
    {
        bool active = false;
        WeatherState state = WEATHER_STATE_FINE;
        float pct = 0.0f; // 0..100 logical percent
        uint32 remainMs = 0;
    };

    struct AutoZone
    {
        bool enabled = false;          // zone is controlled by auto engine
        std::string profile;           // profile name

        // Current logical percent + state (percent is profile-space, 0..100)
        WeatherState curState = WEATHER_STATE_FINE;
        float curPct = 0.0f;

        // Target we are tweening toward
        WeatherState tgtState = WEATHER_STATE_FINE;
        float tgtPct = 0.0f;
        uint32 windowRemainMs = 0;     // how long until a new target is chosen
        uint32 tweenRemainMs = 0;      // time left to finish tween

        Sprinkle sprinkle;             // temporary override

        // book-keeping to clamp sends
        float lastRawSent = -1.0f;
        WeatherState lastStateSent = WEATHER_STATE_FINE;
    };

    // engine globals
    bool   g_EnableModule = true;
    bool   g_Debug = false;

    std::string  g_DayPartMode = "auto";  // auto|morning|afternoon|evening|night
    std::string  g_SeasonMode = "auto";   // auto|spring|summer|autumn|winter

    DayPartStarts g_Starts;

    // Per-daypart per-WeatherState ranges (keyed by WeatherState value: 0,1,3,4,5,6,7,8,22,41,42,86)
    std::unordered_map<uint32, Range> g_StateRanges[(size_t)DayPart::COUNT];

    // per-zone last applied snapshot (for resend)
    std::unordered_map<uint32, LastApplied>  g_LastApplied;

    // zone parent mapping: child -> parent, and reverse registry parent -> children
    std::unordered_map<uint32, uint32> g_ZoneParent; // child->parent
    std::unordered_map<uint32, std::vector<uint32>> g_ZoneChildren; // parent->children

    // profiles + zone assignment for auto engine
    std::unordered_map<std::string, Profile> g_Profiles; // by name lowercased
    std::unordered_map<uint32, std::string> g_ZoneProfile; // controller zone -> profile name (lower)

    // auto engine control
    bool   g_AutoEnabled = false;
    uint32 g_AutoTickMs = 1000;     // tick granularity
    uint32 g_MinWindowSec = 180;
    uint32 g_MaxWindowSec = 480;
    uint32 g_TweenSec = 20;
    float  g_TinyNudge = 0.01f;  // raw delta skip threshold

    std::unordered_map<uint32, AutoZone> g_AutoZones; // only controller zones

    std::mt19937 g_Rng{ std::random_device{}() };
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
    case WEATHER_STATE_BLACKRAIN: // unsupported
    case WEATHER_STATE_BLACKSNOW: // unsupported
    default:
        return false;
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

static inline int ClampMinutes(int v) { return std::clamp(v, 0, 23 * 60 + 59); }
static void ValidateDayPartStarts()
{
    g_Starts.morning = ClampMinutes(g_Starts.morning);
    g_Starts.afternoon = std::max(ClampMinutes(g_Starts.afternoon), g_Starts.morning + 1);
    g_Starts.evening = std::max(ClampMinutes(g_Starts.evening), g_Starts.afternoon + 1);
    g_Starts.night = std::max(ClampMinutes(g_Starts.night), g_Starts.evening + 1);
}

// ======================================
// Intensity mapping helpers
// ======================================
static float ClampToCoreBounds(float g, WeatherState s)
{
    // Allow true 0.0 for FINE (perfectly clear), clamp others conservatively
    if (s == WEATHER_STATE_FINE)
        return std::clamp(g, 0.0f, 0.9999f);

    if (g < 0.0f)  return kMinGrade;
    if (g >= 1.0f) return kMaxGrade;
    return g;
}

static void LoadDayPartConfig()
{
    g_DayPartMode = sConfigMgr->GetOption<std::string>("WeatherVibe.DayPart.Mode", "auto");
    g_SeasonMode = sConfigMgr->GetOption<std::string>("WeatherVibe.Season", "auto");

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
            if (b < a) std::swap(a, b);
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
    // clear previous
    for (size_t i = 0; i < (size_t)DayPart::COUNT; ++i)
        g_StateRanges[i].clear();

    auto makeKey = [](DayPart dp, WeatherState ws)
        {
            std::ostringstream oss;
            oss << "WeatherVibe.Intensity.InternalRange."
                << DayPartTokenUpper(dp) << "."
                << ConfigStateToken(ws);
            return oss.str();
        };

    Range def{ 0.30f, 1.00f }; // sensible defaults; expect config to reduce to ~0.65 tops

    for (DayPart dp : { DayPart::MORNING, DayPart::AFTERNOON, DayPart::EVENING, DayPart::NIGHT })
        for (WeatherState ws : kAcceptedStates)
            g_StateRanges[(size_t)dp][(uint32)ws] = ParseRangePair(makeKey(dp, ws), def);
}

// Converts profile percent (0..1) to raw grade (per-WeatherState/daypart range)
static float MapPercentToRawGrade(DayPart dp, WeatherState state, float percent01)
{
    percent01 = std::clamp(percent01, 0.0f, 1.0f);
    auto const& table = g_StateRanges[(size_t)dp];
    auto it = table.find((uint32)state);
    Range r;

    if (it != table.end()) r = it->second; else r = { 0.30f, 1.00f };

    return r.min + percent01 * (r.max - r.min);
}

static float RawToPercent01(DayPart dp, WeatherState state, float raw)
{
    auto const& table = g_StateRanges[(size_t)dp];
    auto it = table.find((uint32)state);
    Range r = (it != table.end()) ? it->second : Range{ 0.0f, 1.0f };
    if (r.max <= r.min) return 0.0f;
    return std::clamp((raw - r.min) / (r.max - r.min), 0.0f, 1.0f);
}

// ======================================
// Day/Season helpers used by debug/show
// ======================================
static Season GetCurrentSeason()
{
    std::string m = g_SeasonMode;
    std::transform(m.begin(), m.end(), m.begin(), [](unsigned char c) { return char(std::tolower(c)); });

    if (m == "spring") return Season::SPRING;
    if (m == "summer") return Season::SUMMER;
    if (m == "autumn") return Season::AUTUMN;
    if (m == "winter") return Season::WINTER;

    tm lt = GetLocalTimeSafe();
    int yday = lt.tm_yday;
    uint32 seasonIndex = ((yday - 78 + 365) / 91) % 4; // ~Mar 20 as 0

    switch (seasonIndex)
    {
    default:
    case 0: return Season::SPRING;
    case 1: return Season::SUMMER;
    case 2: return Season::AUTUMN;
    case 3: return Season::WINTER;
    }
}

static DayPart GetCurrentDayPart()
{
    std::string mode = g_DayPartMode;
    std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char c) { return char(std::tolower(c)); });

    if (mode == "morning")   return DayPart::MORNING;
    if (mode == "afternoon") return DayPart::AFTERNOON;
    if (mode == "evening")   return DayPart::EVENING;
    if (mode == "night")     return DayPart::NIGHT;

    tm lt = GetLocalTimeSafe();
    int minutes = lt.tm_hour * 60 + lt.tm_min;

    if (minutes >= g_Starts.night || minutes < g_Starts.morning) return DayPart::NIGHT;
    if (minutes >= g_Starts.evening)   return DayPart::EVENING;
    if (minutes >= g_Starts.afternoon) return DayPart::AFTERNOON;
    return DayPart::MORNING;
}

// ======================================
// Zone parent mapping
// ======================================
static uint32 ResolveControllerZone(uint32 zoneId)
{
    auto it = g_ZoneParent.find(zoneId);
    if (it == g_ZoneParent.end()) return zoneId;
    // chain-safe (in case of multi-level mapping)
    uint32 cur = zoneId;
    std::unordered_set<uint32> seen;
    while (g_ZoneParent.count(cur))
    {
        if (!seen.insert(cur).second) break; // cycle guard
        cur = g_ZoneParent[cur];
    }
    return cur;
}

static bool BroadcastZonePacket(uint32 zoneId, WorldPacket const* packet)
{
    bool delivered = false;

    sMapMgr->DoForAllMaps([&](Map* map) -> void
    {
        delivered = map->SendZoneMessage(zoneId, packet) || delivered;
    });

    return delivered;
}

static void BroadcastZoneText(uint32 zoneId, char const* text)
{
    sMapMgr->DoForAllMaps([&](Map* map) -> void
    {
        map->SendZoneText(zoneId, text);
    });
}

// ======================================
// Applies weather to a zone (returns true only when actually delivered to at least one player).
// ======================================
static bool PushWeatherToClient(uint32 zoneIdRaw, WeatherState state, float rawGrade)
{
    uint32 zoneId = ResolveControllerZone(zoneIdRaw);
    float normalizedGrade = ClampToCoreBounds(rawGrade, state);

    // We send to controller and children
    WorldPackets::Misc::Weather weatherPackage(state, normalizedGrade);
    WorldPacket const* weatherPacket = weatherPackage.Write();
    bool delivered = BroadcastZonePacket(zoneId, weatherPacket);
    auto itc = g_ZoneChildren.find(zoneId);
    if (itc != g_ZoneChildren.end())
        for (uint32 child : itc->second)
            delivered = BroadcastZonePacket(child, weatherPacket) || delivered;

    // record last-applied for controller (children will reuse controller snapshot)
    LastApplied& snap = g_LastApplied[zoneId];
    snap.state = state; snap.grade = normalizedGrade; snap.hasValue = true;

    if (g_Debug)
    {
        Season s = GetCurrentSeason();
        DayPart d = GetCurrentDayPart();
        std::ostringstream zmsg;
        zmsg << "|cff00ff00WeatherVibe:|r [DEBUG] season: " << SeasonName(s)
            << " | day: " << DayPartName(d)
            << " | state: " << WeatherStateName(state)
            << " | grade: " << std::fixed << std::setprecision(2) << normalizedGrade
            << " | zone: " << zoneId
            << " | delivered: " << (delivered ? "true" : "false");
        std::string debugText = zmsg.str();
        BroadcastZoneText(zoneId, debugText.c_str());
        if (itc != g_ZoneChildren.end())
            for (uint32 child : itc->second)
                BroadcastZoneText(child, debugText.c_str());
    }

    return delivered;
}

// Re-send last-applied weather for a zone (login/zone-change helper)
static void PushLastAppliedWeatherToClient(uint32 zoneIdRaw, Player* player)
{
    uint32 zoneId = ResolveControllerZone(zoneIdRaw);
    auto it = g_LastApplied.find(zoneId);
    if (it == g_LastApplied.end() || !it->second.hasValue)
        return;

    WorldPackets::Misc::Weather weatherPackage(it->second.state, it->second.grade);
    player->SendDirectMessage(weatherPackage.Write());
}

static void SeedAutoFromLastApplied(uint32 controllerZone, AutoZone& az)
{
    if (az.lastRawSent >= 0.0f) return; // already seeded

    auto it = g_LastApplied.find(controllerZone);
    if (it == g_LastApplied.end() || !it->second.hasValue) return;

    DayPart dp = GetCurrentDayPart();
    WeatherState st = it->second.state;
    float raw = it->second.grade;
    float pct = RawToPercent01(dp, st, raw) * 100.0f;

    az.curState = st;  az.tgtState = st;
    az.curPct = pct; az.tgtPct = pct;
    az.tweenRemainMs = 0;

    az.lastRawSent = raw;
    az.lastStateSent = st;
}

// ======================================
// Auto engine helpers
// ======================================
static std::vector<std::string> SplitCSV(std::string s)
{
    std::vector<std::string> out;
    std::string cur;
    std::istringstream iss(s);
    while (std::getline(iss, cur, ','))
    {
        // trim
        size_t a = cur.find_first_not_of(" \t\n\r");
        size_t b = cur.find_last_not_of(" \t\n\r");
        if (a == std::string::npos) continue;
        out.emplace_back(cur.substr(a, b - a + 1));
    }
    return out;
}

static std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

static void LoadProfiles()
{
    g_Profiles.clear();
    std::string names = sConfigMgr->GetOption<std::string>("WeatherVibe.Profile.Names", "Temperate");
    for (auto name : SplitCSV(names))
    {
        Profile p; p.name = name;
        std::string base = std::string("WeatherVibe.Profile.") + name + ".";
        std::string w = sConfigMgr->GetOption<std::string>(base + "Weights", "");
        if (!w.empty())
        {
            for (auto& kv : SplitCSV(w))
            {
                uint32 state = 0, weight = 0;
                if (std::sscanf(kv.c_str(), " %u = %u ", &state, &weight) == 2)
                {
                    if (IsValidWeatherState(state) && weight > 0)
                        p.weights[state] = weight;
                }
            }
        }
        p.pctMin = (float)sConfigMgr->GetOption<uint32>(base + "Percent.Min", 5u);
        p.pctMax = (float)sConfigMgr->GetOption<uint32>(base + "Percent.Max", 55u);
        if (p.pctMax < p.pctMin) std::swap(p.pctMax, p.pctMin);
        g_Profiles[Lower(name)] = p;
    }

    // zone -> profile
    g_ZoneProfile.clear();
    std::string zpm = sConfigMgr->GetOption<std::string>("WeatherVibe.ZoneProfile.Map", "");
    for (auto& kv : SplitCSV(zpm))
    {
        uint32 zone = 0; char prof[128] = { 0 };
        if (std::sscanf(kv.c_str(), " %u = %127s ", &zone, prof) == 2 && zone)
            g_ZoneProfile[zone] = Lower(prof);
    }
}

static void LoadAutoConfig()
{
    g_AutoEnabled = sConfigMgr->GetOption<uint32>("WeatherVibe.Auto.Enable", 0) != 0;
    g_AutoTickMs = sConfigMgr->GetOption<uint32>("WeatherVibe.Auto.TickMs", 1000);
    g_MinWindowSec = sConfigMgr->GetOption<uint32>("WeatherVibe.Auto.MinWindowSec", 180);
    g_MaxWindowSec = sConfigMgr->GetOption<uint32>("WeatherVibe.Auto.MaxWindowSec", 480);
    g_TweenSec = sConfigMgr->GetOption<uint32>("WeatherVibe.Auto.TweenSec", 20);
    g_TinyNudge = sConfigMgr->GetOption<float>("WeatherVibe.Auto.TinyNudge", 0.01f);
}

static WeatherState PickStateFromWeights(Profile const& p)
{
    // Build discrete distribution
    std::vector<uint32> states; states.reserve(p.weights.size());
    std::vector<double> weights; weights.reserve(p.weights.size());
    for (auto const& kv : p.weights)
    {
        states.push_back(kv.first);
        weights.push_back((double)kv.second);
    }
    if (states.empty())
        return WEATHER_STATE_FINE;

    std::discrete_distribution<size_t> dist(weights.begin(), weights.end());
    return static_cast<WeatherState>(states[dist(g_Rng)]);
}

static float RandPercentBetween(Profile const& p)
{
    if (p.pctMax <= p.pctMin) return p.pctMin;
    std::uniform_real_distribution<float> d(p.pctMin, p.pctMax);
    return d(g_Rng);
}

static uint32 RandWindowMs()
{
    if (g_MaxWindowSec < g_MinWindowSec) std::swap(g_MaxWindowSec, g_MinWindowSec);
    std::uniform_int_distribution<uint32> d(g_MinWindowSec, g_MaxWindowSec);
    return d(g_Rng) * 1000u;
}

static void EnsureAutoZone(uint32 controllerZone)
{
    if (g_AutoZones.count(controllerZone)) return;
    AutoZone az; az.enabled = false; // default off until mapped
    g_AutoZones[controllerZone] = az;
}

static void InitializeAutoZonesFromConfig()
{
    g_AutoZones.clear();
    for (auto const& zprof : g_ZoneProfile)
    {
        uint32 controller = ResolveControllerZone(zprof.first);
        EnsureAutoZone(controller);
        AutoZone& az = g_AutoZones[controller];
        az.enabled = true; // controlled because profile exists
        az.profile = zprof.second; // lowercased name
        az.curState = WEATHER_STATE_FINE;
        az.curPct = 0.0f;
        az.tgtState = WEATHER_STATE_FINE;
        az.tgtPct = 0.0f;
        az.windowRemainMs = 0;
        az.tweenRemainMs = 0;
        az.lastRawSent = -1.0f;
        az.lastStateSent = WEATHER_STATE_FINE;
        az.sprinkle = Sprinkle{};
        SeedAutoFromLastApplied(controller, az);
    }
}

static void SyncAutoWithManual(uint32 zoneIdRaw, WeatherState state, float rawGrade)
{
    if (!g_AutoEnabled) return;

    uint32 controller = ResolveControllerZone(zoneIdRaw);
    auto it = g_AutoZones.find(controller);
    if (it == g_AutoZones.end() || !it->second.enabled) return;

    AutoZone& az = it->second;
    DayPart dp = GetCurrentDayPart();
    float pct = RawToPercent01(dp, state, rawGrade) * 100.0f;

    az.curState = state; az.tgtState = state;
    az.curPct = pct;   az.tgtPct = pct;
    az.tweenRemainMs = 0;

    if (az.windowRemainMs == 0) az.windowRemainMs = RandWindowMs();

    az.lastRawSent = ClampToCoreBounds(rawGrade, state);
    az.lastStateSent = state;
}

static void ChooseNewTarget([[maybe_unused]] uint32 controllerZone, AutoZone& az)
{
    auto itp = g_Profiles.find(az.profile);
    if (itp == g_Profiles.end())
    {
        // fallback: any default profile
        if (!g_Profiles.empty()) itp = g_Profiles.begin();
    }

    if (itp == g_Profiles.end())
    {
        // no profiles at all -> fine 0
        az.tgtState = WEATHER_STATE_FINE;
        az.tgtPct = 0.0f;
        az.windowRemainMs = RandWindowMs();
        az.tweenRemainMs = g_TweenSec * 1000u;
        return;
    }

    Profile const& p = itp->second;
    az.tgtState = PickStateFromWeights(p);
    az.tgtPct = RandPercentBetween(p);
    az.windowRemainMs = RandWindowMs();
    az.tweenRemainMs = g_TweenSec * 1000u;
}

static void ApplyAutoTick(uint32 diffMs)
{
    if (!g_AutoEnabled) return;

    DayPart dp = GetCurrentDayPart();

    for (auto& kv : g_AutoZones)
    {
        uint32 controllerZone = kv.first;
        AutoZone& az = kv.second;
        if (!az.enabled) continue;

        // handle sprinkle override timer first
        if (az.sprinkle.active)
        {
            if (diffMs >= az.sprinkle.remainMs) az.sprinkle.remainMs = 0; else az.sprinkle.remainMs -= diffMs;
            if (az.sprinkle.remainMs == 0) az.sprinkle.active = false; // expire
        }

        // advance window timer & choose new target if needed
        if (az.windowRemainMs == 0)
            ChooseNewTarget(controllerZone, az);
        else
            az.windowRemainMs = (diffMs >= az.windowRemainMs) ? 0 : (az.windowRemainMs - diffMs);

        // tween toward target (or hold if sprinkle active—still tween grade for smoothness)
        if (az.tweenRemainMs == 0)
        {
            // already at target; keep current
            az.curState = az.tgtState;
            az.curPct = az.tgtPct;
        }
        else
        {
            // adopt the new weather STATE immediately; still tween intensity (percent)
            az.curState = az.tgtState;

            float t = 1.0f - (float)az.tweenRemainMs / (float)(g_TweenSec * 1000u);
            float src = az.curPct;
            float dst = az.tgtPct;
            float cur = src + (dst - src) * std::clamp(t, 0.0f, 1.0f);
            az.curPct = cur;
            az.tweenRemainMs = (diffMs >= az.tweenRemainMs) ? 0 : (az.tweenRemainMs - diffMs);
        }

        // Decide what to push this tick (sprinkle overrides state/pct if active)
        WeatherState outState = az.sprinkle.active ? az.sprinkle.state : az.curState;
        float outPct = az.sprinkle.active ? az.sprinkle.pct : az.curPct;

        // Map percent to raw grade for CURRENT daypart (dynamic bands)
        float raw = MapPercentToRawGrade(dp, outState, outPct / 100.0f);
        float norm = ClampToCoreBounds(raw, outState);

        // tiny nudge filter
        float delta = (az.lastRawSent < 0.0f) ? 1.0f : std::fabs(norm - az.lastRawSent);
        bool stateChanged = (outState != az.lastStateSent);
        if (stateChanged || delta >= g_TinyNudge)
        {
            bool delivered = PushWeatherToClient(controllerZone, outState, norm);
            (void)delivered; // used only in debug text
            az.lastRawSent = norm;
            az.lastStateSent = outState;
        }
    }
}

// ======================================
// Commands
// ======================================
// .wvibe set <zoneId> <state:uint> <percentage:0..100>
static bool HandleCommandPercent(ChatHandler* handler, uint32 zoneId, uint32 stateVal, float percentage)
{
    if (!g_EnableModule)
    {
        handler->SendSysMessage("|cff00ff00WeatherVibe:|r module is disabled in config.");
        return false;
    }
    if (!IsValidWeatherState(stateVal))
    {
        handler->SendSysMessage("|cff00ff00WeatherVibe:|r Invalid state. Examples: 0=Fine, 1=Fog, 3=LightRain, 4=MediumRain, 5=HeavyRain, 6=LightSnow, 7=MediumSnow, 8=HeavySnow, 22=LightSandstorm, 41=MediumSandstorm, 42=HeavySandstorm, 86=Thunders.");
        handler->SendSysMessage("Usage: .wvibe set <zoneId> <state:uint> <percentage:0..100>");
        return false;
    }

    float pct01 = std::clamp(percentage, 0.0f, 100.0f) / 100.0f;
    DayPart dp = GetCurrentDayPart();
    float raw = MapPercentToRawGrade(dp, static_cast<WeatherState>(stateVal), pct01);

    bool ok = PushWeatherToClient(zoneId, (WeatherState)stateVal, raw);
    SyncAutoWithManual(zoneId, (WeatherState)stateVal, raw);
    return ok;
}

// .wvibe setRaw <zoneId> <state:uint> <raw:0..1>
static bool HandleCommandRaw(ChatHandler* handler, uint32 zoneId, uint32 stateVal, float grade)
{
    if (!g_EnableModule)
    {
        handler->SendSysMessage("|cff00ff00WeatherVibe:|r module is disabled in config.");
        return false;
    }
    if (!IsValidWeatherState(stateVal))
    {
        handler->SendSysMessage("|cff00ff00WeatherVibe:|r Invalid state. Usage: .wvibe setRaw <zoneId> <state:uint> <raw:0..1>");
        return false;
    }

    float raw = std::clamp(grade, 0.0f, 1.0f);
    bool ok = PushWeatherToClient(zoneId, (WeatherState)stateVal, raw);
    SyncAutoWithManual(zoneId, (WeatherState)stateVal, raw);
    return ok;
}

// --- Auto subcommands ---
static bool HandleAutoOn(ChatHandler* handler)
{
    g_AutoEnabled = true;
    handler->SendSysMessage("|cff00ff00WeatherVibe:|r auto engine: ON");
    return true;
}

static bool HandleAutoOff(ChatHandler* handler)
{
    g_AutoEnabled = false;
    handler->SendSysMessage("|cff00ff00WeatherVibe:|r auto engine: OFF");
    return true;
}

static bool HandleAutoStatus(ChatHandler* handler)
{
    std::ostringstream oss;
    oss << "Auto=" << (g_AutoEnabled ? "on" : "off")
        << " tickMs=" << g_AutoTickMs
        << " window=[" << g_MinWindowSec << "," << g_MaxWindowSec << "]s"
        << " tween=" << g_TweenSec << "s\n";

    for (auto const& kv : g_AutoZones)
    {
        uint32 z = kv.first; AutoZone const& az = kv.second;
        oss << "Zone " << z << " enabled=" << (az.enabled ? "1" : "0")
            << " profile=" << az.profile
            << " cur=" << WeatherStateName(az.curState) << ":" << (int)std::round(az.curPct)
            << "% tgt=" << WeatherStateName(az.tgtState) << ":" << (int)std::round(az.tgtPct)
            << "% windowMs=" << az.windowRemainMs
            << " tweenMs=" << az.tweenRemainMs
            << (az.sprinkle.active ? " sprinkle=1" : "")
            << "\n";
    }

    handler->SendSysMessage(oss.str().c_str());
    return true;
}

static bool HandleAutoSet(ChatHandler* handler, uint32 zoneId, std::string profileName)
{
    uint32 controller = ResolveControllerZone(zoneId);
    std::string key = Lower(profileName);

    if (!g_Profiles.count(key))
    {
        handler->PSendSysMessage("|cff00ff00WeatherVibe:|r Unknown profile '%s'", profileName.c_str());
        return false;
    }

    EnsureAutoZone(controller);
    AutoZone& az = g_AutoZones[controller];
    az.enabled = true;
    az.profile = key;
    az.windowRemainMs = 0; // force a fresh pick
    handler->PSendSysMessage("|cff00ff00WeatherVibe:|r Zone %u is now auto-controlled by profile '%s' (controller=%u)", zoneId, profileName.c_str(), controller);
    return true;
}

static bool HandleAutoClear(ChatHandler* handler, uint32 zoneId)
{
    uint32 controller = ResolveControllerZone(zoneId);
    auto it = g_AutoZones.find(controller);
    if (it != g_AutoZones.end())
    {
        it->second.enabled = false;
        handler->PSendSysMessage("|cff00ff00WeatherVibe:|r Zone %u auto control disabled (controller=%u)", zoneId, controller);
        return true;
    }
    handler->PSendSysMessage("|cff00ff00WeatherVibe:|r Zone %u has no auto control", zoneId);
    return false;
}

static bool HandleAutoSprinkle(ChatHandler* handler, uint32 zoneId, std::string stateToken, float percentage, uint32 durationSec)
{
    if (percentage < 0.0f) percentage = 0.0f; if (percentage > 100.0f) percentage = 100.0f;

    uint32 controller = ResolveControllerZone(zoneId);
    auto it = g_AutoZones.find(controller);
    if (it == g_AutoZones.end())
    {
        handler->PSendSysMessage("|cff00ff00WeatherVibe:|r Zone %u is not under auto control; use .wvibe auto set <zone> <profile> first.", zoneId);
        return false;
    }

    AutoZone& az = it->second;

    WeatherState s = az.curState;
    if (stateToken != "auto")
    {
        // parse numeric or known token
        uint32 val = 0;
        if (std::sscanf(stateToken.c_str(), "%u", &val) == 1 && IsValidWeatherState(val))
            s = static_cast<WeatherState>(val);
        else
        {
            // try names: fine,fog,light_rain,...
            std::string l = Lower(stateToken);
            for (WeatherState ws : kAcceptedStates)
                if (l == WeatherStateName(ws)) { s = ws; break; }
        }
    }

    az.sprinkle.active = true;
    az.sprinkle.state = s;
    az.sprinkle.pct = percentage;
    az.sprinkle.remainMs = durationSec * 1000u;

    handler->PSendSysMessage("|cff00ff00WeatherVibe:|r Sprinkle applied to zone %u (controller=%u): %s %.0f%% for %u sec", zoneId, controller, WeatherStateName(s), percentage, durationSec);
    return true;
}

class WeatherVibe_CommandScript : public CommandScript
{
public:
    WeatherVibe_CommandScript() : CommandScript("WeatherVibe_CommandScript") {}

    static bool HandleWvibeReload(ChatHandler* handler)
    {
        if (!g_EnableModule)
        {
            handler->SendSysMessage("|cff00ff00WeatherVibe:|r is disabled (WeatherVibe.Enable = 0).");
            return false;
        }

        LoadDayPartConfig();
        LoadStateRanges();
        LoadProfiles();
        LoadAutoConfig();
        InitializeAutoZonesFromConfig();

        handler->SendSysMessage("|cff00ff00WeatherVibe:|r reloaded (ranges/dayparts/parents/profiles/auto).");
        return true;
    }

    static bool HandleWvibeShow(ChatHandler* handler)
    {
        if (!g_EnableModule)
        {
            handler->SendSysMessage("|cff00ff00WeatherVibe:|r is disabled (WeatherVibe.Enable = 0).");
            return false;
        }

        if (g_LastApplied.empty())
        {
            handler->SendSysMessage("|cff00ff00WeatherVibe:|r No last-applied weather recorded yet. Use .wvibe set or setRaw to push weather.");
            return true;
        }

        Season s = GetCurrentSeason();
        DayPart d = GetCurrentDayPart();

        std::ostringstream oss;
        oss << "|cff00ff00WeatherVibe:|r show | season=" << SeasonName(s) << " daypart=" << DayPartName(d) << "\n";

        for (auto const& kv : g_LastApplied)
        {
            uint32 zoneId = kv.first;
            LastApplied const& la = kv.second;
            float pct = RawToPercent01(d, la.state, la.grade) * 100.0f;
            oss << "zone " << zoneId
                << " -> last state=" << WeatherStateName(la.state)
                << " raw=" << std::fixed << std::setprecision(2) << la.grade
                << " (" << std::setprecision(0) << pct << "%)"
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

    // auto command table
    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable autoSet =
        {
            { "on",       HandleAutoOn,       SEC_ADMINISTRATOR, Console::Yes },
            { "off",      HandleAutoOff,      SEC_ADMINISTRATOR, Console::Yes },
            { "status",   HandleAutoStatus,   SEC_ADMINISTRATOR, Console::Yes },
            { "set",      HandleAutoSet,      SEC_ADMINISTRATOR, Console::Yes },
            { "clear",    HandleAutoClear,    SEC_ADMINISTRATOR, Console::Yes },
            { "sprinkle", HandleAutoSprinkle, SEC_ADMINISTRATOR, Console::Yes },
        };

        static ChatCommandTable wvibeSet =
        {
            { "set",    HandleWvibeSet,    SEC_ADMINISTRATOR, Console::Yes },
            { "setRaw", HandleWvibeSetRaw, SEC_ADMINISTRATOR, Console::Yes },
            { "reload", HandleWvibeReload, SEC_ADMINISTRATOR, Console::Yes },
            { "show",   HandleWvibeShow,   SEC_ADMINISTRATOR, Console::Yes },
            { "auto",   autoSet }
        };
        static ChatCommandTable root =
        {
            { "wvibe", wvibeSet }
        };
        return root;
    }
};

// ==========================
// Player hooks
// ==========================
class WeatherVibe_PlayerScript : public PlayerScript
{
public:
    WeatherVibe_PlayerScript() : PlayerScript("WeatherVibe_PlayerScript") {}

    void OnPlayerLogin(Player* player) override
    {
         if (!g_EnableModule) 
            return;
        
        ChatHandler(player->GetSession()).SendSysMessage("|cff00ff00WeatherVibe:|r enabled.");
        uint32 controller = ResolveControllerZone(player->GetZoneId());
        if (auto it = g_AutoZones.find(controller); it != g_AutoZones.end() && it->second.enabled)
        {
            SeedAutoFromLastApplied(controller, it->second);
        }
        
        PushLastAppliedWeatherToClient(player->GetZoneId(), player);
    }

    void OnPlayerUpdateZone(Player* player, uint32 newZone, uint32 /*newArea*/) override
    {
        if (!g_EnableModule) 
            return;
        
        uint32 controller = ResolveControllerZone(newZone);
        if (auto it = g_AutoZones.find(controller); it != g_AutoZones.end() && it->second.enabled)
        {
            SeedAutoFromLastApplied(controller, it->second);
        }
        
        PushLastAppliedWeatherToClient(newZone, player);
    }
};

// ==========================
// World hooks
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
        LoadProfiles();
        LoadAutoConfig();
        InitializeAutoZonesFromConfig();

        g_LastApplied.clear();

        LOG_INFO("server.loading", "[WeatherVibe] started (packet mode, per-state ranges, auto engine)");
    }

    void OnUpdate(uint32 diff) override
    {
        if (!g_EnableModule) return;
        static uint32 acc = 0;
        acc += diff;
        while (acc >= g_AutoTickMs)
        {
            ApplyAutoTick(g_AutoTickMs);
            acc -= g_AutoTickMs;
        }
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
