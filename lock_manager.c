#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include "lock_manager.h"

#define MAX_LOCKS 8
#define WAIT_TIMEOUT_SEC 3

static pthread_mutex_t lm_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t lm_cv = PTHREAD_COND_INITIALIZER;

static int nlocks; // number of locks in the system
static int owner[MAX_LOCKS]; // owner[lock_id] = tid if lock_id is held by tid, LM_NONE otherwise
static int waiting_for[LM_MAX_THREADS]; // waiting_for[tid] = lock_id if tid is waiting for lock_id, LM_NONE otherwise
static int abort_flag[LM_MAX_THREADS]; // abort flag for each thread
static int cycles_detected = 0;

static const char *lname(int l) { static const char *n[] = {"A", "B", "C", "D"}; return l >= 0 && l < 4 ? n[l] : "?"; }

int lm_cycle_count(void) {
    return cycles_detected;
}

// Initialize the lock manager with a given number of locks
void lm_init(int num_locks) {
    nlocks = num_locks;
    cycles_detected = 0;
    for (int i = 0; i < nlocks; i++) 
        owner[i] = LM_NONE;
    for (int i = 0; i < LM_MAX_THREADS; i++) {
        waiting_for[i] = LM_NONE;
        abort_flag[i] = 0;
    }
}
// Destroy the lock manager, cleaning up resources
void lm_destroy(void) {
    pthread_mutex_destroy(&lm_mtx);
    pthread_cond_destroy(&lm_cv);
}

// Detects if there is a cycle in the wait-for graph starting from a given thread
static int find_cycle(int start, int cycle[], int *len) {
    int cur = start, n = 0;
    while (n < LM_MAX_THREADS) {
        cycle[n++] = cur;
        int l = waiting_for[cur];        // lock cur wants
        if (l == LM_NONE) 
            return 0;      // cur isn't waiting: no cycle
        int next = owner[l];             // thread that holds it
        if (next == LM_NONE) 
            return 0;   // lock is free: no cycle
        if (next == start) {
             *len = n; 
             return 1; }
        cur = next;
    }
    return 0;
}

int lm_lock(int tid, int lock_id) {
    char buf[96];
    pthread_mutex_lock(&lm_mtx);

    while (owner[lock_id] != LM_NONE) {
        if (abort_flag[tid]) {                         // someone picked us as victim
            abort_flag[tid] = 0; waiting_for[tid] = LM_NONE;
            pthread_mutex_unlock(&lm_mtx);
            return LM_ABORT;
        }

        waiting_for[tid] = lock_id;
        snprintf(buf, sizeof buf, "owner=T%d", owner[lock_id]);
        lm_log(tid, "LOCK_WAIT", lname(lock_id), "WAIT", buf);

        int cycle[LM_MAX_THREADS], len;
        if (find_cycle(tid, cycle, &len)) {
            int victim = cycle[0];
            for (int i = 1; i < len; i++) if (cycle[i] > victim) victim = cycle[i];  // highest id = T2

            if (!abort_flag[victim]) {                 // log once, not on every wakeup
                cycles_detected++;
                char path[64] = "";
                for (int i = 0; i < len; i++) { char t[8]; snprintf(t, sizeof t, "T%d->", cycle[i]); strcat(path, t); }
                snprintf(buf, sizeof buf, "%sT%d victim=T%d", path, cycle[0], victim);
                lm_log(0, "DEADLOCK_DETECTED", "A,B", "ALERT", buf);
                lm_print_state();
            }
            if (victim == tid) {                       // we are the victim: back out ourselves
                waiting_for[tid] = LM_NONE;
                pthread_mutex_unlock(&lm_mtx);
                return LM_DEADLOCK;
            }
            abort_flag[victim] = 1;                    // ask the victim to abort (cooperative)
            pthread_cond_broadcast(&lm_cv);
        }

        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += WAIT_TIMEOUT_SEC;
        if (pthread_cond_timedwait(&lm_cv, &lm_mtx, &ts) == ETIMEDOUT && owner[lock_id] != LM_NONE) {
            waiting_for[tid] = LM_NONE;
            lm_log(tid, "LOCK_TIMEOUT", lname(lock_id), "TIMEOUT", "gave up waiting");
            pthread_mutex_unlock(&lm_mtx);
            return LM_TIMEOUT;
        }
    }

    owner[lock_id] = tid;
    waiting_for[tid] = LM_NONE;
    lm_log(tid, "LOCK_ACQUIRED", lname(lock_id), "OK", "");
    pthread_mutex_unlock(&lm_mtx);
    return LM_OK;
}

int lm_unlock(int tid, int lock_id) {
    pthread_mutex_lock(&lm_mtx);
    if (owner[lock_id] != tid) {                       // never release someone else's lock
        lm_log(tid, "LOCK_RELEASE_DENIED", lname(lock_id), "ERROR", "not owner");
        pthread_mutex_unlock(&lm_mtx);
        return LM_NOT_OWNER;
    }
    owner[lock_id] = LM_NONE;
    lm_log(tid, "LOCK_RELEASED", lname(lock_id), "OK", "");
    pthread_cond_broadcast(&lm_cv);
    pthread_mutex_unlock(&lm_mtx);
    return LM_OK;
}

void lm_release_all(int tid) {
    for (int l = 0; l < nlocks; l++) {
        pthread_mutex_lock(&lm_mtx);
        int mine = (owner[l] == tid);
        pthread_mutex_unlock(&lm_mtx);
        if (mine) lm_unlock(tid, l);
    }
}

// Call with lm_mtx held (it is, from lm_lock)
void lm_print_state(void) {
    printf("  --- Lock table / wait-for state ---\n");
    for (int l = 0; l < nlocks; l++)
        if (owner[l] != LM_NONE) printf("  Lock %s held by T%d\n", lname(l), owner[l]);
    for (int t = 1; t < LM_MAX_THREADS; t++)
        if (waiting_for[t] != LM_NONE)
            printf("  T%d waits for %s (owned by T%d)  =>  edge T%d -> T%d\n",
                   t, lname(waiting_for[t]), owner[waiting_for[t]], t, owner[waiting_for[t]]);
}