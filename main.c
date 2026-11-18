/* enables realpath() and setlinebuf() */
#ifndef _GNU_SOURCE
#  define _GNU_SOURCE 1
#endif

#include "stdint.h"
#include "stdarg.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "time.h"

#include "signal.h"
#include "unistd.h"
#include "errno.h"
#include "sys/stat.h"

#include "lib/tinymap.h"
#include "lib/tinycli.h"
#include "lib/fatrack.h"
#include "lib/pstrack.h"

#include "pm.h"

#include "main/db.c"

int check_file_exist(int *exist, const char *pathname) {
  struct stat stat;
  if (!lstat(pathname, &stat)) { *exist = 1; return 1; }
  if (errno == EACCES) return 0;
  *exist = 0; return 1;
}

/* https://en.cppreference.com/cpp/chrono/c/time */
/* https://www.sqlite.org/datatype3.html */
// long int epoch() {
//   struct timespec spec = {};
//   if (clock_gettime(CLOCK_REALTIME, &spec)) return 0;
//   return spec.tv_sec;
// }
int64_t get_timestamp() { return (int64_t) time(NULL); }
time_t timestamp_time(int64_t timestamp) { return (time_t) timestamp; }
const char *timestamp_localstring(int64_t timestamp) {
  if (!timestamp) return "?";
  static char time[256] = {};
  time_t t = timestamp_time(timestamp);
  if (strftime(time, sizeof(time), "%x %X", localtime(&t)) <= 0) return NULL;
  return time;
}

int main_running = 1;
void main_init_trap(int sig) {
  if (!main_running) exit(2);
  fprintf(stderr, "^C: requesting stop...\n^C again to force exit\n");
  main_running = 0;
}

const char *main_default_database_pathname = "/var/lib/fap/fap.db";

sqlite3 *main_db = NULL;
struct pm *main_pm = NULL;
int main_pm_id = 0;

void main_log(const char *format, ...) {
  va_list args; va_start(args, format);
  vfprintf(stderr, format, args); fprintf(stderr, "\n");
  va_end(args);
}

void parse_exepkgpm(const char **dest_exe_path, const char **dest_pkg_name, const char **dest_pm_name, const char *arg) {
  const char *exe_path = NULL, *pkg_name = NULL, *pm_name = NULL;
  if (*arg == '/') exe_path = arg;
  else {
    /* pkg_name with optional pm_name */
    // TODO use more reasonable length specifier
    static char name[PATH_MAX]; strncpy(name, arg, sizeof(name));
    pkg_name = name;
    char *colon = strchr(pkg_name, ':');
    if (colon) { *colon = 0; pm_name = pkg_name; pkg_name = colon + 1; }
  }
  if (dest_exe_path) *dest_exe_path = exe_path;
  if (dest_pkg_name) *dest_pkg_name = pkg_name;
  if (dest_pm_name) *dest_pm_name = pm_name;
}

int parse_bool(int *ret, const char *arg) {
  if (!arg || !*arg) return 0;
  switch (*arg) {
  case 't': case 'T': case 'y': case 'Y': *ret = 1; return 1;
  case 'f': case 'F': case 'n': case 'N': *ret = 0; return 1;
  }
  return 0;
}

