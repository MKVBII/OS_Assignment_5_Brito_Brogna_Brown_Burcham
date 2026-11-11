#ifndef FILE_MANAGER_H
#define FILE_MANAGER_H

#include <stddef.h>
#include <time.h>

// FILE NAMES

#define FM_FILE_STUDENTS "students.txt"
#define FM_FILE_GRADES "grades.txt"
#define FM_FILE_AUDIT "audit.txt"
#define FM_FILE_BACKUP "backup.txt"

// SETTINGS

#define FM_DATA_DIR "data"
#define FM_MAX_PATH 512
#define FM_MAX_NAME 64
#define FM_MAX_LINE 256


// RETURN CODES

#define FM_OK 0 // success
#define FM_ERR_BAD_NAME -1 // filename is unsafe
#define FM_ERR_NOT_FOUND -2 // file doesn't exist
#define FM_ERR_EXISTS -3 // file already exists
#define FM_ERR_IO -4 // OS couldn't perform IO operation
#define FM_ERR_NO_MATCH -5 // the thing being looked for couldn't be found
#define FM_ERR_TOO_BIG -6 // can't fit in the buffer
#define FM_ERR_NO_STAGED -7 // commit or discard was called but nothing was staged
#define FM_ERR_BAD_ARG -8 // missing argument or argument is a line that contains a newline


// SETUP

int fm_init(void); // make sure folder exists
int fm_reset(void); // restore the sample starting files


// BASIC FILE OPERATIONS

int fm_create(const char *name, const char *content); // make a new file, fail if it already exists
int fm_write(const char *name, const char *content); // create or fully overwrite a file safely
int fm_read_all(const char *name, char *buf, size_t bufsize); // read the whole file into buf
int fm_append(const char *name, const char *line); // add one line to the end 
int fm_replace_line(const char *name, const char *old_line, const char *new_line); // swap one exact line for another


// STAGED UPDATES USED FOR SAFE RECOVERY

int fm_stage_replace(const char *name, const char *old_line, const char *new_line); // prepare a change in <filename>.tmp (real file is untouched)
int fm_commit(const char *name); // make the staged change real
int fm_discard(const char *name); // throw the staged change away
int fm_has_staged(const char *name); // returns 1 if a staged change is waiting, 0 otherwise


// LISTING AND METADATA

int fm_stat(const char *name, long *size_bytes, time_t *modified); // get size and last modified time
int fm_print_info(const char *name); // print "name size modified"
int fm_list(void); // print every .txt file in the data folder


// RECORD HELPERS FOR WORKER THREADS

int fm_find_record(const char *name, const char *id, char *out, size_t outsize); // find the line whjose first field is id
int fm_count_students(int *count); // count valid lines in students.txt
int fm_grade_stats(int *count, double *mean, int *min, int *max); // stats over grades.txt
int fm_count_lines(const char *name, int *count); // number of lines in any file (used in testing)


// ERRORS

const char *fm_strerror(int code); // readable text for error codes

#endif 
