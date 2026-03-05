//===-- fd_init.c ---------------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#define _LARGEFILE64_SOURCE
#define _FILE_OFFSET_BITS 64
#include "fd.h"

#include "klee/klee.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>

exe_file_system_t __exe_fs;

/* NOTE: It is important that these are statically initialized
   correctly, since things that run before main may hit them given the
   current way things are linked. */

/* XXX Technically these flags are initialized w.o.r. to the
   environment we are actually running in. We could patch them in
   klee_init_fds, but we still have the problem that uclibc calls
   prior to main will get the wrong data. Not such a big deal since we
   mostly care about sym case anyway. */


exe_sym_env_t __exe_env = { 
  {{ 0, eOpen | eReadable, 0, 0}, 
   { 1, eOpen | eWriteable, 0, 0}, 
   { 2, eOpen | eWriteable, 0, 0}},
  022,
  0
};

static void __create_new_dfile(exe_disk_file_t *dfile, unsigned size, 
                               const char *name, struct stat64 *defaults) {
  struct stat64 *s = malloc(sizeof(*s));
  if (!s)
    klee_report_error(__FILE__, __LINE__, "out of memory in klee_init_env", "user.err");

  const char *sp;
  char sname[64];
  for (sp=name; *sp; ++sp)
    sname[sp-name] = *sp;
  memcpy(&sname[sp-name], "_stat", 6);

  assert(size);

  dfile->size = size;
  dfile->contents = malloc(dfile->size);
  if (!dfile->contents)
    klee_report_error(__FILE__, __LINE__, "out of memory in klee_init_env", "user.err");
  klee_make_symbolic(dfile->contents, dfile->size, name);
  
  klee_make_symbolic(s, sizeof(*s), sname);

  /* For broken tests */
  if (!klee_is_symbolic(s->st_ino) && 
      (s->st_ino & 0x7FFFFFFF) == 0)
    s->st_ino = defaults->st_ino;
  
  /* Important since we copy this out through getdents, and readdir
     will otherwise skip this entry. For same reason need to make sure
     it fits in low bits. */
  klee_assume((s->st_ino & 0x7FFFFFFF) != 0);

  /* uclibc opendir uses this as its buffer size, try to keep
     reasonable. */
  klee_assume((s->st_blksize & ~0xFFFF) == 0);

  klee_prefer_cex(s, !(s->st_mode & ~(S_IFMT | 0777)));
  klee_prefer_cex(s, s->st_dev == defaults->st_dev);
  klee_prefer_cex(s, s->st_rdev == defaults->st_rdev);
  klee_prefer_cex(s, (s->st_mode&0700) == 0600);
  klee_prefer_cex(s, (s->st_mode&0070) == 0040);
  klee_prefer_cex(s, (s->st_mode&0007) == 0004);
  klee_prefer_cex(s, (s->st_mode&S_IFMT) == S_IFREG);
  klee_prefer_cex(s, s->st_nlink == 1);
  klee_prefer_cex(s, s->st_uid == defaults->st_uid);
  klee_prefer_cex(s, s->st_gid == defaults->st_gid);
  klee_prefer_cex(s, s->st_blksize == 4096);
  klee_prefer_cex(s, s->st_atime == defaults->st_atime);
  klee_prefer_cex(s, s->st_mtime == defaults->st_mtime);
  klee_prefer_cex(s, s->st_ctime == defaults->st_ctime);

  s->st_size = dfile->size;
  s->st_blocks = 8;
  dfile->stat = s;
}

/* Read a file from the real host filesystem using raw syscalls
   (bypasses KLEE POSIX interception). Returns malloc'd buffer;
   sets *out_size to byte count. Returns NULL on failure. */
char *__read_concrete_file(const char *path, unsigned *out_size) {
  int fd = syscall(__NR_open, path, O_RDONLY);
  if (fd < 0)
    return 0;
  /* Determine file size via lseek */
  off_t end = syscall(__NR_lseek, fd, 0, SEEK_END);
  if (end < 0 || end > (off_t)(10u * 1024u * 1024u)) {
    syscall(__NR_close, fd);
    return 0;
  }
  syscall(__NR_lseek, fd, 0, SEEK_SET);
  unsigned size = (unsigned)end;
  char *buf = malloc(size);
  if (!buf) {
    syscall(__NR_close, fd);
    klee_report_error(__FILE__, __LINE__, "out of memory in klee_init_env", "user.err");
  }
  unsigned total = 0;
  while (total < size) {
    int r = syscall(__NR_read, fd, buf + total, size - total);
    if (r <= 0)
      break;
    total += (unsigned)r;
  }
  syscall(__NR_close, fd);

  if (total != size) {
    free(buf);
    return 0;
  }

  *out_size = size;
  return buf;
}

