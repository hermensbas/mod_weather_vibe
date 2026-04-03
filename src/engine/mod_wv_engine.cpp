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

// ─────────────────────────────────────────────────────────────
// RNG
// ─────────────────────────────────────────────────────────────

static std::mt19937& Rng()
{
    static std::mt19937 rng(static_cast<uint32>(std::time(nullptr)));
    return rng;
}

static float RandFloat(float lo, float hi)
{
    if (lo >= hi) return lo;
    return std::uniform_real_distribution<float>(lo, hi)(Rng());
}

static int RandInt(int lo, int hi)
{
    if (lo >= hi) return lo;
    return std::uniform_int_distribution<int>(lo, hi)(Rng());
}

// ─────────────────────────────────────────────────────────────
// Data structures
// ─────────────────────────────────────────────────────────────

// Single weighted weather entry (parsed from config).
struct WeatherEntry
{
    uint32 weight    = 0;
    uint32 stateVal  = 0;
    float  intensMin = 30.0f;
    float  intensMax = 100.0f;
};

using SeasonProfile = std::vector<WeatherEntry>;

// Transition tuning (per profile, from config).
struct TransitionSettings
{
    uint32 stepTimeMinMs = 15000;
    uint32 stepTimeMaxMs = 30000;
    uint32 changesPerHr  = 6;
    uint32 maxConsecSame = 3;
    float  stepSizePct   = 10.0f;
    uint32 minHoldMs     = 30000;         // minimum time weather lingers after arriving
    uint32 seasonBlendMs = 600000;        // how long (ms) to blend with previous season after change
};

// Cross-state transition phase.
enum class TransitionPhase : uint8 { NONE, FADE_OUT, FADE_IN };

// Live transition state — shared by all zones using the same profile.
struct TransitionState
{
    uint32 currentStateVal  = WEATHER_STATE_FINE;
    float  currentPct       = 0.0f;

    uint32 targetStateVal   = WEATHER_STATE_FINE;
    float  targetPct        = 0.0f;

    uint32 stepTimerMs      = 0;
    bool   inTransition     = false;

    uint32 intervalTimerMs    = 0;
    bool   waitingForInterval = false;

    TransitionPhase phase   = TransitionPhase::NONE;
    bool   sameFamily       = false;
    bool   isUpgrade        = false;
    float  fadeOutPct       = 0.0f;
    float  fadeInPct        = 0.0f;

    uint32 changesThisHour  = 0;
    uint32 hourTimerMs      = 3600000u;
    uint32 consecSameCount  = 0;

    uint32 reapplyTimerMs   = 0;          // counts down to next re-broadcast

    // Season blending — smooth the transition when the season changes.
    Season prevSeason       = Season::SPRING;
    Season blendFromSeason  = Season::SPRING;   // the old season we're blending away from
    bool   seasonBlending   = false;
    uint32 seasonBlendTimer = 0;          // counts down from seasonBlendMs
    uint32 seasonBlendTotal = 0;          // snapshot of seasonBlendMs for ratio calc
};

// A named weather profile — loaded once, ticked once, broadcast to all mapped zones.
struct WeatherProfile
{
    std::string         name;
    SeasonProfile       seasons[4];
    TransitionSettings  transition;
    TransitionState     state;
    std::vector<uint32> zoneIds;
};

// ─────────────────────────────────────────────────────────────
// Engine state
// ─────────────────────────────────────────────────────────────

static std::vector<WeatherProfile> gProfiles;
static uint32 gReapplyIntervalMs = 10000;     // WeatherVibe.Profile.ReApply.PerSec (seconds -> ms)

// ─────────────────────────────────────────────────────────────
// Config helpers
// ─────────────────────────────────────────────────────────────

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

// Parse "weight,state,minPct,maxPct" from a config line.
static bool ParseProfileEntry(std::string const& raw, WeatherEntry& out)
{
    float w = 0, sv = 0, mn = 30.0f, mx = 100.0f;
    if (std::sscanf(raw.c_str(), " %f , %f , %f , %f ", &w, &sv, &mn, &mx) != 4)
        return false;

    if (mx < mn) std::swap(mn, mx);

    out.weight    = static_cast<uint32>(w);
    out.stateVal  = static_cast<uint32>(sv);
    out.intensMin = std::clamp(mn, 0.0f, 100.0f);
    out.intensMax = std::clamp(mx, 0.0f, 100.0f);
    return out.weight > 0 && WeatherVibeCore::IsValidWeatherState(out.stateVal);
}

