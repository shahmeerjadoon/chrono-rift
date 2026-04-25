#ifndef SHARED_MEMORY_H
#define SHARED_MEMORY_H

#include <pthread.h>
#include <semaphore.h>
#include <sys/types.h>
#include <time.h>
#include <string.h>

/* ─────────────────────────────────────────────────────────────
   CONSTANTS
───────────────────────────────────────────────────────────── */

#define SHM_NAME            "/chrono_rift"

#define MAX_PLAYERS         4
#define MAX_ENEMIES         9
#define MAX_ENTITIES        (MAX_PLAYERS + MAX_ENEMIES)

#define INV_SIZE            20
#define LTS_SIZE            64

#define MAX_STAMINA_PLAYER  100
#define MAX_STAMINA_ENEMY   150

#define ENEMIES_TO_WIN      10
#define MIN_ENEMIES         2
#define MAX_ENEMIES_SPAWN   9

#define STUN_DURATION_SEC   3
#define ULTIMATE_PAUSE_SEC  10
#define NPC_TURN_TIMEOUT    3

#define LOG_LINES           10
#define LOG_LINE_LEN        128

/* ─────────────────────────────────────────────────────────────
   WEAPON IDs
───────────────────────────────────────────────────────────── */

typedef enum {
    wpnNone          = -1,
    wpnSolarCore     =  0,
    wpnLunarBlade    =  1,
    wpnIronHalberd   =  2,
    wpnVenomDagger   =  3,
    wpnThunderstaff  =  4,
    wpnObsidianAxe   =  5,
    wpnFrostbow      =  6,
    wpnSplinterStick =  7,
    wpnEclipseRelic  =  8,
    wpnCount         =  9
} WeaponID;

/* ─────────────────────────────────────────────────────────────
   WEAPON DEFINITION
───────────────────────────────────────────────────────────── */

typedef struct {
    WeaponID id;
    char     name[32];
    int      slotSize;
    int      damage;
    int      isArtifact;
} WeaponDef;

extern const WeaponDef weapons[wpnCount];

/* ─────────────────────────────────────────────────────────────
   ACTION TYPES
───────────────────────────────────────────────────────────── */

typedef enum {
    actNone      = 0,
    actStrike,
    actExhaust,
    actUseWeapon,
    actSwapIn,
    actHeal,
    actSkip,
    actUltimate,
    actQuit
} ActionType;

/* ─────────────────────────────────────────────────────────────
   ARTIFACT IDs
───────────────────────────────────────────────────────────── */

typedef enum {
    artSolarCore    = 0,
    artLunarBlade   = 1,
    artEclipseRelic = 2,
    artCount        = 3
} ArtifactID;

/* ─────────────────────────────────────────────────────────────
   GAME STATUS
───────────────────────────────────────────────────────────── */

typedef enum {
    gameRunning = 0,
    gameWin     = 1,
    gameLose    = 2,
    gameQuit    = 3
} GameStatus;

/* ─────────────────────────────────────────────────────────────
   INVENTORY
───────────────────────────────────────────────────────────── */

typedef struct {
    int slots[INV_SIZE];    /* holds WeaponID per slot, wpnNone = empty */
} Inventory;

typedef struct {
    WeaponID items[LTS_SIZE];
    int      count;
} LongTermStorage;

/* ─────────────────────────────────────────────────────────────
   ENTITY
───────────────────────────────────────────────────────────── */

typedef struct {
    int    id;
    char   name[32];
    int    isPlayer;        /* 1 = human, 0 = NPC                     */
    int    isAlive;

    int    hp;
    int    maxHp;
    int    damage;
    int    speed;
    float  stamina;
    int    maxStamina;

    int    isStunned;
    struct timespec stunEnd;

    Inventory       inv;
    LongTermStorage lts;

    int    waitingForArtifact;  /* ArtifactID or -1 if not waiting    */
} Entity;

/* ─────────────────────────────────────────────────────────────
   ACTION BUFFER
───────────────────────────────────────────────────────────── */

