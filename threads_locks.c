#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include "threads_locks.h"
#include "file_manager.h"
#include "lock_manager.h"

// Barrier to easily simulate a deadlock
static pthread_barrier_t barrier;

// Current Execution Mode
static ExecMode current_mode;


// Audit queue structure
typedef struct {
    char messages[16][FM_MAX_LINE];
    int count;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} AuditQueue;

static AuditQueue audit_q = {
    .count = 0,
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER
};

/*
Helper to push completed events to T4
*/
void log_audit_event(const char *entry) {
    pthread_mutex_lock(&audit_q.mutex);
    if (audit_q.count < 16) {
        strncpy(audit_q.messages[audit_q.count], entry, FM_MAX_LINE - 1);
        audit_q.messages[audit_q.count][FM_MAX_LINE - 1] = '\0';
        audit_q.count++;
        pthread_cond_signal(&audit_q.cond);
    }
    pthread_mutex_unlock(&audit_q.mutex);
}


// --- Worker Threads ---

/*
T1: Read S101 in students.txt; append S101,Assignment5,95 to grades.txt
*/
void *worker_T1(void *arg) {
    (void)arg;
    char record[FM_MAX_LINE];

    lm_lock(T1_ID, LOCK_A);
    fm_find_record(FM_FILE_STUDENTS, "S101", record, sizeof(record));

    if (current_mode == MODE_DEADLOCK) {
        pthread_barrier_wait(&barrier);
    }

    lm_lock(T1_ID, LOCK_B);
    fm_append(FM_FILE_GRADES, "S101,Assignment5,95");

    lm_unlock(T1_ID, LOCK_B);
    lm_unlock(T1_ID, LOCK_A);

    // Enqueue event for T4
    log_audit_event("2026-11-04,T1,Grade added for S101");

    return NULL;
}

/*
T2: Update grade for S102 in grades.txt and name in students.txt
*/
void *worker_T2(void *arg) {
    (void)arg;

    if (current_mode == MODE_PREVENTION) {
        // Mode B: Prevention (A then B)
        lm_lock(T2_ID, LOCK_A);
        lm_lock(T2_ID, LOCK_B);

        fm_replace_line(FM_FILE_GRADES, "S102,Assignment4,91", "S102,Assignment4,94");
        fm_replace_line(FM_FILE_STUDENTS, "S102,Ben,CSC3320", "S102,Benjamin,CSC3320");

        lm_unlock(T2_ID, LOCK_B);
        lm_unlock(T2_ID, LOCK_A);

    } else {
        // Mode C: Safe Cooperative Recovery
        lm_lock(T2_ID, LOCK_B);

        // Stage edit cleanly
        fm_stage_replace(FM_FILE_STUDENTS, "S102,Ben,CSC3320", "S102,Benjamin,CSC3320");

        // Synchronize with T1 to simulate deadlock (hopefully)
        pthread_barrier_wait(&barrier);

        int rc = lm_lock(T2_ID, LOCK_A);
        if (rc != LM_OK) {                                     // NEW: recovery block
            fm_discard(FM_FILE_STUDENTS);                      // roll back staged change
            lm_release_all(T2_ID);                             // releases only T2's locks (B)
            lm_log(T2_ID, "ROLLBACK_COMPLETE", "files", "OK", "staged changes discarded");
            lm_log(T2_ID, "RETRY_STARTED", "A,B", "OK", "ordered A->B");

            lm_lock(T2_ID, LOCK_A);                            // retry in safe order
            lm_lock(T2_ID, LOCK_B);
        } else {
            fm_discard(FM_FILE_STUDENTS);                      // got A anyway; redo cleanly below
        }

        fm_replace_line(FM_FILE_GRADES, "S102,Assignment4,91", "S102,Assignment4,94");
        fm_replace_line(FM_FILE_STUDENTS, "S102,Ben,CSC3320", "S102,Benjamin,CSC3320");

        lm_unlock(T2_ID, LOCK_B);
        lm_unlock(T2_ID, LOCK_A);

    }

    // Enqueue event for T4 to write to audit.txt
    log_audit_event("2026-11-04,T2,Student record updated for S102");

    return NULL;
}

