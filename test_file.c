/*
Tests all functions of the file manager

RESETS THE 'data/' folder to the starting sample files
*/

#define _POSIX_C_SOURCE 200809L
#include "file_manager.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int passed = 0;
static int failed = 0;

#define CHECK(cond, desc) do { \
    if (cond) {passed++; printf(" PASS %s\n", desc);} \
    else {failed++; printf("  FAIL %s\n", desc);} \
} while (0) // lets the macro act like one statement

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

int main(void) {
  char buf[2048];
  int n, before, after, rc;
  
  // SETUP
  section("setup and sample data");
  
  // fresh starting state
  CHECK(fm_reset() == FM_OK, "reset creates sample files");
  remove(FM_DATA_DIR "/notes.txt");
  
  // section 3.1
  CHECK(file_equals(FM_FILE_STUDENTS, "S101,Ana,CSC3320\nS102,Ben,CSC3320\n"), "students.txt has starting content");
  
  // section 3.1
  CHECK(file_equals(FM_FILE_GRADES, "S101,Assignment4,88\nS102,Assignment4,91\n"),"grades.txt has starting content");
  
  // expected answer for T3
  CHECK(fm_count_students(&n) == FM_OK && n == 2, "T3: Total students = 2");
  
  // CREATE
  section("Create");
  
  // brand new file
  CHECK(fm_create("notes.txt", "hello\n") == FM_OK, "create a new file");
  
  // protects existing data
  CHECK(fm_create("notes.txt", "again\n") == FM_ERR_EXISTS, "crete refuses to overwrite an existing file");
  
  // original content preserved
  CHECK(file_equals("notes.txt", "hello\n"), "existing file left unchanged");
  
  // READ AND FIND
  section ("Read and Find");
  
  // basic read works
  CHECK(fm_read_all(FM_FILE_STUDENTS, buf, sizeof buf) == FM_OK, "read whole file");
  
  // finds exact record
  CHECK(fm_find_record(FM_FILE_STUDENTS, "S101", buf, sizeof buf) == FM_OK && strcmp(buf, "S101,Ana,CSC3320") == 0, "T1: find S101 in students.txt");
  
  // clean error for missing record
  CHECK(fm_find_record(FM_FILE_STUDENTS, "S999", buf, sizeof buf) == FM_ERR_NO_MATCH, "unkown student ID reports 'record not found'");
  
  // matches the whole first field only
  CHECK(fm_find_record(FM_FILE_STUDENTS, "S10", buf, sizeof buf) == FM_ERR_NO_MATCH, "partial ID 'S10' does not match 'S101'");
  
  // APPEND
  section("Append");
  fm_count_lines(FM_FILE_GRADES, &before);
  
  // normal append
  CHECK(fm_append(FM_FILE_GRADES, "S101,Assignment5,95") == FM_OK, "T1: append S101,Assignment5,95");
  
  // lines after appending
  fm_count_lines(FM_FILE_GRADES, &after);
  
  // can't have more than one record
  CHECK(after == before + 1, "append adds exactly one line");
  CHECK(fm_append(FM_FILE_GRADES, "bad\nline") == FM_ERR_BAD_ARG, "append rejects text containing a newline");
  
  // REPLACE
  section("Replace (update in place)");
  
  // lines before replacing
  fm_count_lines(FM_FILE_GRADES, &before);
  
  // the actual update
  CHECK(fm_replace_line(FM_FILE_GRADES, "S102,Assignment4,91", "S102,Assignment4,94") == FM_OK, "T2: change S102 grade from 91 to 94");
  
  // lines after replacing
  fm_count_lines(FM_FILE_GRADES, &after);
  
  // makes sure no lines were added
  CHECK(after == before, "replace keeps the same number of lines");
  
  // look at the new content
  fm_read_all(FM_FILE_GRADES, buf, sizeof buf);
  
  // make sure the old version was removed
  CHECK(strstr(buf, "S102,Assignment4,91") == NULL, "old grade line is gone");
  
  // snapshot before a failing replace and copy of that snapshot for comparison later
  fm_read_all(FM_FILE_GRADES, buf, sizeof buf);
  char saved[2048];
  strcpy(saved, buf);
  
  // clean error
  CHECK(fm_replace_line(FM_FILE_GRADES, "S777,Nope,0", "x,y,z") == FM_ERR_NO_MATCH, "replacing a line that isn't there reports no match");
  
  // no corruption on failure
  CHECK(file_equals(FM_FILE_GRADES, saved), "failed replace leaves file unchanged");
  
  // STAGING
  section("Staged changes; discard (abort) and commit");
  fm_read_all(FM_FILE_STUDENTS, saved, sizeof saved);
  
  // change prepared in students.txt.tmp
  CHECK(fm_stage_replace(FM_FILE_STUDENTS, "S102,Ben,CSC3320", "S102,Benjamin,CSC3320") == FM_OK, "stage Ben -> Benjamin");
  
  // nothing visible yet
  CHECK(file_equals(FM_FILE_STUDENTS, saved), "real file unchagned while change is only staged");
  
  // temp file exists
  CHECK(fm_has_staged(FM_FILE_STUDENTS) == 1, "staged change is waiting");
  
  // throw it away
  CHECK(fm_discard(FM_FILE_STUDENTS) == FM_OK, "discard");
  
  // check the abort
  CHECK(file_equals(FM_FILE_STUDENTS, saved), "after discard, file is identical");
  
  // temp file is gone
  CHECK(fm_has_staged(FM_FILE_STUDENTS) == 0, "no staged change left behind");
  
  // can't commit with nothing staged
  CHECK(fm_commit(FM_FILE_STUDENTS) == FM_ERR_NO_STAGED, "commit with nothing staged is refused");
  
  // retry after the abort
  CHECK(fm_stage_replace(FM_FILE_STUDENTS, "S102,Ben,CSC3320","S102,Benjamin,CSC3320") == FM_OK, "stage Ben -> Benjamin again");
  
  // make it real
  CHECK(fm_commit(FM_FILE_STUDENTS) == FM_OK, "commit staged change");
  
  // updated not duplicated
  CHECK(fm_count_students(&n) == FM_OK && n == 2, "student count is still 2 after update");
  
  // AUDIT AND BACKUP
  section("Audit trail and backup");
  
  // audit line 1
  CHECK(fm_append(FM_FILE_AUDIT, "2026-11-04,T1,Grade added for S101") == FM_OK, "T4: audit entry for T1");
  
  // audit line 2
  CHECK(fm_append(FM_FILE_AUDIT, "2026-11-04,T2,Student record updated for S102") == FM_OK, "T4: audit entry for T2");
  
  // buffers for the backup
  char students[1024], grades[1024], backup[2200];
  
  // reads students holding A
  fm_read_all(FM_FILE_STUDENTS, students, sizeof students);
  
  // reads grades holding B
  fm_read_all(FM_FILE_GRADES, grades, sizeof grades);
  
  // combine into one snapshot and write atomically holding D
  snprintf(backup, sizeof backup, "BACKUP - STUDENT RECORDS\n%sBACKUP - GRADES\n%s", students, grades);
  CHECK(fm_write(FM_FILE_BACKUP, backup) == FM_OK, "write backup,txt");
  
  // FINAL CONTENTS
  section("Final contents match section 3.4 from assignment description");
  
  // expected students
  CHECK(file_equals(FM_FILE_STUDENTS, "S101,Ana,CSC3320\n""S102,Benjamin,CSC3320\n"), FM_FILE_STUDENTS);
  
  // expected grades
  CHECK(file_equals(FM_FILE_GRADES, "S101,Assignment4,88\n""S102,Assignment4,94\n""S101,Assignment5,95\n"), FM_FILE_GRADES);
  
  // expected audit
  CHECK(file_equals(FM_FILE_AUDIT, "2026-11-04,Project initialized\n""2026-11-04,T1,Grade added for S101\n""2026-11-04,T2,Student record updated for S102\n"), FM_FILE_AUDIT);
  
  // expected backup
  CHECK(file_equals(FM_FILE_BACKUP, "BACKUP - STUDENT RECORDS\n""S101,Ana,CSC3320\n""S102,Benjamin,CSC3320\n""BACKUP - GRADES\n""S101,Assignment4,88\n""S102,Assignment4,94\n""S101,Assignment5,95\n"), FM_FILE_BACKUP);
  
  // stuff for T5
  int count, lo, hi;
  double mean;
  
  rc = fm_grade_stats(&count, &mean, &lo, &hi);
  
  printf("T5 stats: count = %d, mean = %.2f, min = %d, max = %d\n", count, mean, lo, hi);
  
  // expectations from section 3.3
  CHECK(rc == FM_OK && count == 3 && fabs(mean - 92.33) < 0.01 && lo == 88 && hi == 95, "T5: count = 3, mean = 92.33, min = 88, max = 95");
  
  // ERRORS AND SAFETY
  section("Errors and safety");
  
  // clean error
  CHECK(fm_read_all("missing.txt", buf, sizeof buf) == FM_ERR_NOT_FOUND, "reading a missing file reports 'file not found'");
  
  // can't be created silently
  CHECK(fm_append("missing.txt", "x") == FM_ERR_NOT_FOUND, "appending to a missing file is refused");
  
  // *see above comment
  CHECK(fm_stat("missing.txt", NULL, NULL) == FM_ERR_NOT_FOUND, "missing file not created from method calls");
  
  // can't escape the folder
  CHECK(fm_read_all("../etc/passwd", buf, sizeof buf) == FM_ERR_BAD_NAME, "rejects bad name");
  
  // no sub paths
  CHECK(fm_append("sub/dir.txt", "x") == FM_ERR_BAD_NAME, "rejects names containing '/'");
  
  // only .txt allowed
  CHECK(fm_create("notes.md", "x") == FM_ERR_BAD_NAME, "rejects non .txt files");
  
  // rejects empty names
  CHECK(fm_create("", "x") == FM_ERR_BAD_NAME, "rejects empty name");
  
  // deliberately small buffer to demonstrate no buffer overflow
  char tiny_buf[8];
  CHECK(fm_read_all(FM_FILE_GRADES, tiny_buf, sizeof tiny_buf) == FM_ERR_TOO_BIG, "too smalle buffer reported, not overflowed");
  
  // LISTING AND METADATA
  section("Lsit files with size and last modified time");
  long size; // file size
  
  // metadata works
  CHECK(fm_stat(FM_FILE_GRADES, &size, NULL) == FM_OK && size > 0, "fm_stat returns a size for grades.txt");
  fm_list(); // print the folder listing
  
  // TEST SUMMARY
  printf("\n%d passed, %d failed\n", passed, failed);
  return failed == 0 ? 0 : 1;
}
