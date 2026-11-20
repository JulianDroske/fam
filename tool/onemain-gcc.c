/* compile with -lsqlite3 */

#define _GNU_SOURCE 1

#include "../lib/tinyradix.h"
#include "../lib/tinymap.h"
#include "../lib/tinycli.h"
#include "../lib/pstrack.h"
#include "../lib/fatrack.h"
#include "../pm.h"

[[gnu::format(printf, 2, 3)]]
int fatrack_readlinkf(char *dest, const char *format, ...);
[[gnu::format(printf, 2, 3)]]
int fatrack_realpathf(char *dest, const char *format, ...);
[[gnu::format(printf, 2, 3)]]
void fatrack_seterrorf(struct fatrack *fatrack, const char *format, ...);
[[gnu::format(printf, 2, 3)]]
int pstrack_realpathf(char *dest, const char *format, ...);
[[gnu::format(printf, 1, 2)]]
int pm_systemf(const char *format, ...);
[[gnu::format(printf, 1, 2)]]
void db_logerror(const char *format, ...);
[[gnu::format(printf, 1, 2)]]
void main_log(const char *format, ...);

#include "../lib/tinyradix.c"
#include "../lib/tinymap.c"
#include "../lib/tinycli.c"
#include "../lib/pstrack.c"
#include "../lib/fatrack.c"

#include "../pm.c"
#include "../main.c"

