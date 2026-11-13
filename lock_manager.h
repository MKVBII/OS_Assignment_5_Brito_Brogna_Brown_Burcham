#ifndef LOCK_MANAGER_H
#define LOCK_MANAGER_H

#define LM_MAX_THREADS 8   // thread ids 1..6 (T1..T6); index 0 unused
#define LM_NONE (-1)

enum { LM_OK = 0, LM_DEADLOCK = -1, LM_ABORT = -2, LM_TIMEOUT = -3, LM_NOT_OWNER = -4 };

void lm_init(int num_locks);
void lm_destroy(void);
int  lm_lock(int tid, int lock_id);     // LM_OK, or an abort code
int  lm_unlock(int tid, int lock_id);   // only releases if tid owns it
void lm_release_all(int tid);           // release every lock tid owns
void lm_print_state(void);              // ownership + wait table
int lm_cycle_count(void);

// Member 4 implements this (thread-safe, writes execution.log)
void lm_log(int tid, const char *event, const char *resource,
            const char *result, const char *details);
#endif