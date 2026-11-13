/*
Tests the logger and the deadlock recovery (Member 4 - Recovery & Logging)

RESETS THE 'data/' folder and EMPTIES execution.log

The output of this program plus execution.log is the test evidence for:
  - log entries identify thread, operation, and outcome
  - timeout or cooperative abort releases owned locks and allows a safe retry
  - a failed task never leaves one file updated and the other stale
*/

#define _POSIX_C_SOURCE 200809L
#include "recovery.h"
#include "logger.h"
#include "file_manager.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>

static int passed = 0;
static int failed = 0;

#define CHECK(cond, desc) do { \
    if (cond) {passed++; printf(" PASS %s\n", desc);} \
    else {failed++; printf("  FAIL %s\n", desc);} \
} while (0) // lets the macro act like one statement

// starting and expected file contents (sections 3.1 and 3.4)
#define STUDENTS_START "S101,Ana,CSC3320\nS102,Ben,CSC3320\n"
#define GRADES_START "S101,Assignment4,88\nS102,Assignment4,91\n"
#define STUDENTS_FINAL "S101,Ana,CSC3320\nS102,Benjamin,CSC3320\n"
#define GRADES_T1_THEN_T2 "S101,Assignment4,88\nS102,Assignment4,94\nS101,Assignment5,95\n"

// the two file locks used by T1 and T2
static pthread_mutex_t lock_a = PTHREAD_MUTEX_INITIALIZER; // students.txt
static pthread_mutex_t lock_b = PTHREAD_MUTEX_INITIALIZER; // grades.txt
static pthread_barrier_t barrier;

/*
Prints a section heading so the output is easier to read
*/
static void section(const char *title) {
  printf("\n== %s ==\n", title);
}

/*
Reads a whole file into a buffer and checks if it equals the expected text
*/
static int file_equals(const char *name, const char *expected) {
  char buf[2048];
  if (fm_read_all(name, buf, sizeof buf) != FM_OK) return 0;
  return strcmp(buf, expected) == 0;
}

/*
Returns 1 if nobody holds the mutex right now
*/
static int mutex_is_free(pthread_mutex_t *m) {
  if (pthread_mutex_trylock(m) != 0) return 0;
  pthread_mutex_unlock(m);
  return 1;
}

/*
Sleeps for a number of milliseconds
*/
static void sleep_ms(int ms) {
  struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000L};
  nanosleep(&t, NULL);
}

/*
Counts the lines of execution.log that contain 'text'. If 'bad_format' is given,
it receives the number of lines that don't have exactly 6 fields
*/
static int count_log_lines(const char *text, int *bad_format) {
  FILE *f = fopen(LOG_FILE, "r");
  if (f == NULL) return -1;
  char line[600];
  int matches = 0, bad = 0;
  while (fgets(line, sizeof line, f) != NULL) {
    int separators = 0;
    for (const char *p = strstr(line, " | "); p != NULL; p = strstr(p + 3, " | ")) separators++;
    if (separators != 5 || line[strlen(line) - 1] != '\n') bad++;
    if (strstr(line, text) != NULL) matches++;
  }
  fclose(f);
  if (bad_format) *bad_format = bad;
  return matches;
}

