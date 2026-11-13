/*
Recovery demo (Member 4 - Recovery & Logging)

Runs the Mode C sequence from the assignment without changing any of the
group's existing files:

  1. T1 holds A (students.txt) and requests B (grades.txt)
  2. T2 holds B and requests A            -> circular wait T1 -> T2 -> T1
  3. T2 gives way: its wait times out (default) or main asks it to abort
  4. T2 rolls back its staged change and releases only B
  5. T1 finishes, then T2 retries once in the safe order A then B

The group's own code is used as is:
  - file_manager.c for every file operation and the staged commit
  - worker_T4 from threads_locks.c writes audit.txt (lock C), fed through
    log_audit_event, so only committed operations reach the audit trail

Usage:
  ./recovery_demo          T2 recovers by timeout
  ./recovery_demo abort    main asks T2 to abort once both threads are stuck
*/

#define _POSIX_C_SOURCE 200809L
#include "recovery.h"
#include "logger.h"
#include "threads_locks.h"
#include "file_manager.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

// defined in threads_locks.c (not listed in threads_locks.h)
void *worker_T4(void *arg);
void log_audit_event(const char *entry);

#define T2_WAIT_TIMEOUT_MS 400 // how long T2 waits for A before giving way
#define RETRY_TIMEOUT_MS 3000 // how long the retry waits for each lock

// the two file locks used by T1 and T2 in this demo
static pthread_mutex_t lock_a = PTHREAD_MUTEX_INITIALIZER; // students.txt
static pthread_mutex_t lock_b = PTHREAD_MUTEX_INITIALIZER; // grades.txt

// makes T1 and T2 take their first lock before either asks for the second (no sleep needed)
static pthread_barrier_t barrier;

static int t2_result;

/*
Sleeps for a number of milliseconds
*/
static void sleep_ms(int ms) {
  struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000L};
  nanosleep(&t, NULL);
}

/*
Barrier wait in the shape recovery_run_T2 expects for its sync point
*/
static void wait_at_barrier(void *arg) {
  (void)arg;
  pthread_barrier_wait(&barrier);
}

/*
T1: read S101 while holding A, then append the new grade while holding A and B
*/
static void *demo_T1(void *arg) {
  (void)arg;
  rec_ctx ctx;
  char record[FM_MAX_LINE];
  rec_init(&ctx, T1_ID);

  if (rec_lock(&ctx, LOCK_A, &lock_a, -1) != REC_OK) return NULL;
  int rc = fm_find_record(FM_FILE_STUDENTS, "S101", record, sizeof record);
  if (rc == FM_OK) log_event(T1_ID, "READ", "A", "OK", "found %s", record);
  else log_event(T1_ID, "READ", "A", "ERROR", "S101: %s", fm_strerror(rc));

  pthread_barrier_wait(&barrier); // T2 now holds B

  if (rec_lock(&ctx, LOCK_B, &lock_b, -1) != REC_OK) {
    rec_release_all(&ctx, "safe abort");
    return NULL;
  }

  if (rc == FM_OK && (rc = fm_append(FM_FILE_GRADES, "S101,Assignment5,95")) == FM_OK) {
    log_event(T1_ID, "OP_COMPLETE", "students,grades", "OK", "grade committed");
    log_audit_event("2026-11-04,T1,Grade added for S101"); // committed, so it goes to audit.txt
  } else {
    log_event(T1_ID, "OP_FAILED", "grades", "ERROR", "%s, grade not added", fm_strerror(rc));
  }

  rec_unlock(&ctx, LOCK_B);
  rec_unlock(&ctx, LOCK_A);
  return NULL;
}

/*
T2: the recovery procedure from recovery.c
*/
static void *demo_T2(void *arg) {
  rec_t2_config *cfg = arg;
  t2_result = recovery_run_T2(cfg);

  // only committed work is recorded as completed
  if (t2_result == REC_OK || t2_result == REC_RETRY_OK) {
    log_audit_event("2026-11-04,T2,Student record updated for S102");
  }
  return NULL;
}

