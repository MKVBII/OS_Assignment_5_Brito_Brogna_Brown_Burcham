#define _POSIX_C_SOURCE 200809L // allow POSIX functions (i.e. localtime_r and strdup)

#include "file_manager.h" // functions and constants from the header
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <ctype.h>

/*
Ensures that a file name is safe (i.e. fits the format "whatever.txt")
*/
static int valid_name(const char *name) {
  if (name == NULL) return 0;
  size_t len = strlen(name);
  if (len < 5 || len >= FM_MAX_NAME) return 0;
  if (strchr(name, '/') || strchr(name, '\\')) return 0; // return 0 if it contains a folder separator (could escape)
  if (strstr(name, "..")) return 0; // .. could point to a parent folder
  if (name[0] == '.') return 0; // no hiden files
  return strcmp(name + len -4, ".txt") == 0; // has to end in .txt
}


/*
Builds "data/<name>" into out, returning 0 on success, -1 otherwise
*/
static int build_path(const char *name, const char *suffix, char *out, size_t outsize) {
  int n = snprintf(out, outsize, "%s/%s%s", FM_DATA_DIR, name, suffix ? suffix : "");
  
  return (n < 0 || (size_t)n >= outsize) ? -1 : 0; // return -1 if snprintf failed or truncated
}

/*
Returns 1 if a file exists at 'path', 0 otherwise
*/
static int path_exists(const char *path) {
  struct stat st;
  
  return stat(path, &st) == 0;
  // *stat only succeeds if the file exists
}

/*
turns the errno into one of our FM codes
*/
static int errno_to_fm(void) {
  return (errno == ENOENT) ? FM_ERR_NOT_FOUND : FM_ERR_IO;
}

/*
Strips newline characters
*/
static void strip_newline(char *s) {
  size_t len = strlen(s);
  
  while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) {
    s[--len] = '\0';
  }
}

/*
Reads one line from f into buf without the newline. Returns 1 (got a line), 0 (end of file, or -1 (line too long for buffer) 
*/
static int read_line(FILE *f, char *buf, size_t bufsize) {
  if (fgets(buf, (int)bufsize, f) == NULL) return 0;
  
  size_t len = strlen(buf);
  
  int has_newline = (len > 0 && buf[len - 1] == '\n');
  
  if (!has_newline && !feof(f)) return -1;
  
  strip_newline(buf);
  
  return 1;
}

/*
Checks that a line given by caller is usable as a single record
*/
static int valid_line(const char *line) {
  if (line == NULL) return 0;
  if (strlen(line) >= FM_MAX_LINE) return 0;
  return strchr(line, '\n') == NULL && strchr(line, '\r') == NULL; // can't hold 2 lines
}


/*
Copies src_path to dst_path, swapping the first line equal to old_line. For new_line, if no line matches, dst is deleted and NO_MATCH is returned 
*/
static int copy_with_replacement(const char *src_path, const char *dst_path, const char *old_line, const char *new_line) {
  FILE *in = fopen(src_path, "r");
  if (in == NULL) return errno_to_fm();
  FILE *out = fopen(dst_path, "w");
  if (out == NULL) { // couldn't create out, cleanup and fail
    fclose(in);
    return FM_ERR_IO;
  }
  
  char line[FM_MAX_LINE + 2];
  int replaced = 0;
  int rc = FM_OK;
  int r;
  
  while ((r = read_line(in, line, sizeof line)) == 1) {
    if (!replaced && strcmp(line, old_line) == 0) {
      fprintf(out, "%s\n", new_line);
      replaced = 1;
    } else {
        fprintf(out, "%s\n", line);
    }
  }
  
  if (r == -1) rc = FM_ERR_TOO_BIG;
  if (ferror(in)) rc = FM_ERR_IO;
  fclose(in);
  if (fclose(out) != 0 && rc == FM_OK) rc = FM_ERR_IO;
  if (rc == FM_OK && !replaced) rc = FM_ERR_NO_MATCH;
  if (rc != FM_OK) remove(dst_path);
  return rc;
}

/*
Writes content to tmp_path then renames it onto final_path (either all of it happens or none of it does)
*/
static int write_atomically(const char *final_path, const char *tmp_path, const char *content) {
  FILE *f = fopen(tmp_path, "w");
  if (f == NULL) return FM_ERR_IO;
  size_t len = strlen(content);
  size_t written = fwrite(content, 1, len, f);
  int close_rc = fclose(f);
  if (written != len || close_rc != 0) {
    remove(tmp_path);
    return FM_ERR_IO;
  }
  if (rename(tmp_path, final_path) != 0) {
    remove(tmp_path);
    return FM_ERR_IO;
  }
  return FM_OK;
}