// Read numbered config keys: baseKey.1, baseKey.2, ... (via GetKeysByString).
static std::vector<std::string> GetMultiOption(std::string const& baseKey)
{
    std::vector<std::string> results;
    std::string prefix = baseKey + ".";

    std::vector<std::string> keys = sConfigMgr->GetKeysByString(prefix);

    std::vector<uint32> indices;
    for (auto const& key : keys)
    {
        std::string suffix = key.substr(prefix.size());
        char* end = nullptr;
        unsigned long idx = std::strtoul(suffix.c_str(), &end, 10);
        if (end != suffix.c_str() && *end == '\0' && idx > 0)
            indices.push_back(static_cast<uint32>(idx));
    }

    std::sort(indices.begin(), indices.end());

    for (uint32 idx : indices)
    {
        std::ostringstream oss;
        oss << baseKey << "." << idx;
        std::string val = Trim(sConfigMgr->GetOption<std::string>(oss.str(), ""));
        if (!val.empty()) results.push_back(val);
    }

    return results;
}

// Parse "zoneId,ProfileName" from a config value.
static bool ParseZoneMapping(std::string const& raw, uint32& outZoneId, std::string& outName)
{
    size_t comma = raw.find(',');
    if (comma == std::string::npos || comma == 0) return false;

    std::string zoneStr = Trim(raw.substr(0, comma));
    std::string name    = Trim(raw.substr(comma + 1));
    if (zoneStr.empty() || name.empty()) return false;

    char* end = nullptr;
    unsigned long z = std::strtoul(zoneStr.c_str(), &end, 10);
    if (end == zoneStr.c_str() || z == 0) return false;

    outZoneId = static_cast<uint32>(z);
    outName   = name;
    return true;
}

static TransitionSettings LoadTransitionSettings(std::string const& profileName)
{
    TransitionSettings ts;

    auto key = [&](char const* suffix) {
        return "WeatherVibe.Profile." + profileName + ".Transition." + suffix;
    };

    uint32 stepMin = sConfigMgr->GetOption<uint32>(key("Step.TimeSeconds.Min"), 15);
    uint32 stepMax = sConfigMgr->GetOption<uint32>(key("Step.TimeSeconds.Max"), 30);
    if (stepMax < stepMin) std::swap(stepMin, stepMax);

    ts.stepTimeMinMs = stepMin * 1000;
    ts.stepTimeMaxMs = stepMax * 1000;
    ts.changesPerHr  = std::max(1u, sConfigMgr->GetOption<uint32>(key("Max.Changes.Per.Hour"), 6));
    ts.maxConsecSame = sConfigMgr->GetOption<uint32>(key("Max.Consecutive.Same"), 3);
    ts.stepSizePct   = std::clamp(sConfigMgr->GetOption<float>(key("StepSize.Perc"), 10.0f), 0.5f, 50.0f);
    ts.minHoldMs     = sConfigMgr->GetOption<uint32>(key("MinHoldSeconds"), 30) * 1000;
    ts.seasonBlendMs = std::min(sConfigMgr->GetOption<uint32>(key("SeasonBlendMinutes"), 10), 60u) * 60u * 1000u;

    return ts;
}

// ─────────────────────────────────────────────────────────────
// Weather family & tier classification
// ─────────────────────────────────────────────────────────────

enum class WeatherFamily : uint8 { FINE, FOG, RAIN, SNOW, SANDSTORM, THUNDER, UNKNOWN };

static WeatherFamily GetWeatherFamily(uint32 sv)
{
    switch (sv)
    {
        case WEATHER_STATE_FINE:             return WeatherFamily::FINE;
        case WEATHER_STATE_FOG:              return WeatherFamily::FOG;
        case WEATHER_STATE_LIGHT_RAIN:
        case WEATHER_STATE_MEDIUM_RAIN:
        case WEATHER_STATE_HEAVY_RAIN:       return WeatherFamily::RAIN;
        case WEATHER_STATE_LIGHT_SNOW:
        case WEATHER_STATE_MEDIUM_SNOW:
        case WEATHER_STATE_HEAVY_SNOW:       return WeatherFamily::SNOW;
        case WEATHER_STATE_LIGHT_SANDSTORM:
        case WEATHER_STATE_MEDIUM_SANDSTORM:
        case WEATHER_STATE_HEAVY_SANDSTORM:  return WeatherFamily::SANDSTORM;
        case WEATHER_STATE_THUNDERS:         return WeatherFamily::THUNDER;
        default:                             return WeatherFamily::UNKNOWN;
    }
}