/* --- Constraint Markers Language --- */
#define CONSTRAINT_NONE   0
#define CONSTRAINT_ALPHA  1
#define CONSTRAINT_DIGIT  2
#define CONSTRAINT_ALNUM  3
#define CONSTRAINT_PRINT  4
#define CONSTRAINT_UPPER  5
#define CONSTRAINT_LOWER  6
#define CONSTRAINT_HEX    7
#define CONSTRAINT_RANGE  8
#define CONSTRAINT_SET    9
#define CONSTRAINT_SPACE 10
#define CONSTRAINT_ANY   11

typedef struct {
  unsigned char type;       /* CONSTRAINT_* constant */
  unsigned char range_lo;   /* for CONSTRAINT_RANGE: low bound (decimal) */
  unsigned char range_hi;   /* for CONSTRAINT_RANGE: high bound (decimal) */
  char set_chars[32];       /* for CONSTRAINT_SET: valid characters */
  unsigned char set_len;    /* for CONSTRAINT_SET: number of valid chars */
} byte_constraint_t;

/* Parse a ?{...} constraint specifier.
   template_data[start] must be the '{' after the marker.
   Returns the index of '}', or -1 on parse error.
   Fills *constraint with the parsed result. */
static int __parse_constraint(const char *template_data,
                              unsigned template_size,
                              unsigned start,
                              byte_constraint_t *constraint) {
  unsigned end = start + 1;

  /* Find closing '}' */
  while (end < template_size && template_data[end] != '}')
    end++;
  if (end >= template_size)
    return -1;  /* no closing brace */

  const char *spec = template_data + start + 1;  /* skip '{' */
  unsigned spec_len = end - start - 1;

  constraint->type = CONSTRAINT_NONE;
  constraint->set_len = 0;

  if (spec_len == 5 && memcmp(spec, "alpha", 5) == 0) {
    constraint->type = CONSTRAINT_ALPHA;
  } else if (spec_len == 5 && memcmp(spec, "digit", 5) == 0) {
    constraint->type = CONSTRAINT_DIGIT;
  } else if (spec_len == 5 && memcmp(spec, "alnum", 5) == 0) {
    constraint->type = CONSTRAINT_ALNUM;
  } else if (spec_len == 5 && memcmp(spec, "print", 5) == 0) {
    constraint->type = CONSTRAINT_PRINT;
  } else if (spec_len == 5 && memcmp(spec, "upper", 5) == 0) {
    constraint->type = CONSTRAINT_UPPER;
  } else if (spec_len == 5 && memcmp(spec, "lower", 5) == 0) {
    constraint->type = CONSTRAINT_LOWER;
  } else if (spec_len == 5 && memcmp(spec, "space", 5) == 0) {
    constraint->type = CONSTRAINT_SPACE;
  } else if (spec_len == 3 && memcmp(spec, "hex", 3) == 0) {
    constraint->type = CONSTRAINT_HEX;
  } else if (spec_len == 3 && memcmp(spec, "any", 3) == 0) {
    constraint->type = CONSTRAINT_ANY;
  } else if (spec_len > 6 && memcmp(spec, "range:", 6) == 0) {
    /* Parse range:X-Y where X and Y are decimal byte values (0-255) */
    constraint->type = CONSTRAINT_RANGE;
    unsigned lo = 0, hi = 0;
    unsigned p = 6;
    while (p < spec_len && spec[p] != '-')
      lo = lo * 10 + (spec[p++] - '0');
    if (p < spec_len) p++;  /* skip '-' */
    while (p < spec_len)
      hi = hi * 10 + (spec[p++] - '0');
    constraint->range_lo = (unsigned char)lo;
    constraint->range_hi = (unsigned char)hi;
  } else if (spec_len > 4 && memcmp(spec, "set:", 4) == 0) {
    /* Parse set:abc — each char after ':' is a valid value */
    constraint->type = CONSTRAINT_SET;
    constraint->set_len = spec_len - 4;
    if (constraint->set_len > 32) constraint->set_len = 32;
    memcpy(constraint->set_chars, spec + 4, constraint->set_len);
  } else {
    return -1;  /* unknown constraint */
  }

  return (int)end;  /* index of '}' */
}

