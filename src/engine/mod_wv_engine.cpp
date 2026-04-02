#include "mod_wv_core.h"

#include "Config.h"
#include "Log.h"
#include "ScriptMgr.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

// ============================================================
// RNG — module-local Mersenne Twister
// ============================================================
static std::mt19937& Rng()
{
    static std::mt19937 rng(static_cast<uint32>(std::time(nullptr)));
    return rng;
}

static float RandFloat(float lo, float hi)
{
    if (lo >= hi) { return lo; }
    return std::uniform_real_distribution<float>(lo, hi)(Rng());
}

static int RandInt(int lo, int hi) // inclusive
{
    if (lo >= hi) { return lo; }
    return std::uniform_int_distribution<int>(lo, hi)(Rng());
}

// ============================================================
// Data structures
// ============================================================

// One entry from e.g. WeatherVibe.Profile.DunMorogh.Spring=50,6,30,90
// intensMin / intensMax are percentages: 0..100
struct WeatherEntry
{
    uint32 weight    = 0;
    uint32 stateVal  = 0;
    float  intensMin = 30.0f;   // percent
    float  intensMax = 100.0f;  // percent
};

// Per-season profile (list of weighted entries)
using SeasonProfile = std::vector<WeatherEntry>;

// Per-zone transition settings
struct TransitionSettings
{
    uint32 stepTimeMinMs   = 30000;
    uint32 stepTimeMaxMs   = 60000;
    uint32 changesPerHr    = 10;
    uint32 maxConsecSame   = 3;
    float  stepSizePct     = 5.0f;   // intensity change per step in percentage points
};

// Cross-state transition phase
enum class TransitionPhase : uint8
{
    NONE = 0,    // same-state transition or idle
    FADE_OUT,    // old state fading toward its exit point
    FADE_IN      // new state fading toward its target
};

// Live transition state for a zone — all intensity values in percent (0..100)
struct ZoneTransition
{
    uint32 currentStateVal  = 0;
    float  currentPct       = 0.0f;   // percent

    uint32 targetStateVal   = 0;
    float  targetPct        = 0.0f;   // percent

    uint32 stepTimerMs      = 0;
    bool   inTransition     = false;

    // Selection interval timer — total time between weather selections.
    // Counts down during ALL phases (transition + hold).
    // When it fires, next weather is selected regardless of current phase.
    uint32 intervalTimerMs  = 0;
    bool   waitingForInterval = false;  // transition done, waiting for interval to expire

    // Cross-state transition state
    TransitionPhase phase       = TransitionPhase::NONE;
    bool            sameFamily  = false;
    bool            isUpgrade   = false;
    float           fadeOutPct  = 0.0f;
    float           fadeInPct   = 0.0f;

    uint32 changesThisHour  = 0;
    uint32 hourTimerMs      = 3600000u;
    uint32 consecSameCount  = 0;
};

// Full zone profile
struct ZoneProfile
{
    std::string        name;
    uint32             zoneId = 0;
    SeasonProfile      seasons[4];   // indexed by Season enum
    TransitionSettings transition;
    ZoneTransition     state;
};

// ============================================================
// Engine state
// ============================================================
static std::vector<ZoneProfile> gZoneProfiles;

// ============================================================
// Config helpers
// ============================================================
static std::string Trim(std::string const& s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    size_t e = s.find_last_not_of(" \t\r\n");
    return (b == std::string::npos) ? "" : s.substr(b, e - b + 1);
}

static char const* SeasonKey(Season s)
{
    switch (s)
    {
        case Season::SPRING: return "Spring";
        case Season::SUMMER: return "Summer";
        case Season::AUTUMN: return "Autumn";
        case Season::WINTER: return "Winter";
        default:             return "Spring";
    }
}

// Parse "weight,stateVal,intensMinPct,intensMaxPct"
// Config values are 0..100 percentages (e.g. 30,90 means 30%..90% intensity)
static bool ParseProfileEntry(std::string const& raw, WeatherEntry& out)
{
    float w = 0, sv = 0, mn = 30.0f, mx = 100.0f;
    if (std::sscanf(raw.c_str(), " %f , %f , %f , %f ", &w, &sv, &mn, &mx) == 4)
    {
        if (mx < mn) { std::swap(mn, mx); }
        out.weight    = static_cast<uint32>(w);
        out.stateVal  = static_cast<uint32>(sv);
        out.intensMin = std::clamp(mn, 0.0f, 100.0f);
        out.intensMax = std::clamp(mx, 0.0f, 100.0f);
        return out.weight > 0 && WeatherVibeCore::IsValidWeatherState(out.stateVal);
    }
    return false;
}

