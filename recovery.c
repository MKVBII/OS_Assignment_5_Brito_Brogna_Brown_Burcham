#define _POSIX_C_SOURCE 200809L // allow POSIX functions (i.e. pthread_mutex_timedlock and clock_gettime)

#include "recovery.h"
#include "logger.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#define MAX_THREAD_ID 6 // T1..T6 (0 is main)
#define WAIT_SLICE_MS 10 // how often a waiting thread checks for an abort request
#define SNAPSHOT_SIZE 4096 // largest file that can be restored if a commit fails part way

// names used in the log
static const char *lock_names[NUM_LOCKS] = {"A", "B", "C", "D"};
static const char *lock_files[NUM_LOCKS] = {FM_FILE_STUDENTS, FM_FILE_GRADES, FM_FILE_AUDIT, FM_FILE_BACKUP};

// shared wait/abort table, protected by table_mutex
static pthread_mutex_t table_mutex = PTHREAD_MUTEX_INITIALIZER;
static int abort_flag[MAX_THREAD_ID + 1]; // 1 = this thread was asked to give way
static int waiting_on[MAX_THREAD_ID + 1]; // lock id + 1 the thread is blocked on (0 = not waiting)
static int owner_of[NUM_LOCKS]; // thread id + 1 that holds the lock (0 = unknown or free)


/*
Returns 1 if the lock id is one of A, B, C, D
*/
static int valid_lock(int lock_id) {
  return lock_id >= 0 && lock_id < NUM_LOCKS;
}

/*
Returns 1 if the thread id has a slot in the shared table
*/
static int valid_thread(int thread_id) {
  return thread_id >= 0 && thread_id <= MAX_THREAD_ID;
}

/*
Returns the lock id that protects a file, or -1 if the file has no lock
*/
static int lock_for_file(const char *name) {
  for (int i = 0; i < NUM_LOCKS; i++) {
    if (strcmp(name, lock_files[i]) == 0) return i;
  }
  return -1;
}

/*
Fills 'out' with the time 'ms' milliseconds from now (for pthread_mutex_timedlock)
*/
static void time_from_now(struct timespec *out, int ms) {
  clock_gettime(CLOCK_REALTIME, out);
  out->tv_sec += ms / 1000;
  out->tv_nsec += (long)(ms % 1000) * 1000000L;
  if (out->tv_nsec >= 1000000000L) {
    out->tv_sec++;
    out->tv_nsec -= 1000000000L;
  }
}

/*
Milliseconds between two times (b - a)
*/
static long ms_between(const struct timespec *a, const struct timespec *b) {
  return (b->tv_sec - a->tv_sec) * 1000L + (b->tv_nsec - a->tv_nsec) / 1000000L;
}

/*
Reads this thread's abort flag
*/
static int abort_requested(int thread_id) {
  if (!valid_thread(thread_id)) return 0;
  pthread_mutex_lock(&table_mutex);
  int flag = abort_flag[thread_id];
  pthread_mutex_unlock(&table_mutex);
  return flag;
}

/*
Records which lock a thread is blocked on (-1 = not waiting)
*/
static void set_waiting(int thread_id, int lock_id) {
  if (!valid_thread(thread_id)) return;
  pthread_mutex_lock(&table_mutex);
  waiting_on[thread_id] = lock_id + 1;
  pthread_mutex_unlock(&table_mutex);
}

/*
Records which thread holds a lock (-1 = free)
*/
static void set_owner(int lock_id, int thread_id) {
  pthread_mutex_lock(&table_mutex);
  owner_of[lock_id] = thread_id + 1;
  pthread_mutex_unlock(&table_mutex);
}


// COOPERATIVE ABORT

void recovery_request_abort(int thread_id) {
  if (!valid_thread(thread_id)) return;
  pthread_mutex_lock(&table_mutex);
  abort_flag[thread_id] = 1; // only a flag is set, the thread's locks are not touched
  pthread_mutex_unlock(&table_mutex);
  log_event(thread_id, "ABORT_REQUESTED", "-", "OK", "victim=T%d", thread_id);
}

void recovery_clear_abort(int thread_id) {
  if (!valid_thread(thread_id)) return;
  pthread_mutex_lock(&table_mutex);
  abort_flag[thread_id] = 0;
  pthread_mutex_unlock(&table_mutex);
}

int recovery_waiting_for(int thread_id) {
  if (!valid_thread(thread_id)) return -1;
  pthread_mutex_lock(&table_mutex);
  int lock_id = waiting_on[thread_id] - 1;
  pthread_mutex_unlock(&table_mutex);
  return lock_id;
}