/* Apply a constraint to a single symbolic byte via klee_assume.
   contents[offset] must already be symbolic. */
static void __apply_byte_constraint(char *contents, unsigned offset,
                                    byte_constraint_t *c) {
  switch (c->type) {
    case CONSTRAINT_ALPHA:
      klee_assume((contents[offset] >= 'A' && contents[offset] <= 'Z') ||
                  (contents[offset] >= 'a' && contents[offset] <= 'z'));
      break;
    case CONSTRAINT_DIGIT:
      klee_assume(contents[offset] >= '0' && contents[offset] <= '9');
      break;
    case CONSTRAINT_ALNUM:
      klee_assume((contents[offset] >= 'A' && contents[offset] <= 'Z') ||
                  (contents[offset] >= 'a' && contents[offset] <= 'z') ||
                  (contents[offset] >= '0' && contents[offset] <= '9'));
      break;
    case CONSTRAINT_PRINT:
      klee_assume(contents[offset] >= 32 && contents[offset] <= 126);
      break;
    case CONSTRAINT_UPPER:
      klee_assume(contents[offset] >= 'A' && contents[offset] <= 'Z');
      break;
    case CONSTRAINT_LOWER:
      klee_assume(contents[offset] >= 'a' && contents[offset] <= 'z');
      break;
    case CONSTRAINT_HEX:
      klee_assume((contents[offset] >= '0' && contents[offset] <= '9') ||
                  (contents[offset] >= 'a' && contents[offset] <= 'f') ||
                  (contents[offset] >= 'A' && contents[offset] <= 'F'));
      break;
    case CONSTRAINT_SPACE:
      klee_assume(contents[offset] == ' '  || contents[offset] == '\t' ||
                  contents[offset] == '\n' || contents[offset] == '\r');
      break;
    case CONSTRAINT_RANGE:
      klee_assume((unsigned char)contents[offset] >= c->range_lo);
      klee_assume((unsigned char)contents[offset] <= c->range_hi);
      break;
    case CONSTRAINT_SET: {
      int valid = 0;
      unsigned ci;
      for (ci = 0; ci < c->set_len; ci++)
        valid = valid || (contents[offset] == c->set_chars[ci]);
      klee_assume(valid);
      break;
    }
    default:  /* CONSTRAINT_NONE, CONSTRAINT_ANY — no constraint */
      break;
  }
}

/* Parse a template and create a dfile with mixed concrete/symbolic content.
   Marker char alone (e.g., '?') = unconstrained symbolic byte.
   Marker followed by '{...}' (e.g., '?{digit}') = constrained symbolic byte.
   All other characters = concrete bytes.
   Uses klee_make_symbolic_bytes (Objective 1.1). */
