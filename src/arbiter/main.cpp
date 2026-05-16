#include "chrono_shm.hpp"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#include <pthread.h>
#include <chrono>
#include <cstdint>
#include <random>
#include <time.h>
#include <unistd.h>

static SharedMemory* gShm = nullptr;
static pid_t gAspPid = 0;

/* optional timing dump for the report */
struct TurnaroundStats {
    uint64_t count;
    uint64_t sum_ns;
    uint64_t min_ns;
    uint64_t max_ns;
};

static TurnaroundStats gTaPlayerE2e = {0, 0, 0, 0};
static TurnaroundStats gTaPlayerArbiter = {0, 0, 0, 0};
static TurnaroundStats gTaNpcE2e = {0, 0, 0, 0};
static TurnaroundStats gTaNpcArbiter = {0, 0, 0, 0};

static void taRecord(TurnaroundStats* s, uint64_t ns) {
    if (s->count == 0) {
        s->min_ns = s->max_ns = ns;
    } else {
        if (ns < s->min_ns) s->min_ns = ns;
        if (ns > s->max_ns) s->max_ns = ns;
    }
    s->count++;
    s->sum_ns += ns;
}

static double taNsToMs(uint64_t ns) { return (double)ns / 1e6; }

static void taFprintRow(FILE* f, const char* label, const TurnaroundStats* s) {
    if (!s->count) {
        fprintf(f, "%-32s %8s %10s %10s %10s\n", label, "0", "n/a", "n/a", "n/a");
        return;
    }
    double avg = (double)s->sum_ns / (double)s->count;
    fprintf(f, "%-32s %8llu %10.3f %10.3f %10.3f\n", label, (unsigned long long)s->count,
            taNsToMs(s->min_ns), avg / 1e6, taNsToMs(s->max_ns));
}

static void writeTurnaroundAnalysis(int roll, int party, int concurrentCap) {
    const char* path = "turnaround_analysis.txt";
    FILE* f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "turnaround: could not write %s\n", path);
        return;
    }
    fprintf(f, "Chrono Rift turnaround analysis\n");
    fprintf(f, "Timer: std::chrono::steady_clock\n");
    fprintf(f, "Roll %d  party %d  concurrentCap %d\n\n", roll, party, concurrentCap);
    fprintf(f,
            "Player E2E: from sem_post(toPlayer) through sem_wait(humanReady) and processPlayerAction.\n"
            "  Includes human think time and HIP thread scheduling.\n");
    fprintf(f,
            "Player arbiter: time inside processPlayerAction only (rule application on shared state).\n\n");
    fprintf(f,
            "NPC E2E: from sem_post(toNpc) through sem_timedwait(aiReady) and processEnemyAction if any.\n");
    fprintf(f,
            "NPC arbiter: time inside processEnemyAction only.\n"
            "Timeout turns still count as one NPC E2E sample (wait ends near npcTurnTimeoutSeconds).\n\n");
    fprintf(f, "%-32s %8s %10s %10s %10s\n", "METRIC", "COUNT", "MIN_ms", "AVG_ms", "MAX_ms");
    fprintf(f,
            "---------------------------------------- -------- ---------- ---------- ----------\n");
    taFprintRow(f, "Player turn end-to-end", &gTaPlayerE2e);
    taFprintRow(f, "Player arbiter apply", &gTaPlayerArbiter);
    taFprintRow(f, "NPC turn end-to-end", &gTaNpcE2e);
    taFprintRow(f, "NPC arbiter apply", &gTaNpcArbiter);
    fprintf(f, "\nNotes for the report\n");
    fprintf(f, "Player end-to-end time includes waiting for you to pick an action in HIP.\n");
    fprintf(f, "Player arbiter apply is only the rule update after your action arrives.\n");
    fprintf(f, "NPC end-to-end time includes ASP thread wake-up and sem post back to the arbiter.\n");
    fprintf(f, "If NPC MIN_ms is near %d000, some turns hit the timeout path.\n", cr::npcTurnTimeoutSeconds);
    fprintf(f, "Disable this output with CHRONO_TA_ANALYSIS=0.\n");
    fclose(f);
}

static int taAnalysisEnabled() {
    const char* e = getenv("CHRONO_TA_ANALYSIS");
    if (e && e[0] == '0' && e[1] == '\0') return 0;
    return 1;
}

static void onTerminate(int) {
    if (gShm) gShm->sigTermReceived = 1;
}

static void resumeAspIfFrozen(SharedMemory* shm);

static void installSignalHandler(int sig, void (*handler)(int)) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(sig, &sa, nullptr);
}

static void initMutexPshared(pthread_mutex_t* m) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_setpshared(&a, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(m, &a);
    pthread_mutexattr_destroy(&a);
}

static void initSemPshared(sem_t* s, unsigned value) {
    sem_init(s, 1, value);
}

static void clearInv(Inventory* inv) {
    for (int i = 0; i < cr::invSize; i++) inv->slots[i] = weaponIds::none;
}

static int findFirstFit(Inventory* inv, int need) {
    if (need <= 0 || need > cr::invSize) return -1;
    int run = 0;
    int start = -1;
    for (int i = 0; i < cr::invSize; i++) {
        if (inv->slots[i] == weaponIds::none) {
            if (run == 0) start = i;
            run++;
            if (run >= need) return start;
        } else {
            run = 0;
            start = -1;
        }
    }
    return -1;
}

static int moveWeaponToLts(Entity* e, int startSlot, int slotSize) {
    if (e->lts.count >= cr::ltsSize) return 0;
    int wpn = e->inv.slots[startSlot];
    if (wpn == weaponIds::none) return 0;
    for (int i = 0; i < slotSize; i++) {
        if (startSlot + i >= cr::invSize) return 0;
        if (e->inv.slots[startSlot + i] != wpn) return 0;
    }
    e->lts.items[e->lts.count++] = wpn;
    for (int i = 0; i < slotSize; i++) e->inv.slots[startSlot + i] = weaponIds::none;
    return 1;
}

struct InvBlock {
    int start;
    int weaponId;
    int len;
};

static int collectWeaponBlocks(const Inventory* inv, InvBlock* out, int maxOut) {
    int n = 0;
    for (int i = 0; i < cr::invSize && n < maxOut;) {
        int id = inv->slots[i];
        if (id == weaponIds::none) {
            i++;
            continue;
        }
        int sz = weaponTable[id].slotSize;
        int ok = 1;
        for (int k = 0; k < sz; k++) {
            if (i + k >= cr::invSize || inv->slots[i + k] != id) {
                ok = 0;
                break;
            }
        }
        if (!ok) {
            i++;
            continue;
        }
        out[n].start = i;
        out[n].weaponId = id;
        out[n].len = sz;
        n++;
        i += sz;
    }
    return n;
}

static void sortEvictOrderDesc(int* order, int k, const InvBlock* blocks) {
    for (int a = 0; a < k; a++) {
        for (int b = a + 1; b < k; b++) {
            if (blocks[order[a]].start < blocks[order[b]].start) {
                int t = order[a];
                order[a] = order[b];
                order[b] = t;
            }
        }
    }
}

static int nextCombination(int* c, int k, int M) {
    int i = k - 1;
    while (i >= 0 && c[i] == M - k + i) i--;
    if (i < 0) return 0;
    c[i]++;
    for (int j = i + 1; j < k; j++) c[j] = c[j - 1] + 1;
    return 1;
}