static bool IsSameFamily(uint32 a, uint32 b)
{
    WeatherFamily fa = GetWeatherFamily(a);
    WeatherFamily fb = GetWeatherFamily(b);
    if (fa == WeatherFamily::FINE || fa == WeatherFamily::FOG ||
        fa == WeatherFamily::THUNDER || fa == WeatherFamily::UNKNOWN)
        return false;
    return fa == fb;
}

// Intensity tier: 0 = standalone, 1 = light, 2 = medium, 3 = heavy.
static uint8 GetWeatherTier(uint32 sv)
{
    switch (sv)
    {
        case WEATHER_STATE_FINE:
        case WEATHER_STATE_FOG:
        case WEATHER_STATE_THUNDERS:           return 0;
        case WEATHER_STATE_LIGHT_RAIN:
        case WEATHER_STATE_LIGHT_SNOW:
        case WEATHER_STATE_LIGHT_SANDSTORM:    return 1;
        case WEATHER_STATE_MEDIUM_RAIN:
        case WEATHER_STATE_MEDIUM_SNOW:
        case WEATHER_STATE_MEDIUM_SANDSTORM:   return 2;
        case WEATHER_STATE_HEAVY_RAIN:
        case WEATHER_STATE_HEAVY_SNOW:
        case WEATHER_STATE_HEAVY_SANDSTORM:    return 3;
        default:                               return 0;
    }
}

static bool IsUpgrade(uint32 current, uint32 target)
{
    return GetWeatherTier(target) > GetWeatherTier(current);
}

// Natural escalation: standalone always reachable, +/-1 tier within family,
// cross-family only at tier 1.
static bool IsNaturalTransition(uint32 current, uint32 candidate)
{
    uint8 cTier = GetWeatherTier(candidate);
    if (cTier == 0) return true;

    uint8 curTier = GetWeatherTier(current);
    if (curTier == 0) return cTier == 1;

    if (GetWeatherFamily(current) == GetWeatherFamily(candidate))
    {
        int d = static_cast<int>(cTier) - static_cast<int>(curTier);
        return d >= -1 && d <= 1;
    }

    return cTier == 1;
}

// ─────────────────────────────────────────────────────────────
// Fog bridge boost
// ─────────────────────────────────────────────────────────────

// Returns true when fog should be preferred as a natural bridge between
// clear weather and precipitation (or vice versa).
static bool ShouldBoostFog(uint32 currentState, uint32 candidateState)
{
    if (GetWeatherFamily(candidateState) != WeatherFamily::FOG)
        return false;

    WeatherFamily curFam = GetWeatherFamily(currentState);

    // Fine -> Fog  (fog as "something's coming")
    if (curFam == WeatherFamily::FINE)
        return true;

    // Light precipitation -> Fog  (fog as "clearing up")
    if (GetWeatherTier(currentState) == 1 &&
        (curFam == WeatherFamily::RAIN || curFam == WeatherFamily::SNOW ||
         curFam == WeatherFamily::SANDSTORM))
        return true;

    return false;
}

static constexpr float FOG_BRIDGE_MULTIPLIER = 1.5f;

// ─────────────────────────────────────────────────────────────
// Weighted random selection (three-pass: natural+consec -> natural -> any)
// ─────────────────────────────────────────────────────────────

static WeatherEntry const* PickEntry(SeasonProfile const& profile, uint32 currentState,
                                      uint32 maxConsecSame, uint32 consecCount)
{
    if (profile.empty()) return nullptr;

    bool excludeConsec = (consecCount >= maxConsecSame);

    for (int pass = 0; pass < 3; ++pass)
    {
        std::vector<size_t> eligible;
        std::vector<uint32> effectiveWeights;
        uint32 totalWeight = 0;

        for (size_t i = 0; i < profile.size(); ++i)
        {
            if (pass < 2 && !IsNaturalTransition(currentState, profile[i].stateVal))
                continue;
            if (pass == 0 && excludeConsec && profile[i].stateVal == currentState)
                continue;

            uint32 w = profile[i].weight;

            // Boost fog when it serves as a natural bridge between clear
            // skies and precipitation (or vice versa).
            if (ShouldBoostFog(currentState, profile[i].stateVal))
                w = static_cast<uint32>(static_cast<float>(w) * FOG_BRIDGE_MULTIPLIER);

            eligible.push_back(i);
            effectiveWeights.push_back(w);
            totalWeight += w;
        }

        if (eligible.empty() || totalWeight == 0)
            continue;

        uint32 roll = std::uniform_int_distribution<uint32>(0, totalWeight - 1)(Rng());
        uint32 acc = 0;
        for (size_t j = 0; j < eligible.size(); ++j)
        {
            acc += effectiveWeights[j];
            if (roll < acc) return &profile[eligible[j]];
        }
        return &profile[eligible.back()];
    }

    return nullptr;
}

