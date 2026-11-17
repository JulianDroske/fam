#ifndef _PM_H_
#define _PM_H_

#include "stddef.h"
#include "limits.h"

#include "lib/tinymap.h"

#define PM_NAME_MAX 256

enum pm_tinymap_value_mask {
  PM_TINYMAP_RESERVED_MASK = UINT_MAX,
  PM_TINYMAP_PKG_EXIST = (long int) UINT_MAX + 1,
};

struct pm {
  const char *name;
  int (*avail)();
  int (*queryfile)(char *dest_pkg_name, const char *file);
  int (*checkpkg)(int *exist, const char *name);
  int (*checkpkgs)(struct tinymap *exist_map);
};

struct pm *pm_autoget();
struct pm *pm_get(const char *type);

#endif