/*
Logger stress thread: writes 50 entries as fast as it can
*/
static void *log_writer(void *arg) {
  int id = *(int *)arg;
  for (int i = 0; i < 50; i++) {
    log_event(id, "STRESS_TEST", "-", "OK", "entry %d from thread %d with some extra text to make the line longer", i, id);
  }
  return NULL;
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
static void *t1_thread(void *arg) {
  (void)arg;
  rec_ctx ctx;
  char record[FM_MAX_LINE];
  rec_init(&ctx, T1_ID);

  rec_lock(&ctx, LOCK_A, &lock_a, -1);
  fm_find_record(FM_FILE_STUDENTS, "S101", record, sizeof record);
  pthread_barrier_wait(&barrier); // T2 now holds B

  rec_lock(&ctx, LOCK_B, &lock_b, -1);
  fm_append(FM_FILE_GRADES, "S101,Assignment5,95");
  log_event(T1_ID, "OP_COMPLETE", "students,grades", "OK", "grade committed");
  rec_unlock(&ctx, LOCK_B);
  rec_unlock(&ctx, LOCK_A);
  return NULL;
}

/*
T2 thread: runs the recovery procedure and hands back its result
*/
static void *t2_thread(void *arg) {
  rec_t2_config *cfg = arg;
  static int result;
  result = recovery_run_T2(cfg);
  return &result;
}

/*
Starts T1 and T2 in the deadlock order and returns T2's result.
If use_detector is 1, main plays the detector: it waits until both threads
are blocked on each other and then asks T2 to abort
*/
static int run_deadlock_scenario(int wait_timeout_ms, int use_detector) {
  pthread_t t1, t2;
  rec_t2_config cfg = {&lock_a, &lock_b, wait_timeout_ms, 2000, wait_at_barrier, NULL};
  void *result;

  fm_reset();
  pthread_barrier_init(&barrier, NULL, 2);
  pthread_create(&t1, NULL, t1_thread, NULL);
  pthread_create(&t2, NULL, t2_thread, &cfg);

  if (use_detector) {
    // wait table: T1 waits for B and T2 waits for A (the cycle T1 -> T2 -> T1)
    for (int i = 0; i < 2000; i++) {
      if (recovery_waiting_for(T1_ID) == LOCK_B && recovery_waiting_for(T2_ID) == LOCK_A) break;
      sleep_ms(1);
    }
    log_event(LOG_MAIN, "DEADLOCK_DETECTED", "A,B", "ALERT", "cycle=T1->T2->T1");
    recovery_request_abort(T2_ID);
  }

  pthread_join(t1, NULL);
  pthread_join(t2, &result);
  pthread_barrier_destroy(&barrier);
  return *(int *)result;
}

/*
Holds lock A for 600 ms so that T2's retry cannot get it
*/
static void *hog_thread(void *arg) {
  pthread_barrier_t *ready = arg;
  pthread_mutex_lock(&lock_a);
  pthread_barrier_wait(ready);
  sleep_ms(600);
  pthread_mutex_unlock(&lock_a);
  return NULL;
}

int main(void) {
  int bad, n;
  rec_ctx ctx, other;
  rec_txn txn;

  fm_init();
  log_reset();
  log_set_echo(0); // entries go to execution.log only, the screen shows PASS/FAIL

  // LOGGER
  section("thread-safe execution.log");

  pthread_t writers[6];
  int ids[6] = {1, 2, 3, 4, 5, 6};
  for (int i = 0; i < 6; i++) pthread_create(&writers[i], NULL, log_writer, &ids[i]);
  for (int i = 0; i < 6; i++) pthread_join(writers[i], NULL);

  n = count_log_lines("STRESS_TEST", &bad);
  CHECK(n == 300, "6 threads x 50 entries = 300 lines, none lost");
  CHECK(bad == 0, "every line is complete: timestamp | thread | event | resource | result | details");
  CHECK(count_log_lines("| T4 | STRESS_TEST | - | OK |", NULL) == 50, "entries identify thread, operation, and outcome");

  // STAGED COMMIT
  section("staged commit");

  fm_reset();
  rec_init(&ctx, T2_ID);
  rec_txn_begin(&txn, T2_ID);
  rec_lock(&ctx, LOCK_A, &lock_a, 100);
  rec_lock(&ctx, LOCK_B, &lock_b, 100);
  rec_txn_stage_replace(&txn, FM_FILE_GRADES, "S102,Assignment4,91", "S102,Assignment4,94");
  rec_txn_stage_replace(&txn, FM_FILE_STUDENTS, "S102,Ben,CSC3320", "S102,Benjamin,CSC3320");
  CHECK(file_equals(FM_FILE_GRADES, GRADES_START) && file_equals(FM_FILE_STUDENTS, STUDENTS_START), "staging leaves both real files untouched");
  CHECK(rec_txn_commit(&txn, &ctx) == REC_OK, "commit succeeds while holding A and B");
  CHECK(file_equals(FM_FILE_STUDENTS, STUDENTS_FINAL), "students.txt updated (Ben -> Benjamin)");
  CHECK(file_equals(FM_FILE_GRADES, "S101,Assignment4,88\nS102,Assignment4,94\n"), "grades.txt updated (91 -> 94), no duplicate line");
  rec_release_all(&ctx, NULL);

  // ROLLBACK
  section("rollback");

  fm_reset();
  rec_txn_begin(&txn, T2_ID);
  rec_txn_stage_replace(&txn, FM_FILE_GRADES, "S102,Assignment4,91", "S102,Assignment4,94");
  CHECK(fm_has_staged(FM_FILE_GRADES) == 1, "a staged change is waiting");
  CHECK(rec_txn_rollback(&txn, NULL) == 1, "rollback discards 1 staged change");
  CHECK(fm_has_staged(FM_FILE_GRADES) == 0 && file_equals(FM_FILE_GRADES, GRADES_START), "nothing staged is left and grades.txt is unchanged");

  // commit must not run without the locks
  rec_txn_stage_replace(&txn, FM_FILE_GRADES, "S102,Assignment4,91", "S102,Assignment4,94");
  rec_txn_stage_replace(&txn, FM_FILE_STUDENTS, "S102,Ben,CSC3320", "S102,Benjamin,CSC3320");
  rec_lock(&ctx, LOCK_B, &lock_b, 100);
  CHECK(rec_txn_commit(&txn, &ctx) == REC_SAFE_ABORT, "commit refused while lock A is not held");
  CHECK(file_equals(FM_FILE_GRADES, GRADES_START) && file_equals(FM_FILE_STUDENTS, STUDENTS_START), "refused commit changed neither file");
  rec_txn_rollback(&txn, NULL);
  rec_release_all(&ctx, NULL);

  // LOCK OWNERSHIP
  section("a thread releases only locks it owns");

  rec_init(&other, T1_ID);
  rec_lock(&other, LOCK_A, &lock_a, 100); // "T1" holds A
  CHECK(rec_unlock(&ctx, LOCK_A) == REC_NOT_OWNER, "T2 cannot release lock A held by T1");
  CHECK(rec_release_all(&ctx, NULL) == 0, "T2 release-all releases nothing it doesn't hold");
  CHECK(!mutex_is_free(&lock_a), "lock A is still held by T1");
  CHECK(rec_lock(&ctx, LOCK_A, &lock_a, 50) == REC_TIMEOUT, "T2 waiting for A times out instead of hanging");
  CHECK(!rec_holds(&ctx, LOCK_A), "a timed out lock is not recorded as held");
  CHECK(rec_unlock(&other, LOCK_A) == REC_OK && mutex_is_free(&lock_a), "T1 releases its own lock");

  // TIMEOUT AND RETRY
  section("recovery: timeout and retry");

  int timeouts_before = count_log_lines("| T2 | LOCK_TIMEOUT | A |", NULL);
  CHECK(run_deadlock_scenario(300, 0) == REC_RETRY_OK, "T2 timed out, rolled back, and committed on retry");
  CHECK(file_equals(FM_FILE_STUDENTS, STUDENTS_FINAL), "final students.txt matches section 3.4");
  CHECK(file_equals(FM_FILE_GRADES, GRADES_T1_THEN_T2), "final grades.txt matches section 3.4 (grade appended once)");
  CHECK(mutex_is_free(&lock_a) && mutex_is_free(&lock_b), "both locks are free afterwards");
  CHECK(!fm_has_staged(FM_FILE_GRADES) && !fm_has_staged(FM_FILE_STUDENTS), "no staged files left behind");
  CHECK(count_log_lines("| T2 | LOCK_TIMEOUT | A |", NULL) == timeouts_before + 1, "log shows T2's wait for A timing out");
  CHECK(count_log_lines("| T2 | ROLLBACK_COMPLETE |", NULL) >= 1 && count_log_lines("| T2 | RETRY_SUCCESS |", NULL) == 1, "log shows rollback and successful retry");

  // DETECTOR ABORT AND RETRY
  section("recovery: cooperative abort and retry");

  CHECK(run_deadlock_scenario(-1, 1) == REC_RETRY_OK, "T2 aborted on request, rolled back, and committed on retry");
  CHECK(file_equals(FM_FILE_STUDENTS, STUDENTS_FINAL) && file_equals(FM_FILE_GRADES, GRADES_T1_THEN_T2), "final files match section 3.4");
  CHECK(mutex_is_free(&lock_a) && mutex_is_free(&lock_b), "both locks are free afterwards");
  CHECK(count_log_lines("| T2 | ABORT_REQUESTED |", NULL) == 1 && count_log_lines("| T2 | LOCK_ABORTED | A |", NULL) == 1, "log shows the abort request and T2 giving up its wait");

  // SAFE ABORT
  section("recovery: safe abort when the retry also fails");

  fm_reset();
  pthread_t hog;
  pthread_barrier_t ready;
  pthread_barrier_init(&ready, NULL, 2);
  pthread_create(&hog, NULL, hog_thread, &ready);
  pthread_barrier_wait(&ready); // the hog now holds A for 600 ms

  rec_t2_config cfg = {&lock_a, &lock_b, 100, 100, NULL, NULL};
  CHECK(recovery_run_T2(&cfg) == REC_SAFE_ABORT, "T2 reports a safe abort");
  CHECK(!mutex_is_free(&lock_a), "the other thread's lock A was not force-unlocked");
  CHECK(mutex_is_free(&lock_b), "T2 released its own lock B");
  CHECK(file_equals(FM_FILE_GRADES, GRADES_START) && file_equals(FM_FILE_STUDENTS, STUDENTS_START), "neither file was changed");
  CHECK(!fm_has_staged(FM_FILE_GRADES) && !fm_has_staged(FM_FILE_STUDENTS), "no staged files left behind");
  pthread_join(hog, NULL);
  pthread_barrier_destroy(&ready);

  // MISSING FILE AND MISSING RECORD
  section("errors do not corrupt data");

  fm_reset();
  remove(FM_DATA_DIR "/" FM_FILE_STUDENTS);
  CHECK(recovery_run_T2(&cfg) == REC_SAFE_ABORT, "missing students.txt gives a safe abort");
  CHECK(file_equals(FM_FILE_GRADES, GRADES_START), "grades.txt was not left half updated");
  CHECK(mutex_is_free(&lock_a) && mutex_is_free(&lock_b), "both locks are free afterwards");

  fm_reset();
  CHECK(recovery_run_T2(&cfg) == REC_OK && recovery_run_T2(&cfg) == REC_SAFE_ABORT, "running T2 twice: second run finds no record and aborts safely");
  CHECK(file_equals(FM_FILE_GRADES, "S101,Assignment4,88\nS102,Assignment4,94\n") && file_equals(FM_FILE_STUDENTS, STUDENTS_FINAL), "files still hold exactly one update");
  CHECK(count_log_lines("| T2 | SAFE_ABORT |", NULL) == 3, "log reports each safe abort");

  // LOG FORMAT
  section("execution.log format");

  n = count_log_lines("", &bad);
  CHECK(n > 300 && bad == 0, "every entry in execution.log has all 6 fields");

  // SUMMARY
  fm_reset();
  log_close();
  printf("\n%d passed, %d failed\n", passed, failed);
  printf("Full trace written to %s\n", LOG_FILE);
  return failed == 0 ? 0 : 1;
}