// Collect repeated-key values for a base key with .1, .2, .3, … suffixes.
// Uses GetKeysByString to discover which keys actually exist in config,
// then reads only those — no probing non-existent keys, no warnings.
static std::vector<std::string> GetMultiOption(std::string const& baseKey)
{
    std::vector<std::string> results;
    std::string prefix = baseKey + ".";

    // GetKeysByString returns all loaded config keys that start with the prefix
    std::vector<std::string> keys = sConfigMgr->GetKeysByString(prefix);

    // Extract numeric indices, filter out non-numeric suffixes (e.g. "Transition.X")
    std::vector<uint32> indices;
    for (auto const& key : keys)
    {
        std::string suffix = key.substr(prefix.size());

        char* end = nullptr;
        unsigned long idx = std::strtoul(suffix.c_str(), &end, 10);
        if (end != suffix.c_str() && *end == '\0' && idx > 0)
        {
            indices.push_back(static_cast<uint32>(idx));
        }
    }

    std::sort(indices.begin(), indices.end());

    for (uint32 idx : indices)
    {
        std::ostringstream oss;
        oss << baseKey << "." << idx;
        std::string vn = Trim(sConfigMgr->GetOption<std::string>(oss.str(), ""));
        if (!vn.empty()) { results.push_back(vn); }
    }

    return results;
}

static TransitionSettings LoadTransitionSettings(std::string const& profileName)
{
    TransitionSettings ts;
    auto key = [&](char const* suffix) -> std::string
    {
        return "WeatherVibe.Profile." + profileName + ".Transition." + suffix;
    };

    uint32 stepMin = sConfigMgr->GetOption<uint32>(key("Step.TimeSeconds.Min"), 30);
    uint32 stepMax = sConfigMgr->GetOption<uint32>(key("Step.TimeSeconds.Max"), 60);
    if (stepMax < stepMin) { std::swap(stepMin, stepMax); }

    ts.stepTimeMinMs   = stepMin * 1000;
    ts.stepTimeMaxMs   = stepMax * 1000;
    ts.changesPerHr    = std::max(1u, sConfigMgr->GetOption<uint32>(key("Max.Changes.Per.Hour"), 10));
    ts.maxConsecSame   = sConfigMgr->GetOption<uint32>(key("Max.Consecutive.Same"), 3);
    ts.stepSizePct     = std::clamp(sConfigMgr->GetOption<float>(key("StepSize.Perc"), 5.0f), 0.5f, 50.0f);

    return ts;
}

// Weather family classification — determines how cross-state transitions behave.
// Same family: directional ramp (upgrade ramps up through 100%, downgrade fades down through 0%).
// Different family: always fade old to 0%, build new from 0%.
enum class WeatherFamily : uint8
{
    FINE,
    FOG,
    RAIN,
    SNOW,
    SANDSTORM,
    THUNDER,
    UNKNOWN
};

static WeatherFamily GetWeatherFamily(uint32 stateVal)
{
    switch (stateVal)
    {
    case WEATHER_STATE_FINE:                                                        return WeatherFamily::FINE;
    case WEATHER_STATE_FOG:                                                         return WeatherFamily::FOG;
    case WEATHER_STATE_LIGHT_RAIN: case WEATHER_STATE_MEDIUM_RAIN: case WEATHER_STATE_HEAVY_RAIN: return WeatherFamily::RAIN;
    case WEATHER_STATE_LIGHT_SNOW: case WEATHER_STATE_MEDIUM_SNOW: case WEATHER_STATE_HEAVY_SNOW: return WeatherFamily::SNOW;
    case WEATHER_STATE_LIGHT_SANDSTORM: case WEATHER_STATE_MEDIUM_SANDSTORM: case WEATHER_STATE_HEAVY_SANDSTORM: return WeatherFamily::SANDSTORM;
    case WEATHER_STATE_THUNDERS:                                                    return WeatherFamily::THUNDER;
    default:                                                                        return WeatherFamily::UNKNOWN;
    }
}