/* try to cram weapon in bag; kicks stuff to LTS if needed */
static int placeWeaponFit(Entity* e, int wpnId) {
    if (wpnId < 0 || wpnId >= weaponIds::count) return 0;
    if (wpnId != weaponIds::solarCore && wpnId != weaponIds::lunarBlade) {
        int hasSolar = 0, hasLunar = 0;
        for (int i = 0; i < cr::invSize; i++) {
            if (e->inv.slots[i] == weaponIds::solarCore) hasSolar = 1;
            if (e->inv.slots[i] == weaponIds::lunarBlade) hasLunar = 1;
        }
        /* eclipse pickup is allowed to yeet solar OR lunar to storage — normal drops dont get that pass */
        if (hasSolar && hasLunar && wpnId != weaponIds::eclipseRelic) {
            if (gShm)
                logEvent(gShm, "allocator: Solar+Lunar fill entire inventory — no slot for other weapon");
            return 0;
        }
    }
    int need = weaponTable[wpnId].slotSize;
    int at = findFirstFit(&e->inv, need);
    if (at >= 0) {
        for (int i = 0; i < need; i++) e->inv.slots[at + i] = wpnId;
        return 1;
    }

    InvBlock blocks[cr::invSize];
    int M = collectWeaponBlocks(&e->inv, blocks, cr::invSize);
    if (M == 0) return 0;

    for (int k = 1; k <= M; k++) {
        int c[cr::invSize];
        for (int i = 0; i < k; i++) c[i] = i;
        for (;;) {
            Entity sim;
            memcpy(&sim, e, sizeof(Entity));
            int evOrder[cr::invSize];
            for (int i = 0; i < k; i++) evOrder[i] = c[i];
            sortEvictOrderDesc(evOrder, k, blocks);

            int okSim = 1;
            for (int i = 0; i < k; i++) {
                int bi = evOrder[i];
                if (!moveWeaponToLts(&sim, blocks[bi].start, blocks[bi].len)) {
                    okSim = 0;
                    break;
                }
            }
            if (okSim && findFirstFit(&sim.inv, need) >= 0) {
                for (int i = 0; i < k; i++) {
                    int bi = evOrder[i];
                    if (!moveWeaponToLts(e, blocks[bi].start, blocks[bi].len)) return 0;
                }
                int placeAt = findFirstFit(&e->inv, need);
                if (placeAt < 0) return 0;
                for (int i = 0; i < need; i++) e->inv.slots[placeAt + i] = wpnId;
                return 1;
            }

            if (!nextCombination(c, k, M)) break;
        }
    }
    return 0;
}

static void spawnEnemy(SharedMemory* shm, int slot) {
    Entity* en = &shm->enemies[slot];
    memset(en, 0, sizeof(Entity));
    en->id = shm->numPlayers + slot;
    en->isPlayer = 0;
    en->isAlive = 1;
    shm->enemySpawnSeq++;
    snprintf(en->name, sizeof(en->name), "#%d E%d", shm->enemySpawnSeq, slot);
    /* HP: last two digits of roll + rand[50,200]. Damage: second-last digit of roll + 10. */
    int roll = shm->rollNumber;
    int last2 = roll % 100;
    int d2 = (roll / 10) % 10;
    en->maxHp = last2 + (rand() % 151 + 50);
    en->hp = en->maxHp;
    en->damage = d2 + 10;
    en->speed = rand() % 21 + 10;
    en->maxStamina = cr::maxStaminaEnemy;
    en->stamina = 0;
    en->waitingArtifact = -1;
    clearInv(&en->inv);
    en->lts.count = 0;
    en->swapInCooldownWeapon = weaponIds::none;
}

static int countAliveEnemies(SharedMemory* shm) {
    int n = 0;
    for (int i = 0; i < cr::maxEnemies; i++)
        if (shm->enemies[i].isAlive) n++;
    return n;
}

static int weaponToArtifactId(int wpn) {
    if (wpn == weaponIds::solarCore) return (int)ArtifactId::solarCore;
    if (wpn == weaponIds::lunarBlade) return (int)ArtifactId::lunarBlade;
    if (wpn == weaponIds::eclipseRelic) return (int)ArtifactId::eclipseRelic;
    return -1;
}

/* grab relic off the dirt — flip artifact row then stuff inventory */
static int tryPickupEclipseRelicFromGround(SharedMemory* shm, Entity* actor) {
    const int art = (int)ArtifactId::eclipseRelic;
    pthread_mutex_lock(&shm->artifacts.lock);
    if (!shm->eclipseRelicOnGround) {
        pthread_mutex_unlock(&shm->artifacts.lock);
        return 0;
    }
    if (shm->artifacts.table[art].holderId != -1) {
        pthread_mutex_unlock(&shm->artifacts.lock);
        return 0;
    }
    shm->artifacts.table[art].exists = 1;
    shm->artifacts.table[art].holderId = actor->id;
    shm->eclipseRelicOnGround = 0;
    pthread_mutex_unlock(&shm->artifacts.lock);

    if (!placeWeaponFit(actor, weaponIds::eclipseRelic)) {
        pthread_mutex_lock(&shm->artifacts.lock);
        shm->artifacts.table[art].holderId = -1;
        shm->artifacts.table[art].exists = 0;
        shm->eclipseRelicOnGround = 1;
        pthread_mutex_unlock(&shm->artifacts.lock);
        return 0;
    }
    actor->waitingArtifact = -1;
    return 1;
}

static int tryAcquireArtifactForEntity(SharedMemory* shm, int entityId, int wpn) {
    int art = weaponToArtifactId(wpn);
    if (art < 0) return 1;
    pthread_mutex_lock(&shm->artifacts.lock);
    if (!shm->artifacts.table[art].exists) {
        pthread_mutex_unlock(&shm->artifacts.lock);
        return 0;
    }
    /* if you actually have the item in inv, fix holderId (it can get out of sync after weird drops) */
    Entity* ent = entityById(shm, entityId);
    if (ent && inventoryHasWeapon(ent, wpn)) {
        shm->artifacts.table[art].holderId = entityId;
        pthread_mutex_unlock(&shm->artifacts.lock);
        return 1;
    }
    int h = shm->artifacts.table[art].holderId;
    if (h != -1 && h != entityId) {
        pthread_mutex_unlock(&shm->artifacts.lock);
        return 0;
    }
    shm->artifacts.table[art].holderId = entityId;
    pthread_mutex_unlock(&shm->artifacts.lock);
    return 1;
}

static void releaseArtifactsForEntity(SharedMemory* shm, int entityId) {
    pthread_mutex_lock(&shm->artifacts.lock);
    for (int i = 0; i < (int)ArtifactId::count; i++) {
        if (shm->artifacts.table[i].holderId == entityId) shm->artifacts.table[i].holderId = -1;
    }
    pthread_mutex_unlock(&shm->artifacts.lock);
}

