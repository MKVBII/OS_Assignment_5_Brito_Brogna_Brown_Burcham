#include <stdio.h>
#include "threads_locks.h"
#include "file_manager.h"

int main(void) {
    fm_init();
    threads_init();

    printf("\n==============================================\n");
    printf("  MODE B: DEADLOCK PREVENTION (ORDERED A->B)\n");
    printf("==============================================\n");
    run_thread_demo(MODE_PREVENTION);

    printf("\n========================================================\n");
    printf("  MODE A: DEADLOCK, SAFE COOPERATIVE RECOVERY, & RETRY\n");
    printf("========================================================\n");
    run_thread_demo(MODE_DEADLOCK);

    threads_cleanup();
    return 0;
}