static bool IsSameFamily(uint32 stateA, uint32 stateB)
{
    WeatherFamily a = GetWeatherFamily(stateA);
    WeatherFamily b = GetWeatherFamily(stateB);
    // FINE, FOG, THUNDER, UNKNOWN are each their own family — never "same" with anything else
    if (a == WeatherFamily::FINE || a == WeatherFamily::FOG || a == WeatherFamily::THUNDER || a == WeatherFamily::UNKNOWN) { return false; }
    return a == b;
}

// Weather intensity tier — used to enforce natural escalation.
// Weather can only move ±1 tier within the same family.
// Cross-family transitions can only enter at Tier 1 (light).
// Tier 0 (standalone) types are always reachable from any state.
static uint8 GetWeatherTier(uint32 stateVal)
{
    switch (stateVal)
    {
        // Tier 0 — standalone
    case WEATHER_STATE_FINE:
    case WEATHER_STATE_FOG:
    case WEATHER_STATE_THUNDERS:
        return 0;
        // Tier 1 — light
    case WEATHER_STATE_LIGHT_RAIN:
    case WEATHER_STATE_LIGHT_SNOW:
    case WEATHER_STATE_LIGHT_SANDSTORM:
        return 1;
        // Tier 2 — medium
    case WEATHER_STATE_MEDIUM_RAIN:
    case WEATHER_STATE_MEDIUM_SNOW:
    case WEATHER_STATE_MEDIUM_SANDSTORM:
        return 2;
        // Tier 3 — heavy
    case WEATHER_STATE_HEAVY_RAIN:
    case WEATHER_STATE_HEAVY_SNOW:
    case WEATHER_STATE_HEAVY_SANDSTORM:
        return 3;
    default:
        return 0;
    }
}

// Determine if a transition from currentState to candidateState is natural.
// Rules:
//   - Standalone types (Fine, Fog, Thunder) are always eligible
//   - Same family: allowed if within ±1 tier (Light→Medium, not Light→Heavy)
//   - Cross-family: only light variants (Tier 1) are eligible entry points
static bool IsNaturalTransition(uint32 currentState, uint32 candidateState)
{
    uint8 candidateTier = GetWeatherTier(candidateState);

    // Standalone types are always reachable
    if (candidateTier == 0) { return true; }

    uint8 currentTier = GetWeatherTier(currentState);

    // From standalone (Tier 0): can only reach Tier 1 (light variants)
    if (currentTier == 0) { return candidateTier == 1; }

    WeatherFamily currentFamily = GetWeatherFamily(currentState);
    WeatherFamily candidateFamily = GetWeatherFamily(candidateState);

    if (currentFamily == candidateFamily)
    {
        // Same family: allow ±1 tier step
        int diff = static_cast<int>(candidateTier) - static_cast<int>(currentTier);
        return diff >= -1 && diff <= 1;
    }
    else
    {
        // Cross-family: can only enter at Tier 1 (light)
        return candidateTier == 1;
    }
}

// ============================================================
// Weighted random selection
// ============================================================
static WeatherEntry const* PickEntry(SeasonProfile const& profile, uint32 currentStateVal,
                                      uint32 maxConsecSame, uint32 consecCount)
{
    if (profile.empty()) { return nullptr; }

    bool doExcludeConsec = (consecCount >= maxConsecSame);

    // First pass: collect entries that are natural transitions AND not excluded by consecutive limit
    std::vector<size_t> eligible;
    uint32 totalWeight = 0;

    for (size_t i = 0; i < profile.size(); ++i)
    {
        if (!IsNaturalTransition(currentStateVal, profile[i].stateVal)) { continue; }
        if (doExcludeConsec && profile[i].stateVal == currentStateVal) { continue; }
        eligible.push_back(i);
        totalWeight += profile[i].weight;
    }

    // Second pass fallback: relax consecutive exclusion but keep natural transition filter
    if (eligible.empty() || totalWeight == 0)
    {
        eligible.clear();
        totalWeight = 0;
        for (size_t i = 0; i < profile.size(); ++i)
        {
            if (!IsNaturalTransition(currentStateVal, profile[i].stateVal)) { continue; }
            eligible.push_back(i);
            totalWeight += profile[i].weight;
        }
    }

    // Third pass fallback: allow anything (profile might only have non-natural entries)
    if (eligible.empty() || totalWeight == 0)
    {
        eligible.clear();
        totalWeight = 0;
        for (size_t i = 0; i < profile.size(); ++i)
        {
            eligible.push_back(i);
            totalWeight += profile[i].weight;
        }
    }

    if (totalWeight == 0) { return nullptr; }

    uint32 roll = std::uniform_int_distribution<uint32>(0, totalWeight - 1)(Rng());
    uint32 acc  = 0;

    for (size_t idx : eligible)
    {
        acc += profile[idx].weight;
        if (roll < acc) { return &profile[idx]; }
    }

    return &profile[eligible.back()];
}