static void maybeSpawnReplacements(SharedMemory* shm) {
    if (shm->enemiesKilled >= cr::enemiesToWin) return;
    while (countAliveEnemies(shm) < shm->concurrentCap && shm->totalEnemiesSpawned < cr::enemiesToWin) {
        int slot = -1;
        for (int i = 0; i < cr::maxEnemies; i++) {
            if (!shm->enemies[i].isAlive) {
                slot = i;
                break;
            }
        }
        if (slot < 0) break;
        spawnEnemy(shm, slot);
        shm->totalEnemiesSpawned++;
        shm->aspNpcSlotIndex = slot;
        sem_post(&shm->aspNpcSlotWakeup);
        char buf[cr::logLineLen];
        snprintf(buf, sizeof(buf), "REINFORCEMENT %s — new unit (kills %d/%d), not the old guy healing",
                 shm->enemies[slot].name, shm->enemiesKilled, cr::enemiesToWin);
        logEvent(shm, buf);
    }
}

static int pickDropWeapon(SharedMemory* shm, Entity* defeated) {
    if (defeated && inventoryHasWeapon(defeated, weaponIds::eclipseRelic) &&
        shm->artifacts.table[(int)ArtifactId::eclipseRelic].exists) {
        return weaponIds::eclipseRelic;
    }
    if (defeated && !defeated->isPlayer) {
        for (int i = 0; i < cr::invSize; i++) {
            if (defeated->inv.slots[i] != weaponIds::none) return weaponIds::none;
        }
    }
    static const int kDropCandidates[] = {weaponIds::ironHalberd, weaponIds::venomDagger, weaponIds::thunderstaff,
                                          weaponIds::obsidianAxe, weaponIds::frostbow, weaponIds::splinterStick};
    return kDropCandidates[rand() % (int)(sizeof kDropCandidates / sizeof kDropCandidates[0])];
}

static void addMonoMs(struct timespec* ts, int ms) {
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000L;
    while (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

static void maybeQueueDrop(SharedMemory* shm, Entity* defeated) {
    /* Always pulse HIP kill banner so defeat is visible immediately (independent of loot RNG). */
    clock_gettime(CLOCK_MONOTONIC, &shm->enemyKillBannerEndMono);
    addMonoMs(&shm->enemyKillBannerEndMono, 4500);

    int dw = pickDropWeapon(shm, defeated);
    if (dw == weaponIds::none) {
        shm->dropPending = 0;
        shm->dropDecided = 0;
        logEvent(shm, "enemy defeated: NPC held weapons — no drop (spec rule)");
        return;
    }

    /* Loot probability: default 100% so weapon popup matches expectations; override with CHRONO_DROP_PCT=0–100. */
    int pct = 100;
    if (const char* ep = getenv("CHRONO_DROP_PCT")) {
        pct = atoi(ep);
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
    }
    int roll = rand() % 100;
    if (roll < pct) {
        shm->dropPending = 1;
        shm->dropWeapon = dw;
        shm->dropDecided = 0;
        char buf[cr::logLineLen];
        snprintf(buf, sizeof(buf), "weapon dropped: %s", weaponTable[shm->dropWeapon].name);
        logEvent(shm, buf);
    } else {
        shm->dropPending = 0;
        shm->dropDecided = 0;
        logEvent(shm, "enemy defeated: no weapon drop this time");
    }
}

static void fxUltimateDeniedOverlay(SharedMemory* shm) {
    clock_gettime(CLOCK_MONOTONIC, &shm->ultimateDeniedStartMono);
    shm->ultimateDeniedEndMono = shm->ultimateDeniedStartMono;
    addMonoMs(&shm->ultimateDeniedEndMono, cr::ultimateDeniedOverlayMs);
}

static void flashEnemyHit(SharedMemory* shm, int slot) {
    if (slot < 0 || slot >= cr::maxEnemies) return;
    shm->hitFlashEnemySlot = slot;
    clock_gettime(CLOCK_MONOTONIC, &shm->hitFlashEnemyEndMono);
    addMonoMs(&shm->hitFlashEnemyEndMono, 480);
}

static void flashPlayerHit(SharedMemory* shm, int playerIdx) {
    if (playerIdx < 0 || playerIdx >= cr::maxPlayers) return;
    shm->hitFlashPlayerIdx = playerIdx;
    clock_gettime(CLOCK_MONOTONIC, &shm->hitFlashPlayerEndMono);
    addMonoMs(&shm->hitFlashPlayerEndMono, 480);
}

static void fxPlayerProjectile(SharedMemory* shm, int playerIdx, int enemySlot) {
    shm->fxProjKind = 1;
    shm->fxProjPlayerIdx = playerIdx;
    shm->fxProjEnemySlot = enemySlot;
    clock_gettime(CLOCK_MONOTONIC, &shm->fxProjStartMono);
    shm->fxProjEndMono = shm->fxProjStartMono;
    addMonoMs(&shm->fxProjEndMono, 560);
}

static void fxEnemyProjectile(SharedMemory* shm, int enemySlot, int playerIdx) {
    shm->fxProjKind = 2;
    shm->fxProjEnemySlot = enemySlot;
    shm->fxProjPlayerIdx = playerIdx;
    clock_gettime(CLOCK_MONOTONIC, &shm->fxProjStartMono);
    shm->fxProjEndMono = shm->fxProjStartMono;
    addMonoMs(&shm->fxProjEndMono, 560);
}

static void fxHealGlow(SharedMemory* shm, int playerIdx) {
    shm->fxHealPlayerIdx = playerIdx;
    clock_gettime(CLOCK_MONOTONIC, &shm->fxHealEndMono);
    addMonoMs(&shm->fxHealEndMono, 720);
}

static void fxSpecialWeapon(SharedMemory* shm, int weaponId) {
    shm->fxSpecialWeaponId = weaponId;
    snprintf(shm->fxSpecialText, sizeof(shm->fxSpecialText), "SPECIAL EFFECT: %s", weaponTable[weaponId].name);
    clock_gettime(CLOCK_MONOTONIC, &shm->fxSpecialEndMono);
    addMonoMs(&shm->fxSpecialEndMono, 1300);
}

static void fxDamagePopupEnemy(SharedMemory* shm, int enemySlot, int dmg) {
    shm->dmgPopupKind = 1;
    shm->dmgPopupEnemySlot = enemySlot;
    shm->dmgPopupValue = dmg;
    clock_gettime(CLOCK_MONOTONIC, &shm->dmgPopupStartMono);
    shm->dmgPopupEndMono = shm->dmgPopupStartMono;
    addMonoMs(&shm->dmgPopupEndMono, 620);
}

static void fxDamagePopupPlayer(SharedMemory* shm, int playerIdx, int dmg) {
    shm->dmgPopupKind = 2;
    shm->dmgPopupPlayerIdx = playerIdx;
    shm->dmgPopupValue = dmg;
    clock_gettime(CLOCK_MONOTONIC, &shm->dmgPopupStartMono);
    shm->dmgPopupEndMono = shm->dmgPopupStartMono;
    addMonoMs(&shm->dmgPopupEndMono, 620);
}

struct StunWakeCtx {
    SharedMemory* shm;
    int victimId;
    pid_t proc;
    pid_t tid;
    struct timespec wakeAt;
};

/* timer thread — unstick sigsuspend with SIGUSR2 when stun ends */
static void* stunWakeThreadMain(void* p) {
    StunWakeCtx* c = static_cast<StunWakeCtx*>(p);
    if (!c || !c->shm) {
        free(c);
        return nullptr;
    }
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &c->wakeAt, nullptr);
    pthread_mutex_lock(&c->shm->stateLock);
    Entity* v = entityById(c->shm, c->victimId);
    if (v && v->isAlive) v->isStunned = 0;
    pthread_mutex_unlock(&c->shm->stateLock);
    if (c->proc > 0 && c->tid > 0) {
#ifdef __linux__
        tgkill(c->proc, c->tid, SIGUSR2);
#else
        kill(c->proc, SIGUSR2);
#endif
    } else if (c->proc > 0) {
        kill(c->proc, SIGUSR2);
    }
    free(c);
    return nullptr;
}

/* ping SIGUSR1 so their handler traps in sigsuspend til we SIGUSR2 later */
static void applyStun(SharedMemory* shm, Entity* victim) {
    victim->isStunned = 1;
    clock_gettime(CLOCK_MONOTONIC, &victim->stunEnd);
    addMonoMs(&victim->stunEnd, cr::stunSeconds * 1000);

    pid_t proc = 0;
    pid_t tid = 0;
    if (victim->isPlayer) {
        proc = shm->humanPid;
        if (victim->id >= 0 && victim->id < cr::maxPlayers) tid = shm->playerOsTid[victim->id];
    } else {
        proc = shm->aiPid;
        int slot = enemySlotFromId(shm, victim->id);
        if (slot >= 0 && slot < cr::maxEnemies) tid = shm->npcOsTid[slot];
    }

    if (proc > 0 && tid > 0) {
#ifdef __linux__
        if (tgkill(proc, tid, SIGUSR1) != 0) kill(proc, SIGUSR1);
#else
        kill(proc, SIGUSR1);
#endif
    } else if (proc > 0) {
        kill(proc, SIGUSR1);
    }

    StunWakeCtx* ctx = static_cast<StunWakeCtx*>(malloc(sizeof(StunWakeCtx)));
    if (!ctx) return;
    ctx->shm = shm;
    ctx->victimId = victim->id;
    ctx->proc = proc;
    ctx->tid = tid;
    ctx->wakeAt = victim->stunEnd;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_t th;
    pthread_create(&th, &a, stunWakeThreadMain, ctx);
    pthread_attr_destroy(&a);
}

static int stillStunned(Entity* e) {
    if (!e->isStunned) return 0;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec > e->stunEnd.tv_sec ||
        (now.tv_sec == e->stunEnd.tv_sec && now.tv_nsec >= e->stunEnd.tv_nsec)) {
        e->isStunned = 0;
        return 0;
    }
    return 1;
}