void print_help() {
  fprintf(stderr, "%s",
    "fap - file app process monitor (demo)" "\n"
    "usage: fap [option...] <command> [param...]" "\n"
    "\n"
    "options:" "\n"
    "  -h, --help                show this help and exit" "\n"
    "  -D, --database <file>     specify database path" "\n"
    "  -P, --pm <name>           specify package manager to interact" "\n"
    "                            available: apk, dpkg" "\n"
    "\n"
    "commands & params:" "\n"
    "| m, monitor [dir...]       monitor file changes and save to database" "\n"
    "                              defaults to monitoring all dirs (/)" "\n"
    "    -n, --dry-run             - do not write to database" "\n"
    "    -o, --operations <ops>    - filter file operations on output" "\n"
    "                                  +: create, -: delete" "\n"
    "                                  <: move from, >: move to, =: move" "\n"
    "                                  e.g. fap monitor -o '+-='" "\n"
    "    -f, --format <fmt>        - output format" "\n"
    "                                  %T - time     %E - exe full path" "\n"
    "                                  %P - exe pid  %N - target file path" "\n"
    "                                  %O - operation(s)" "\n"
    "    -a, --all                 - also record deleted files" "\n"
    "| c, config [exe/pkg...]    set/list settings for programs" "\n"
    "                              see below for argument format" "\n"
    "    -m, --mask <bool>         - set whether to treat an operation being" "\n"
    "                                performed by the parent process" "\n"
    "| l, list                   list known files and related programs" "\n"
    "    -a, --all                 - also show deleted files" "\n"
    "| s, show [exe/pkg...]      show programs with their file changes" "\n"
    "                              see below for argument format" "\n"
    "    -p, --package             - catagorise as packages if possible" "\n"
    "    -c, --creation            - only show related file creation" "\n"
    "    -a, --all                 - also show deleted files" "\n"
    "| o, orphans                list files created by uninstalled programs" "\n"
    "    -a, --all                 - also show deleted files" "\n"
    "    --forget                  - delete records of these files in database" "\n"
    "    --delete                  - delete these files" "\n"
    "| f, forget <type> <arg...> delete specific type of records in database" "\n"
    "    #type        #arg" "\n"
    "    file         path         forget files" "\n"
    "    exe          path         forget executables with all related files" "\n"
    "    pkg, package pm:pkg       forget packages with all related exe and file records" "\n"
    "| u, update                 recheck file, program and package status" "\n"
    "\n"
    "argument formats:" "\n"
    "  exe/pkg                   <executable path> | [<package manager>:]<package>" "\n"
    "                            <executable path> shall begin with '/'" "\n"
  );
}