// ============================================================
// Transition scheduling
// ============================================================

// Calculate the interval between weather selections, with ±25% randomisation
static uint32 CalcSelectionIntervalMs(TransitionSettings const& ts)
{
    uint32 baseMs = 3600000u / ts.changesPerHr;
    int lo = static_cast<int>(baseMs * 75 / 100);
    int hi = static_cast<int>(baseMs * 125 / 100);
    if (lo < 1000) { lo = 1000; }
    if (hi < lo)   { hi = lo;   }
    return static_cast<uint32>(RandInt(lo, hi));
}

// Within same family, determines if target is a stronger effect (upgrade).
// Only meaningful for Rain, Snow, Sandstorm families.
static bool IsUpgrade(uint32 currentState, uint32 targetState)
{
    return targetState > currentState;
}

static void ScheduleNextEvent(ZoneProfile& zp, Season currentSeason)
{
    SeasonProfile const& profile = zp.seasons[(size_t)currentSeason];
    ZoneTransition&      st      = zp.state;
    TransitionSettings&  ts      = zp.transition;

    if (profile.empty()) { return; }

    // Enforce changes-per-hour — when reached, wait for hour reset
    if (st.changesThisHour >= ts.changesPerHr)
    {
        st.waitingForInterval = true;
        st.intervalTimerMs    = st.hourTimerMs;
        return;
    }

    WeatherEntry const* entry = PickEntry(profile, st.currentStateVal, ts.maxConsecSame, st.consecSameCount);
    if (!entry) { return; }

    bool samePick = (entry->stateVal == st.currentStateVal);

    // Target intensity is always a random value within the entry's full range
    float targetPct = RandFloat(entry->intensMin, entry->intensMax);

    if (samePick)
    {
        st.consecSameCount++;
    }
    else
    {
        st.consecSameCount = 1;
    }

    st.targetStateVal = entry->stateVal;
    st.targetPct      = targetPct;
    st.inTransition   = true;
    st.waitingForInterval = false;
    st.changesThisHour++;

    // Start interval timer — total time budget for this weather selection.
    // Includes fadeOut + fadeIn + hold at target. Next selection when it expires.
    st.intervalTimerMs = CalcSelectionIntervalMs(ts);

    // Cross-state transition: determine fade-out and fade-in points
    if (!samePick && st.currentPct > 0.0f)
    {
        st.sameFamily = IsSameFamily(st.currentStateVal, entry->stateVal);
        st.isUpgrade  = st.sameFamily && IsUpgrade(st.currentStateVal, entry->stateVal);
        st.phase      = TransitionPhase::FADE_OUT;

        if (st.sameFamily)
        {
            st.fadeOutPct = st.isUpgrade ? 100.0f : 0.0f;
            st.fadeInPct  = st.isUpgrade ? 0.0f   : 100.0f;
        }
        else
        {
            st.fadeOutPct = 0.0f;
            st.fadeInPct  = 0.0f;
        }
    }
    else
    {
        st.phase = TransitionPhase::NONE;
    }

    st.stepTimerMs = static_cast<uint32>(RandInt(
        static_cast<int>(ts.stepTimeMinMs),
        static_cast<int>(ts.stepTimeMaxMs)
    ));
}