/*
Splits a copy of 'a,b,c' into exactly three non empty fields, returning 1 if successful
*/
static int split3(char *line, char **f1, char **f2, char **f3) {
  char *c1 = strchr(line, ',');
  if (c1 == NULL) return 0;
  char *c2 = strchr(c1 + 1, ',');
  if (c2 == NULL) return 0;
  if (strchr(c2 + 1, ',') != NULL) return 0;
  *c1 = '\0';
  *c2 = '\0';
  *f1 = line;
  *f2 = c1 + 1;
  *f3 = c2 + 1;
  return **f1 && **f2 && **f3;
}

/*
A valid student ID is "S" followed by one or more digits
*/
static int valid_student_id(const char *id) {
  if (id[0] != 'S' || id[1] == '\0') return 0;
  for (const char *p = id + 1; *p; p++) {
    if (!isdigit((unsigned char)*p)) return 0;
  }
  return 1;
}

/*
Parse a grade into 'out' (must be a whole number from 0 to 100)
*/
static int parse_grade(const char *s, int *out) {
  char *end;
  errno = 0;
  long v = strtol(s, &end, 10);
  if (errno != 0 || end == s || *end != '\0') return 0;
  if (v < 0 || v > 100) return 0;
  *out = (int)v;
  return 1;
}

/*
Sorts file names alphabetically (used by qsort)
*/
static int compare_names(const void *a, const void *b) {
  return strcmp((const char *)a, (const char *)b);
}


int fm_init(void) {
  if (mkdir(FM_DATA_DIR, 0755) == 0) return FM_OK;
  return (errno == EEXIST) ? FM_OK : FM_ERR_IO;
}

int fm_reset(void) {
  int rc = fm_init();
  if (rc != FM_OK) return rc;
  rc = fm_write(FM_FILE_STUDENTS, "S101,Ana,CSC3320\n""S102,Ben,CSC3320\n");
  if (rc != FM_OK) return rc;
  rc = fm_write(FM_FILE_GRADES, "S101,Assignment4,88\n""S102,Assignment4,91\n");
  if (rc != FM_OK) return rc;
  rc = fm_write(FM_FILE_AUDIT, "2026-11-04,Project initialized\n");
  if (rc != FM_OK) return rc;
  const char *files[] = {FM_FILE_STUDENTS, FM_FILE_GRADES, FM_FILE_AUDIT, FM_FILE_BACKUP};
  for (int i = 0; i < 4; i++) {
    fm_discard(files[i]);
  }
  
  char path[FM_MAX_PATH];
  build_path(FM_FILE_BACKUP, NULL, path, sizeof path);
  remove(path);
  return FM_OK; 
}

int fm_create(const char *name, const char *content) {
  if (!valid_name(name)) return FM_ERR_BAD_NAME;
  char path[FM_MAX_PATH];
  if (build_path(name, NULL, path, sizeof path) != 0) return FM_ERR_BAD_NAME;
  
  FILE *f = fopen(path, "wx");
  if (f == NULL) return (errno == EEXIST) ? FM_ERR_EXISTS : FM_ERR_IO;
  if (content) fputs(content, f);
  return (fclose(f) == 0) ? FM_OK : FM_ERR_IO;
}  
  
int fm_write(const char *name, const char *content) {
  if (!valid_name(name)) return FM_ERR_BAD_NAME;
  if (content == NULL) return FM_ERR_BAD_ARG;
  char path[FM_MAX_PATH], tmp[FM_MAX_PATH];
  if (build_path(name, NULL, path, sizeof path) != 0 || build_path(name, ".wtmp", tmp, sizeof tmp) != 0) return FM_ERR_BAD_NAME;
  return write_atomically(path, tmp, content);
}

int fm_read_all(const char *name, char *buf, size_t bufsize) {
  if (!valid_name(name)) return FM_ERR_BAD_NAME;
  if (buf == NULL || bufsize == 0) return FM_ERR_BAD_ARG;
  char path[FM_MAX_PATH];
  if (build_path(name, NULL, path, sizeof path) != 0) return FM_ERR_BAD_NAME;
  
  FILE *f = fopen(path, "r");
  if (f == NULL) return errno_to_fm();
  size_t n = fread(buf, 1, bufsize - 1, f);
  buf[n] = '\0';
  int too_big = (n == bufsize - 1 && fgetc(f) != EOF);
  int had_error = ferror(f);
  fclose(f);
  if (had_error) return FM_ERR_IO;
  
  return too_big ? FM_ERR_TOO_BIG : FM_OK;
}