// TODO prevent opening db in dry run mode
int main_monitor(int argc, const char **argv) {
  sqlite3 *db = main_db;

  const char **opt_dryrun = NULL, **opt_operations = NULL,
    **opt_format = NULL, **opt_all = NULL;
  struct tinycli_option opts[] = {
    { "dry-run", 'n', 0, &opt_dryrun },
    { "operations", 'o', 1, &opt_operations },
    { "format", 'f', 1, &opt_format },
    { "all", 'a', 0, &opt_all },
    { NULL }
  };

  int cliopts_len = tinycli_parse(opts, --argc, ++argv);
  if (cliopts_len < 0) {
    main_log("invalid argument around %s", argv[-cliopts_len - 1]);
    print_help(); return 1;
  }
  argc -= cliopts_len; argv += cliopts_len;

  int save_db = opt_dryrun == NULL;
  unsigned int filter_ops = FAN_CREATE | FAN_DELETE | FAN_MOVE;
  if (opt_operations) {
    filter_ops = 0;
    const char *spec = opt_operations[1];
    for (const char *s = spec; *s; ++s) {
      switch (*s) {
      case '+': filter_ops |= FAN_CREATE; break;
      case '-': filter_ops |= FAN_DELETE; break;
      case '<': filter_ops |= FAN_MOVED_FROM; break;
      case '>': filter_ops |= FAN_MOVED_TO; break;
      case '=': filter_ops |= FAN_MOVE; break;
      default: main_log("unrecognized operation: %c", *s);
        print_help(); return 1;
      }
    }
  }
  const char *output_format = "(%T) %E(%P): %O %F";
  if (opt_format) output_format = opt_format[1];
  int keep_deleted = opt_all != NULL;

  struct pstrack pstrack = {};
  if (!pstrack_init(&pstrack, 64)) {
    main_log("failed to init pstrack: %d", errno);
    return 1;
  }

  struct fatrack fatrack = {};
  if (!fatrack_init(&fatrack, FAN_ONDIR | FAN_EVENT_ON_CHILD | FAN_CREATE | FAN_DELETE | FAN_MOVE)) {
    main_log("failed to init fatrack: %s", fatrack.error);
    pstrack_fini(&pstrack);
    return 1;
  }

  int ret = 0;

  while (argc) {
    static char path[PATH_MAX];
    struct stat st;
    if (realpath(*argv, path) && !stat(path, &st) && S_ISDIR(st.st_mode)) {
      if (!fatrack_markpath(&fatrack, path)) {
        main_log("failed to record target directory: %s -> %s: %s", *argv, path, fatrack.error);
        ret = 1; goto on_end;
      }
    }
    else {
      main_log("invalid directory in argument: %s", *argv);
      ret = 1; goto on_end;
    }
    --argc; ++argv;
  }

  fatrack_marksubmounts(&fatrack);

  pid_t self_pid = getpid();

  while (main_running && pstrack_alive(&pstrack)) {
    int64_t timestamp = get_timestamp();

    struct fatrack_event ev = {};
    int stat = fatrack_next(&fatrack, &ev);
    if (fatrack_haserror(&fatrack))
      main_log("fatrack: %s", fatrack.error);
    if (stat < 0) break;
    if (!stat) continue;

    struct fanotify_event_metadata *metadata = &ev.metadata;

    unsigned int mask = metadata->mask;
    int create = mask & (FAN_CREATE | FAN_MOVED_TO),
      delete = mask & (FAN_DELETE | FAN_MOVED_FROM);

    struct pstrack_procinfo proc = {};
    if (!pstrack_query(&pstrack, &proc, metadata->pid)) {
      main_log("pstrack: failed to query pid %d: %d", metadata->pid, errno);
      continue;
    }

    if (mask & filter_ops) {
      static char ops[8];
      char *o = ops;
      /* reserved for further monitored operations */
      if (mask & FAN_OPEN) *o++ = 'O';
      if (mask & FAN_ACCESS) *o++ = 'A';
      if (mask & FAN_CLOSE) *o++ = 'C';
      if (mask & FAN_CREATE) *o++ = '+';
      if (mask & FAN_DELETE) *o++ = '-';
      if (mask & FAN_MOVED_FROM) *o++ = '<';
      if (mask & FAN_MOVED_TO) *o++ = '>';
      *o = 0;

      for (const char *f = output_format; *f; ++f) {
        if (*f == '%') {
          switch (*++f) {
          case 'T': printf("%s", timestamp_localstring(timestamp)); break;
          case 'E': printf("%s", proc.pathname); break;
          case 'P': printf("%d", metadata->pid); break;
          case 'F': printf("%s", ev.filepath); break;
          case 'O': printf("%s", ops); break;
          default: printf("%c", *f); break;
          }
        }
        else printf("%c", *f);
      }
      puts("");
    }

    if (!save_db) continue;

    /* update database */

    int exe_id = db_add_exe(db, proc.pathname);
    /* use parent process if ignored */
    int exe_pid = metadata->pid;
    while (exe_id && db_check_exe_ignored(db, exe_id)) {
      exe_pid = proc.ppid;
      if (exe_pid == self_pid) { exe_id = 0; break; }
      if (!pstrack_query(&pstrack, &proc, exe_pid)) {
        main_log("pstrack: failed to query pid %d: %d", exe_pid, errno);
        exe_id = 0; break;
      }
      exe_id = db_add_exe(db, proc.pathname);
    }
    if (!exe_id) {
      main_log("failed to record exe %s", proc.pathname);
      continue;
    }

    int pkg_id = db_get_exe_pkg(db, exe_id);
    if (!pkg_id && main_pm && main_pm_id) {
      static char name[PM_NAME_MAX];
      if (main_pm->queryfile(name, proc.pathname))
        pkg_id = db_add_pkg(db, main_pm_id, name);
      if (pkg_id)
        if (!db_set_exe_pkg(db, exe_id, pkg_id))
          main_log("failed to assign exe %d with pkg %d", exe_id, pkg_id);
    }

    const char *filepath = ev.filepath;
    /* fanotify may report an event with +- */
    if (!keep_deleted && delete)
      db_del_file(db, filepath);
    else {
      if (create) {
        if (!db_record_file_create(db, filepath, exe_id, timestamp))
          { main_log("failed to record file creation %s", filepath); break; }
      }
      if (delete)
        db_record_file_delete(db, filepath, exe_id, timestamp);
    }
  }

  on_end:
  fatrack_fini(&fatrack); pstrack_fini(&pstrack);
  return ret;
}