static void onUltimateAlarm(int) {
    if (gShm) gShm->sigAlarmReceived = 1;
}

static void resumeAspIfFrozen(SharedMemory* shm) {
    if (!shm->aspFrozen || gAspPid <= 0) return;
    kill(gAspPid, SIGCONT);
    shm->aspFrozen = 0;
    shm->ultimateActive = 0;
    logEvent(shm, "ultimate ended: ASP resumed (SIGCONT)");
}

enum { kDeadlockMaxEntities = cr::maxPlayers + cr::maxEnemies };

struct DeadlockDfsCtx {
    SharedMemory* shm;
    const int* waitFor;
    int totalIds;
    int* color;
    int* pathStack;
    int pathLen;
    int* cycleBuf;
    int* cycleLenOut;
};

/* dfs artifact wait graph — back edge = cycle */
static bool dfsArtifactWait(DeadlockDfsCtx* c, int u) {
    c->color[u] = 1;
    c->pathStack[c->pathLen++] = u;

    int wa = c->waitFor[u];
    int v = -1;
    if (wa >= 0 && wa < (int)ArtifactId::count && c->shm->artifacts.table[wa].exists) {
        int h = c->shm->artifacts.table[wa].holderId;
        Entity* he = (h >= 0 && h < c->totalIds) ? entityById(c->shm, h) : nullptr;
        if (he && he->isAlive && h != u) v = h;
    }

    if (v < 0) {
        c->pathLen--;
        c->color[u] = 2;
        return false;
    }
    if (c->color[v] == 1) {
        int idx = c->pathLen - 1;
        while (idx >= 0 && c->pathStack[idx] != v) idx--;
        if (idx < 0) {
            c->pathLen--;
            c->color[u] = 2;
            return false;
        }
        *c->cycleLenOut = c->pathLen - idx;
        for (int k = 0; k < *c->cycleLenOut; k++) c->cycleBuf[k] = c->pathStack[idx + k];
        return true;
    }
    if (c->color[v] == 0 && dfsArtifactWait(c, v)) return true;
    c->pathLen--;
    c->color[u] = 2;
    return false;
}

static void* deadlockWatch(void* arg) {
    SharedMemory* shm = static_cast<SharedMemory*>(arg);
    for (;;) {
        int waitFor[kDeadlockMaxEntities];
        char logBuf[cr::logLineLen] = {0};
        int releaseHolder = -1;

        pthread_mutex_lock(&shm->stateLock);
        bool running = shm->gameStatus == GameStatus::running;
        if (!running) {
            pthread_mutex_unlock(&shm->stateLock);
            break;
        }
        int np = shm->numPlayers;
        int totalIds = np + cr::maxEnemies;
        for (int i = 0; i < totalIds; i++) {
            Entity* e = entityById(shm, i);
            waitFor[i] = (e && e->isAlive) ? e->waitingArtifact : -1;
        }

        int color[kDeadlockMaxEntities];
        int pathStack[kDeadlockMaxEntities];
        int cycleBuf[kDeadlockMaxEntities];
        int cycleLen = 0;
        for (int i = 0; i < kDeadlockMaxEntities; i++) color[i] = 0;

        DeadlockDfsCtx ctx = {shm, waitFor, totalIds, color, pathStack, 0, cycleBuf, &cycleLen};

        pthread_mutex_lock(&shm->artifacts.lock);
        for (int s = 0; s < totalIds && releaseHolder < 0; s++) {
            if (color[s] != 0) continue;
            ctx.pathLen = 0;
            if (!dfsArtifactWait(&ctx, s)) continue;
            int maxInCycle = cycleBuf[0];
            for (int k = 1; k < cycleLen; k++)
                if (cycleBuf[k] > maxInCycle) maxInCycle = cycleBuf[k];
            releaseHolder = maxInCycle;
            for (int k = 0; k < (int)ArtifactId::count; k++) {
                if (shm->artifacts.table[k].holderId == releaseHolder) shm->artifacts.table[k].holderId = -1;
            }
            snprintf(logBuf, sizeof(logBuf), "deadlock watch: cycle broken; released artifacts held by entity %d",
                     releaseHolder);
        }
        pthread_mutex_unlock(&shm->artifacts.lock);

        if (releaseHolder >= 0) {
            Entity* ev = entityById(shm, releaseHolder);
            if (ev) ev->waitingArtifact = -1;
            logEvent(shm, logBuf);
        }
        pthread_mutex_unlock(&shm->stateLock);

        struct timespec ts = {0, 200000000};
        nanosleep(&ts, nullptr);
    }
    return nullptr;
}

