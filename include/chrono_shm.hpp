#ifndef CHRONO_SHM_HPP
#define CHRONO_SHM_HPP

#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <time.h>

namespace cr {
const int maxPlayers = 4;
const int maxEnemies = 9;
const int invSize = 20;
const int ltsSize = 64;
const int maxStaminaPlayer = 100;
const int maxStaminaEnemy = 150;
const int enemiesToWin = 10;
const int minEnemies = 2;
const int maxEnemiesSpawn = 9;
const int logLines = 10;
const int logLineLen = 128;
const int stunSeconds = 3;
const int ultimatePauseSeconds = 10;
// how long the red "ultimate denied" splash hangs around (ms)
const int ultimateDeniedOverlayMs = 2000;
const int npcTurnTimeoutSeconds = 3;
// shm id — arbiter stuffs this into env so HIP/ASP can attach
const char* const shmidEnvVar = "CHRONO_SHMID";
// multiply stamina ticks by this (1 = spec-ish default)
const float staminaRegenScale = 1.0f;
// extra knob just for enemies if we ever want them faster/slower
const float enemyStaminaRegenMul = 1.0f;
}

namespace weaponIds {
const int none = -1;
const int solarCore = 0;
const int lunarBlade = 1;
const int ironHalberd = 2;
const int venomDagger = 3;
const int thunderstaff = 4;
const int obsidianAxe = 5;
const int frostbow = 6;
const int splinterStick = 7;
const int eclipseRelic = 8;
const int count = 9;
}

struct WeaponDef {
    int id;
    char name[32];
    int slotSize;
    int damage;
    int isArtifact;
};

extern const WeaponDef weaponTable[weaponIds::count];

enum class ActionType : int {
    none = 0,
    strike,
    exhaust,
    useWeapon,
    swapIn,
    heal,
    skip,
    ultimate,
    pickupEclipseRelic,
    quit
};

enum class ArtifactId : int { solarCore = 0, lunarBlade = 1, eclipseRelic = 2, count = 3 };

enum class GameStatus : int { running = 0, win = 1, lose = 2, quit = 3 };

struct Inventory {
    int slots[cr::invSize];
};

struct LongTermStorage {
    int items[cr::ltsSize];
    int count;
};

struct Entity {
    int id;
    char name[32];
    int isPlayer;
    int isAlive;
    int hp;
    int maxHp;
    int damage;
    int speed;
    float stamina;
    int maxStamina;
    int isStunned;
    struct timespec stunEnd;
    Inventory inv;
    LongTermStorage lts;
    int waitingArtifact;
    // weapon you just swapped in — cant mash use same turn
    int swapInCooldownWeapon;
    // first moment we hit full stamina (arbiter uses this for fcfs tie-break)
    int readyStampValid;
    struct timespec readyAtMono;
};

struct Action {
    int actorId;
    ActionType type;
    int targetId;
    int weapon;
    int ltsIndex;
    int ready;
};

struct ArtifactEntry {
    int holderId;
    int exists;
};

struct ArtifactTable {
    ArtifactEntry table[3];
    pthread_mutex_t lock;
};

struct SharedMemory {
    pthread_mutex_t stateLock;
    pthread_mutex_t actionLock;
    pthread_mutex_t logLock;
    sem_t toPlayer[cr::maxPlayers];
    sem_t toNpc[cr::maxEnemies];
    sem_t humanReady;
    sem_t aiReady;
    // bump ASP when a replacement enemy shows up (lazy thread spawn)
    sem_t aspNpcSlotWakeup;

    pid_t arbiterPid;
    pid_t humanPid;
    pid_t aiPid;

    // linux tid — arbiter uses tgkill for stun pings (0 = dunno yet)
    pid_t playerOsTid[cr::maxPlayers];
    pid_t npcOsTid[cr::maxEnemies];

    // SIGTERM / SIGALRM handlers poke these; arbiter main loop clears under lock (0/1)
    int sigTermReceived;
    int sigAlarmReceived;