// ============================================================
// Per-zone tick
// ============================================================
static void TickZone(ZoneProfile& zp, uint32 diff, Season currentSeason)
{
    ZoneTransition&     st  = zp.state;
    TransitionSettings& ts  = zp.transition;

    // ---- Hourly rate counter reset ----
    if (st.hourTimerMs <= diff)
    {
        st.hourTimerMs     = 3600000u;
        st.changesThisHour = 0;
    }
    else
    {
        st.hourTimerMs -= diff;
    }

    // ---- Interval timer always counts down (during transitions AND waiting) ----
    if (st.intervalTimerMs > diff)
    {
        st.intervalTimerMs -= diff;
    }
    else
    {
        st.intervalTimerMs = 0;
    }

    // ---- Waiting for interval to expire (transition already done) ----
    if (st.waitingForInterval)
    {
        if (st.intervalTimerMs == 0)
        {
            st.waitingForInterval = false;
            ScheduleNextEvent(zp, currentSeason);
        }
        return;
    }

    // ---- Nothing scheduled yet ----
    if (!st.inTransition)
    {
        ScheduleNextEvent(zp, currentSeason);
        return;
    }

    // ---- Active transition: step timer running ----
    if (st.stepTimerMs > diff)
    {
        st.stepTimerMs -= diff;
        return;
    }

    float stepSize = ts.stepSizePct;

    auto nextStepTimer = [&]()
    {
        return static_cast<uint32>(RandInt(
            static_cast<int>(ts.stepTimeMinMs),
            static_cast<int>(ts.stepTimeMaxMs)
        ));
    };

    // Helper: step currentPct toward a goal by stepSize, return true if arrived
    auto stepToward = [&](float goal) -> bool
    {
        float remaining = goal - st.currentPct;

        if (std::abs(remaining) <= stepSize)
        {
            st.currentPct = goal;
            return true;
        }

        st.currentPct += (remaining > 0.0f ? stepSize : -stepSize);
        return false;
    };

    // ---- Fade out: old state fading toward its exit point ----
    if (st.phase == TransitionPhase::FADE_OUT)
    {
        bool done = stepToward(st.fadeOutPct);

        float sendPct = std::clamp(st.currentPct, 0.0f, 100.0f);
        sWeatherVibeCore.PushWeatherPercent(zp.zoneId, static_cast<WeatherState>(st.currentStateVal), sendPct);

        if (done)
        {
            // Switch to new state — fadeIn starts on the very next tick (no gap)
            st.currentStateVal = st.targetStateVal;
            st.currentPct      = st.fadeInPct;
            st.phase           = TransitionPhase::FADE_IN;
            st.stepTimerMs     = 0;
        }
        else
        {
            st.stepTimerMs = nextStepTimer();
        }
        return;
    }

    // ---- Fade in: new state fading toward its target ----
    if (st.phase == TransitionPhase::FADE_IN)
    {
        bool done = stepToward(st.targetPct);

        float sendPct = std::clamp(st.currentPct, 0.0f, 100.0f);
        sWeatherVibeCore.PushWeatherPercent(zp.zoneId, static_cast<WeatherState>(st.targetStateVal), sendPct);

        if (done)
        {
            st.phase           = TransitionPhase::NONE;
            st.currentStateVal = st.targetStateVal;
            st.inTransition    = false;
            st.waitingForInterval = true;
        }
        else
        {
            st.stepTimerMs = nextStepTimer();
        }
        return;
    }

    // ---- Same-state transition: step toward target ----
    bool arrived = stepToward(st.targetPct);

    float sendPct = std::clamp(st.currentPct, 0.0f, 100.0f);
    sWeatherVibeCore.PushWeatherPercent(zp.zoneId, static_cast<WeatherState>(st.targetStateVal), sendPct);

    if (arrived)
    {
        st.currentStateVal = st.targetStateVal;
        st.inTransition    = false;
        st.waitingForInterval = true;
    }
    else
    {
        st.stepTimerMs = nextStepTimer();
    }
}

// Parse "zoneId,ProfileName" from a single config value
static bool ParseZoneProfileMapping(std::string const& raw, uint32& outZoneId, std::string& outName)
{
    size_t comma = raw.find(',');
    if (comma == std::string::npos || comma == 0) { return false; }

    std::string zoneStr = Trim(raw.substr(0, comma));
    std::string name    = Trim(raw.substr(comma + 1));

    if (zoneStr.empty() || name.empty()) { return false; }

    char* end = nullptr;
    unsigned long z = std::strtoul(zoneStr.c_str(), &end, 10);
    if (end == zoneStr.c_str() || z == 0) { return false; }

    outZoneId = static_cast<uint32>(z);
    outName   = name;
    return true;
}