int main_config(int argc, const char **argv) {
  sqlite3 *db = main_db;

  const char **opt_mask = NULL;
  struct tinycli_option opts[] = {
    { "mask", 'm', 1, &opt_mask },
    { NULL }
  };

  int cliopts_len = tinycli_parse(opts, --argc, ++argv);
  if (cliopts_len < 0) {
    main_log("invalid argument around %s", argv[-cliopts_len - 1]);
    print_help(); return 1;
  }
  argc -= cliopts_len; argv += cliopts_len;

  int mask = 0;
  if (opt_mask && !parse_bool(&mask, opt_mask[1]))
    { main_log("invalid mask value: %s", opt_mask[1]); return 1; }
  int show_mode = !opt_mask;

  do {
    const char *exe_path = NULL, *pkg_name = NULL, *pm_name = NULL;
    if (argc) {
      const char *arg = *argv++; --argc;
      parse_exepkgpm(&exe_path, &pkg_name, &pm_name, arg);
    }

    struct db_exepkgpm_query q = {
      .args = { exe_path, pkg_name, pm_name },
    };
    while (main_running && db_query_exepkgpm(db, &q)) {
      if (show_mode) {
        puts("");
        if (pkg_name) printf("[%s:%s]\n" "mask: %d\n", q.pm_name, q.pkg_name, q.pkg_ignored);
        else {
          printf("%s", q.exe_path);
          if (q.pkg_name) printf(" [%s:%s]", q.pm_name, q.pkg_name);
          printf("\n" "mask: %d\n", q.exe_ignored);
        }
        continue;
      }

      if (pkg_name) {
        if (opt_mask && !db_set_pkg_ignored(db, q.pkg_id, mask))
          main_log("failed to set mask for pkg [%s:%s]", q.pm_name, q.pkg_name);
      }
      else {
        if (opt_mask && !db_set_exe_ignored(db, q.exe_id, mask))
          main_log("failed to set mask for program %s", q.exe_path);
      }
    }
    db_fini_query(&q.stmt);
  } while (main_running && argc);

  return 0;
}

int main_list(int argc, const char **argv) {
  sqlite3 *db = main_db;

  const char **opt_all = NULL;
  struct tinycli_option opts[] = {
    { "all", 'a', 0, &opt_all },
    { NULL }
  };

  int cliopts_len = tinycli_parse(opts, --argc, ++argv);
  if (cliopts_len < 0) {
    main_log("invalid argument around %s", argv[-cliopts_len - 1]);
    print_help(); return 1;
  }
  argc -= cliopts_len; argv += cliopts_len;

  struct db_fileexepkgpm_all_query q = { .args = { .include_nonexist = opt_all != NULL } };
  while (main_running && db_query_fileexepkgpm_all(db, &q)) {
    printf("%s", q.file_path);
    if (!q.file_exist) {
      printf(" (deleted");
      if (q.delexe_path) printf(" by %s", q.delexe_path);
      if (q.delpkg_name) printf(" [%s:%s]", q.delpm_name, q.delpkg_name);
      if (q.file_time_delete) printf(" (%s)", timestamp_localstring(q.file_time_delete));
      printf(")");
    }
    printf(": %s", q.exe_path? q.exe_path: "(unknown)");
    if (q.pkg_name) printf(" [%s:%s]", q.pm_name, q.pkg_name);
    if (q.file_time_create) printf(" (%s)", timestamp_localstring(q.file_time_create));
    puts("");
  }
  db_fini_query(&q.stmt);

  return 0;
}