static void processPlayerAction(SharedMemory* shm, Action* a) {
    Entity* actor = entityById(shm, a->actorId);
    if (!actor || !actor->isAlive) return;
    if (shm->dropPending && shm->dropDecided) {
        if (shm->dropPlayerTakes) {
            int ok = tryAcquireArtifactForEntity(shm, actor->id, shm->dropWeapon);
            if (ok && placeWeaponFit(actor, shm->dropWeapon)) {
                char buf[cr::logLineLen];
                snprintf(buf, sizeof(buf), "picked up drop: %s", weaponTable[shm->dropWeapon].name);
                logEvent(shm, buf);
                actor->waitingArtifact = -1;
            } else if (!ok) {
                int art = weaponToArtifactId(shm->dropWeapon);
                actor->waitingArtifact = art;
                logEvent(shm, "artifact busy: waiting on resource lock");
            } else {
                logEvent(shm, "inventory full / fragmented: drop lost this turn");
            }
        } else {
            int picked = 0;
            for (int slot = 0; slot < cr::maxEnemies && !picked; slot++) {
                Entity* en = &shm->enemies[slot];
                if (!en->isAlive) continue;
                int holderOk = tryAcquireArtifactForEntity(shm, en->id, shm->dropWeapon);
                if (!holderOk) {
                    int art = weaponToArtifactId(shm->dropWeapon);
                    en->waitingArtifact = art;
                    continue;
                }
                en->waitingArtifact = -1;
                if (placeWeaponFit(en, shm->dropWeapon)) {
                    char buf[cr::logLineLen];
                    snprintf(buf, sizeof(buf), "player skipped drop, enemy %s picked %s", en->name,
                             weaponTable[shm->dropWeapon].name);
                    logEvent(shm, buf);
                    picked = 1;
                }
            }
            if (!picked) {
                logEvent(shm, "drop not placed: no enemy could accept (artifacts busy or inventory+LTS full)");
            }
        }
        shm->dropPending = 0;
        shm->dropDecided = 0;
    }
    if (a->type == ActionType::quit) {
        shm->gameStatus = GameStatus::quit;
        logEvent(shm, "quit requested from HIP");
        return;
    }
    if (a->type == ActionType::pickupEclipseRelic) {
        if (tryPickupEclipseRelicFromGround(shm, actor)) {
            logEvent(shm, "picked up Eclipse Relic from the environment");
            actor->stamina = 0;
        } else {
            logEvent(shm, "eclipse relic pickup failed (not on ground or inventory full)");
            actor->stamina = actor->maxStamina * 0.5f;
        }
        return;
    }
    if (a->type == ActionType::skip) {
        actor->stamina = actor->maxStamina * 0.5f;
        logEvent(shm, "player skipped (stamina -> 50 percent)");
        return;
    }
    if (a->type == ActionType::heal) {
        int add = actor->maxHp / 10;
        actor->hp += add;
        if (actor->hp > actor->maxHp) actor->hp = actor->maxHp;
        actor->stamina = 0;
        logEvent(shm, "player healed 10 percent");
        fxHealGlow(shm, a->actorId);
        return;
    }
    if (a->type == ActionType::ultimate) {
        if (!canUseUltimate(actor)) {
            logEvent(shm, "ultimate denied (need solar+lunar in primary inv)");
            fxUltimateDeniedOverlay(shm);
            actor->stamina = 0;
            return;
        }
        int gotSolar = tryAcquireArtifactForEntity(shm, actor->id, weaponIds::solarCore);
        int gotLunar = tryAcquireArtifactForEntity(shm, actor->id, weaponIds::lunarBlade);
        if (!gotSolar || !gotLunar) {
            actor->waitingArtifact = !gotSolar ? (int)ArtifactId::solarCore : (int)ArtifactId::lunarBlade;
            if (gotSolar && !gotLunar) {
                pthread_mutex_lock(&shm->artifacts.lock);
                if (shm->artifacts.table[(int)ArtifactId::solarCore].holderId == actor->id) {
                    shm->artifacts.table[(int)ArtifactId::solarCore].holderId = -1;
                }
                pthread_mutex_unlock(&shm->artifacts.lock);
            }
            actor->stamina = 0;
            logEvent(shm, "ultimate blocked: required artifact lock is busy");
            fxUltimateDeniedOverlay(shm);
            return;
        }
        actor->waitingArtifact = -1;
        shm->ultimateActive = 1;
        shm->aspFrozen = 1;
        alarm(0);
        installSignalHandler(SIGALRM, onUltimateAlarm);
        alarm(cr::ultimatePauseSeconds);
        if (gAspPid > 0) {
            kill(gAspPid, SIGSTOP);
            logEvent(shm, "ultimate: SIGSTOP -> ASP (10s)");
        }
        actor->stamina = 0;
        return;
    }
    if (a->type == ActionType::swapIn) {
        if (a->ltsIndex < 0 || a->ltsIndex >= actor->lts.count) {
            actor->stamina = 0;
            return;
        }
        int w = actor->lts.items[a->ltsIndex];
        int lockOk = tryAcquireArtifactForEntity(shm, actor->id, w);
        if (!lockOk) {
            actor->waitingArtifact = weaponToArtifactId(w);
            actor->stamina = 0;
            logEvent(shm, "swap in blocked: artifact currently held by another entity");
            return;
        }
        actor->waitingArtifact = -1;
        for (int k = a->ltsIndex; k + 1 < actor->lts.count; k++) actor->lts.items[k] = actor->lts.items[k + 1];
        actor->lts.count--;
        if (!placeWeaponFit(actor, w)) {
            actor->lts.items[actor->lts.count++] = w;
            logEvent(shm, "swap in failed (no contiguous space after evict)");
        } else {
            actor->swapInCooldownWeapon = w;
            logEvent(shm, "swap in from long term storage");
        }
        actor->stamina = 0;
        return;
    }
    if (a->type == ActionType::strike) {
        Entity* target = entityById(shm, a->targetId);
        if (!target || !target->isAlive || target->isPlayer) {
            actor->stamina = 0;
            logEvent(shm, "strike whiff");
            return;
        }
        target->hp -= actor->damage;
        actor->stamina = 0;
        char buf[cr::logLineLen];
        snprintf(buf, sizeof(buf), "player strike -> %s (%d dmg)", target->name, actor->damage);
        logEvent(shm, buf);
        flashEnemyHit(shm, enemySlotFromId(shm, target->id));
        fxPlayerProjectile(shm, a->actorId, enemySlotFromId(shm, target->id));
        fxDamagePopupEnemy(shm, enemySlotFromId(shm, target->id), actor->damage);
        if (target->hp <= 0) {
            target->isAlive = 0;
            releaseArtifactsForEntity(shm, target->id);
            shm->enemiesKilled++;
            logEvent(shm, "enemy defeated");
            maybeQueueDrop(shm, target);
            if (shm->enemiesKilled == 3) {
                shm->eclipseRelicOnGround = 1;
                logEvent(shm, "eclipse relic appears on the battlefield");
            }
            maybeSpawnReplacements(shm);
        }
        return;
    }
    if (a->type == ActionType::exhaust) {
        Entity* target = entityById(shm, a->targetId);
        if (!target || !target->isAlive || target->isPlayer) {
            actor->stamina = 0;
            logEvent(shm, "exhaust whiff");
            return;
        }
        target->stamina -= actor->damage;
        if (target->stamina < 0) target->stamina = 0;
        actor->stamina = 0;
        flashEnemyHit(shm, enemySlotFromId(shm, target->id));
        fxPlayerProjectile(shm, a->actorId, enemySlotFromId(shm, target->id));
        fxDamagePopupEnemy(shm, enemySlotFromId(shm, target->id), actor->damage);
        logEvent(shm, "exhaust hit (stamina drained)");
        return;
    }
    if (a->type == ActionType::useWeapon) {
        int wid = a->weapon;
        if (wid < 0 || wid >= weaponIds::count) {
            logEvent(shm, "weapon denied: invalid weapon id");
            actor->stamina = 0;
            return;
        }
        if (!inventoryHasWeapon(actor, wid)) {
            char buf[cr::logLineLen];
            snprintf(buf, sizeof(buf), "weapon denied: %s not present in inventory", weaponTable[wid].name);
            logEvent(shm, buf);
            actor->stamina = 0;
            return;
        }
        if (actor->swapInCooldownWeapon != weaponIds::none && actor->swapInCooldownWeapon == wid) {
            char buf[cr::logLineLen];
            snprintf(buf, sizeof(buf), "weapon denied: %s was just swapped in (cooldown, use next turn)",
                     weaponTable[wid].name);
            logEvent(shm, buf);
            actor->stamina = 0;
            return;
        }
        Entity* target = entityById(shm, a->targetId);
        if (!target || !target->isAlive || target->isPlayer) {
            actor->stamina = 0;
            logEvent(shm, "weapon whiff");
            return;
        }
        int dmg = weaponTable[wid].damage;
        target->hp -= dmg;
        actor->stamina = 0;
        if (wid == weaponIds::thunderstaff) {
            applyStun(shm, target);
            fxSpecialWeapon(shm, wid);
        } else if (weaponTable[wid].isArtifact) {
            fxSpecialWeapon(shm, wid);
        }
        char buf[cr::logLineLen];
        snprintf(buf, sizeof(buf), "weapon %s -> %s (%d)", weaponTable[wid].name, target->name, dmg);
        logEvent(shm, buf);
        flashEnemyHit(shm, enemySlotFromId(shm, target->id));
        fxPlayerProjectile(shm, a->actorId, enemySlotFromId(shm, target->id));
        fxDamagePopupEnemy(shm, enemySlotFromId(shm, target->id), dmg);
        if (target->hp <= 0) {
            target->isAlive = 0;
            releaseArtifactsForEntity(shm, target->id);
            shm->enemiesKilled++;
            maybeQueueDrop(shm, target);
            if (shm->enemiesKilled == 3) {
                shm->eclipseRelicOnGround = 1;
                logEvent(shm, "eclipse relic appears on the battlefield");
            }
            maybeSpawnReplacements(shm);
        }
    }
}

