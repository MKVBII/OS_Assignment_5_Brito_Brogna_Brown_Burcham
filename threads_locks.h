#ifndef THREADS_LOCKS_H
#define THREADS_LOCKS_H

#include <pthread.h>

// Lock IDs
#define LOCK_A 0 // students.txt
#define LOCK_B 1 // grades.txt
#define LOCK_C 2 // audit.txt
#define LOCK_D 3 // backup.txt
#define NUM_LOCKS 4

// Thread IDs
#define T1_ID 1
#define T2_ID 2
#define T3_ID 3
#define T4_ID 4
#define T5_ID 5
#define T6_ID 6

// Execution Modes
typedef enum {
    // Mode A: T1 (A->B), T2 (B->A) with barrier forcing circular wait
    MODE_DEADLOCK = 0,
    // Mode B: T1 (A->B). T2 (A->B) ordered locks
    MODE_PREVENTION = 1
} ExecMode;

// Function declarations
void threads_init(void);
void run_thread_demo(ExecMode mode);
void threads_cleanup(void);

#endif
