#ifndef _IBTOP_H_
#define _IBTOP_H_
#include <stdint.h>
#include <sys/types.h>

#define P_GUID "%016"PRIx64
#define P_TRID "%016"PRIx64

struct ib_net_ent {
  uint64_t ne_guid;
  uint16_t ne_lid;
  uint8_t ne_port;
  unsigned int ne_is_hca:1;
};

/* Flags are GDBM_FLAGS.  Readers should use 0. */
void *ib_net_db_open(const char *path, int flags, mode_t mode);
int ib_net_db_store(void *db, const char *host, const struct ib_net_ent *ne);
int ib_net_db_fetch(void *db, const char *host, struct ib_net_ent *ne);
int ib_net_db_fill(void *db, FILE *file, const char *path);
void ib_net_db_close(void *db);

#endif
