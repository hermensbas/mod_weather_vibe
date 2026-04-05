// mod_weather_vibe.cpp — AzerothCore module entry point
// Registers all script hooks and initialises the module.

#include "mod_wv_core.h"

#include "Chat.h"
#include "Player.h"
#include "ScriptMgr.h"

// Forward declarations for registration functions defined in their own TUs.
void RegisterWeatherVibeCommands();
void RegisterWeatherVibeEngine();

// ============================================================
// PlayerScript — login + zone-change resend
// ============================================================
class WeatherVibe_PlayerScript : public PlayerScript
{
public:
    WeatherVibe_PlayerScript() : PlayerScript("WeatherVibe_PlayerScript", { PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_UPDATE_ZONE }) {}

    void OnPlayerLogin(Player* player) override
    {
        if (!sWeatherVibeCore.IsEnabled())
        {
            return;
        }

        if (sWeatherVibeCore.IsAnnounceEnabled())
            ChatHandler(player->GetSession()).SendSysMessage("This server is running the |cff4CFF00WeatherVibe |rmodule.");

        sWeatherVibeCore.PushLastAppliedWeatherToClient(player->GetZoneId(), player);
    }

    void OnPlayerUpdateZone(Player* player, uint32 newZone, uint32 /*newArea*/) override
    {
        if (!sWeatherVibeCore.IsEnabled())
        {
            return;
        }

        sWeatherVibeCore.PushLastAppliedWeatherToClient(newZone, player);
    }
};

// ============================================================
// WorldScript — startup initialisation (core only)
// Engine startup is handled inside mod_wv_engine.cpp.
// ============================================================
class WeatherVibe_WorldScript : public WorldScript
{
public:
    WeatherVibe_WorldScript() : WorldScript("WeatherVibe_WorldScript", { WORLDHOOK_ON_STARTUP }) {}

    void OnStartup() override
    {
        sWeatherVibeCore.OnStartup();
    }
};

// ============================================================
// Module entry point — called by AzerothCore loader
// ============================================================
void Addmod_weather_vibeScripts()
{
    new WeatherVibe_PlayerScript();
    new WeatherVibe_WorldScript();

    RegisterWeatherVibeCommands();
    RegisterWeatherVibeEngine();
}
