#pragma once

#include "chrono_shm.hpp"

#include <pthread.h>

/*
 * glass = key queue between SFML poll thread and player pthreads (see cpp top).
 * not a real glass dont @ me
 */
void onStunInterrupt(int sig);
void onStunWake(int sig);
void installHipStunHandlers(void);

void requestUiShutdown(void);
void enqueueKey(int c);
int dequeueKeyBlocking(void);
void drainKeyQueue(void);

void setMenuNavEnabled(int on);
void menuNavGrid(int dRow, int dCol);
int menuNavPickChar(void);

void setTargetNavEnabled(int on);
void targetNavDelta(int delta, SharedMemory* shm);
int targetNavConfirmSlot(void);

void setWeaponNavEnabled(int on);
void weaponNavDelta(int delta);
int weaponNavConfirmId(void);

void setSwapNavEnabled(int on);
void swapNavDelta(int delta);
int swapNavConfirmLtsIndex(void);

inline constexpr int kSwapCommitBase = 0x4000;

void setUltMenuEnabled(int on);
void ultMenuDelta(int delta);
int ultMenuConfirmAction(void);

void setEclipsePickupModalVisible(int on);
int eclipsePickupModalIsVisible(void);
void setUiPrompt(const char* a, const char* b = "", const char* c = "");
void setHeroTurnBanner(int heroIdx, const Entity* p);