/*
Prints one data file with its size and last modified time
*/
static void show_file(const char *name) {
  char buf[2048];
  int rc = fm_read_all(name, buf, sizeof buf);
  printf("\n%s:\n", name);
  if (rc == FM_OK) printf("%s", buf);
  else printf("  (%s)\n", fm_strerror(rc));
  fm_print_info(name);
}

int main(int argc, char *argv[]) {
  int use_abort = (argc > 1 && strcmp(argv[1], "abort") == 0);
  pthread_t t1, t2, t4;

  if (fm_init() != FM_OK || fm_reset() != FM_OK) {
    printf("could not set up the data folder\n");
    return 1;
  }
  if (log_reset() != 0) {
    printf("could not open %s\n", LOG_FILE);
    return 1;
  }
  threads_init(); // the group's locks, used by worker_T4 for audit.txt

  printf("\n========================================================\n");
  printf("  MODE C: RECOVERY (%s)\n", use_abort ? "COOPERATIVE ABORT AND RETRY" : "TIMEOUT AND RETRY");
  printf("========================================================\n");
  log_event(LOG_MAIN, "MODE_START", "A,B", "OK", "T1 locks A then B, T2 locks B then A");

  rec_t2_config cfg = {
    .lock_a = &lock_a,
    .lock_b = &lock_b,
    .wait_timeout_ms = use_abort ? -1 : T2_WAIT_TIMEOUT_MS, // -1 = wait until asked to abort
    .retry_timeout_ms = RETRY_TIMEOUT_MS,
    .sync_point = wait_at_barrier,
    .sync_arg = NULL
  };

  pthread_barrier_init(&barrier, NULL, 2);
  pthread_create(&t4, NULL, worker_T4, NULL);
  pthread_create(&t1, NULL, demo_T1, NULL);
  pthread_create(&t2, NULL, demo_T2, &cfg);

  // wait (up to 2 s) until both threads are stuck on each other, then show the wait table
  int stuck = 0;
  for (int i = 0; i < 2000 && !stuck; i++) {
    stuck = recovery_waiting_for(T1_ID) == LOCK_B && recovery_waiting_for(T2_ID) == LOCK_A;
    if (!stuck) sleep_ms(1);
  }

  if (stuck) {
    log_event(LOG_MAIN, "WAIT_TABLE", "A", "WAIT", "owner=T%d waiting=T2", recovery_owner_of(LOCK_A));
    log_event(LOG_MAIN, "WAIT_TABLE", "B", "WAIT", "owner=T%d waiting=T1", recovery_owner_of(LOCK_B));
    log_event(LOG_MAIN, "CIRCULAR_WAIT", "A,B", "ALERT", "T1->T2->T1");
    if (use_abort) recovery_request_abort(T2_ID);
    else log_event(LOG_MAIN, "RECOVERY_WAIT", "A,B", "OK", "T2 will give way when its %d ms wait runs out", T2_WAIT_TIMEOUT_MS);
  }

  pthread_join(t1, NULL);
  pthread_join(t2, NULL);
  pthread_barrier_destroy(&barrier);

  // stop the audit writer once every committed entry is queued
  log_audit_event("STOP");
  pthread_join(t4, NULL);

  int ok = (t2_result == REC_OK || t2_result == REC_RETRY_OK);
  log_event(LOG_MAIN, "MODE_END", "A,B", ok ? "OK" : "FAIL", "T2 result: %s", rec_strerror(t2_result));

  printf("\n========== FINAL FILE CONTENTS ==========\n");
  show_file(FM_FILE_STUDENTS);
  show_file(FM_FILE_GRADES);
  show_file(FM_FILE_AUDIT);
  printf("=========================================\n");
  printf("Full trace written to %s\n", LOG_FILE);

  threads_cleanup();
  log_close();
  return ok ? 0 : 1;
}