// ─────────────────────────────────────────────────────────────
// Scheduling & transition
// ─────────────────────────────────────────────────────────────

// Calculate interval between weather selections (~3600000/changesPerHr, +/-25%).
static uint32 CalcSelectionIntervalMs(TransitionSettings const& ts)
{
    uint32 baseMs = 3600000u / ts.changesPerHr;
    int lo = static_cast<int>(baseMs * 75 / 100);
    int hi = static_cast<int>(baseMs * 125 / 100);
    if (lo < 1000) lo = 1000;
    if (hi < lo)   hi = lo;
    return static_cast<uint32>(RandInt(lo, hi));
}

// Broadcast weather to all zones mapped to this profile (keeps them synced).
static void BroadcastWeather(WeatherProfile const& prof, WeatherState state, float pct)
{
    for (uint32 zoneId : prof.zoneIds)
        sWeatherVibeCore.PushWeatherPercent(zoneId, state, pct);
}

// Broadcast a debug message to all zones mapped to this profile.
static void BroadcastDebugText(WeatherProfile const& prof, char const* text)
{
    for (uint32 zoneId : prof.zoneIds)
        sWeatherVibeCore.BroadcastZoneText(zoneId, text);
}

// Step intensity toward a goal by fixed step size. Returns true when arrived.
static bool StepToward(float& current, float goal, float stepSize)
{
    float remaining = goal - current;
    if (std::abs(remaining) <= stepSize)
    {
        current = goal;
        return true;
    }
    current += (remaining > 0.0f ? stepSize : -stepSize);
    return false;
}

// Forward declaration.
static void ScheduleNextEvent(WeatherProfile& prof, Season season);

// Ensure the interval timer has at least minHoldMs remaining so weather
// lingers long enough for players to experience it after a transition.
static void EnforceMinHold(TransitionState& st, TransitionSettings const& ts)
{
    if (ts.minHoldMs > 0 && st.intervalTimerMs < ts.minHoldMs)
        st.intervalTimerMs = ts.minHoldMs;
}

// ─────────────────────────────────────────────────────────────
// Per-profile tick (called once per profile per server tick)
// ─────────────────────────────────────────────────────────────

