#include "chrono_shm.hpp"

namespace {
const int kSolarCoreSlotSize = 10;
const int kLunarBladeSlotSize = 10;
}

/* handout numbers — solar+lunar = full bag unless arbiter special-cases something */
static_assert(kSolarCoreSlotSize + kLunarBladeSlotSize == cr::invSize,
              "Solar Core + Lunar Blade must sum to primary inventory size (hard spec)");

const WeaponDef weaponTable[weaponIds::count] = {
    {weaponIds::solarCore, "Solar Core", kSolarCoreSlotSize, 95, 1},
    {weaponIds::lunarBlade, "Lunar Blade", kLunarBladeSlotSize, 90, 1},
    {weaponIds::ironHalberd, "Iron Halberd", 7, 55, 0},
    {weaponIds::venomDagger, "Venom Dagger", 4, 30, 0},
    {weaponIds::thunderstaff, "Thunderstaff", 6, 50, 0},
    {weaponIds::obsidianAxe, "Obsidian Axe", 5, 45, 0},
    {weaponIds::frostbow, "Frostbow", 6, 48, 0},
    {weaponIds::splinterStick, "Splinter Stick", 2, 12, 0},
    /* eclipse relic — progression weapon, not on the tiny printed table */
    {weaponIds::eclipseRelic, "Eclipse Relic", 6, 60, 1},
};