typedef struct {
    int        actorId;
    ActionType type;
    int        targetId;    /* -1 if not needed                       */
    WeaponID   weapon;      /* for actUseWeapon and actSwapIn         */
    int        ltsIndex;    /* for actSwapIn                          */
    int        ready;       /* 1 = arbiter should process this        */
} Action;

/* ─────────────────────────────────────────────────────────────
   ARTIFACT RESOURCE TABLE
───────────────────────────────────────────────────────────── */

typedef struct {
    int holderId;   /* entity id holding it, -1 if free               */
    int exists;     /* eclipseRelic starts 0 until spawned            */
} ArtifactEntry;

typedef struct {
    ArtifactEntry   table[artCount];
    pthread_mutex_t lock;
} ArtifactTable;

/* ─────────────────────────────────────────────────────────────
   SHARED MEMORY
───────────────────────────────────────────────────────────── */

typedef struct {

    /* ── Sync primitives (ALL must be PTHREAD_PROCESS_SHARED) ── */
    pthread_mutex_t stateLock;
    pthread_mutex_t actionLock;
    pthread_mutex_t logLock;
    sem_t           turnReady;
    sem_t           actionDone;

    /* ── Process IDs ────────────────────────────────────────── */
    pid_t arbiterPid;
    pid_t humanPid;
    pid_t aiPid;

    /* ── Game state ─────────────────────────────────────────── */
    GameStatus gameStatus;
    int        currentActorId;
    int        enemiesKilled;
    int        numPlayers;
    int        numEnemies;
    int        totalEnemiesEver;

    /* ── Entities ───────────────────────────────────────────── */
    Entity players[MAX_PLAYERS];
    Entity enemies[MAX_ENEMIES];

    /* ── Action buffer ──────────────────────────────────────── */
    Action pendingAction;

    /* ── Artifact table ─────────────────────────────────────── */
    ArtifactTable artifacts;

    /* ── Stun ───────────────────────────────────────────────── */
    int stunTargetId;
    int stunIsPlayer;   /* 1 = target in players[], 0 = enemies[]     */

    /* ── Ultimate ───────────────────────────────────────────── */
    int ultimateActive;

    /* ── Weapon drop ────────────────────────────────────────── */
    int      dropPending;
    WeaponID dropWeapon;
    int      dropDecided;
    int      dropPlayerTakes;

    /* ── UI log ring buffer ─────────────────────────────────── */
    char logLines[LOG_LINES][LOG_LINE_LEN];
    int  logHead;

} SharedMemory;

/* ─────────────────────────────────────────────────────────────
   INLINE HELPERS
───────────────────────────────────────────────────────────── */

static inline Entity *getEntity(SharedMemory *shm, int id) {
    if (id < shm->numPlayers)
        return &shm->players[id];
    return &shm->enemies[id - shm->numPlayers];
}

static inline int canUseUltimate(Entity *e) {
    int hasSolar = 0, hasLunar = 0;
    for (int i = 0; i < INV_SIZE; i++) {
        if (e->inv.slots[i] == wpnSolarCore)  hasSolar = 1;
        if (e->inv.slots[i] == wpnLunarBlade) hasLunar = 1;
    }
    return hasSolar && hasLunar;
}

static inline int hasWeapon(Entity *e, WeaponID wpn) {
    for (int i = 0; i < INV_SIZE; i++)
        if (e->inv.slots[i] == wpn) return 1;
    return 0;
}

static inline void logEvent(SharedMemory *shm, const char *msg) {
    pthread_mutex_lock(&shm->logLock);
    strncpy(shm->logLines[shm->logHead], msg, LOG_LINE_LEN - 1);
    shm->logLines[shm->logHead][LOG_LINE_LEN - 1] = '\0';
    shm->logHead = (shm->logHead + 1) % LOG_LINES;
    pthread_mutex_unlock(&shm->logLock);
}

#endif /* SHARED_MEMORY_H */