#include "chrono_shm.hpp"

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static SharedMemory* gShm = nullptr;

static pthread_mutex_t gNpcEnsureLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t gNpcThreads[cr::maxEnemies];
static int gNpcSlotArg[cr::maxEnemies];
static int gNpcStarted[cr::maxEnemies];

/* SIGUSR1 trap — sleep in sigsuspend until SIGUSR2 wakes us. yeah its ugly but spec wants signals not spinning */
static void onStunInterrupt(int) {
    sigset_t suspendMask;
    sigfillset(&suspendMask);
    sigdelset(&suspendMask, SIGUSR2);
    sigsuspend(&suspendMask);
}

static void onStunWake(int) {
    (void)0;
}

static void installStunHandlers() {
    struct sigaction sa1;
    memset(&sa1, 0, sizeof(sa1));
    sa1.sa_handler = onStunInterrupt;
    sigemptyset(&sa1.sa_mask);
    sigaddset(&sa1.sa_mask, SIGUSR1);
    sigaction(SIGUSR1, &sa1, nullptr);

    struct sigaction sa2;
    memset(&sa2, 0, sizeof(sa2));
    sa2.sa_handler = onStunWake;
    sigemptyset(&sa2.sa_mask);
    sigaction(SIGUSR2, &sa2, nullptr);
}

/* still inside stun timer window */
static int enemyInStunWindow(SharedMemory* shm, int slot) {
    pthread_mutex_lock(&shm->stateLock);
    Entity* self = &shm->enemies[slot];
    if (!self->isStunned) {
        pthread_mutex_unlock(&shm->stateLock);
        return 0;
    }
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int inWin = (now.tv_sec < self->stunEnd.tv_sec ||
                 (now.tv_sec == self->stunEnd.tv_sec && now.tv_nsec < self->stunEnd.tv_nsec));
    pthread_mutex_unlock(&shm->stateLock);
    return inWin;
}

static void* npcThread(void* pv) {
    int slot = *static_cast<int*>(pv);
    SharedMemory* shm = gShm;

#ifdef __linux__
    pthread_mutex_lock(&shm->stateLock);
    shm->npcOsTid[slot] = (pid_t)syscall(SYS_gettid);
    pthread_mutex_unlock(&shm->stateLock);
#endif

    while (1) {
        while (sem_wait(&shm->toNpc[slot]) == -1 && errno == EINTR) {
        }
        if (shm->humanThreadsShouldExit) break;

        pthread_mutex_lock(&shm->stateLock);
        int expectedId = shm->numPlayers + slot;
        int cur = shm->currentActorId;
        Entity* self = &shm->enemies[slot];
        int alive = self->isAlive;
        pthread_mutex_unlock(&shm->stateLock);

        if (!alive || cur != expectedId) continue;

        if (enemyInStunWindow(shm, slot)) {
            sigset_t blockUsr2;
            sigemptyset(&blockUsr2);
            sigaddset(&blockUsr2, SIGUSR2);
            sigset_t oldmask;
            pthread_sigmask(SIG_BLOCK, &blockUsr2, &oldmask);
            while (enemyInStunWindow(shm, slot)) {
                sigsuspend(&oldmask);
            }
            pthread_sigmask(SIG_SETMASK, &oldmask, nullptr);
        }

        pthread_mutex_lock(&shm->stateLock);
        cur = shm->currentActorId;
        alive = shm->enemies[slot].isAlive;
        pthread_mutex_unlock(&shm->stateLock);
        if (!alive || cur != expectedId) continue;

        Action a = {};
        a.actorId = expectedId;
        a.ready = 1;
        if (rand() % 5 == 0) {
            a.type = ActionType::skip;
        } else {
            a.type = ActionType::strike;
            pthread_mutex_lock(&shm->stateLock);
            int pool[cr::maxPlayers];
            int n = 0;
            for (int i = 0; i < shm->numPlayers; i++) {
                if (shm->players[i].isAlive) pool[n++] = shm->players[i].id;
            }
            pthread_mutex_unlock(&shm->stateLock);
            if (n == 0)
                a.type = ActionType::skip;
            else
                a.targetId = pool[rand() % n];
        }

        pthread_mutex_lock(&shm->actionLock);
        shm->pendingAction = a;
        pthread_mutex_unlock(&shm->actionLock);
        sem_post(&shm->aiReady);
    }
    return nullptr;
}