void __create_mixed_dfile(exe_disk_file_t *dfile,
                          const char *template_data,
                          unsigned template_size,
                          const char *name,
                          char marker,
                          struct stat64 *defaults) {
  unsigned i, j, out_size;

  if (template_size == 0)
    klee_report_error(__FILE__, __LINE__, "template file is empty", "user.err");

  /* --- Pass 1: compute output size ---
     Each marker ('?') or marker+constraint ('?{digit}') produces 1 output byte.
     Each non-marker character produces 1 output byte.
     Input chars consumed per output byte varies, but each produces exactly 1. */
  out_size = 0;
  for (i = 0; i < template_size; i++) {
    if (template_data[i] == marker) {
      if (i + 1 < template_size && template_data[i + 1] == '{') {
        /* Skip past closing '}' */
        unsigned end = i + 2;
        while (end < template_size && template_data[end] != '}') end++;
        if (end < template_size) i = end;  /* loop's i++ will advance past '}' */
      }
      /* else: plain marker, consumes just 1 char (the loop's i++ handles it) */
    }
    out_size++;
  }

  /* --- Allocate contents, mask, and constraints --- */
  dfile->size = out_size;
  dfile->contents = malloc(out_size);
  if (!dfile->contents)
    klee_report_error(__FILE__, __LINE__, "out of memory in klee_init_env", "user.err");

  unsigned char *mask = malloc(out_size);
  if (!mask)
    klee_report_error(__FILE__, __LINE__, "out of memory in klee_init_env", "user.err");

  byte_constraint_t *constraints = malloc(out_size * sizeof(byte_constraint_t));
  if (!constraints)
    klee_report_error(__FILE__, __LINE__, "out of memory in klee_init_env", "user.err");

  /* --- Pass 2: fill contents, mask, and constraints --- */
  j = 0;  /* output index */
  for (i = 0; i < template_size; i++) {
    if (template_data[i] == marker) {
      /* This is a symbolic byte */
      dfile->contents[j] = 0;   /* placeholder */
      mask[j] = 1;
      constraints[j].type = CONSTRAINT_NONE;

      if (i + 1 < template_size && template_data[i + 1] == '{') {
        /* Parse constraint: ?{...} */
        int close = __parse_constraint(template_data, template_size,
                                       i + 1, &constraints[j]);
        if (close < 0)
          klee_report_error(__FILE__, __LINE__,
                            "malformed constraint in template (missing '}' or unknown type)",
                            "user.err");
        i = (unsigned)close;  /* advance past '}'; loop's i++ handles next char */
      }
      /* else: plain '?' — unconstrained (CONSTRAINT_NONE already set) */
    } else {
      /* Concrete byte */
      dfile->contents[j] = template_data[i];
      mask[j] = 0;
      constraints[j].type = CONSTRAINT_NONE;
    }
    j++;
  }

  /* --- Make selected bytes symbolic --- */
  klee_make_symbolic_bytes(dfile->contents, out_size, name, mask, out_size);

  /* --- Apply constraints to symbolic bytes --- */
  for (j = 0; j < out_size; j++) {
    if (mask[j]) {
      if (constraints[j].type != CONSTRAINT_NONE &&
          constraints[j].type != CONSTRAINT_ANY)
        __apply_byte_constraint(dfile->contents, j, &constraints[j]);
      else
        /* Unconstrained symbolic: prefer printable ASCII (soft hint) */
        klee_posix_prefer_cex(dfile->contents,
                              (32 <= dfile->contents[j] &&
                               dfile->contents[j] <= 126));
    }
  }

  free(mask);
  free(constraints);

  /* --- Set up stat struct (same logic as __create_new_dfile) --- */
  struct stat64 *s = malloc(sizeof(*s));
  if (!s)
    klee_report_error(__FILE__, __LINE__, "out of memory in klee_init_env", "user.err");

  const char *sp;
  char sname[64];
  for (sp = name; *sp; ++sp)
    sname[sp - name] = *sp;
  memcpy(&sname[sp - name], "_stat", 6);

  klee_make_symbolic(s, sizeof(*s), sname);

  /* For broken tests */
  if (!klee_is_symbolic(s->st_ino) &&
      (s->st_ino & 0x7FFFFFFF) == 0)
    s->st_ino = defaults->st_ino;

  klee_assume((s->st_ino & 0x7FFFFFFF) != 0);
  klee_assume((s->st_blksize & ~0xFFFF) == 0);

  klee_prefer_cex(s, !(s->st_mode & ~(S_IFMT | 0777)));
  klee_prefer_cex(s, s->st_dev == defaults->st_dev);
  klee_prefer_cex(s, s->st_rdev == defaults->st_rdev);
  klee_prefer_cex(s, (s->st_mode & 0700) == 0600);
  klee_prefer_cex(s, (s->st_mode & 0070) == 0040);
  klee_prefer_cex(s, (s->st_mode & 0007) == 0004);
  klee_prefer_cex(s, (s->st_mode & S_IFMT) == S_IFREG);
  klee_prefer_cex(s, s->st_nlink == 1);
  klee_prefer_cex(s, s->st_uid == defaults->st_uid);
  klee_prefer_cex(s, s->st_gid == defaults->st_gid);
  klee_prefer_cex(s, s->st_blksize == 4096);
  klee_prefer_cex(s, s->st_atime == defaults->st_atime);
  klee_prefer_cex(s, s->st_mtime == defaults->st_mtime);
  klee_prefer_cex(s, s->st_ctime == defaults->st_ctime);

  s->st_size = dfile->size;
  s->st_blocks = 8;
  dfile->stat = s;
}
/* n_files: number of symbolic input files, excluding stdin
   file_length: size in bytes of each symbolic file, including stdin
   sym_stdout_flag: 1 if stdout should be symbolic, 0 otherwise
   save_all_writes_flag: 1 if all writes are executed as expected, 0 if 
                         writes past the initial file size are discarded 
			 (file offset is always incremented)
   max_failures: maximum number of system call failures */