static void TickProfile(WeatherProfile& prof, uint32 diff, Season season)
{
    TransitionState&    st = prof.state;
    TransitionSettings& ts = prof.transition;

    // ── Season change detection ──
    if (season != st.prevSeason)
    {
        if (ts.seasonBlendMs > 0)
        {
            st.blendFromSeason  = st.prevSeason;  // snapshot before overwrite
            st.seasonBlending   = true;
            st.seasonBlendTimer = ts.seasonBlendMs;
            st.seasonBlendTotal = ts.seasonBlendMs;

            if (sWeatherVibeCore.IsDebug())
            {
                LOG_INFO("server.loading",
                    "[WeatherVibe] '{}' season changed {} -> {}, blending for {}s",
                    prof.name, SeasonKey(st.prevSeason), SeasonKey(season),
                    ts.seasonBlendMs / 1000);

                char buf[256];
                snprintf(buf, sizeof(buf),
                    "|cff00ff00[WeatherVibe]|r [DEBUG][SCHEDULER] '%s' season %s -> %s, blending %us",
                    prof.name.c_str(), SeasonKey(st.prevSeason), SeasonKey(season),
                    ts.seasonBlendMs / 1000);
                BroadcastDebugText(prof, buf);
            }
        }
        st.prevSeason = season;
    }

    // Tick down blend timer.
    if (st.seasonBlending)
    {
        if (st.seasonBlendTimer <= diff)
            st.seasonBlending = false;
        else
            st.seasonBlendTimer -= diff;
    }

    // Hourly counter reset.
    if (st.hourTimerMs <= diff) { st.hourTimerMs = 3600000u; st.changesThisHour = 0; }
    else                        { st.hourTimerMs -= diff; }

    // Interval timer always counts down (during transitions AND hold).
    st.intervalTimerMs = (st.intervalTimerMs > diff) ? st.intervalTimerMs - diff : 0;

    // Waiting for interval after transition completed — hold at current weather.
    if (st.waitingForInterval)
    {
        if (st.intervalTimerMs == 0)
        {
            st.waitingForInterval = false;

            if (sWeatherVibeCore.IsDebug())
            {
                LOG_INFO("server.loading",
                    "[WeatherVibe] '{}' interval expired, selecting next weather (changes {}/{})",
                    prof.name, st.changesThisHour, ts.changesPerHr);

                char buf[256];
                snprintf(buf, sizeof(buf),
                    "|cff00ff00[WeatherVibe]|r [DEBUG][SCHEDULER] '%s' interval expired, selecting next weather (changes %u/%u)",
                    prof.name.c_str(), st.changesThisHour, ts.changesPerHr);
                BroadcastDebugText(prof, buf);
            }

            ScheduleNextEvent(prof, season);
        }
        else if (gReapplyIntervalMs > 0)
        {
            // Periodic re-broadcast while holding — ensures late joiners /
            // clients that lost state see the correct weather.  Never fires
            // during an active transition (fade-out / fade-in / stepping).
            if (st.reapplyTimerMs <= diff)
            {
                BroadcastWeather(prof,
                    static_cast<WeatherState>(st.currentStateVal),
                    std::clamp(st.currentPct, 0.0f, 100.0f));

                st.reapplyTimerMs = gReapplyIntervalMs;

               /* if (sWeatherVibeCore.IsDebug())
                {
                    LOG_INFO("server.loading",
                        "[WeatherVibe] '{}' reapply: {}% {}",
                        prof.name, static_cast<int>(st.currentPct),
                        WeatherVibeCore::WeatherStateName(
                            static_cast<WeatherState>(st.currentStateVal)));

                    char buf[256];
                    snprintf(buf, sizeof(buf),
                        "|cff00ff00[WeatherVibe]|r [DEBUG][SCHEDULER] '%s' reapply: %d%% %s",
                        prof.name.c_str(), static_cast<int>(st.currentPct),
                        WeatherVibeCore::WeatherStateName(
                            static_cast<WeatherState>(st.currentStateVal)));
                    BroadcastDebugText(prof, buf);
                }*/
            }
            else
            {
                st.reapplyTimerMs -= diff;
            }
        }
        return;
    }

    // Nothing scheduled yet — first tick or after reset.
    if (!st.inTransition)
    {
        ScheduleNextEvent(prof, season);
        return;
    }

    // Step timer still running — wait.
    if (st.stepTimerMs > diff) { st.stepTimerMs -= diff; return; }

    auto nextStep = [&]() {
        return static_cast<uint32>(RandInt(
            static_cast<int>(ts.stepTimeMinMs), static_cast<int>(ts.stepTimeMaxMs)));
    };

    // ── Fade out: old state toward exit point ──
    if (st.phase == TransitionPhase::FADE_OUT)
    {
        bool done = StepToward(st.currentPct, st.fadeOutPct, ts.stepSizePct);
        BroadcastWeather(prof, static_cast<WeatherState>(st.currentStateVal),
                         std::clamp(st.currentPct, 0.0f, 100.0f));

        if (done)
        {
            st.currentStateVal = st.targetStateVal;
            st.currentPct      = st.fadeInPct;
            st.phase           = TransitionPhase::FADE_IN;
            st.stepTimerMs     = 0;  // no gap between fadeOut and fadeIn
        }
        else { st.stepTimerMs = nextStep(); }
        return;
    }

    // ── Fade in: new state toward target ──
    if (st.phase == TransitionPhase::FADE_IN)
    {
        bool done = StepToward(st.currentPct, st.targetPct, ts.stepSizePct);
        BroadcastWeather(prof, static_cast<WeatherState>(st.targetStateVal),
                         std::clamp(st.currentPct, 0.0f, 100.0f));

        if (done)
        {
            st.phase              = TransitionPhase::NONE;
            st.currentStateVal    = st.targetStateVal;
            st.inTransition       = false;
            st.waitingForInterval = true;
            EnforceMinHold(st, ts);

            if (sWeatherVibeCore.IsDebug())
            {
                LOG_INFO("server.loading",
                    "[WeatherVibe] '{}' fadeIn arrived at {}% {}, holding for {}ms, changes {}/{}",
                    prof.name, static_cast<int>(st.currentPct),
                    WeatherVibeCore::WeatherStateName(static_cast<WeatherState>(st.currentStateVal)),
                    st.intervalTimerMs, st.changesThisHour, ts.changesPerHr);

                char buf[256];
                snprintf(buf, sizeof(buf),
                    "|cff00ff00[WeatherVibe]|r [DEBUG][SCHEDULER] '%s' fadeIn arrived at %d%% %s, holding %ums, changes %u/%u",
                    prof.name.c_str(), static_cast<int>(st.currentPct),
                    WeatherVibeCore::WeatherStateName(static_cast<WeatherState>(st.currentStateVal)),
                    st.intervalTimerMs, st.changesThisHour, ts.changesPerHr);
                BroadcastDebugText(prof, buf);
            }
        }
        else { st.stepTimerMs = nextStep(); }
        return;
    }

    // ── Same-state: step toward target ──
    bool arrived = StepToward(st.currentPct, st.targetPct, ts.stepSizePct);
    BroadcastWeather(prof, static_cast<WeatherState>(st.targetStateVal),
                     std::clamp(st.currentPct, 0.0f, 100.0f));

    if (arrived)
    {
        st.currentStateVal    = st.targetStateVal;
        st.inTransition       = false;
        st.waitingForInterval = true;
        EnforceMinHold(st, ts);

        if (sWeatherVibeCore.IsDebug())
        {
            LOG_INFO("server.loading",
                "[WeatherVibe] '{}' arrived at {}% {}, holding for {}ms, changes {}/{}",
                prof.name, static_cast<int>(st.currentPct),
                WeatherVibeCore::WeatherStateName(static_cast<WeatherState>(st.currentStateVal)),
                st.intervalTimerMs, st.changesThisHour, ts.changesPerHr);

            char buf[256];
            snprintf(buf, sizeof(buf),
                "|cff00ff00[WeatherVibe]|r [DEBUG][SCHEDULER] '%s' arrived at %d%% %s, holding %ums, changes %u/%u",
                prof.name.c_str(), static_cast<int>(st.currentPct),
                WeatherVibeCore::WeatherStateName(static_cast<WeatherState>(st.currentStateVal)),
                st.intervalTimerMs, st.changesThisHour, ts.changesPerHr);
            BroadcastDebugText(prof, buf);
        }
    }
    else { st.stepTimerMs = nextStep(); }
}