/*
T3: Count valid student records
*/
void *worker_T3(void *arg) {
    (void)arg;
    int count = 0;
    lm_lock(T3_ID, LOCK_A);
    fm_count_students(&count);
    lm_unlock(T3_ID, LOCK_A);
    return NULL;
}

/*
T4: Audit Logger Thread (Lock C)
*/
void *worker_T4(void *arg) {
    (void)arg;

    while (1) {
        char msg[FM_MAX_LINE];

        pthread_mutex_lock(&audit_q.mutex);
        while (audit_q.count == 0) {
            pthread_cond_wait(&audit_q.cond, &audit_q.mutex);
        }

        strncpy(msg, audit_q.messages[0], FM_MAX_LINE);
        for (int i = 0; i < audit_q.count - 1; i++) {
            strcpy(audit_q.messages[i], audit_q.messages[i + 1]);
        }
        audit_q.count--;

        pthread_mutex_unlock(&audit_q.mutex);

        // Shutdown signal check
        if (strcmp(msg, "STOP") == 0) {
            break;
        }

        // Acquire Lock C and write committed record to audit.txt
        lm_lock(T4_ID, LOCK_C);
        fm_append(FM_FILE_AUDIT, msg);
        lm_unlock(T4_ID, LOCK_C);
    }

    return NULL;
}

/*
T5: Compute grade statistics
*/
void *worker_T5(void *arg) {
    (void)arg;
    int count, min, max;
    double mean;
    lm_lock(T5_ID, LOCK_B);
    fm_grade_stats(&count, &mean, &min, &max);
    lm_unlock(T5_ID, LOCK_B);
    return NULL;
}

/*
T6: Create backup snapshot
*/
void *worker_T6(void *arg) {
    (void)arg;
    char students_buf[1024], grades_buf[1024], backup_buf[2200];
    
    if (lm_lock(T6_ID, LOCK_A) != LM_OK) { lm_release_all(T6_ID); return NULL; }
    fm_read_all(FM_FILE_STUDENTS, students_buf, sizeof(students_buf));

    if (lm_lock(T6_ID, LOCK_B) != LM_OK) { lm_release_all(T6_ID); return NULL; }
    fm_read_all(FM_FILE_GRADES, grades_buf, sizeof(grades_buf));

    if (lm_lock(T6_ID, LOCK_D) != LM_OK) { lm_release_all(T6_ID); return NULL; }
    snprintf(backup_buf, sizeof(backup_buf),
             "BACKUP - STUDENT RECORDS\n%sBACKUP - GRADES\n%s", students_buf, grades_buf);
    fm_write(FM_FILE_BACKUP, backup_buf);

    lm_unlock(T6_ID, LOCK_D);
    lm_unlock(T6_ID, LOCK_B);
    lm_unlock(T6_ID, LOCK_A);
    return NULL;
}


// --- Management Functions ---

void threads_init(void) {
    lm_init(NUM_LOCKS);
}

void run_thread_demo(ExecMode mode) {
    current_mode = mode;
    pthread_t threads[6];

    lm_init(NUM_LOCKS);
    pthread_barrier_init(&barrier, NULL, 2);
    fm_reset();

    // 1. Start logger thread T4 first
    pthread_create(&threads[3], NULL, worker_T4, NULL);

    // 2. Start workers T1, T2, T3, T5, T6
    pthread_create(&threads[0], NULL, worker_T1, NULL);
    pthread_create(&threads[1], NULL, worker_T2, NULL);
    pthread_create(&threads[2], NULL, worker_T3, NULL);
    pthread_create(&threads[4], NULL, worker_T5, NULL);
    pthread_create(&threads[5], NULL, worker_T6, NULL);

    // 3. Wait for workers to finish file operations
    pthread_join(threads[0], NULL);
    pthread_join(threads[1], NULL);
    pthread_join(threads[2], NULL);
    pthread_join(threads[4], NULL);
    pthread_join(threads[5], NULL);

    // 4. Send STOP signal to T4 and join it
    log_audit_event("STOP");
    pthread_join(threads[3], NULL);

    pthread_barrier_destroy(&barrier);

    printf("Deadlocks detected this run: %d\n", lm_cycle_count());
}

void threads_cleanup(void) {
    lm_destroy();
}
