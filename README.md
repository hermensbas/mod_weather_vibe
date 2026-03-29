# WeatherVibe (AzerothCore module) (beta version)

https://youtu.be/4CxG-eAT9D4

Bring your world to life with **mod_weather_vibe**. This module gives each zone a
distinct **mood**—misty mornings in Elwynn, a **gloomy** Duskwood that rumbles to life,
**biting** Wintergrasp squalls, and **rolling thunderheads** over Stranglethorn. Weather
no longer just _flips_; it **evolves** naturally over time with small shifts,
occasional bursts, and regional spillovers that make the world feel **alive** and
**immersive**.

- Per-state **intensity bands** (maps % → raw grade) with 0% = fully clear for **FINE**.
- **Auto-rotation engine**: picks states by profile weights, holds them for a window, and smoothly tweens intensities.
- **Zone parenting** (optional): capitals/starter zones can inherit a parent zone’s weather.
- **Sprinkle**: temporary overrides (e.g., “snow 40% for 30s”).
- Rich admin commands: `.wvibe set`, `.wvibe setRaw`, `.wvibe auto ...`, `.wvibe show`, `.wvibe reload`.

> ✅ **Important:** In your core config set `ActivateWeather = 0`.  
> That disables the default `WeatherMgr` so it won’t fight WeatherVibe’s packets.


TODO:
- Seasonal weight; prolly on global level to keep simple to configure.
- Profiles details, dayparts for example.
- Profile zone tweaking

---

## Contents

