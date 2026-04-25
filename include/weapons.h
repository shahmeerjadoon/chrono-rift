#include "shared_memory.h"

const WeaponDef weapons[wpnCount] = {
    [wpnSolarCore]     = { wpnSolarCore,     "Solar Core",      10, 95, 1 },
    [wpnLunarBlade]    = { wpnLunarBlade,    "Lunar Blade",     10, 90, 1 },
    [wpnIronHalberd]   = { wpnIronHalberd,   "Iron Halberd",     7, 55, 0 },
    [wpnVenomDagger]   = { wpnVenomDagger,   "Venom Dagger",     4, 30, 0 },
    [wpnThunderstaff]  = { wpnThunderstaff,  "Thunderstaff",     6, 50, 0 },
    [wpnObsidianAxe]   = { wpnObsidianAxe,   "Obsidian Axe",     5, 45, 0 },
    [wpnFrostbow]      = { wpnFrostbow,      "Frostbow",         6, 48, 0 },
    [wpnSplinterStick] = { wpnSplinterStick, "Splinter Stick",   2, 12, 0 },
    [wpnEclipseRelic]  = { wpnEclipseRelic,  "Eclipse Relic",    5, 60, 1 },
};