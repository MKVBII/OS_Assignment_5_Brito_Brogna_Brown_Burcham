#ifndef LOGGER_H
#define LOGGER_H

/*
Thread-safe execution log (Member 4 - Recovery & Logging)

Every entry is one line in execution.log:

  timestamp | thread | event | resource | result | details
  09:10:00.110 | T1 | LOCK_WAIT | B | WAIT | owner=T2

execution.log is the detailed trace of what the threads did. It is separate
from audit.txt, which is the short history of completed operations that T4
writes.
*/

#define LOG_FILE "execution.log" // kept in the project folder
#define LOG_MAIN 0 // thread id for the main thread (workers use T1_ID..T6_ID)

int log_init(void); // open execution.log for appending, returns 0 or -1
int log_reset(void); // empty execution.log and start again, returns 0 or -1
void log_set_echo(int on); // 1 = also print every entry to the screen (default), 0 = file only
void log_close(void); // flush and close the file

// write one entry (details is a printf style format)
void log_event(int thread_id, const char *event, const char *resource, const char *result, const char *details, ...)
#if defined(__GNUC__)
  __attribute__((format(printf, 5, 6)))
#endif
  ;

#endif