void klee_init_fds(unsigned n_files, unsigned file_length,
                   unsigned stdin_length, int sym_stdout_flag,
                   int save_all_writes_flag, unsigned max_failures) {
  unsigned k;
  char name[7] = "?_data";
  struct stat64 s;

  stat64(".", &s);

  __exe_fs.n_sym_files = n_files;
  __exe_fs.sym_files = malloc(sizeof(*__exe_fs.sym_files) * n_files);
  if (n_files && !__exe_fs.sym_files)
    klee_report_error(__FILE__, __LINE__, "out of memory in klee_init_env", "user.err");

  for (k=0; k < n_files; k++) {
    name[0] = 'A' + k;
    __create_new_dfile(&__exe_fs.sym_files[k], file_length, name, &s);
  }
  
  /* setting symbolic stdin */
  if (stdin_length) {
    __exe_fs.sym_stdin = malloc(sizeof(*__exe_fs.sym_stdin));
    if (!__exe_fs.sym_stdin)
      klee_report_error(__FILE__, __LINE__, "out of memory in klee_init_env", "user.err");
    __create_new_dfile(__exe_fs.sym_stdin, stdin_length, "stdin", &s);
    __exe_env.fds[0].dfile = __exe_fs.sym_stdin;
  }
  else __exe_fs.sym_stdin = NULL;

  __exe_fs.max_failures = max_failures;
  if (__exe_fs.max_failures) {
    __exe_fs.read_fail = malloc(sizeof(*__exe_fs.read_fail));
    __exe_fs.write_fail = malloc(sizeof(*__exe_fs.write_fail));
    __exe_fs.close_fail = malloc(sizeof(*__exe_fs.close_fail));
    __exe_fs.ftruncate_fail = malloc(sizeof(*__exe_fs.ftruncate_fail));
    __exe_fs.getcwd_fail = malloc(sizeof(*__exe_fs.getcwd_fail));
    if (!(__exe_fs.read_fail && __exe_fs.write_fail && __exe_fs.close_fail
          && __exe_fs.ftruncate_fail && __exe_fs.getcwd_fail))
      klee_report_error(__FILE__, __LINE__, "out of memory in klee_init_env", "user.err");

    klee_make_symbolic(__exe_fs.read_fail, sizeof(*__exe_fs.read_fail), "read_fail");
    klee_make_symbolic(__exe_fs.write_fail, sizeof(*__exe_fs.write_fail), "write_fail");
    klee_make_symbolic(__exe_fs.close_fail, sizeof(*__exe_fs.close_fail), "close_fail");
    klee_make_symbolic(__exe_fs.ftruncate_fail, sizeof(*__exe_fs.ftruncate_fail), "ftruncate_fail");
    klee_make_symbolic(__exe_fs.getcwd_fail, sizeof(*__exe_fs.getcwd_fail), "getcwd_fail");
  }

  /* setting symbolic stdout */
  if (sym_stdout_flag) {
    __exe_fs.sym_stdout = malloc(sizeof(*__exe_fs.sym_stdout));
    if (!__exe_fs.sym_stdout)
      klee_report_error(__FILE__, __LINE__, "out of memory in klee_init_env", "user.err");
    __create_new_dfile(__exe_fs.sym_stdout, 1024, "stdout", &s);
    __exe_env.fds[1].dfile = __exe_fs.sym_stdout;
    __exe_fs.stdout_writes = 0;
  }
  else __exe_fs.sym_stdout = NULL;
  
  __exe_env.save_all_writes = save_all_writes_flag;
}