int fm_append(const char *name, const char *line) {
  if (!valid_name(name)) return FM_ERR_BAD_NAME;
  if (!valid_line(line)) return FM_ERR_BAD_ARG;
  char path[FM_MAX_PATH];
  if (build_path(name, NULL, path, sizeof path) != 0) return FM_ERR_BAD_NAME;
  if (!path_exists(path)) return FM_ERR_NOT_FOUND;
  int needs_newline = 0;
  FILE *chk = fopen(path, "r");
  if (chk == NULL) return FM_ERR_IO;
  if (fseek(chk, -1, SEEK_END) == 0) needs_newline = (fgetc(chk) != '\n');
  fclose(chk);
  
  FILE *f = fopen(path, "a");
  if (f == NULL) return FM_ERR_IO;
  if (needs_newline) fputc('\n', f);
  fprintf(f, "%s\n", line);
  return (fclose(f) == 0) ? FM_OK : FM_ERR_IO;
}

int fm_replace_line(const char *name, const char *old_line, const char *new_line) {
  if (!valid_name(name)) return FM_ERR_BAD_NAME;
  if (!valid_line(old_line) || !valid_line(new_line)) return FM_ERR_BAD_ARG;
  char path[FM_MAX_PATH], tmp[FM_MAX_PATH];
  if (build_path(name, NULL, path, sizeof path) != 0 || build_path(name, ".wtmp", tmp, sizeof tmp) != 0) return FM_ERR_BAD_NAME;
  
  int rc = copy_with_replacement(path, tmp, old_line, new_line);
  if (rc != FM_OK) return rc;
  if (rename(tmp, path) != 0) {
    remove(tmp);
    return FM_ERR_IO;
  }
  return FM_OK;
}

int fm_stage_replace(const char *name, const char *old_line, const char *new_line) {
  if (!valid_name(name)) return FM_ERR_BAD_NAME;
  if (!valid_line(old_line) || !valid_line(new_line)) return FM_ERR_BAD_ARG;
  char path[FM_MAX_PATH], staged[FM_MAX_PATH], work[FM_MAX_PATH];
  if (build_path(name, NULL, path, sizeof path) != 0 ||
      build_path(name, ".tmp", staged, sizeof staged) != 0 ||
      build_path(name, ".tmp2", work, sizeof work) != 0) 
        return FM_ERR_BAD_NAME;
  
  const char *source = path_exists(staged) ? staged : path;
  int rc = copy_with_replacement(source, work, old_line, new_line);
  if (rc != FM_OK) return rc;
  if (rename(work, staged) != 0) {
    remove(work);
    return FM_ERR_IO;
  }
  return FM_OK;
}

int fm_commit(const char *name) {
  if (!valid_name(name)) return FM_ERR_BAD_NAME;
  char path[FM_MAX_PATH], staged[FM_MAX_PATH];
  if (build_path(name, NULL, path, sizeof path) != 0 || build_path(name, ".tmp", staged, sizeof staged) != 0) return FM_ERR_BAD_NAME;
  if (!path_exists(staged)) return FM_ERR_NO_STAGED;
  return (rename(staged, path) == 0) ? FM_OK : FM_ERR_IO;
}

int fm_discard(const char *name) {
  if (!valid_name(name)) return FM_ERR_BAD_NAME;
  char staged[FM_MAX_PATH];
  if (build_path(name, ".tmp", staged, sizeof staged) != 0) return FM_ERR_BAD_NAME;
  if (remove(staged) == 0) return FM_OK;
  return (errno == ENOENT) ? FM_ERR_NO_STAGED : FM_ERR_IO;
}

int fm_has_staged(const char *name) {
  if (!valid_name(name)) return 0;
  char staged[FM_MAX_PATH];
  if (build_path(name, ".tmp", staged, sizeof staged) != 0) return 0;
  return path_exists(staged);
}

int fm_stat(const char *name, long *size_bytes, time_t *modified) {
  if (!valid_name(name)) return FM_ERR_BAD_NAME;
  char path[FM_MAX_PATH];
  if (build_path(name, NULL, path, sizeof path) != 0) return FM_ERR_BAD_NAME;
  struct stat st;
  if (stat(path, &st) != 0) return errno_to_fm();
  if (size_bytes) *size_bytes = (long)st.st_size;
  if (modified) *modified = st.st_mtime;
  return FM_OK;
}

int fm_print_info (const char *name) {
  long size;
  time_t mtime;
  int rc = fm_stat(name, &size, &mtime);
  if (rc != FM_OK) return rc;
  struct tm tm_local;
  localtime_r(&mtime, &tm_local);
  char when[32];
  strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &tm_local);
  printf("  %-14s %6ld bytes  modified %s\n", name, size, when);
  return FM_OK;
}

int fm_list(void) {
  DIR *dir = opendir(FM_DATA_DIR);
  if (dir == NULL) return errno_to_fm();
  char names[64][FM_MAX_NAME];
  int count = 0;
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL && count < 64) {
    if (valid_name(entry->d_name)) {
      strncpy(names[count], entry->d_name, FM_MAX_NAME - 1);
      names[count][FM_MAX_NAME - 1] = '\0';
      count++;
    }
  }
  closedir(dir);
  qsort(names, count, FM_MAX_NAME, compare_names);
  printf("Files in %s/:\n", FM_DATA_DIR);
  for (int i = 0; i < count; i++) {
    fm_print_info(names[i]);
  }
  if (count == 0) printf("  (no .txt files found)\n");
  return FM_OK;
}

