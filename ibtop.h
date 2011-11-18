#ifndef _IBTOP_H_
#define _IBTOP_H_
#include <stdint.h>
#include <sys/types.h>

struct ib_net_ent {
  uint64_t ca_guid;
  uint64_t sw_guid;
  uint16_t ca_lid;
  uint16_t sw_lid;
  uint8_t ca_port;
  uint8_t sw_port;
};

/* Flags are GDBM_FLAGS.  Readers should use 0. */
void *ib_net_db_open(const char *path, int flags, mode_t mode);
int ib_net_db_store(void *db, const char *ca, const struct ib_net_ent *ent);
int ib_net_db_fetch(void *db, const char *ca, struct ib_net_ent *ent);
int ib_net_db_fill(void *db, FILE *file, const char *path);
void ib_net_db_close(void *db);

#endif