int main_show(int argc, const char **argv) {
  sqlite3 *db = main_db;

  const char **opt_organise_package = NULL, **opt_creation = NULL, **opt_all = NULL;
  struct tinycli_option opts[] = {
    { "packages", 'p', 0, &opt_organise_package },
    { "creation", 'c', 0, &opt_creation },
    { "all", 'a', 0, &opt_all },
    { NULL }
  };

  int cliopts_len = tinycli_parse(opts, --argc, ++argv);
  if (cliopts_len < 0) {
    main_log("invalid argument around %s", argv[-cliopts_len - 1]);
    print_help(); return 1;
  }
  argc -= cliopts_len; argv += cliopts_len;

  int organise_package = opt_organise_package != NULL;
  int only_creation = opt_creation != NULL;
  int show_all = opt_all != NULL;

  int last_exe_id = 0, last_pkg_id = 0;
  do {
    const char *exe_path = NULL, *pkg_name = NULL, *pm_name = NULL;
    if (argc) {
      const char *arg = *argv++; --argc;
      parse_exepkgpm(&exe_path, &pkg_name, &pm_name, arg);
    }

    struct db_exepkgpm_query q = {
      .args = { exe_path, pkg_name, pm_name },
    };
    while (main_running && db_query_exepkgpm(db, &q)) {
      if (organise_package && q.pkg_id) {
        if (last_pkg_id != q.pkg_id)
          printf("\n[%s:%s]\n", q.pm_name, q.pkg_name);
      }
      else {
        if (last_exe_id != q.exe_id) {
          printf("\n%s", q.exe_path);
          if (pkg_name) printf(" [%s:%s]", q.pm_name, q.pkg_name);
          puts(":");
        }
      }
      last_exe_id = q.exe_id; last_pkg_id = q.pkg_id;

      sqlite3_stmt *stmt = db_stmt(db,
        "select file_exist, file_path, file_time_create, exe_id_create, exe_id_delete from file"
        "  where (file_exist or ?3) and (exe_id_create = ?1 or (not ?2 and exe_id_delete = ?1));",
        "iii", q.exe_id, only_creation, show_all);
      if (!stmt) { main_log("failed to prepare sql query"); continue; }
      while (sqlite3_step(stmt) == SQLITE_ROW) {
        int exist, exe_id_create, exe_id_delete; const char *file_path;
        int64_t time_create;
        db_stmt_columns(stmt, "isIii",
          &exist, &file_path, &time_create, &exe_id_create, &exe_id_delete);

        static char ops[4] = {}; char *o = ops;
        if (q.exe_id == exe_id_create)
          { *o++ = '+'; printf("(%s) ", timestamp_localstring(time_create)); }
        if (q.exe_id == exe_id_delete) *o++ = '-';
        *o++ = 0;
        printf("%s %s%s\n", ops, file_path, exist? "": " (deleted)");
      }
      sqlite3_finalize(stmt);
    }
    if (q.stmt) sqlite3_finalize(q.stmt);
  } while (main_running && argc);

  return 0;
}

