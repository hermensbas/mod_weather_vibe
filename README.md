# WeatherVibe (AzerothCore module)

[![Intro!](https://i3.ytimg.com/vi/iCn14KLCerY/maxresdefault.jpg)](https://www.youtube.com/watch?v=iCn14KLCerY)

- New and final version! (the old can be found [here](https://github.com/hermensbas/mod_weather_vibe/tree/main_beta))
- demo: https://youtu.be/GRyTk5umEno
- another demo: https://youtu.be/23jTJcT-n44
- **Pull request**: tweaking weather profiles are more then welcome

Bring your world to life with **mod_weather_vibe**. This module gives each zone a
distinct **mood** — misty mornings in Elwynn, a **gloomy** Duskwood that rumbles to life,
**biting** Wintergrasp squalls, and **rolling thunderheads** over Stranglethorn. Weather
no longer just _flips_; it **evolves** naturally over time with smooth intensity
transitions, seasonal awareness, fog as a natural bridge between states, and
regional syncing that makes the world feel **alive** and **immersive**.

> ✅ **Important:** In your core config (worldserver.conf) set `ActivateWeather = 0`.
> That disables the default `WeatherMgr` so it won't fight WeatherVibe's packets.

---

## Features

- **Packet mode** — sends `WorldPackets::Misc::Weather` directly to zones via `SendZoneMessage`.
- **Per-state intensity bands** — define min/max raw grade per weather state and daypart. The engine maps a logical percentage (0–100) to the configured raw grade range.
- **Season-aware profiles** — each profile defines weighted weather entries per season (Spring, Summer, Autumn, Winter). Seasons are computed from server time or forced via config.
- **Smooth transitions** — weather doesn't snap between states. The engine fades out the old state, fades in the new one, with configurable step sizes and timing. Same-family transitions (e.g. LightSnow → HeavySnow) use crossfade; cross-family transitions (e.g. Rain → Snow) fade through zero.
- **Natural escalation** — the engine enforces tier-stepping: you can't jump from Fine to HeavyRain in one pick. Weather must escalate through Light → Medium → Heavy within a family.
- **Fog bridge boost** — fog gets a natural weight boost when transitioning between clear skies and precipitation (or vice versa), making it appear as a realistic "something's coming" or "clearing up" signal.
- **Minimum hold time** — weather lingers at its target intensity for a configurable minimum duration before the next change, so players actually experience the weather.
- **Season blending** — when the season changes, the previous season's weather entries gradually fade out over a configurable window instead of switching instantly.
- **Periodic reapply** — the current weather is periodically re-broadcast to catch late joiners or clients that lost state, with staggered timing across profiles to avoid packet bursts.
- **Zone syncing** — multiple zones sharing the same profile name get identical weather, same transitions, same timing (e.g. Stormwind synced with Elwynn Forest).
- **Login & zone-change delivery** — players receive the current weather on login and when changing zones.
- **Admin commands** — `.wvibe set`, `.wvibe setRaw`, `.wvibe show` for manual control and inspection.

---

## Contents

- [Installation](#installation)
- [Configuration](#configuration)
  - [Core toggles & debug](#core-toggles--debug)
  - [Season & Dayparts](#season--dayparts)
  - [Intensity ranges](#intensity-ranges)
  - [Weather profiles](#weather-profiles)
  - [Transition settings](#transition-settings)
  - [Zone-to-profile mapping and synced zones](#zone-to-profile-mapping-and-synced-zones)
  - [Reapply interval](#reapply-interval)
- [Commands](#commands)
- [How it works](#how-it-works)
- [Troubleshooting](#troubleshooting)
- [License](#license)

---

## Installation

1. Place the module in your AzerothCore `modules` folder:
   ```
   azerothcore/modules/mod-weather-vibe/
   ```
2. Reconfigure & build:
   ```bash
   cmake .. -DCMAKE_BUILD_TYPE=Release
   make -j"$(nproc)"
   ```
3. In your worldserver config, set:
   ```ini
   ActivateWeather = 0
   ```
4. Copy `mod_weather_vibe.conf.dist` to your config directory and adjust as needed, then start `worldserver`.

---

## Configuration

### Core toggles & debug

```ini
WeatherVibe.Enable = 1
WeatherVibe.Debug = 0

# Toggle the automatic weather profile engine.
# When disabled, manual .wvibe set / setRaw commands still work.
WeatherVibe.Profile.Enable = 1

# How often (seconds) the engine re-broadcasts the current weather
# as a safety net for late joiners. 0 = disabled.
WeatherVibe.Profile.ReApply.PerSec = 10
```

### Season & Dayparts

```ini
# auto computes from server date. Force with: spring|summer|autumn|winter
WeatherVibe.Season = auto

# auto computes from server clock. Force with: morning|afternoon|evening|night
WeatherVibe.DayPart.Mode = auto

# Daypart boundaries (HH:MM)
WeatherVibe.DayPart.MORNING.Start   = 06:00
WeatherVibe.DayPart.AFTERNOON.Start = 12:00
WeatherVibe.DayPart.EVENING.Start   = 18:00
WeatherVibe.DayPart.NIGHT.Start     = 22:00
```

### Intensity ranges

These map a **logical percentage (0–100)** to a **raw grade (0.0–1.0)** per daypart and weather state. The WoW client generally shows weather effects from ~0.30 raw grade upward; Fine is visible from 0.00 (clear sky).

```ini
# Format: WeatherVibe.Intensity.InternalRange.<DAYPART>.<StateName> = <minRaw>, <maxRaw>
WeatherVibe.Intensity.InternalRange.MORNING.Fine       = 0.00, 0.30
WeatherVibe.Intensity.InternalRange.MORNING.Fog        = 0.15, 0.30
WeatherVibe.Intensity.InternalRange.MORNING.LightRain  = 0.25, 0.50
WeatherVibe.Intensity.InternalRange.MORNING.MediumRain = 0.25, 0.70
WeatherVibe.Intensity.InternalRange.MORNING.HeavyRain  = 0.25, 0.90
# ... repeat for AFTERNOON, EVENING, NIGHT and all states
```

**Supported states:** `Fine (0)`, `Fog (1)`, `LightRain (3)`, `MediumRain (4)`, `HeavyRain (5)`, `LightSnow (6)`, `MediumSnow (7)`, `HeavySnow (8)`, `LightSandstorm (22)`, `MediumSandstorm (41)`, `HeavySandstorm (42)`, `Thunders (86)`.

### Weather profiles

Each profile defines weighted weather entries per season. Entries use numbered suffixes (`.1`, `.2`, `.3`, ...) and follow the format: `weight, weatherType, intensityMinPct, intensityMaxPct`.

```ini
# DunMorogh Spring: snow dominant, some fog, rare sleet
WeatherVibe.Profile.DunMorogh.Spring.1 = 10,0,40,70    # Fine, weight 10, 40-70%
WeatherVibe.Profile.DunMorogh.Spring.2 = 15,1,30,60    # Fog, weight 15, 30-60%
WeatherVibe.Profile.DunMorogh.Spring.3 = 5,3,30,50     # LightRain, weight 5
WeatherVibe.Profile.DunMorogh.Spring.4 = 40,6,30,50    # LightSnow, weight 40
WeatherVibe.Profile.DunMorogh.Spring.5 = 15,7,30,70    # MediumSnow, weight 15
WeatherVibe.Profile.DunMorogh.Spring.6 = 10,8,30,80    # HeavySnow, weight 10
```

The engine picks a state by weighted random selection, then picks an intensity uniformly within the entry's min/max percentage range, which is then mapped through the InternalRange for the current daypart.

### Transition settings

Each profile has its own transition tuning:

```ini
# How long (seconds) each intensity step takes (randomized within range)
WeatherVibe.Profile.DunMorogh.Transition.Step.TimeSeconds.Min = 15
WeatherVibe.Profile.DunMorogh.Transition.Step.TimeSeconds.Max = 35

# How much intensity changes per step (percentage points)
WeatherVibe.Profile.DunMorogh.Transition.StepSize.Perc = 20

# Maximum weather changes per hour (budget system)
WeatherVibe.Profile.DunMorogh.Transition.Max.Changes.Per.Hour = 12

# Force variety: max consecutive picks of the same state before forcing a change
WeatherVibe.Profile.DunMorogh.Transition.Max.Consecutive.Same = 2

# Minimum seconds weather holds at target after a transition completes
WeatherVibe.Profile.DunMorogh.Transition.MinHoldSeconds = 60

# Minutes to blend the previous season's entries after a season change (0 = instant)
WeatherVibe.Profile.DunMorogh.Transition.SeasonBlendMinutes = 10
```

### Zone-to-profile mapping and synced zones

Assign zones to profiles using numbered entries in the format `<zoneId>,<ProfileName>`.

```ini
# Format: WeatherVibe.ZoneProfile.<N> = <zoneId>,<ProfileName>
WeatherVibe.ZoneProfile.1  = 1,DunMorogh         # Dun Morogh
WeatherVibe.ZoneProfile.2  = 12,ElwynnForest      # Elwynn Forest
WeatherVibe.ZoneProfile.3  = 1519,ElwynnForest    # Stormwind synced to Elwynn
```

**How synced zones work:** When multiple zones reference the **same profile name**, the engine treats them as a single weather unit. They share one state machine — one set of timers, one transition phase, one current intensity. Every weather broadcast goes to all zones in the group simultaneously, so players in any of those zones see identical weather at all times.

This is designed for **connected areas** where it would break immersion if the weather changed as you walked through a gate or border. Typical uses:

- **Cities within their outdoor zone** — Stormwind (1519) shares the `ElwynnForest` profile with Elwynn Forest (12), so stepping through the gates doesn't change the sky.
- **Split zones that feel like one area** — Orgrimmar (1637) synced to Durotar (14), Ironforge (1537) synced to Dun Morogh (1), Thunder Bluff (1638) synced to The Barrens (17).
- **Battleground instances** — Arathi Basin (3358) shares `ArathiHighlands` so the battleground has the same weather as the outdoor zone.

Each profile name only creates **one** weather engine instance regardless of how many zones are mapped to it. Adding more zones to a profile has zero performance cost — it just adds zone IDs to the broadcast list.

### Reapply interval

The engine periodically re-broadcasts weather during hold phases to ensure late joiners see the correct weather. Timing is staggered across profiles to avoid packet bursts.
Aside of having core hook when player enters a zone changed and reapplying the weather, the reapply is simple extra measure to assure the correct weather.

```ini
# Seconds between re-broadcasts (0 = disabled, default = 30)
WeatherVibe.Profile.ReApply.PerSec = 30
```

---

## Commands

All commands require GM administrator level.

| Command | Description |
|---------|-------------|
| `.wvibe set <zoneId> <state> <percentage>` | Set weather using logical percentage (mapped through InternalRange for current daypart) |
| `.wvibe setRaw <zoneId> <state> <raw>` | Set weather using raw grade directly (0.0–1.0, bypasses mapping) |
| `.wvibe show` | List last-applied weather for all active zones |

**Examples:**
```
.wvibe set 1 6 40       # Zone 1, LightSnow (6), 40%
.wvibe setRaw 1 6 0.55  # Zone 1, LightSnow (6), raw grade 0.55
```

---

## How it works

1. **Profile loading** — on startup, the engine reads all profile entries and zone mappings from config. Profiles are loaded once; zones sharing a profile name share a single weather state machine.

2. **State selection** — the engine picks a weather state using weighted random selection with three-pass fallback: first it tries natural transitions (tier ±1) while respecting the consecutive-same limit, then natural transitions without the limit, then any entry as a last resort. Fog gets a weight boost when it would serve as a natural bridge between clear and precipitation.

3. **Transition execution** — once a target is picked, the engine steps intensity toward the target in configurable increments. Cross-state transitions use a two-phase fade: fade out the old state, then fade in the new. Same-family transitions (e.g. LightSnow → HeavySnow) crossfade smoothly. Each step broadcasts the current intensity to all zones in the profile.

4. **Hold phase** — after arriving at the target intensity, the weather holds for at least `MinHoldSeconds` or the remaining interval budget, whichever is longer. During the hold, periodic re-broadcasts ensure late joiners see the correct weather.

5. **Season awareness** — seasons are computed from server date (or forced via config). When the season changes, entries from the previous season remain eligible for a configurable blending window, with weights gradually fading to zero.

6. **Client delivery** — on login and zone change, the `PlayerScript` pushes the last-applied weather to the player's client so they immediately see the current conditions.

---

## Troubleshooting

**Weather stops after a few seconds** — confirm `ActivateWeather = 0` in your core config. The default `WeatherMgr` will overwrite WeatherVibe's packets if left enabled.

**Weather changes too quickly** — increase `MinHoldSeconds` so weather lingers longer. Decrease `Max.Changes.Per.Hour` to reduce the hourly budget. Increase `Max.Consecutive.Same` to allow longer stretches of the same state.

**Weather never changes** — check that `WeatherVibe.Profile.Enable = 1` and that the zone has a valid profile mapping. Use `.wvibe show` to see the current state. Enable `WeatherVibe.Debug = 1` to see transition events in zone chat.

**`.wvibe set` looks different from `.wvibe setRaw`** — `.wvibe set` passes through percentage mapping (depends on your InternalRange and current daypart). `.wvibe setRaw` sends the raw grade directly. If they look different, your InternalRange doesn't map linearly to 0–1.

**Weather looks too weak or too strong** — adjust the InternalRange for that state and daypart. Narrowing the range constrains how intense the effect can get; shifting it up makes even low percentages more visible.

**Zones not syncing** — ensure both zones reference the exact same profile name in the `ZoneProfile` mapping. Profile names are case-sensitive.

---

## License

This module follows the same license policy as your AzerothCore distribution unless stated otherwise in the repository. Contributions welcome — profiles and zone mappings are great PRs!
