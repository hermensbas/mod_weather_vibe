#include "mod_wv_core.h"

#include "Log.h"
#include "ScriptMgr.h"

// ============================================================
// Engine entry points
// ============================================================

// Called once on server startup after WeatherVibeCore::OnStartup().
// Load engine-specific config, build zone profiles, etc.
void WeatherVibeEngine_Init()
{
    // TODO: load zone profiles / biome mappings from config or DB

    LOG_INFO("server.loading", "[WeatherVibe] engine initialised (placeholder)");
}

// Called every engine tick (wire up via WorldScript::OnUpdate if needed).
// Drive scheduled weather changes here.
void WeatherVibeEngine_Update(uint32 /*diff*/)
{
    // TODO: evaluate zone timers, roll random transitions,
    //       call sWeatherVibeCore.PushWeatherToClient(...) when a change fires.
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
        if (!sWeatherVibeCore.IsEnabled())
        {
            return;
        }

        WeatherVibeEngine_Init();
    }

    void OnUpdate(uint32 diff) override
    {
        if (!sWeatherVibeCore.IsEnabled())
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