int recovery_owner_of(int lock_id) {
  if (!valid_lock(lock_id)) return -1;
  pthread_mutex_lock(&table_mutex);
  int thread_id = owner_of[lock_id] - 1;
  pthread_mutex_unlock(&table_mutex);
  return thread_id;
}


// LOCKS A THREAD OWNS

void rec_init(rec_ctx *ctx, int thread_id) {
  ctx->thread_id = thread_id;
  for (int i = 0; i < NUM_LOCKS; i++) {
    ctx->mutex[i] = NULL;
    ctx->held[i] = 0;
  }
}

int rec_holds(const rec_ctx *ctx, int lock_id) {
  return ctx != NULL && valid_lock(lock_id) && ctx->held[lock_id];
}

int rec_lock(rec_ctx *ctx, int lock_id, pthread_mutex_t *mutex, int timeout_ms) {
  if (ctx == NULL || mutex == NULL || !valid_lock(lock_id)) return REC_BAD_ARG;
  if (ctx->held[lock_id]) return REC_BAD_ARG; // locking it again would wait on ourselves

  int tid = ctx->thread_id;
  int rc = REC_OK;

  // fast path, lock is free
  if (pthread_mutex_trylock(mutex) != 0) {
    int owner = recovery_owner_of(lock_id);
    if (owner >= 0) log_event(tid, "LOCK_WAIT", lock_names[lock_id], "WAIT", "owner=T%d", owner);
    else log_event(tid, "LOCK_WAIT", lock_names[lock_id], "WAIT", "lock is busy");
    set_waiting(tid, lock_id);

    struct timespec start, now, slice;
    clock_gettime(CLOCK_REALTIME, &start);

    // wait in short slices so that an abort request or the timeout is noticed quickly
    for (;;) {
      if (abort_requested(tid)) {
        rc = REC_ABORTED;
        break;
      }
      clock_gettime(CLOCK_REALTIME, &now);
      if (timeout_ms >= 0 && ms_between(&start, &now) >= timeout_ms) {
        rc = REC_TIMEOUT;
        break;
      }
      time_from_now(&slice, WAIT_SLICE_MS);
      int r = pthread_mutex_timedlock(mutex, &slice);
      if (r == 0) break; // got the lock
      if (r != ETIMEDOUT) {
        rc = REC_BAD_ARG;
        break;
      }
    }
    set_waiting(tid, -1);
  }

  if (rc == REC_OK) {
    ctx->mutex[lock_id] = mutex;
    ctx->held[lock_id] = 1;
    set_owner(lock_id, tid);
    log_event(tid, "LOCK_ACQUIRED", lock_names[lock_id], "OK", "%s", lock_files[lock_id]);
  } else if (rc == REC_TIMEOUT) {
    log_event(tid, "LOCK_TIMEOUT", lock_names[lock_id], "FAIL", "gave up after %d ms, lock not taken", timeout_ms);
  } else if (rc == REC_ABORTED) {
    log_event(tid, "LOCK_ABORTED", lock_names[lock_id], "FAIL", "abort requested while waiting, lock not taken");
  } else {
    log_event(tid, "LOCK_ERROR", lock_names[lock_id], "ERROR", "mutex could not be locked");
  }
  return rc;
}

int rec_unlock(rec_ctx *ctx, int lock_id) {
  if (ctx == NULL || !valid_lock(lock_id)) return REC_BAD_ARG;

  // a thread may only release a lock that is in its own record
  if (!ctx->held[lock_id]) {
    log_event(ctx->thread_id, "LOCK_RELEASE", lock_names[lock_id], "DENY", "not the owner, nothing released");
    return REC_NOT_OWNER;
  }

  set_owner(lock_id, -1);
  ctx->held[lock_id] = 0;
  pthread_mutex_unlock(ctx->mutex[lock_id]);
  ctx->mutex[lock_id] = NULL;
  log_event(ctx->thread_id, "LOCK_RELEASED", lock_names[lock_id], "OK", "%s", lock_files[lock_id]);
  return REC_OK;
}

int rec_release_all(rec_ctx *ctx, const char *reason) {
  if (ctx == NULL) return 0;
  int released = 0;

  // reverse order (D, C, B, A)
  for (int i = NUM_LOCKS - 1; i >= 0; i--) {
    if (!ctx->held[i]) continue;
    set_owner(i, -1);
    ctx->held[i] = 0;
    pthread_mutex_unlock(ctx->mutex[i]);
    ctx->mutex[i] = NULL;
    log_event(ctx->thread_id, "LOCK_RELEASED", lock_names[i], "OK", "%s", reason ? reason : lock_files[i]);
    released++;
  }
  return released;
}


// STAGED COMMIT