static void ensureNpcThreadForSlot(int slot) {
    if (slot < 0 || slot >= cr::maxEnemies) return;
    pthread_mutex_lock(&gNpcEnsureLock);
    if (gNpcStarted[slot]) {
        pthread_mutex_unlock(&gNpcEnsureLock);
        return;
    }
    gNpcSlotArg[slot] = slot;
    pthread_create(&gNpcThreads[slot], nullptr, npcThread, &gNpcSlotArg[slot]);
    gNpcStarted[slot] = 1;
    pthread_mutex_unlock(&gNpcEnsureLock);
}

/* arbiter pokes semaphore when a slot wakes up — spawn thread once per slot */
static void* aspSlotSupervisor(void* /*arg*/) {
    SharedMemory* shm = gShm;
    for (;;) {
        while (sem_wait(&shm->aspNpcSlotWakeup) == -1 && errno == EINTR) {
        }
        int exitReq = 0;
        int slot = -1;
        pthread_mutex_lock(&shm->stateLock);
        exitReq = shm->humanThreadsShouldExit;
        slot = shm->aspNpcSlotIndex;
        pthread_mutex_unlock(&shm->stateLock);
        if (exitReq) break;
        ensureNpcThreadForSlot(slot);
    }
    return nullptr;
}

int main() {
    const char* ev = getenv(cr::shmidEnvVar);
    if (!ev || !ev[0]) {
        fprintf(stderr, "asp: missing env %s (start via arbiter_exec)\n", cr::shmidEnvVar);
        return 1;
    }
    gShm = cr::shmSegmentAt(atoi(ev));
    if (!gShm) {
        perror("asp shmat");
        return 1;
    }

    installStunHandlers();

    srand((unsigned)gShm->rollNumber ^ (unsigned)getpid());

    memset(gNpcStarted, 0, sizeof(gNpcStarted));

    int concurrentCap = 0;
    int numEnemies = 0;
    pthread_mutex_lock(&gShm->stateLock);
    concurrentCap = gShm->concurrentCap;
    numEnemies = gShm->numEnemies;
    pthread_mutex_unlock(&gShm->stateLock);

    int nSlots = concurrentCap;
    if (numEnemies > nSlots) nSlots = numEnemies;
    if (nSlots < 0) nSlots = 0;
    if (nSlots > cr::maxEnemies) nSlots = cr::maxEnemies;

    for (int i = 0; i < nSlots; i++) ensureNpcThreadForSlot(i);

    pthread_t supervisor;
    if (pthread_create(&supervisor, nullptr, aspSlotSupervisor, nullptr) != 0) {
        perror("asp pthread_create supervisor");
        cr::shmSegmentDetach(gShm);
        return 1;
    }

    {
        sigset_t blk;
        sigemptyset(&blk);
        sigaddset(&blk, SIGUSR1);
        sigaddset(&blk, SIGUSR2);
        pthread_sigmask(SIG_BLOCK, &blk, nullptr);
    }

    while (1) {
        pthread_mutex_lock(&gShm->stateLock);
        GameStatus st = gShm->gameStatus;
        pthread_mutex_unlock(&gShm->stateLock);
        if (st != GameStatus::running) break;
        usleep(40000);
    }

    pthread_join(supervisor, nullptr);
    for (int i = 0; i < cr::maxEnemies; i++) {
        if (gNpcStarted[i]) pthread_join(gNpcThreads[i], nullptr);
    }
    cr::shmSegmentDetach(gShm);
    return 0;
}