static void processEnemyAction(SharedMemory* shm, Action* a) {
    Entity* actor = entityById(shm, a->actorId);
    if (!actor || !actor->isAlive || actor->isPlayer) return;
    if (a->type == ActionType::skip) {
        actor->stamina = actor->maxStamina * 0.5f;
        logEvent(shm, "enemy skipped");
        return;
    }
    if (a->type == ActionType::strike) {
        Entity* tgt = entityById(shm, a->targetId);
        if (!tgt || !tgt->isPlayer || !tgt->isAlive) return;
        tgt->hp -= actor->damage;
        actor->stamina = 0;
        char buf[cr::logLineLen];
        snprintf(buf, sizeof(buf), "enemy hit %s (%d)", tgt->name, actor->damage);
        logEvent(shm, buf);
        flashPlayerHit(shm, tgt->id);
        fxEnemyProjectile(shm, enemySlotFromId(shm, actor->id), tgt->id);
        fxDamagePopupPlayer(shm, tgt->id, actor->damage);
        if (tgt->hp <= 0) {
            tgt->isAlive = 0;
            releaseArtifactsForEntity(shm, tgt->id);
            logEvent(shm, "player down");
        }
    }
}

static int allPlayersDead(SharedMemory* shm) {
    for (int i = 0; i < shm->numPlayers; i++)
        if (shm->players[i].isAlive) return 0;
    return 1;
}

static int monoLess(const struct timespec* a, const struct timespec* b) {
    if (a->tv_sec != b->tv_sec) return a->tv_sec < b->tv_sec;
    return a->tv_nsec < b->tv_nsec;
}

static int sameMono(const struct timespec* a, const struct timespec* b) {
    return a->tv_sec == b->tv_sec && a->tv_nsec == b->tv_nsec;
}

/* whos turn — max stam + earliest readyAtMono wins */
static int pickNextActor(SharedMemory* shm, int* outId) {
    int bestId = -1;
    struct timespec bestTs = {};
    int haveBest = 0;

    auto consider = [&](Entity* e) {
        if (!e->isAlive) return;
        if (stillStunned(e)) return;
        if (e->stamina < (float)e->maxStamina) return;
        if (!e->readyStampValid) return;
        if (!haveBest || monoLess(&e->readyAtMono, &bestTs) ||
            (sameMono(&e->readyAtMono, &bestTs) && e->id < bestId)) {
            bestId = e->id;
            bestTs = e->readyAtMono;
            haveBest = 1;
        }
    };

    for (int i = 0; i < shm->numPlayers; i++) consider(&shm->players[i]);
    if (!shm->aspFrozen) {
        for (int i = 0; i < cr::maxEnemies; i++) consider(&shm->enemies[i]);
    }

    *outId = bestId;
    return bestId >= 0;
}

static void tickStamina(SharedMemory* shm, float dt) {
    float pace = shm->staminaPace;
    if (pace < 0.01f) pace = 0.01f;
    if (pace > 10.f) pace = 10.f;
    const float g = cr::staminaRegenScale;
    for (int i = 0; i < shm->numPlayers; i++) {
        Entity* e = &shm->players[i];
        if (!e->isAlive) continue;
        if (stillStunned(e)) continue;
        e->stamina += (float)e->speed * dt * pace * g;
        if (e->stamina >= (float)e->maxStamina) {
            e->stamina = (float)e->maxStamina;
            if (!e->readyStampValid) {
                clock_gettime(CLOCK_MONOTONIC, &e->readyAtMono);
                e->readyStampValid = 1;
            }
        } else {
            e->readyStampValid = 0;
        }
    }
    if (!shm->aspFrozen) {
        for (int i = 0; i < cr::maxEnemies; i++) {
            Entity* e = &shm->enemies[i];
            if (!e->isAlive) continue;
            if (stillStunned(e)) continue;
            e->stamina += (float)e->speed * dt * pace * cr::enemyStaminaRegenMul * g;
            if (e->stamina >= (float)e->maxStamina) {
                e->stamina = (float)e->maxStamina;
                if (!e->readyStampValid) {
                    clock_gettime(CLOCK_MONOTONIC, &e->readyAtMono);
                    e->readyStampValid = 1;
                }
            } else {
                e->readyStampValid = 0;
            }
        }
    }
}

