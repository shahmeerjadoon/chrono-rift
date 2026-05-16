#include "chrono_shm.hpp"

void logEvent(SharedMemory* shm, const char* msg) {
    pthread_mutex_lock(&shm->logLock);
    strncpy(shm->logLines[shm->logHead], msg, cr::logLineLen - 1);
    shm->logLines[shm->logHead][cr::logLineLen - 1] = '\0';
    shm->logHead = (shm->logHead + 1) % cr::logLines;
    pthread_mutex_unlock(&shm->logLock);
}