void rec_txn_begin(rec_txn *txn, int thread_id) {
  txn->thread_id = thread_id;
  txn->count = 0;
}

int rec_txn_stage_replace(rec_txn *txn, const char *name, const char *old_line, const char *new_line) {
  if (txn == NULL || name == NULL || old_line == NULL || new_line == NULL) return REC_BAD_ARG;

  int lock_id = lock_for_file(name);
  const char *resource = (lock_id >= 0) ? lock_names[lock_id] : name;

  // is this file already part of the transaction?
  int known = 0;
  for (int i = 0; i < txn->count; i++) {
    if (strcmp(txn->files[i], name) == 0) known = 1;
  }
  if (!known && (txn->count >= NUM_LOCKS || strlen(name) >= FM_MAX_NAME)) return REC_BAD_ARG;

  // a leftover staged file from an earlier crashed run must not leak into this change
  if (!known) fm_discard(name);

  int rc = fm_stage_replace(name, old_line, new_line);
  if (rc != FM_OK) {
    log_event(txn->thread_id, "STAGE_FAILED", resource, "ERROR", "%s: %s", name, fm_strerror(rc));
    return REC_SAFE_ABORT;
  }

  if (!known) strcpy(txn->files[txn->count++], name);
  log_event(txn->thread_id, "STAGED", resource, "OK", "%s: %s -> %s (not written yet)", name, old_line, new_line);
  return REC_OK;
}

int rec_txn_rollback(rec_txn *txn, const char *reason) {
  if (txn == NULL) return 0;
  int discarded = 0;

  for (int i = 0; i < txn->count; i++) {
    if (fm_discard(txn->files[i]) == FM_OK) discarded++;
  }
  txn->count = 0;
  log_event(txn->thread_id, "ROLLBACK_COMPLETE", "files", "OK", "%d staged change(s) discarded, %s", discarded, reason ? reason : "no file was changed");
  return discarded;
}

int rec_txn_commit(rec_txn *txn, const rec_ctx *ctx) {
  static char snapshot[NUM_LOCKS][SNAPSHOT_SIZE]; // original contents, protected by commit_mutex
  static pthread_mutex_t commit_mutex = PTHREAD_MUTEX_INITIALIZER;

  if (txn == NULL || ctx == NULL || txn->count == 0) return REC_BAD_ARG;

  // check everything before touching any file
  for (int i = 0; i < txn->count; i++) {
    int lock_id = lock_for_file(txn->files[i]);

    // commit is only allowed once the lock of every file is held
    if (lock_id < 0 || !rec_holds(ctx, lock_id)) {
      log_event(txn->thread_id, "COMMIT_REFUSED", lock_id >= 0 ? lock_names[lock_id] : "?", "ERROR", "lock for %s is not held, nothing written", txn->files[i]);
      return REC_SAFE_ABORT;
    }
    if (!fm_has_staged(txn->files[i])) {
      log_event(txn->thread_id, "COMMIT_REFUSED", lock_names[lock_id], "ERROR", "staged change for %s is missing, nothing written", txn->files[i]);
      return REC_SAFE_ABORT;
    }
  }

  pthread_mutex_lock(&commit_mutex);

  // remember the original files so a failure part way can be undone
  for (int i = 0; i < txn->count; i++) {
    int rc = fm_read_all(txn->files[i], snapshot[i], SNAPSHOT_SIZE);
    if (rc != FM_OK) {
      pthread_mutex_unlock(&commit_mutex);
      log_event(txn->thread_id, "COMMIT_REFUSED", "files", "ERROR", "cannot snapshot %s (%s), nothing written", txn->files[i], fm_strerror(rc));
      return REC_SAFE_ABORT;
    }
  }

  // make the staged changes real
  for (int i = 0; i < txn->count; i++) {
    int rc = fm_commit(txn->files[i]);
    if (rc == FM_OK) continue;

    // put back the files that were already committed and drop the rest
    for (int j = 0; j < i; j++) fm_write(txn->files[j], snapshot[j]);
    for (int j = i; j < txn->count; j++) fm_discard(txn->files[j]);
    pthread_mutex_unlock(&commit_mutex);
    log_event(txn->thread_id, "COMMIT_FAILED", "files", "ERROR", "%s: %s, earlier files restored", txn->files[i], fm_strerror(rc));
    txn->count = 0;
    return REC_SAFE_ABORT;
  }

  pthread_mutex_unlock(&commit_mutex);
  log_event(txn->thread_id, "COMMIT", "files", "OK", "%d file(s) updated together", txn->count);
  txn->count = 0;
  return REC_OK;
}


// T2 WITH RECOVERY