- [Features](#features)
- [Installation](#installation)
- [Configuration](#configuration)
  - [Core toggles & debug](#core-toggles--debug)
  - [Season & Dayparts](#season--dayparts)
  - [Intensity ranges (InternalRange)](#intensity-ranges-internalrange)
  - [Auto engine](#auto-engine)
  - [Profiles](#profiles)
  - [Zone → Profile mapping](#zone--profile-mapping)
- [Commands](#commands)
  - [Direct set](#direct-set)
  - [Auto engine controls](#auto-engine-controls)
  - [Inspect & reload](#inspect--reload)
- [Examples](#examples)
- [How percentages map to visuals](#how-percentages-map-to-visuals)
- [Troubleshooting](#troubleshooting)
- [License](#license)

---

## Features

- **Packet mode**: Sends `WorldPackets::Misc::Weather` directly to zones (and optional children).
- **Per-state intensity bands**: You define min/max raw grade per state and daypart.  
  Effects are usually **visible from ~0.30** (except **Fine** which is visible from **0.00**).
- **Profiles**: Weighted state selection + percent bands (Min/Max %) for how strong the effect should be.
- **Auto engine**: Rotates states per zone, holds them for a random window (within your min/max), and tweens.
- **Sprinkle**: Temporary override of state/percent for a duration without altering the profile.
- **Day/Season aware**: Your InternalRange bands can vary by daypart.

---

## Installation

1. Place the module in your AzerothCore `modules` folder, e.g.:
   ```
   azerothcore/modules/mod-weather-vibe/
   ```
2. Reconfigure & build AzerothCore:
   ```bash
   cmake .. -DCMAKE_BUILD_TYPE=Release
   make -j"$(nproc)"
   ```
3. Ensure your worldserver config includes:
   ```ini
   ActivateWeather = 0
   ```
4. Add the WeatherVibe config keys (see below) to your server `.conf`, then start `worldserver`.

---

## Configuration

Copy these into your server config and adjust as needed.

### Core toggles & debug

```ini
# Enable/disable the module
WeatherVibe.Enable = 1

# Broadcast debug info to the zone whenever weather is pushed (optional)
WeatherVibe.Debug = 0
```

### Season & Dayparts

```ini
# Force a season or let the module compute it by date
WeatherVibe.Season = auto                 # auto|spring|summer|autumn|winter

# Force a daypart or let the module compute it by clock and daypart starts
WeatherVibe.DayPart.Mode = auto           # auto|morning|afternoon|evening|night

# Daypart start times (HH:MM)
WeatherVibe.DayPart.MORNING.Start   = 06:00
WeatherVibe.DayPart.AFTERNOON.Start = 12:00
WeatherVibe.DayPart.EVENING.Start   = 18:00
WeatherVibe.DayPart.NIGHT.Start     = 22:00
```

### Intensity ranges (InternalRange)

These ranges map a **logical percentage (0–100%)** to a **raw grade (0.0–1.0)** per **daypart** and **weather state**.  
As a rule of thumb, visuals generally appear from **~0.30**, except **Fine** which is visible from **0.00**.

**Format (per state per daypart):**
```ini
WeatherVibe.Intensity.InternalRange.<DAYPART>.<StateName> = <minRaw>, <maxRaw>
```

**Suggested baseline (keep caps below ~0.65 for routine play):**
```ini
# Fine may be truly clear at 0.00
WeatherVibe.Intensity.InternalRange.MORNING.Fine   = 0.00, 1.00
WeatherVibe.Intensity.InternalRange.AFTERNOON.Fine = 0.00, 1.00
WeatherVibe.Intensity.InternalRange.EVENING.Fine   = 0.00, 1.00
WeatherVibe.Intensity.InternalRange.NIGHT.Fine     = 0.00, 1.00

# Typical non-fine bands (visuals from ~0.30)
WeatherVibe.Intensity.InternalRange.MORNING.LightSnow     = 0.30, 1.00
WeatherVibe.Intensity.InternalRange.AFTERNOON.LightSnow   = 0.30, 1.00
# ...repeat for other states (Fog, Light/Medium/Heavy Rain/Snow, Sandstorm, Thunders)
```

**Supported states:**  
`Fine (0)`, `Fog (1)`, `LightRain (3)`, `MediumRain (4)`, `HeavyRain (5)`,  
`LightSnow (6)`, `MediumSnow (7)`, `HeavySnow (8)`,  
`LightSandstorm (22)`, `MediumSandstorm (41)`, `HeavySandstorm (42)`,  
`Thunders (86)`.

> Note: `BlackRain`/`BlackSnow` are not used.

### Auto engine

```ini
# Master switch for auto rotation
WeatherVibe.Auto.Enable       = 0

# Engine tick granularity (ms)
WeatherVibe.Auto.TickMs       = 1000

# A picked state lives within this window before a new pick (seconds)
WeatherVibe.Auto.MinWindowSec = 180
WeatherVibe.Auto.MaxWindowSec = 480

# Seconds to smoothly ramp intensity on changes
WeatherVibe.Auto.TweenSec     = 90

# If the computed raw grade changes by less than this, skip sending (anti-spam)
WeatherVibe.Auto.TinyNudge    = 0.01

```

**What these mean (quick guide):**
- **Enable**: Turns auto on/off globally.
- **TickMs**: How often the engine processes/tweens and possibly sends packets.
- **Min/MaxWindowSec**: Each pick is held for a random time in this range.
- **TweenSec**: Duration of cross-fade toward the next target.
- **TinyNudge**: Ignore very small raw changes to avoid chatty updates.

### Profiles

Define one or more named climate profiles, each with **state weights** and a **percent band**.

```ini
# Declare profile names (comma-separated)
WeatherVibe.Profile.Names = Temperate,Tundra,Desert

# Weights: <stateId>=<weight> pairs (states missing or weight=0 won't be picked)
WeatherVibe.Profile.Tundra.Weights = 0=35,1=8,3=8,4=6,5=3,6=15,7=15,8=5,22=0,41=0,42=0,86=5

# Percent band used when the profile picks a target (logical %, later mapped via InternalRange)
WeatherVibe.Profile.Tundra.Percent.Min = 5
WeatherVibe.Profile.Tundra.Percent.Max = 60
```

**Notes:**
- The engine picks a **state** by discrete distribution of weights.
- It then picks a **percent** uniformly in `[Min, Max]`, which will be mapped to a **raw grade** using your `InternalRange` for the **current daypart** and **state**.

### Zone → Profile mapping

Assign which **controller** zones the auto engine will drive and with which profile.  
(Children can inherit via zone parenting—see next section.)

```ini
WeatherVibe.ZoneProfile.Map = 1=Temperate,3=Temperate,8=Tundra,10=Desert
```

- Key: `ZoneID`
- Value: `ProfileName` (must exist in `Profile.Names`)
- If the mapped profile is missing, **DefaultProfile** will be used (if configured).

On each push, packets are sent to the **controller** and all of its **children**.

---

## Commands

> All commands require GM **SEC_ADMINISTRATOR** and can be used from console (`Console::Yes`).

### Direct set

```
.wvibe set <zoneId> <state:uint> <percentage:0..100>
```
- Picks a state and **logical percentage**, which is mapped to raw using `InternalRange` for the **current daypart**.
- Example:
  - `.wvibe set 1 6 40` → Zone 1, `LightSnow (6)`, 40% (mapped to raw via current daypart’s `LightSnow` range).

```
.wvibe setRaw <zoneId> <state:uint> <raw:0..1>
```
- Sends the **raw grade** directly (bypasses percentage mapping).  
- Example: `.wvibe setRaw 1 6 0.55`

### Auto engine controls

```
.wvibe auto on
.wvibe auto off
```
Enable/disable the global auto engine.

```
.wvibe auto status
```
Shows engine settings and per-zone state/targets, remaining window/tween times, and sprinkle status.

```
.wvibe auto set <zoneId> <profileName|default>
```
Enable auto control for a zone with the given profile.  
Use `default` to apply `WeatherVibe.Auto.DefaultProfile`.

```
.wvibe auto clear <zoneId>
```
Disable auto control for a zone.

```
.wvibe auto sprinkle <zoneId> <state|auto> <percentage:0..100> <durationSec>
```
Apply a temporary override (e.g., “snow 50% for 30s”).  
Use `auto` to keep the current state but force a percent spike.

### Inspect & reload

```
.wvibe show
```
Lists last applied weather for each controller zone, reporting both **raw** and **mapped %** under the **current daypart**.

```
.wvibe reload
```
Reloads dayparts, ranges, profiles, zone parents, and auto config.

---

## Examples

**Always-snowing tundra zone:**
```ini
# Make Tundra favor snow heavily
WeatherVibe.Profile.Tundra.Weights = 0=10,6=40,7=35,8=15,86=5

# Stronger snow band
WeatherVibe.Profile.Tundra.Percent.Min = 25
WeatherVibe.Profile.Tundra.Percent.Max = 60

# Map the zone and enable auto
WeatherVibe.ZoneProfile.Map = 8=Tundra
WeatherVibe.Auto.Enable = 1
```

**Manual sprinkle for a short blizzard:**
```
.wvibe auto sprinkle 8 8 60 30
```
Zone `8`, `HeavySnow (8)`, 60% for 30 seconds.

---

## How percentages map to visuals

1. You (or the auto engine) produce a **logical %** in `[0..100]`.
2. WeatherVibe converts `%` → `raw` using **InternalRange** for the **current daypart** and **state**:
   ```
   raw = min + (%/100) * (max - min)
   ```
3. That `raw` is clamped safely for the engine (0 is allowed for **Fine** to be perfectly clear).
4. The packet is sent to all players in the controller zone (and optional children).

> Tip: If weather looks too weak/strong at a given %, narrow or shift the `InternalRange` for that state/daypart.

---

## Troubleshooting

- **Weather stops after a few seconds**  
  - Confirm `ActivateWeather = 0` in the core config (prevents `WeatherMgr` from overwriting).
  - If using **auto**, check `.wvibe auto status`:
    - Ensure the **profile weights** don’t heavily favor `Fine (0)` if you expect constant snow/rain.
    - Increase `WeatherVibe.Auto.MinWindowSec` / `MaxWindowSec` if states change too quickly.
  - Verify your **InternalRange** caps: a very low `max` can make effects appear to “vanish”.
- **`.wvibe set` looks different from `.wvibe setRaw`**  
  `.wvibe set` passes through **percentage mapping** (depends on your `InternalRange` and daypart).  
  `.wvibe setRaw` applies the raw grade directly (no mapping). Align your `InternalRange` with expectations.
- **No changes when sending tiny tweaks**  
  Increase or reduce `WeatherVibe.Auto.TinyNudge` (anti-spam threshold in raw grade space).
- **Profile missing on startup / zone mapping uses unknown profile**  
  Set `WeatherVibe.Auto.DefaultProfile = <ExistingProfileName>` and/or use `.wvibe auto set <zone> default`.

---

## License

This module follows the same license policy as your AzerothCore distribution unless stated otherwise in the repository.  
Contributions welcome—profiles and zone mappings are great PRs!
