#ifndef MOD_WV_CORE_H
#define MOD_WV_CORE_H

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
    bool         hasValue = false; // anti-spam guard
};

// ============================================================
// WeatherVibeCore — singleton
// ============================================================
class WeatherVibeCore
{
public:
    // Singleton accessor
    static WeatherVibeCore& Instance();

    // Lifecycle (called from WorldScript hooks in mod_weather_vibe.cpp)
    void OnStartup();

    // Config reload (also callable from .wvibe reload command)
    void LoadDayPartConfig();
    void LoadStateRanges();

    // Time/season queries
    DayPart GetCurrentDayPart() const;
    Season  GetCurrentSeason()  const;

    // Intensity mapping
    float MapPercentToRawGrade(DayPart dp, WeatherState state, float percent01) const;

    // Weather dispatch
    bool PushWeatherToClient(uint32 zoneId, WeatherState state, float rawGrade);
    void PushLastAppliedWeatherToClient(uint32 zoneId, Player* player);

    // State accessors (read-only for commands/engine)
    bool IsEnabled() const { return m_enabled; }
    bool IsDebug()   const { return m_debug;   }

    std::unordered_map<uint32, LastApplied> const& GetLastApplied() const { return m_lastApplied; }

    // Name helpers (static — no state dependency)
    static char const* WeatherStateName(WeatherState s);
    static char const* DayPartName(DayPart d);
    static char const* SeasonName(Season s);

private:
    WeatherVibeCore()  = default;
    ~WeatherVibeCore() = default;
    WeatherVibeCore(WeatherVibeCore const&)            = delete;
    WeatherVibeCore& operator=(WeatherVibeCore const&) = delete;

    // Map resolution — cached zone → base continent map lookup.
    // Populates m_zoneToMapId on first call per zone via AreaTableEntry.
    // Returns nullptr if the zone is unknown or the map is not loaded.
    Map* GetMap(uint32 zoneId);

    // Broadcast helpers — both use GetMap, no DoForAllMaps scan.
    bool BroadcastZonePacket(uint32 zoneId, WorldPacket const* packet);
    void BroadcastZoneText(uint32 zoneId, char const* text);

    // Internal helpers
    static tm    GetLocalTimeSafe();
    static int   ParseHHMM(std::string const& s, int defMinutes);
    static int   ClampMinutes(int v);
    static float ClampToCoreBounds(float g);
    static char const* DayPartTokenUpper(DayPart d);
    static char const* ConfigStateToken(WeatherState s);
    static bool  IsValidWeatherState(uint32 value);
    static Range ParseRangePair(std::string const& key, Range def);

    void ValidateDayPartStarts();

    // Module state
    bool          m_enabled     = true;
    bool          m_debug       = false;
    DayPartStarts m_starts;
    std::string   m_dayPartMode = "auto";
    std::string   m_seasonMode  = "auto";

    // Per-daypart per-WeatherState intensity ranges
    std::unordered_map<uint32, Range> m_stateRanges[(size_t)DayPart::COUNT];

    // Per-zone last-applied weather cache
    std::unordered_map<uint32, LastApplied> m_lastApplied;

    // Zone → mapId cache (populated lazily on first push per zone).
    // Avoids repeated AreaTable lookups and eliminates DoForAllMaps scans.
    std::unordered_map<uint32, uint32> m_zoneToMapId;

    static std::array<WeatherState, 12> const kAcceptedStates;

    static constexpr float kMinGrade = 0.0001f;
    static constexpr float kMaxGrade = 0.9999f;
};

// Convenience macro (mirrors AzerothCore idiom)
#define sWeatherVibeCore WeatherVibeCore::Instance()

#endif // MOD_WV_CORE_H