// ============================================================
// Init — load all profiles from config
// ============================================================
void WeatherVibeEngine_Init()
{
    gZoneProfiles.clear();

    // Zone profile mappings use the .1, .2, .3 suffix scheme:
    //   WeatherVibe.ZoneProfile.1 = 1,DunMorogh
    //   WeatherVibe.ZoneProfile.2 = 12,ElwynnForest
    auto mappings = GetMultiOption("WeatherVibe.ZoneProfile");

    std::vector<std::pair<uint32, std::string>> zoneProfilePairs;

    for (auto const& raw : mappings)
    {
        uint32 zoneId = 0;
        std::string profileName;
        if (ParseZoneProfileMapping(raw, zoneId, profileName))
        {
            zoneProfilePairs.push_back({ zoneId, profileName });
        }
        else
        {
            LOG_WARN("server.loading",
                "[WeatherVibe] engine: could not parse zone profile mapping '{}'", raw);
        }
    }

    if (zoneProfilePairs.empty())
    {
        LOG_INFO("server.loading", "[WeatherVibe] engine: no zone profiles configured");
        return;
    }

    Season seasons[4] = { Season::SPRING, Season::SUMMER, Season::AUTUMN, Season::WINTER };

    for (auto const& [zoneId, profileName] : zoneProfilePairs)
    {
        ZoneProfile zp;
        zp.name   = profileName;
        zp.zoneId = zoneId;

        bool anyEntries = false;

        for (Season s : seasons)
        {
            std::ostringstream baseKey;
            baseKey << "WeatherVibe.Profile." << profileName << "." << SeasonKey(s);

            auto lines = GetMultiOption(baseKey.str());
            SeasonProfile& sp = zp.seasons[(size_t)s];

            for (auto const& line : lines)
            {
                WeatherEntry entry;
                if (ParseProfileEntry(line, entry))
                {
                    sp.push_back(entry);
                    anyEntries = true;
                }
                else
                {
                    LOG_WARN("server.loading",
                        "[WeatherVibe] engine: could not parse profile entry '{}' for {}.{}",
                        line, profileName, SeasonKey(s));
                }
            }
        }

        if (!anyEntries)
        {
            LOG_WARN("server.loading",
                "[WeatherVibe] engine: profile '{}' (zone {}) has no valid entries, skipping",
                profileName, zoneId);
            continue;
        }

        zp.transition = LoadTransitionSettings(profileName);

        zp.state                  = {};
        zp.state.currentStateVal    = WEATHER_STATE_FINE;
        zp.state.currentPct         = 0.0f;
        zp.state.targetStateVal     = WEATHER_STATE_FINE;
        zp.state.targetPct          = 0.0f;
        zp.state.hourTimerMs        = 3600000u;
        zp.state.inTransition       = false;
        zp.state.waitingForInterval = false;
        zp.state.phase              = TransitionPhase::NONE;

        gZoneProfiles.push_back(std::move(zp));

        LOG_INFO("server.loading",
            "[WeatherVibe] engine: loaded profile '{}' -> zone {}",
            profileName, zoneId);
    }

    LOG_INFO("server.loading",
        "[WeatherVibe] engine initialised — {} zone profile(s) active",
        gZoneProfiles.size());
}

// ============================================================
// Update — called every server tick
// ============================================================
void WeatherVibeEngine_Update(uint32 diff)
{
    if (gZoneProfiles.empty()) { return; }

    Season currentSeason = sWeatherVibeCore.GetCurrentSeason();

    for (ZoneProfile& zp : gZoneProfiles)
    {
        TickZone(zp, diff, currentSeason);
    }
}

// ============================================================
// WorldScript — hooks engine into the server loop
// ============================================================
class WeatherVibe_EngineScript : public WorldScript
{
public:
    WeatherVibe_EngineScript() : WorldScript("WeatherVibe_EngineScript") {}

    void OnStartup() override
    {
        if (!sWeatherVibeCore.IsEnabled() || !sWeatherVibeCore.IsProfileEnabled())
        {
            return;
        }

        WeatherVibeEngine_Init();
    }

    void OnUpdate(uint32 diff) override
    {
        if (!sWeatherVibeCore.IsEnabled() || !sWeatherVibeCore.IsProfileEnabled())
        {
            return;
        }

        WeatherVibeEngine_Update(diff);
    }
};

// ============================================================
// Registration — called from Addmod_weather_vibeScripts()
// ============================================================
void RegisterWeatherVibeEngine()
{
    new WeatherVibe_EngineScript();
}
