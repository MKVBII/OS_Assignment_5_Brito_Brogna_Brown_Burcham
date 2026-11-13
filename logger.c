#define _POSIX_C_SOURCE 200809L // allow POSIX functions (i.e. localtime_r and clock_gettime)

#include "logger.h"
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>

// one mutex protects the file handle, the echo setting, and every write
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static FILE *log_file = NULL;
static int log_echo = 1;

/*
Opens the log file with the given mode ("a" keeps old entries, "w" empties it)
Caller must hold log_mutex
*/
static int open_log(const char *mode) {
  if (log_file != NULL) fclose(log_file);
  log_file = fopen(LOG_FILE, mode);
  return (log_file != NULL) ? 0 : -1;
}

int log_init(void) {
  pthread_mutex_lock(&log_mutex);
  int rc = (log_file != NULL) ? 0 : open_log("a");
  pthread_mutex_unlock(&log_mutex);
  return rc;
}

int log_reset(void) {
  pthread_mutex_lock(&log_mutex);
  int rc = open_log("w");
  pthread_mutex_unlock(&log_mutex);
  return rc;
}

void log_set_echo(int on) {
  pthread_mutex_lock(&log_mutex);
  log_echo = on;
  pthread_mutex_unlock(&log_mutex);
}

void log_close(void) {
  pthread_mutex_lock(&log_mutex);
  if (log_file != NULL) {
    fclose(log_file);
    log_file = NULL;
  }
  pthread_mutex_unlock(&log_mutex);
}

void log_event(int thread_id, const char *event, const char *resource, const char *result, const char *details, ...) {
  char name[16], text[256], stamp[16], line[512];
  
  // "MAIN" for the main thread, "T1".."T6" for workers
  if (thread_id == LOG_MAIN) snprintf(name, sizeof name, "MAIN");
  else snprintf(name, sizeof name, "T%d", thread_id);
  
  // fill in the details text
  va_list args;
  va_start(args, details);
  vsnprintf(text, sizeof text, details, args);
  va_end(args);
  
  // the lock is taken before reading the clock so that entries are written in timestamp order
  pthread_mutex_lock(&log_mutex);
  
  struct timespec now;
  struct tm tm_local;
  clock_gettime(CLOCK_REALTIME, &now);
  localtime_r(&now.tv_sec, &tm_local);
  strftime(stamp, sizeof stamp, "%H:%M:%S", &tm_local);
  
  // build the whole entry first so it is written in one piece and can never be split by another thread
  snprintf(line, sizeof line, "%s.%03ld | %s | %s | %s | %s | %s\n", stamp, now.tv_nsec / 1000000L, name, event, resource, result, text);
  
  if (log_file == NULL) open_log("a"); // works even if log_init was never called
  if (log_file != NULL) {
    fputs(line, log_file);
    fflush(log_file); // entry reaches the file even if the program is stopped with ctrl + c
  }
  if (log_echo) {
    fputs(line, stdout);
    fflush(stdout);
  }
  
  pthread_mutex_unlock(&log_mutex);
}