    GameStatus gameStatus;
    int currentActorId;
    int enemiesKilled;
    int totalEnemiesSpawned;
    int numPlayers;
    int numEnemies;
    int concurrentCap;
    int rollNumber;
    int enemySpawnSeq;

    Entity players[cr::maxPlayers];
    Entity enemies[cr::maxEnemies];

    Action pendingAction;

    ArtifactTable artifacts;

    int stunTargetId;
    int stunIsPlayer;

    int ultimateActive;
    int aspFrozen;

    int dropPending;
    int dropWeapon;
    int dropDecided;
    int dropPlayerTakes;

    // relic sitting on the floor — kill 3 flips this on, pickup clears it
    int eclipseRelicOnGround;

    // kill banner fades out at this time
    struct timespec enemyKillBannerEndMono;

    // ultimate denied popup window
    struct timespec ultimateDeniedStartMono;
    struct timespec ultimateDeniedEndMono;

    char logLines[cr::logLines][cr::logLineLen];
    int logHead;

    int humanThreadsShouldExit;
    // which enemy slot ASP should wake up for (arbiter fills before posting aspNpcSlotWakeup)
    int aspNpcSlotIndex;

    /* HIP battlefield: monotonic clock end time + who got hit (arbiter writes) */
    int hitFlashEnemySlot;
    int hitFlashPlayerIdx;
    struct timespec hitFlashEnemyEndMono;
    struct timespec hitFlashPlayerEndMono;

    /* UI FX (arbiter sets on actions; HIP reads) */
    int fxProjKind;
    int fxProjEnemySlot;
    int fxProjPlayerIdx;
    struct timespec fxProjStartMono;
    struct timespec fxProjEndMono;

    int fxHealPlayerIdx;
    struct timespec fxHealEndMono;

    int fxSpecialWeaponId;
    struct timespec fxSpecialEndMono;
    char fxSpecialText[64];

    float staminaPace;

    int uiTargetEnemySlot;
    struct timespec uiTargetEndMono;

    int dmgPopupKind;  // 1 enemy hit, 2 player hit
    int dmgPopupValue;
    int dmgPopupEnemySlot;
    int dmgPopupPlayerIdx;
    struct timespec dmgPopupStartMono;
    struct timespec dmgPopupEndMono;
};

inline Entity* entityById(SharedMemory* shm, int id) {
    if (id < 0) return nullptr;
    if (id < shm->numPlayers) return &shm->players[id];
    int ei = id - shm->numPlayers;
    if (ei >= 0 && ei < cr::maxEnemies) return &shm->enemies[ei];
    return nullptr;
}

inline int enemySlotFromId(SharedMemory* shm, int id) {
    return id - shm->numPlayers;
}

inline int canUseUltimate(const Entity* e) {
    int hasSolar = 0;
    int hasLunar = 0;
    for (int i = 0; i < cr::invSize; i++) {
        if (e->inv.slots[i] == weaponIds::solarCore) hasSolar = 1;
        if (e->inv.slots[i] == weaponIds::lunarBlade) hasLunar = 1;
    }
    return hasSolar && hasLunar;
}

inline int inventoryHasWeapon(Entity* e, int wpn) {
    for (int i = 0; i < cr::invSize; i++)
        if (e->inv.slots[i] == wpn) return 1;
    return 0;
}

void logEvent(SharedMemory* shm, const char* msg);

namespace cr {
// shmget IPC_PRIVATE — arbiter only really
inline int shmSegmentCreate() {
    return shmget(IPC_PRIVATE, sizeof(SharedMemory), IPC_CREAT | IPC_EXCL | 0600);
}

inline SharedMemory* shmSegmentAt(int shmid) {
    void* p = shmat(shmid, nullptr, 0);
    if (p == reinterpret_cast<void*>(-1)) return nullptr;
    return static_cast<SharedMemory*>(p);
}

inline void shmSegmentDetach(void* addr) {
    if (addr) shmdt(addr);
}

inline void shmSegmentMarkRemoved(int shmid) {
    if (shmid >= 0) shmctl(shmid, IPC_RMID, nullptr);
}
}  // namespace cr

#endif