#define T2_OLD_GRADE "S102,Assignment4,91"
#define T2_NEW_GRADE "S102,Assignment4,94"
#define T2_OLD_STUDENT "S102,Ben,CSC3320"
#define T2_NEW_STUDENT "S102,Benjamin,CSC3320"

int recovery_run_T2(const rec_t2_config *cfg) {
  if (cfg == NULL || cfg->lock_a == NULL || cfg->lock_b == NULL) return REC_BAD_ARG;

  rec_ctx ctx;
  rec_txn txn;
  rec_init(&ctx, T2_ID);
  rec_txn_begin(&txn, T2_ID);
  recovery_clear_abort(T2_ID);

  // FIRST ATTEMPT: deadlock order, B then A

  if (rec_lock(&ctx, LOCK_B, cfg->lock_b, -1) != REC_OK) goto safe_abort;

  // the grade change is only staged, grades.txt itself is still untouched
  if (rec_txn_stage_replace(&txn, FM_FILE_GRADES, T2_OLD_GRADE, T2_NEW_GRADE) != REC_OK) goto safe_abort;

  // let the caller line T1 up so that it holds A (i.e. pthread_barrier_wait)
  if (cfg->sync_point != NULL) cfg->sync_point(cfg->sync_arg);

  int rc = rec_lock(&ctx, LOCK_A, cfg->lock_a, cfg->wait_timeout_ms);

  if (rc == REC_OK) {
    // no deadlock this time, finish normally
    if (rec_txn_stage_replace(&txn, FM_FILE_STUDENTS, T2_OLD_STUDENT, T2_NEW_STUDENT) != REC_OK) goto safe_abort;
    if (rec_txn_commit(&txn, &ctx) != REC_OK) goto safe_abort;
    log_event(T2_ID, "OP_COMPLETE", "students,grades", "OK", "records updated");
    rec_unlock(&ctx, LOCK_A);
    rec_unlock(&ctx, LOCK_B);
    return REC_OK;
  }
  if (rc != REC_TIMEOUT && rc != REC_ABORTED) goto safe_abort;

  // RECOVERY: T2 gives way

  log_event(T2_ID, "RETRY_ABORT", "B", "OK", rc == REC_TIMEOUT ? "wait for A timed out, backing off" : "chosen as victim, backing off");
  rec_txn_rollback(&txn, "no partial write"); // 1. throw away the staged grade change
  rec_release_all(&ctx, "cooperative abort"); // 2. release only the lock T2 holds (B), T1 can now finish
  recovery_clear_abort(T2_ID);

  // RETRY ONCE: safe order, A then B

  log_event(T2_ID, "RETRY_STARTED", "A,B", "OK", "ordered A->B");
  rec_txn_begin(&txn, T2_ID);

  if (rec_lock(&ctx, LOCK_A, cfg->lock_a, cfg->retry_timeout_ms) != REC_OK) goto safe_abort;
  if (rec_lock(&ctx, LOCK_B, cfg->lock_b, cfg->retry_timeout_ms) != REC_OK) goto safe_abort;

  // stage both changes again from the current files (T1 may have changed them)
  if (rec_txn_stage_replace(&txn, FM_FILE_GRADES, T2_OLD_GRADE, T2_NEW_GRADE) != REC_OK) goto safe_abort;
  if (rec_txn_stage_replace(&txn, FM_FILE_STUDENTS, T2_OLD_STUDENT, T2_NEW_STUDENT) != REC_OK) goto safe_abort;
  if (rec_txn_commit(&txn, &ctx) != REC_OK) goto safe_abort;

  log_event(T2_ID, "RETRY_SUCCESS", "students,grades", "OK", "ordered locks A->B");
  rec_unlock(&ctx, LOCK_B);
  rec_unlock(&ctx, LOCK_A);
  return REC_RETRY_OK;

safe_abort:
  // nothing was committed: drop staged changes, release what T2 holds, and report it
  rec_txn_rollback(&txn, "no file was changed");
  rec_release_all(&ctx, "safe abort");
  recovery_clear_abort(T2_ID);
  log_event(T2_ID, "SAFE_ABORT", "students,grades", "FAIL", "T2 could not finish, files left consistent");
  return REC_SAFE_ABORT;
}

const char *rec_strerror(int code) {
  switch (code) {
  case REC_RETRY_OK: return "committed on retry";
  case REC_OK: return "ok";
  case REC_TIMEOUT: return "timed out waiting for lock";
  case REC_ABORTED: return "abort requested while waiting";
  case REC_NOT_OWNER: return "lock not held by this thread";
  case REC_SAFE_ABORT: return "safe abort, no file changed";
  case REC_BAD_ARG: return "invalid argument";
  default: return "unknown error";
  }
}