int main_orphans(int argc, const char **argv) {
  sqlite3 *db = main_db;

  const char **opt_all = NULL, **opt_forget = NULL, **opt_delete = NULL;
  struct tinycli_option opts[] = {
    { "all", 'a', 0, &opt_all },
    { "forget", 0, 0, &opt_forget },
    { "delete", 0, 0, &opt_delete },
    { NULL }
  };

  int cliopts_len = tinycli_parse(opts, --argc, ++argv);
  if (cliopts_len < 0) {
    main_log("invalid argument around %s", argv[-cliopts_len - 1]);
    print_help(); return 1;
  }
  argc -= cliopts_len; argv += cliopts_len;

  int show_delete = opt_all != NULL,
    perform_forget = opt_forget != NULL,
    perform_delete = opt_delete != NULL;

  sqlite3_stmt *stmt = db_stmt(db,
    "select file.file_path, file.file_time_create, exe.exe_id, exe.exe_path,"
    "       pkg.pkg_name, pm.pm_name from file"
    "  left join exe on exe.exe_id = file.exe_id_create"
    "  left join pkg on pkg.pkg_id = exe.pkg_id"
    "  left join pm on pm.pm_id = pkg.pm_id"
    "  where (? or file.file_exist) and"
    "    (not coalesce(exe.exe_exist, false) or"
    /* include records with no pkg assigned */
    "    not coalesce(pkg.pkg_exist, true));",
    "i", show_delete);
  if (!stmt) { main_log("failed to prepare sql query"); return 1; }
  while (main_running && sqlite3_step(stmt) == SQLITE_ROW) {
    const char *file_path, *exe_path, *pkg_name, *pm_name; int exe_id;
    int64_t time_create;
    db_stmt_columns(stmt, "sIisss",
      &file_path, &time_create, &exe_id, &exe_path, &pkg_name, &pm_name);

    // TODO if file now not exist or exe/pkg now exist, update and skip

    printf("(%s) %s: %s", timestamp_localstring(time_create), file_path, exe_path);
    if (pkg_name && pm_name) printf(" [%s:%s]", pm_name, pkg_name);
    puts("");

    int exist = 0;
    if (perform_delete && check_file_exist(&exist, file_path) && exist) {
      /*
        remove() from C standard quals to unlink() for files and rmdir()
        for directories, the latter will success only on empty ones.
      */
      if (!remove(file_path)) {
        if (!db_set_file_exist(db, file_path, 0, get_timestamp()))
          main_log("failed to record file deletion for %s", file_path);
      }
      else main_log("failed to delete %s: %d", file_path, errno);
    }

    if (perform_forget) {
      if (!db_del_file(db, file_path))
        main_log("failed to delete record for file %s", file_path);
    }
  }
  sqlite3_finalize(stmt);

  return 0;
}

int main_forget(int argc, const char **argv) {
  sqlite3 *db = main_db;

  struct tinycli_option opts[] = { { NULL } };

  int cliopts_len = tinycli_parse(opts, --argc, ++argv);
  if (cliopts_len < 0) {
    main_log("invalid argument around %s", argv[-cliopts_len - 1]);
    print_help(); return 1;
  }
  argc -= cliopts_len; argv += cliopts_len;

  if (argc < 2) {
    main_log("needs at least a <type> with an <arg>");
    print_help(); return 1;
  }

  const char *type = *argv++; --argc;
  if (!strcmp(type, "file")) {
    while (argc) {
      const char *path = *argv++; --argc;
      if (!db_del_file(db, path))
        main_log("failed to delete record of file %s", path);
    }
  }
  else if (!strcmp(type, "exe")) {
    while (argc) {
      const char *path = *argv++; --argc;
      if (!db_del_exefile_byexepath(db, path))
        main_log("failed to delete record of exe %s", path);
    }
  }
  else if (!strcmp(type, "pkg") || !strcmp(type, "package")) {
    while (argc) {
      const char *pkg = *argv++; --argc;
      const char *pkg_name = NULL, *pm_name = NULL;
      parse_exepkgpm(NULL, &pkg_name, &pm_name, pkg);
      if (!pkg_name || !pm_name)
        { main_log("invalid package: '%s'", pkg); continue; }
      int pkg_id = db_find_pkg_bypmnamepkgname(db, pm_name, pkg_name);
      if (!pkg_id)
        { main_log("failed to find package: %s", pkg); continue; }

      struct db_exe_bypkg_query q = {
        .args = { .pkg_id = pkg_id },
      };
      int error = 0;
      while (main_running && db_query_exe_bypkg(db, &q)) {
        if (!db_del_exefile_byexe(db, q.exe_id)) {
          main_log("failed to delete record of exe %d", q.exe_id);
          error = 1; break;
        }
      }
      if (!db_fini_query(&q.stmt)) {
        main_log("failed to fetch exe list with pkg id %d", pkg_id);
        continue;
      }
      if (error) continue;

      if (!db_del_pkg_updateexe(db, pkg_id))
        main_log("failed to delete record of pkg %s", pkg);
    }
  }
  else {
    main_log("unrecognized type %s", type);
    print_help(); return 1;
  }

  return 0;
}

