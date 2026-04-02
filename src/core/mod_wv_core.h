#ifndef MOD_WV_CORE_H
#define MOD_WV_CORE_H

#include "Chat.h"             // ChatHandler
#include "Player.h"
#include "Weather.h"          // WeatherState enum

#include <string>
#include <unordered_map>
#include <array>

// ============================================================
// Enums & POD structs (shared across translation units)
// ============================================================
enum class Season : uint8
{
    SPRING = 0,
    SUMMER,
    AUTUMN,
    WINTER
};

enum class DayPart : uint8
{
    MORNING = 0,
    AFTERNOON,
    EVENING,
    NIGHT,
    COUNT
};

struct Range
{
    float min = 0.0f;
    float max = 1.0f;
};

struct DayPartStarts
{
    int morning   = 6  * 60;   // 06:00
    int afternoon = 12 * 60;   // 12:00
    int evening   = 18 * 60;   // 18:00
    int night     = 22 * 60;   // 22:00
};

struct LastApplied
{
    WeatherState state    = WEATHER_STATE_FINE;
    float        grade    = 0.f;
    bool         hasValue = false;
};

// ============================================================
// WeatherVibeCore — singleton
// ============================================================
class WeatherVibeCore
{
public:
    // Singleton accessor
    static WeatherVibeCore& Instance();

    // Lifecycle
    void OnStartup();

    // Config reload — reloads daypart + intensity ranges
    void ReloadConfig();

    // Time/season queries
    DayPart GetCurrentDayPart() const;
    Season  GetCurrentSeason()  const;

    // Percent: 0..100 — mapped through daypart intensity ranges
    bool PushWeatherPercent(uint32 zoneId, WeatherState state, float percentage);
    // Raw: 0..1 — sent directly, bypasses daypart range mapping
    bool PushWeatherDebug(uint32 zoneId, WeatherState state, float rawGrade);

    // Push last applied weather to players client.
    void PushLastAppliedWeatherToClient(uint32 zoneId, Player* player);

    // Last-applied read access — used by commands (.wvibe show)
    std::unordered_map<uint32, LastApplied> const& GetLastApplied() const { return m_lastApplied; }

    // State accessors
    bool IsEnabled()        const { return m_enabled; }
    bool IsProfileEnabled() const { return m_profileEnabled; }
    bool IsDebug()          const { return m_debug;   }

    // Name helpers (static — no state dependency)
    static char const* WeatherStateName(WeatherState s);
    static char const* DayPartName(DayPart d);
    static char const* SeasonName(Season s);

    // Validation
    static bool IsValidWeatherState(uint32 value);

private:
    WeatherVibeCore()  = default;
    ~WeatherVibeCore() = default;
    WeatherVibeCore(WeatherVibeCore const&)            = delete;
    WeatherVibeCore& operator=(WeatherVibeCore const&) = delete;

    // Config loading — called from OnStartup and ReloadConfig
    void LoadDayPartConfig();
    void LoadIntensityRangesConfig();

    // Core dispatch — all callers go through PushWeatherPercent
    float MapPercentToRawGrade(DayPart dp, WeatherState state, float percent01) const;
    bool  PushWeatherToClient(uint32 zoneId, WeatherState state, float rawGrade, float percentage = 0.f);

    // Map resolution — cached zone → base continent map lookup
    Map* GetMap(uint32 zoneId);

    // Broadcast helpers
    bool BroadcastZonePacket(uint32 zoneId, WorldPacket const* packet);
    void BroadcastZoneText(uint32 zoneId, char const* text);

    // Internal helpers
    static tm    GetLocalTimeSafe();
    static int   ParseHHMM(std::string const& s, int defMinutes);
    static int   ClampMinutes(int v);
    static float ClampToCoreBounds(float g);
    static char const* DayPartTokenUpper(DayPart d);
    static char const* ConfigStateToken(WeatherState s);
    static Range ParseRangePair(std::string const& key, Range def);

    void ValidateDayPartStarts();

    // Module state
    bool          m_enabled        = true;
    bool          m_profileEnabled = true;
    bool          m_debug          = false;
    DayPartStarts m_starts;

    // Cached daypart/season mode — resolved once on config load
    bool    m_dayPartAuto    = true;
    DayPart m_dayPartFixed   = DayPart::MORNING;
    bool    m_seasonAuto     = true;
    Season  m_seasonFixed    = Season::SPRING;

    // Per-daypart per-WeatherState intensity ranges
    std::unordered_map<uint32, Range> m_stateRanges[(size_t)DayPart::COUNT];

    // Per-zone last-applied weather cache
    std::unordered_map<uint32, LastApplied> m_lastApplied;

    // Zone → mapId cache (populated lazily on first push per zone)
    std::unordered_map<uint32, uint32> m_zoneToMapId;

    static std::array<WeatherState, 12> const kAcceptedStates;

    static constexpr float kMinGrade = 0.0001f;
    static constexpr float kMaxGrade = 0.9999f;
};

// Convenience macro (mirrors AzerothCore idiom)
#define sWeatherVibeCore WeatherVibeCore::Instance()

#endif // MOD_WV_CORE_H