int fm_find_record(const char *name, const char *id, char *out, size_t outsize) {
  if (!valid_name(name)) return FM_ERR_BAD_NAME;
  if (id == NULL || *id == '\0' || out == NULL || outsize == 0) return FM_ERR_BAD_ARG;
  char path[FM_MAX_PATH];
  if (build_path(name, NULL, path, sizeof path) != 0) return FM_ERR_BAD_NAME;
  
  FILE *f = fopen(path, "r");
  if (f == NULL) return errno_to_fm();
  char line[FM_MAX_LINE + 2];
  size_t idlen = strlen(id);
  int rc = FM_ERR_NO_MATCH;
  int r;
  while ((r = read_line(f, line, sizeof line)) == 1) {
    if (strncmp(line, id, idlen) == 0 && line[idlen] == ',') {
      if (strlen(line) >= outsize) {
        rc = FM_ERR_TOO_BIG;
        break;
      }
      strcpy(out, line);
      rc = FM_OK;
      break;
    }
  }
  if (r == -1) rc = FM_ERR_TOO_BIG;
  fclose(f);
  return rc;
}

int fm_count_students(int *count) {
  if (count == NULL) return FM_ERR_BAD_ARG;
  char path[FM_MAX_PATH];
  build_path(FM_FILE_STUDENTS, NULL, path, sizeof path);
  FILE *f = fopen(path, "r");
  if (f == NULL) return errno_to_fm();
  char line[FM_MAX_LINE + 2];
  int n = 0, r;
  while((r = read_line(f, line, sizeof line)) == 1) {
    char *id, *student, *course;
    if (split3(line, &id, &student, &course) && valid_student_id(id)) n++;
  }
  fclose(f);
  if (r == -1) return FM_ERR_TOO_BIG;
  *count = n;
  return FM_OK;
}

int fm_grade_stats(int *count, double *mean, int *min, int *max) {
  if (!count || !mean || !min || !max) return FM_ERR_BAD_ARG;
  char path[FM_MAX_PATH];
  build_path(FM_FILE_GRADES, NULL, path, sizeof path);
  FILE *f = fopen(path, "r");
  if (f == NULL) return errno_to_fm();
  
  char line[FM_MAX_LINE + 2];
  int n = 0, lo = 0, hi = 0, r;
  long sum = 0;
  while ((r = read_line(f, line, sizeof line)) == 1) {
    char *id, *assignment, *grade_text;
    int grade;
    if (!split3(line, &id, &assignment, &grade_text)) continue; // skip malformed lines
    if (!valid_student_id(id)) continue; // skip bad IDs
    if (!parse_grade(grade_text, &grade)) continue;
    if (n == 0 || grade < lo) lo = grade;
    if (n == 0 || grade > hi) hi = grade;
    sum += grade;
    n++;
  }
  fclose(f);
  if (r == -1) return FM_ERR_TOO_BIG;
  *count = n;
  *mean = (n > 0) ? (double)sum / n : 0.0;
  *min = lo;
  *max = hi;
  return FM_OK;
}

int fm_count_lines(const char *name, int *count) {
  if (!valid_name(name)) return FM_ERR_BAD_NAME;
  if (count == NULL) return FM_ERR_BAD_ARG;
  char path[FM_MAX_PATH];
  if (build_path(name, NULL, path, sizeof path) != 0) return FM_ERR_BAD_NAME;
  FILE *f = fopen(path, "r");
  if (f == NULL) return errno_to_fm();
  char line[FM_MAX_LINE + 2];
  int n = 0, r;
  while ((r = read_line(f, line, sizeof line)) == 1) {
    if (line[0] != '\0') n++;
  }
  fclose(f);
  if (r == -1) return FM_ERR_TOO_BIG;
  *count = n;
  return FM_OK;
}

const char *fm_strerror(int code) {
  switch (code) {
  case FM_OK: return "ok";
  case FM_ERR_BAD_NAME: return "invalid or unsafe file name";
  case FM_ERR_NOT_FOUND: return "file not found";
  case FM_ERR_EXISTS: return "file already exists";
  case FM_ERR_IO: return "I/O error";
  case FM_ERR_NO_MATCH: return "record not found";
  case FM_ERR_TOO_BIG: return "data too large for buffer";
  case FM_ERR_NO_STAGED: return "no staged change";
  case FM_ERR_BAD_ARG: return "invalid argument";
  default: return "unkown error";
  }
}