/* stamina ticks even while ur staring at menus — main loop would stall otherwise */
static void* staminaTickerThread(void* arg) {
    SharedMemory* shm = static_cast<SharedMemory*>(arg);
    struct timespec last;
    clock_gettime(CLOCK_MONOTONIC, &last);
    const long tickNs = 8000000L; /* ~8ms tick */

    for (;;) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        float dt = (float)(now.tv_sec - last.tv_sec) + (float)(now.tv_nsec - last.tv_nsec) / 1e9f;
        last = now;
        if (dt > 0.12f) dt = 0.12f;
        if (dt < 0.0005f) dt = 0.0005f;

        pthread_mutex_lock(&shm->stateLock);
        if (shm->gameStatus != GameStatus::running) {
            pthread_mutex_unlock(&shm->stateLock);
            break;
        }
        tickStamina(shm, dt);
        pthread_mutex_unlock(&shm->stateLock);

        struct timespec req;
        req.tv_sec = 0;
        req.tv_nsec = tickNs;
        struct timespec rem;
        while (nanosleep(&req, &rem) == -1 && errno == EINTR) req = rem;
    }
    return nullptr;
}

int main(int argc, char** argv) {
    (void)argc;
    const char* binDir = ".";
    if (argv[0]) {
        const char* slash = strrchr(argv[0], '/');
        if (slash) {
            static char pathbuf[512];
            size_t n = (size_t)(slash - argv[0]);
            if (n >= sizeof(pathbuf)) n = sizeof(pathbuf) - 1;
            memcpy(pathbuf, argv[0], n);
            pathbuf[n] = '\0';
            binDir = pathbuf;
        }
    }

    int party = 1;
    for (;;) {
        printf("Select party size (1-4 human-controlled characters): ");
        fflush(stdout);
        if (scanf("%d", &party) != 1) {
            party = 1;
            break;
        }
        if (party >= 1 && party <= 4) break;
        printf("Invalid — enter an integer from 1 to 4.\n");
    }
    if (party >= 2)
        printf("Local MULTIPLAYER: %d human heroes — hot-seat turns, one shared keyboard (bonus mode).\n",
               party);
    printf("(HIP opens an SFML window next — display / X11 or Wayland + SFML deps required)\n");
    fflush(stdout);

    /* Official roll number for stat formulas (0643 → 643). Override for grading: CHRONO_ROLL */
    int roll = 643;
    const char* envRoll = getenv("CHRONO_ROLL");
    if (envRoll && envRoll[0]) roll = atoi(envRoll);
    srand((unsigned)roll);

    int shmSegId = cr::shmSegmentCreate();
    if (shmSegId < 0) {
        perror("shmget");
        return 1;
    }
    SharedMemory* shm = cr::shmSegmentAt(shmSegId);
    if (!shm) {
        perror("shmat");
        cr::shmSegmentMarkRemoved(shmSegId);
        return 1;
    }
    {
        char ebuf[32];
        snprintf(ebuf, sizeof(ebuf), "%d", shmSegId);
        if (setenv(cr::shmidEnvVar, ebuf, 1) != 0) {
            fprintf(stderr, "setenv %s failed\n", cr::shmidEnvVar);
            cr::shmSegmentDetach(shm);
            cr::shmSegmentMarkRemoved(shmSegId);
            return 1;
        }
    }
    memset(shm, 0, sizeof(SharedMemory));
    gShm = shm;
    shm->fxSpecialWeaponId = -1;
    shm->sigTermReceived = 0;
    shm->sigAlarmReceived = 0;

    installSignalHandler(SIGTERM, onTerminate);

    initMutexPshared(&shm->stateLock);
    initMutexPshared(&shm->actionLock);
    initMutexPshared(&shm->logLock);
    initMutexPshared(&shm->artifacts.lock);
    for (int i = 0; i < cr::maxPlayers; i++) initSemPshared(&shm->toPlayer[i], 0);
    for (int i = 0; i < cr::maxEnemies; i++) initSemPshared(&shm->toNpc[i], 0);
    initSemPshared(&shm->humanReady, 0);
    initSemPshared(&shm->aiReady, 0);
    initSemPshared(&shm->aspNpcSlotWakeup, 0);

    shm->gameStatus = GameStatus::running;
    shm->rollNumber = roll;
    shm->numPlayers = party;
    /* Uniform 2–9; deterministic from roll only (same CHRONO_ROLL → same cap). */
    {
        std::mt19937 capRng((unsigned)roll ^ 0xA53CCA55u);
        std::uniform_int_distribution<int> capDist(cr::minEnemies, cr::maxEnemiesSpawn);
        shm->concurrentCap = capDist(capRng);
    }
    shm->numEnemies = shm->concurrentCap;
    shm->totalEnemiesSpawned = shm->concurrentCap;
    shm->arbiterPid = getpid();

    /* Player HP: roll number + rand[100,1000]. Damage: last digit of roll + 10. Speed: 100/party. */
    int d1 = roll % 10;
    for (int i = 0; i < party; i++) {
        Entity* p = &shm->players[i];
        memset(p, 0, sizeof(Entity));
        p->id = i;
        p->isPlayer = 1;
        p->isAlive = 1;
        snprintf(p->name, sizeof(p->name), "hero %d", i);
        p->maxHp = roll + (rand() % 901 + 100);
        p->hp = p->maxHp;
        p->damage = d1 + 10;
        p->speed = 100 / party;
        if (p->speed < 1) p->speed = 1;
        p->maxStamina = cr::maxStaminaPlayer;
        p->stamina = 0;
        clearInv(&p->inv);
        p->lts.count = 0;
        p->swapInCooldownWeapon = weaponIds::none;
        if (i == 0) {
            /* Spec: Solar Core + Lunar Blade occupy all 20 slots; ultimate eligible with both equipped. */
            placeWeaponFit(p, weaponIds::solarCore);
            placeWeaponFit(p, weaponIds::lunarBlade);
        } else {
            placeWeaponFit(p, weaponIds::splinterStick);
            placeWeaponFit(p, weaponIds::venomDagger);
        }
    }

    for (int i = 0; i < cr::maxEnemies; i++) shm->enemies[i].isAlive = 0;
    for (int i = 0; i < shm->concurrentCap; i++) spawnEnemy(shm, i);

    shm->artifacts.table[(int)ArtifactId::solarCore].holderId = -1;
    shm->artifacts.table[(int)ArtifactId::lunarBlade].holderId = -1;
    shm->artifacts.table[(int)ArtifactId::eclipseRelic].holderId = -1;
    shm->artifacts.table[(int)ArtifactId::solarCore].exists = 1;
    shm->artifacts.table[(int)ArtifactId::lunarBlade].exists = 1;
    shm->artifacts.table[(int)ArtifactId::eclipseRelic].exists = 0;
    // hero 0 starts with both, so lock both artifacts for consistency
    if (shm->numPlayers > 0) {
        shm->artifacts.table[(int)ArtifactId::solarCore].holderId = shm->players[0].id;
        shm->artifacts.table[(int)ArtifactId::lunarBlade].holderId = shm->players[0].id;
    }

    {
        float pace = 1.0f;
        const char* pe = getenv("CHRONO_PACE");
        if (pe && pe[0]) pace = (float)atof(pe);
        if (pace < 0.01f) pace = 0.01f;
        if (pace > 10.f) pace = 10.f;
        shm->staminaPace = pace;
    }

    char hipPath[512];
    char aspPath[512];
    snprintf(hipPath, sizeof(hipPath), "%s/hip_exec", binDir);
    snprintf(aspPath, sizeof(aspPath), "%s/asp_exec", binDir);

    pid_t hipPid = fork();
    if (hipPid == 0) {
        execl(hipPath, "hip_exec", (char*)nullptr);
        perror("execl hip");
        _exit(1);
    }
    shm->humanPid = hipPid;

    pid_t aspPid = fork();
    if (aspPid == 0) {
        execl(aspPath, "asp_exec", (char*)nullptr);
        perror("execl asp");
        _exit(1);
    }
    shm->aiPid = aspPid;
    gAspPid = aspPid;

    logEvent(shm, "arbiter online: SysV shm + forks ok (ui runs inside HIP render thread)");

    pthread_t deadT;
    pthread_create(&deadT, nullptr, deadlockWatch, shm);

    pthread_t staminaT;
    pthread_create(&staminaT, nullptr, staminaTickerThread, shm);

    for (;;) {
        pthread_mutex_lock(&shm->stateLock);
        GameStatus gsLoop = shm->gameStatus;
        pthread_mutex_unlock(&shm->stateLock);
        if (gsLoop != GameStatus::running) break;

        int st = 0;
        pid_t h = waitpid(hipPid, &st, WNOHANG);
        if (h == hipPid) {
            pthread_mutex_lock(&shm->stateLock);
            shm->gameStatus = GameStatus::quit;
            logEvent(shm, "HIP exited early -> arbiter stopping safely");
            pthread_mutex_unlock(&shm->stateLock);
            break;
        }
        pid_t aPid = waitpid(aspPid, &st, WNOHANG);
        if (aPid == aspPid) {
            pthread_mutex_lock(&shm->stateLock);
            shm->gameStatus = GameStatus::quit;
            logEvent(shm, "ASP exited early -> arbiter stopping safely");
            pthread_mutex_unlock(&shm->stateLock);
            break;
        }

        pthread_mutex_lock(&shm->stateLock);
        if (shm->sigTermReceived) {
            shm->sigTermReceived = 0;
            shm->gameStatus = GameStatus::quit;
            logEvent(shm, "SIGTERM received by arbiter -> quit");
        }
        if (shm->sigAlarmReceived) {
            shm->sigAlarmReceived = 0;
            alarm(0);
            resumeAspIfFrozen(shm);
        }
        int keepRunning = (shm->gameStatus == GameStatus::running);
        pthread_mutex_unlock(&shm->stateLock);
        if (!keepRunning) break;

        pthread_mutex_lock(&shm->stateLock);
        if (shm->enemiesKilled >= cr::enemiesToWin) {
            shm->gameStatus = GameStatus::win;
            pthread_mutex_unlock(&shm->stateLock);
            break;
        }
        if (allPlayersDead(shm)) {
            shm->gameStatus = GameStatus::lose;
            pthread_mutex_unlock(&shm->stateLock);
            break;
        }

        int actor = -1;
        if (!pickNextActor(shm, &actor)) {
            pthread_mutex_unlock(&shm->stateLock);
            usleep(32000);
            continue;
        }
        shm->currentActorId = actor;
        Entity* who = entityById(shm, actor);
        memset(&shm->pendingAction, 0, sizeof(Action));
        shm->pendingAction.ready = 0;

        if (!who) {
            pthread_mutex_unlock(&shm->stateLock);
            continue;
        }
        if (who->isPlayer) {
            int idx = actor;
            shm->players[idx].swapInCooldownWeapon = weaponIds::none;
            pthread_mutex_unlock(&shm->stateLock);
            auto ta_t0 = std::chrono::steady_clock::now();
            sem_post(&shm->toPlayer[idx]);
            sem_wait(&shm->humanReady);
            pthread_mutex_lock(&shm->stateLock);
            auto ta_t1 = std::chrono::steady_clock::now();
            processPlayerAction(shm, &shm->pendingAction);
            auto ta_t2 = std::chrono::steady_clock::now();
            if (taAnalysisEnabled()) {
                uint64_t e2e = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(ta_t2 - ta_t0)
                                   .count();
                uint64_t ap =
                    (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(ta_t2 - ta_t1).count();
                taRecord(&gTaPlayerE2e, e2e);
                taRecord(&gTaPlayerArbiter, ap);
            }
        } else {
            int slot = enemySlotFromId(shm, actor);
            pthread_mutex_unlock(&shm->stateLock);
            auto ta_t0 = std::chrono::steady_clock::now();
            sem_post(&shm->toNpc[slot]);
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += cr::npcTurnTimeoutSeconds;
            int w = 0;
            while ((w = sem_timedwait(&shm->aiReady, &ts)) == -1 && errno == EINTR) {
            }
            pthread_mutex_lock(&shm->stateLock);
            if (w == -1) {
                auto ta_t2 = std::chrono::steady_clock::now();
                if (taAnalysisEnabled()) {
                    uint64_t e2e = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(ta_t2 - ta_t0)
                                       .count();
                    taRecord(&gTaNpcE2e, e2e);
                }
                logEvent(shm, "npc turn timeout -> skip");
                if (who && who->isAlive && !who->isPlayer) who->stamina = who->maxStamina * 0.5f;
            } else {
                auto ta_t1 = std::chrono::steady_clock::now();
                processEnemyAction(shm, &shm->pendingAction);
                auto ta_t2 = std::chrono::steady_clock::now();
                if (taAnalysisEnabled()) {
                    uint64_t e2e = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(ta_t2 - ta_t0)
                                       .count();
                    uint64_t ap =
                        (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(ta_t2 - ta_t1).count();
                    taRecord(&gTaNpcE2e, e2e);
                    taRecord(&gTaNpcArbiter, ap);
                }
            }
        }

        pthread_mutex_unlock(&shm->stateLock);
    }

    if (shm->aspFrozen && gAspPid > 0) kill(gAspPid, SIGCONT);

    shm->humanThreadsShouldExit = 1;
    for (int i = 0; i < cr::maxPlayers; i++) sem_post(&shm->toPlayer[i]);
    for (int i = 0; i < cr::maxEnemies; i++) sem_post(&shm->toNpc[i]);
    sem_post(&shm->aspNpcSlotWakeup);

    int st = 0;
    waitpid(hipPid, &st, 0);
    waitpid(aspPid, &st, 0);

    pthread_join(staminaT, nullptr);
    pthread_join(deadT, nullptr);

    GameStatus finalStatus = shm->gameStatus;
    if (taAnalysisEnabled()) {
        writeTurnaroundAnalysis(roll, party, shm->concurrentCap);
        printf("\n--- Turnaround analysis (std::chrono::steady_clock) ---\n");
        printf("%-32s %8s %10s %10s %10s\n", "METRIC", "COUNT", "MIN_ms", "AVG_ms", "MAX_ms");
        printf("---------------------------------------- -------- ---------- ---------- ----------\n");
        taFprintRow(stdout, "Player turn end-to-end", &gTaPlayerE2e);
        taFprintRow(stdout, "Player arbiter apply", &gTaPlayerArbiter);
        taFprintRow(stdout, "NPC turn end-to-end", &gTaNpcE2e);
        taFprintRow(stdout, "NPC arbiter apply", &gTaNpcArbiter);
        printf("Full write-up: turnaround_analysis.txt\n");
    }
    cr::shmSegmentDetach(shm);
    cr::shmSegmentMarkRemoved(shmSegId);
    printf("game over (status %d)\n", (int)finalStatus);
    return 0;
}
