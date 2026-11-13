#include <stdio.h>
#include "lock_manager.h"

void lm_log(int tid, const char *event, const char *resource,
            const char *result, const char *details) {
    printf("  [LOG] T%d | %s | %s | %s | %s\n", tid, event, resource, result, details);
}