int main_update_pkgs(sqlite3 *db, struct pm *pm, struct tinymap *pkg_map) {
  if (!pm || !tinymap_len(pkg_map)) return 0;

  if (!pm->checkpkgs(pkg_map)) {
    main_log("failed to query package status");
    return 0;
  }

  struct tinymap_node *node = NULL;
  while (tinymap_enum(pkg_map, &node)) {
    static char name[PM_NAME_MAX];
    const char *key = tinymap_node_key(pkg_map, node);
    strncpy(name, key, node->key_len);
    if (!db_set_pkg_exist(db,
      node->value & PM_TINYMAP_RESERVED_MASK,
      node->value & PM_TINYMAP_PKG_EXIST > 0))
      main_log("failed to set status of package %s", name);
  }

  return 1;
}

int main_update(int argc, const char **argv) {
  sqlite3 *db = main_db;

  /* file */

  sqlite3_stmt *stmt = db_stmt(db, "select file_path, file_exist from file;", NULL);
  if (!stmt) { main_log("failed to prepare sql query"); return 1; }
  while (main_running && sqlite3_step(stmt) == SQLITE_ROW) {
    const char *file_path; int file_exist;
    db_stmt_columns(stmt, "si", &file_path, &file_exist);

    int exist = 0;
    if (check_file_exist(&exist, file_path)) {
      if (exist != file_exist && !db_set_file_exist(db, file_path, exist, get_timestamp()))
        main_log("failed to update status for file %s", file_path);
    }
    else main_log("failed to check file %s existence: %d", file_path, errno);
  }
  sqlite3_finalize(stmt);

  if (!main_running) return 0;

  /* program/exe */

  stmt = db_stmt(db, "select exe_id, exe_path, exe_exist, pkg_id from exe;", NULL);
  if (!stmt) { main_log("failed to prepare sql query"); return 1; }
  while (main_running && sqlite3_step(stmt) == SQLITE_ROW) {
    int exe_id, exe_exist, pkg_id; const char *exe_path;
    db_stmt_columns(stmt, "isii", &exe_id, &exe_path, &exe_exist, &pkg_id);

    int exist = 0;
    if (check_file_exist(&exist, exe_path)) {
      if (exist != exe_exist && !db_set_exe_exist(db, exe_id, exist))
        main_log("failed to update status for program %s", exe_path);
    }
    else main_log("failed to check program %s existence: %d", exe_path, errno);

    if (exist && !pkg_id && main_pm && main_pm_id) {
      static char name[PM_NAME_MAX];
      if (main_pm->queryfile(name, exe_path)) {
        pkg_id = db_add_pkg(db, main_pm_id, name);
        if (!pkg_id) main_log("failed to record package [%s:%s]", main_pm->name, name);
        else if (!db_set_exe_pkg(db, exe_id, pkg_id))
          main_log("failed to update package [%s:%s]", main_pm->name, name);
      }
      else main_log("failed to query package for file %s", exe_path);
    }
  }
  sqlite3_finalize(stmt);

  if (!main_running) return 0;

  /* package */

  struct tinymap pkg_map = {};
  if (!tinymap_init(&pkg_map)) {
    main_log("failed to init tinymap"); return 1;
  }

  stmt = db_stmt(db,
    "select pkg.pkg_id, pkg.pkg_name, pkg.pkg_exist, pm.pm_id, pm.pm_name"
    "  from pkg left join pm on pm.pm_id = pkg.pm_id order by pkg.pm_id;", NULL);
  if (!stmt) { main_log("failed to prepare sql query"); return 1; }
  int last_pm_id = 0; struct pm *pkg_pm = NULL;
  while (main_running && sqlite3_step(stmt) == SQLITE_ROW) {
    int pkg_id, pkg_exist, pm_id; const char *pkg_name, *pm_name;
    db_stmt_columns(stmt, "isiis", &pkg_id, &pkg_name, &pkg_exist, &pm_id, &pm_name);

    if (last_pm_id != pm_id) {
      if (pkg_pm) main_update_pkgs(db, pkg_pm, &pkg_map);
      tinymap_clear(&pkg_map);
      pkg_pm = pm_get(pm_name);
      if (!pkg_pm) main_log("failed to get package manager %s", pm_name);
      last_pm_id = pm_id;
    }

    if (!pkg_pm) { main_log("skipped package %s", pkg_name); continue; }

    long int value = pkg_id & PM_TINYMAP_RESERVED_MASK;
    if (value != pkg_id) {
      main_log("tinymap mask overflow detected, abort update");
      pkg_pm = NULL;
      break;
    }

    if (!tinymap_add(&pkg_map, pkg_name, strlen(pkg_name), value))
      main_log("warning: failed to add pkg %s into tinymap", pkg_name);
  }
  sqlite3_finalize(stmt);
  if (pkg_pm) main_update_pkgs(db, pkg_pm, &pkg_map);
  tinymap_fini(&pkg_map);

  return 0;
}

