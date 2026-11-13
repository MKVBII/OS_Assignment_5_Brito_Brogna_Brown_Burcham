#ifndef RECOVERY_H
#define RECOVERY_H

/*
Deadlock recovery (Member 4 - Recovery & Logging)

Three pieces:

  1. rec_ctx  - a thread's own record of the locks it holds. Locks are taken
                with a timeout, and a thread can only release a lock that is
                in its own record, so it can never unlock another thread's
                mutex.
  2. rec_txn  - staged commit. Changes are prepared with fm_stage_replace
                (the real files are untouched) and made real together only
                once every needed lock is held. Rollback throws them away.
  3. recovery_run_T2 - the full T2 task with abort, rollback, release, and
                one retry in the safe order A then B.

Recovery is cooperative: T2 gives way by itself, either because its wait for
lock A timed out or because the detector called recovery_request_abort(T2_ID).
*/

#include <pthread.h>
#include "threads_locks.h" // LOCK_A.., NUM_LOCKS, T1_ID..
#include "file_manager.h"

// RETURN CODES

#define REC_RETRY_OK 1 // first attempt was abandoned, the retry committed
#define REC_OK 0 // success
#define REC_TIMEOUT -1 // gave up waiting, lock was NOT taken
#define REC_ABORTED -2 // abort was requested while waiting, lock was NOT taken
#define REC_NOT_OWNER -3 // tried to release a lock this thread doesn't hold
#define REC_SAFE_ABORT -4 // task did not finish, no file was changed
#define REC_BAD_ARG -5 // missing argument or bad lock id


// LOCKS A THREAD OWNS

typedef struct {
  int thread_id; // T1_ID..T6_ID
  pthread_mutex_t *mutex[NUM_LOCKS]; // the mutex behind each lock this thread holds
  int held[NUM_LOCKS]; // 1 if this thread holds that lock
} rec_ctx;

void rec_init(rec_ctx *ctx, int thread_id); // start with no locks held
int rec_lock(rec_ctx *ctx, int lock_id, pthread_mutex_t *mutex, int timeout_ms); // take a lock, timeout_ms < 0 waits until an abort is requested
int rec_unlock(rec_ctx *ctx, int lock_id); // release one lock, only if this thread holds it
int rec_release_all(rec_ctx *ctx, const char *reason); // release every lock this thread holds, returns how many
int rec_holds(const rec_ctx *ctx, int lock_id); // returns 1 if this thread holds the lock, 0 otherwise


// COOPERATIVE ABORT (called by the deadlock detector)

void recovery_request_abort(int thread_id); // ask a thread to give way, its pending rec_lock returns REC_ABORTED
void recovery_clear_abort(int thread_id); // forget an abort request
int recovery_waiting_for(int thread_id); // lock id the thread is blocked on, or -1 (wait table entry)
int recovery_owner_of(int lock_id); // thread id that took the lock through rec_lock, or -1


// STAGED COMMIT

typedef struct {
  int thread_id;
  int count; // number of files with a staged change
  char files[NUM_LOCKS][FM_MAX_NAME];
} rec_txn;

void rec_txn_begin(rec_txn *txn, int thread_id); // start with nothing staged
int rec_txn_stage_replace(rec_txn *txn, const char *name, const char *old_line, const char *new_line); // prepare a change (real file is untouched)
int rec_txn_commit(rec_txn *txn, const rec_ctx *ctx); // make every staged change real, all of them or none of them
int rec_txn_rollback(rec_txn *txn, const char *reason); // throw every staged change away, returns how many


// T2 WITH RECOVERY

typedef struct {
  pthread_mutex_t *lock_a; // students.txt
  pthread_mutex_t *lock_b; // grades.txt
  int wait_timeout_ms; // how long the first attempt waits for A (< 0 = until the detector requests an abort)
  int retry_timeout_ms; // how long the retry waits for each lock
  void (*sync_point)(void *arg); // called while holding B, before requesting A (i.e. the barrier wait), may be NULL
  void *sync_arg;
} rec_t2_config;

/*
Runs T2 in the deadlock order (B then A). If the wait for A times out or an
abort is requested: roll back, release B, and retry once in the order A then B.
Returns REC_OK, REC_RETRY_OK, or REC_SAFE_ABORT (no file was changed).
*/
int recovery_run_T2(const rec_t2_config *cfg);

const char *rec_strerror(int code); // readable text for return codes

#endif