// ─────────────────────────────────────────────────────────────
// Schedule next weather selection for a profile
// ─────────────────────────────────────────────────────────────

static void ScheduleNextEvent(WeatherProfile& prof, Season season)
{
    TransitionState&     st     = prof.state;
    TransitionSettings&  ts     = prof.transition;

    // ── Build effective entry list, optionally blending two seasons ──
    SeasonProfile const& currentEntries = prof.seasons[(size_t)season];

    // During a season blend window, merge the previous season's entries with
    // scaled-down weights so the transition feels gradual rather than instant.
    SeasonProfile blended;
    SeasonProfile const* entries = &currentEntries;

    if (st.seasonBlending && st.seasonBlendTotal > 0)
    {
        SeasonProfile const& oldEntries = prof.seasons[(size_t)st.blendFromSeason];

        if (!oldEntries.empty() && !currentEntries.empty())
        {
            // blendRatio: 1.0 right after the change, fading to 0.0 at end of window.
            float blendRatio = static_cast<float>(st.seasonBlendTimer)
                             / static_cast<float>(st.seasonBlendTotal);
            blendRatio = std::clamp(blendRatio, 0.0f, 1.0f);

            // Start with the full current season entries.
            blended = currentEntries;

            // Append previous season entries with scaled weights.
            for (auto const& old : oldEntries)
            {
                WeatherEntry scaled = old;
                scaled.weight = static_cast<uint32>(
                    static_cast<float>(old.weight) * blendRatio + 0.5f);
                if (scaled.weight > 0)
                    blended.push_back(scaled);
            }

            entries = &blended;
        }
    }

    if (entries->empty()) return;

    // Hourly budget exhausted — park until hour resets.
    if (st.changesThisHour >= ts.changesPerHr)
    {
        st.waitingForInterval = true;
        st.intervalTimerMs    = st.hourTimerMs;
        return;
    }

    WeatherEntry const* entry = PickEntry(*entries, st.currentStateVal,
                                           ts.maxConsecSame, st.consecSameCount);
    if (!entry) return;

    bool samePick   = (entry->stateVal == st.currentStateVal);
    float targetPct = RandFloat(entry->intensMin, entry->intensMax);

    st.consecSameCount    = samePick ? st.consecSameCount + 1 : 1;
    st.targetStateVal     = entry->stateVal;
    st.targetPct          = targetPct;
    st.inTransition       = true;
    st.waitingForInterval = false;
    st.changesThisHour++;

    // Interval timer = total time budget (fadeOut + fadeIn + hold at target).
    st.intervalTimerMs = CalcSelectionIntervalMs(ts);

    // Determine cross-state fade direction.
    if (!samePick && st.currentPct > 0.0f)
    {
        st.sameFamily = IsSameFamily(st.currentStateVal, entry->stateVal);
        st.isUpgrade  = st.sameFamily && IsUpgrade(st.currentStateVal, entry->stateVal);
        st.phase      = TransitionPhase::FADE_OUT;

        if (st.sameFamily)
        {
            // Same family upgrade:   old->100%, new starts at 0%
            // Same family downgrade: old->0%,   new starts at 100%
            st.fadeOutPct = st.isUpgrade ? 100.0f : 0.0f;
            st.fadeInPct  = st.isUpgrade ? 0.0f   : 100.0f;
        }
        else
        {
            // Cross-family: old fades to 0%, new builds from 0%
            st.fadeOutPct = 0.0f;
            st.fadeInPct  = 0.0f;
        }
    }
    else
    {
        st.phase = TransitionPhase::NONE;
    }

    st.stepTimerMs = static_cast<uint32>(RandInt(
        static_cast<int>(ts.stepTimeMinMs), static_cast<int>(ts.stepTimeMaxMs)));

    if (sWeatherVibeCore.IsDebug())
    {
        LOG_INFO("server.loading",
            "[WeatherVibe] '{}' scheduled: {} -> {} at {}%, interval={}ms, phase={}",
            prof.name,
            WeatherVibeCore::WeatherStateName(static_cast<WeatherState>(st.currentStateVal)),
            WeatherVibeCore::WeatherStateName(static_cast<WeatherState>(st.targetStateVal)),
            static_cast<int>(st.targetPct), st.intervalTimerMs,
            st.phase == TransitionPhase::FADE_OUT ? "fadeOut" :
            st.phase == TransitionPhase::FADE_IN  ? "fadeIn"  : "direct");

        char const* phaseStr = st.phase == TransitionPhase::FADE_OUT ? "fadeOut" :
                               st.phase == TransitionPhase::FADE_IN  ? "fadeIn"  : "direct";
        char buf[256];
        snprintf(buf, sizeof(buf),
            "|cff00ff00[WeatherVibe]|r [DEBUG][SCHEDULER] '%s' scheduled: %s -> %s at %d%%, interval=%ums, phase=%s",
            prof.name.c_str(),
            WeatherVibeCore::WeatherStateName(static_cast<WeatherState>(st.currentStateVal)),
            WeatherVibeCore::WeatherStateName(static_cast<WeatherState>(st.targetStateVal)),
            static_cast<int>(st.targetPct), st.intervalTimerMs, phaseStr);
        BroadcastDebugText(prof, buf);
    }
}