struct main_command {
  const char *name; char alias;
  int (*func)(int argc, const char **argv);
} commands[] = {
  { "monitor", 'm', main_monitor },
  { "config", 'c', main_config },
  { "list", 'l', main_list },
  { "show", 's', main_show },
  { "orphans", 'o', main_orphans },
  { "forget", 'f', main_forget },
  { "update", 'u', main_update },
  { NULL }
};

int main(int argc, const char **argv) {
  struct sigaction sighandler = { .sa_handler = main_init_trap };
  if (sigaction(SIGINT, &sighandler, NULL)) perror("sigaction");

  sqlite3_config(SQLITE_CONFIG_LOG, db_sqlite3_logger, NULL);
  setlinebuf(stdout);

  const char **opt_help = NULL, **opt_database = NULL, **opt_pm = NULL;
  struct tinycli_option opts[] = {
    { "help", 'h', 0, &opt_help },
    { "database", 'D', 1, &opt_database },
    { "pm", 'p', 1, &opt_pm },
    { NULL }
  };

  int cliopts_len = tinycli_parse(opts, --argc, ++argv);
  if (cliopts_len < 0) {
    main_log("invalid argument around %s", argv[-cliopts_len - 1]);
    print_help(); return 1;
  }

  if (opt_help) { print_help(); return 0; }

  argc -= cliopts_len; argv += cliopts_len;
  if (argc <= 0) {
    main_log("needs a command"); print_help(); return 1;
  }

  int ret = 1;

  const char *db_path = main_default_database_pathname;
  if (opt_database) { db_path = *opt_database; }
  else {
    const char *path = getenv("FAP_DB");
    if (path && *path) db_path = path;
  }
  if (db_path && !*db_path) db_path = NULL;
  main_db = db_open(db_path);
  if (!main_db) {
    main_log("failed to open database %s", db_path);
    return 1;
  }

  const char *pm_name = NULL;
  if (opt_pm) pm_name = opt_pm[1];
  else pm_name = getenv("FAP_PM");
  if (pm_name && !*pm_name) pm_name = NULL;
  main_pm = pm_get(pm_name);
  if (main_pm) {
    main_pm_id = db_add_pm(main_db, main_pm->name);
    if (!main_pm_id) main_log("failed to record package manager %s", main_pm->name);
  } else main_log("warning: failed to get package manager %s", pm_name);

  const char *name = argv[0];
  struct main_command *command = NULL;
  if (!name[0]) { main_log("invalid command"); goto on_end; }
  for (struct main_command *cmd = commands; !command && cmd->name; ++cmd) {
    if (!name[1]) {
      if (cmd->alias && name[0] == cmd->alias) command = cmd;
    }
    else if (!strcmp(name, cmd->name)) command = cmd;
  }

  if (!command) {
    main_log("unrecognized command: %s", name);
    print_help(); goto on_end;
  }

  ret = command->func(argc, argv);

  on_end:
  main_pm = NULL;
  if (main_db) sqlite3_close(main_db);
  return ret;
}

