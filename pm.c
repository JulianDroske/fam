#include "pm.h"

#include "stdio.h"
#include "stdlib.h"
#include "stddef.h"
#include "stdarg.h"
#include "string.h"

#include "unistd.h"

#include "sys/wait.h"

void pm_trim(char *str) {
  if (!str || !*str) return;
  int len = strlen(str);
  while (len && str[len - 1] == '\n') str[--len] = 0;
}

struct pm_exec {
  int fd; pid_t pid;
  char buf[4096]; ssize_t pos, len;
  int inited, eof; char ch;
};

int pm_exec_next(struct pm_exec *e, char *ch) {
  if (e->eof || e->len <= 0 && e->inited) return 0;
  e->inited = 1;
  if (ch) *ch = e->ch;

  if (e->pos >= e->len) {
    e->pos = 0; e->len = read(e->fd, e->buf, sizeof(e->buf));
  }
  if (e->pos < e->len) { e->ch = e->buf[e->pos++]; return 1; }
  if (!e->eof) { e->eof = 1; return 1; }
  return 0;
}

int pm_exec_open(struct pm_exec *e, const char *cmd, const char **argv) {
  int link[2] = {};
  if (pipe(link) < 0) return 0;
  pid_t pid = fork();
  if (pid < 0) {
    close(link[0]); close(link[1]);
    return 0;
  }

  if (pid) {
    close(link[1]);
    e->fd = link[0]; e->pid = pid;
    e->pos = e->len = 0;
    e->inited = e->eof = 0; e->ch = 0;
  }
  else {
    dup2(link[1], STDOUT_FILENO);
    close(link[0]); close(link[1]);
    /* https://pubs.opengroup.org/onlinepubs/9699919799/functions/exec.html */
    execvp(cmd, (char *const*) argv);
    perror("execvp"); exit(1);
  }

  pm_exec_next(e, NULL);
  return 1;
}

int pm_exec_char_space(char ch) {
  return ch == ' ' || ch == '\n' || ch == '\r';
}

size_t pm_exec_readfield(struct pm_exec *e, char *dest, size_t len) {
  while (pm_exec_char_space(e->ch))
    if (!pm_exec_next(e, NULL)) break;
  size_t pos = 0;
  while (pos < len && !pm_exec_char_space(e->ch)) {
    *dest++ = e->ch; ++pos;
    if (!pm_exec_next(e, NULL)) break;
  }
  return pos;
}

int pm_exec_close(struct pm_exec *e) {
  int ret = 1;
  waitpid(e->pid, &ret, 0);
  close(e->fd);
  return !ret;
}

int pm_readexecv(char *buf, size_t size, const char *cmd, const char **argv) {
  struct pm_exec e = {};
  int len = 0; int ret = 1;
  if (!pm_exec_open(&e, cmd, argv)) return 0;
  if (buf && size) {
    len = read(e.fd, buf, size); buf[len] = 0;
  }
  return pm_exec_close(&e);
}

int pm_readexec(char *buf, size_t size, const char *cmd, ...) {
  static const char *argv[16] = {};
  int argv_len = 0;

  va_list args; va_start(args, cmd);
  const char *arg = NULL;
  while ((arg = va_arg(args, const char *)) != NULL) {
    argv[argv_len] = arg;
    if (++argv_len >= 15) break;
  }
  argv[argv_len] = NULL;
  va_end(args);

  return pm_readexecv(buf, size, cmd, argv);
}

int pm_systemf(const char *format, ...) {
  static char cmdline[256];
  va_list args; va_start(args, format);
  vsnprintf(cmdline, sizeof(cmdline), format, args);
  va_end(args);
  return !system(cmdline);
}

char *pm_copy_tinymap_keys(char **argv, struct tinymap *map) {
  size_t keys_size = map->keys_len + map->nodes_len;
  char *buffer = (char *) malloc(keys_size);
  if (!buffer) { perror("pm_copy_tinymap_keys: malloc"); return 0; }
  struct tinymap_node *node = NULL;
  for (char *b = buffer; tinymap_enum(map, &node); b += node->key_len + 1) {
    strncpy(b, tinymap_node_key(map, node), node->key_len);
    *argv++ = b;
  }
  return buffer;
}

/* apk */

int pm_apk_avail() { return !system("command -v apk > /dev/null 2>&1;"); }

int pm_apk_queryfile(char *dest_pkg_name, const char *filepath) {
  if (!pm_readexec(dest_pkg_name, PM_NAME_MAX, "apk", "apk", "info", "-qW", filepath, NULL))
    return 0;
  pm_trim(dest_pkg_name);
  return 1;
}

int pm_apk_checkpkg(int *exist, const char *name) {
  static char output[PM_NAME_MAX];
  if (!pm_readexec(output, sizeof(output), "apk", "apk", "info", "--installed", name, NULL))
    return 0;
  pm_trim(output); *exist = !strcmp(output, name);
  return 1;
}

int pm_apk_checkpkgs(struct tinymap *map) {
  static const char *pre[] = { "apk", "info", "--installed", NULL };

  size_t len = tinymap_len(map) + sizeof(pre) / sizeof(pre[0]);
  const char **args = (const char **) malloc(len * sizeof(char *));
  if (!args) return 0;
  const char **a = args;
  for (const char **p = pre; *p; ++p) *a++ = *p;
  char *mem = pm_copy_tinymap_keys((char **) a, map);
  if (!mem) { free(args); return 0; }
  a += tinymap_len(map);
  *a = NULL;

  struct pm_exec e = {};
  int status = pm_exec_open(&e, "apk", args);
  free(args); free(mem);
  if (!status) return 0;

  char buf[PM_NAME_MAX + 1];
  for (size_t len = 0; (len = pm_exec_readfield(&e, buf, PM_NAME_MAX)) > 0; len = 0) {
    struct tinymap_node *node = NULL;
    if (!tinymap_find(map, &node, buf, len)) {
      fprintf(stderr, "failed to find pkg %*.*s in map\n", (int) len, (int) len, buf);
      break;
    }
    node->value |= PM_TINYMAP_PKG_EXIST;
  }
  return pm_exec_close(&e);
}

/* dpkg */

int pm_dpkg_avail() { return !system("command -v dpkg > /dev/null 2>&1;"); }

int pm_dpkg_queryfile(char *dest_pkg_name, const char *filepath) {
  if (!pm_readexec(dest_pkg_name, PM_NAME_MAX, "dpkg", "dpkg", "-S", filepath, NULL))
    return 0;
  char *colon = strchr(dest_pkg_name, ':');
  if (!colon) return 0;
  *colon = 0;
  return 1;
}

int pm_dpkg_checkpkg(int *exist, const char *name) {
  return pm_readexec(NULL, 0, "dpkg", "dpkg", "-s", name, NULL);
}

/* pm */

struct pm pm_backends[] = {
  { "apk",
    pm_apk_avail, pm_apk_queryfile,
    pm_apk_checkpkg, pm_apk_checkpkgs },
  { "dpkg",
    pm_dpkg_avail, pm_dpkg_queryfile,
    pm_dpkg_checkpkg },
  { NULL },
};

struct pm *pm_autoget() {
  for (struct pm *pm = pm_backends; pm->name; ++pm) {
    if (pm->avail()) return pm;
  }
  return NULL;
}

struct pm *pm_get(const char *type) {
  if (!type) return pm_autoget();
  for (struct pm *pm = pm_backends; pm->name; ++pm) {
    if (!strcmp(pm->name, type)) return pm;
  }
  return NULL;
}