// ─────────────────────────────────────────────────────────────
// Init — load profiles and zone mappings from config
// ─────────────────────────────────────────────────────────────

void WeatherVibeEngine_Init()
{
    gProfiles.clear();

    // Load reapply interval (seconds → ms, 0 = disabled).
    uint32 reapplySec = sConfigMgr->GetOption<uint32>("WeatherVibe.Profile.ReApply.PerSec", 10);
    gReapplyIntervalMs = reapplySec * 1000;

    // Parse zone-to-profile mappings and group by profile name.
    // Zones sharing the same profile name get synced weather.
    auto mappings = GetMultiOption("WeatherVibe.ZoneProfile");

    LOG_INFO("server.loading", "[WeatherVibe] found {} zone mapping(s)", mappings.size());

    std::unordered_map<std::string, std::vector<uint32>> profileZones;
    for (auto const& raw : mappings)
    {
        uint32 zoneId = 0;
        std::string name;
        if (ParseZoneMapping(raw, zoneId, name))
            profileZones[name].push_back(zoneId);
        else
            LOG_WARN("server.loading", "[WeatherVibe] bad zone mapping '{}'", raw);
    }

    if (profileZones.empty())
    {
        LOG_INFO("server.loading", "[WeatherVibe] no zone profiles configured");
        return;
    }

    // Load each unique profile once — shared state for all its zones.
    Season allSeasons[] = { Season::SPRING, Season::SUMMER, Season::AUTUMN, Season::WINTER };

    for (auto const& [profileName, zones] : profileZones)
    {
        WeatherProfile prof;
        prof.name    = profileName;
        prof.zoneIds = zones;
        bool anyEntries = false;

        for (Season s : allSeasons)
        {
            std::ostringstream baseKey;
            baseKey << "WeatherVibe.Profile." << profileName << "." << SeasonKey(s);

            for (auto const& line : GetMultiOption(baseKey.str()))
            {
                WeatherEntry entry;
                if (ParseProfileEntry(line, entry))
                {
                    prof.seasons[(size_t)s].push_back(entry);
                    anyEntries = true;
                }
                else
                {
                    LOG_WARN("server.loading", "[WeatherVibe] bad entry '{}' for {}.{}",
                        line, profileName, SeasonKey(s));
                }
            }
        }

        if (!anyEntries)
        {
            LOG_WARN("server.loading", "[WeatherVibe] profile '{}' has no entries, skipping", profileName);
            continue;
        }

        prof.transition = LoadTransitionSettings(profileName);
        prof.state      = {};
        gProfiles.push_back(std::move(prof));

        // Log with zone list.
        std::ostringstream zl;
        for (size_t i = 0; i < zones.size(); ++i) { if (i) zl << ", "; zl << zones[i]; }
        LOG_INFO("server.loading", "[WeatherVibe] profile '{}' -> zone(s) [{}]", profileName, zl.str());
    }

    LOG_INFO("server.loading", "[WeatherVibe] {} profile(s) active", gProfiles.size());

    // Stagger reapply timers so profiles don't all re-broadcast on the same tick.
    // Each profile gets an evenly-spaced offset within the interval window.
    if (gReapplyIntervalMs > 0 && gProfiles.size() > 0)
    {
        uint32 staggerStep = gReapplyIntervalMs / static_cast<uint32>(gProfiles.size());
        if (staggerStep == 0) staggerStep = 1;

        for (size_t i = 0; i < gProfiles.size(); ++i)
            gProfiles[i].state.reapplyTimerMs = staggerStep * static_cast<uint32>(i);

        LOG_INFO("server.loading",
            "[WeatherVibe] reapply every {}s, stagger step {}ms across {} profile(s)",
            gReapplyIntervalMs / 1000, staggerStep, gProfiles.size());
    }
    else
    {
        LOG_INFO("server.loading", "[WeatherVibe] reapply disabled");
    }

    // Seed prevSeason so the first tick doesn't false-trigger a blend window.
    Season initSeason = sWeatherVibeCore.GetCurrentSeason();
    for (auto& p : gProfiles)
    {
        p.state.prevSeason      = initSeason;
        p.state.blendFromSeason = initSeason;
    }
}

// ─────────────────────────────────────────────────────────────
// Update — called every server tick
// ─────────────────────────────────────────────────────────────

void WeatherVibeEngine_Update(uint32 diff)
{
    if (gProfiles.empty()) return;

    Season season = sWeatherVibeCore.GetCurrentSeason();
    for (WeatherProfile& prof : gProfiles)
        TickProfile(prof, diff, season);
}

// ─────────────────────────────────────────────────────────────
// WorldScript
// ─────────────────────────────────────────────────────────────

class WeatherVibe_EngineScript : public WorldScript
{
public:
    WeatherVibe_EngineScript() : WorldScript("WeatherVibe_EngineScript") {}

    void OnStartup() override
    {
        if (!sWeatherVibeCore.IsEnabled() || !sWeatherVibeCore.IsProfileEnabled()) return;
        WeatherVibeEngine_Init();
    }

    void OnUpdate(uint32 diff) override
    {
        if (!sWeatherVibeCore.IsEnabled() || !sWeatherVibeCore.IsProfileEnabled()) return;
        WeatherVibeEngine_Update(diff);
    }
};

void RegisterWeatherVibeEngine()
{
    new WeatherVibe_EngineScript();
}
