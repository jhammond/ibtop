#include <malloc.h>
#include <stdint.h>
#include <ctype.h>
#include <inttypes.h>
#include <sys/types.h>
#include "trace.h"
#include "string1.h"
#include "ib-net-db.h"
#include "gdbm.h"

#define IB_NET_DISC_OUT "IB_NET_DISC.OUT"
#define IB_NET_DB_PATH "IB_NET.DB"

void *ib_net_db_open(const char *path, int flags, mode_t mode)
{
  void *db = NULL;

  if (path == NULL)
    path = IB_NET_DB_PATH;

  /* gdbm runs very slowly on Lustre if you allow it to use a 2M block size. */
  db = gdbm_open((char*) path, 4096, flags, mode, NULL /* fatal_func() */);
  if (db == NULL)
    ERROR("cannot open IB net DB `%s': %s\n", path, gdbm_strerror(gdbm_errno));

  return db;
}

int ib_net_db_store(void *db, const char *host, const struct ib_net_ent *ne)
{
  datum key = {
    .dptr = (char *) host,
    .dsize = strlen(host) + 1,
  }, val = {
    .dptr = (char *) ne,
    .dsize = sizeof(*ne),
  };

  if (gdbm_store(db, key, val, GDBM_REPLACE) < 0) {
    ERROR("cannot store IB net DB entry for host `%s': %s\n",
          host, gdbm_strerror(gdbm_errno));
    return -1;
  }

  return 0;
}

int ib_net_db_fetch(void *db, const char *host, struct ib_net_ent *ne)
{
  int rc = 0;

  datum key = {
    .dptr = (char *) host,
    .dsize = strlen(host) + 1,
  };

  datum val = gdbm_fetch(db, key);
  if (val.dptr == NULL) {
    TRACE("no IB net DB entry for host `%s'\n", host);
    goto out;
  }

  if (val.dsize != sizeof(*ne))
    FATAL("bad IB net DB entry for host `%s' size %zu, expected %zu\n",
          host, (size_t) val.dsize, sizeof(*ne));

  memcpy(ne, val.dptr, sizeof(*ne));
  rc = 1;
 out:
  free(val.dptr);

  return rc;
}

/* awk -v RS="\n\n" -v ORS="\n\n" '/i115-308/' current_net.out  */
/* vendid=0x2c9
   devid=0xb924
   sysimgguid=0x144fa5eb880051
   switchguid=0x144fa5eb880050(144fa5eb880050)
   Switch  24 "S-00144fa5eb880050"        # "MT47396 Infiniscale-III Mellanox Technologies" base port 0 lid 3229 lmc 0
   [1] "H-00144fa5eb88002c"[1](144fa5eb88002d) # "i115-312 HCA-1" lid 5290 4xSDR
   ... */

int ib_net_db_fill(void *db, FILE *file, const char *path)
{
  int rc = -1;
  int file_is_ours = 0;
  char *line = NULL;
  size_t line_size = 0;

  if (file == NULL && path == NULL)
    path = IB_NET_DISC_OUT;

  if (path == NULL)
    path = "-";

  if (file == NULL) {
    file = fopen(path, "r");
    file_is_ours = 1;
  }

  if (file == NULL) {
    ERROR("cannot open `%s': %m\n", path);
    goto out;
  }

  while (getline(&line, &line_size, file) >= 0) {
    uint64_t sw_guid;
    uint16_t sw_lid;

    /* Scan for switch records. */
    if (sscanf(line, "Switch %*d \"S-%"SCNx64"\" # \"%*[^\"]\" %*s port %*d lid %"SCNu16,
               &sw_guid, &sw_lid) != 2)
      continue;

    TRACE("sw_guid "P_GUID", sw_lid %"PRIu16", line `%s'\n",
          sw_guid, sw_lid, chop(line, '\n'));

    /* OK, we have a switch record. */

    while (getline(&line, &line_size, file) >= 0) {
      uint64_t hca_guid;
      uint16_t hca_lid;
      uint8_t sw_port, hca_port;
      char hca_desc[64], *host;
      unsigned int use_hca = 0; /* TODO */

      if (isspace(*line))
        break;

      /* [1] "H-00144fa5eb88002c"[1](144fa5eb88002d) # "i115-312 HCA-1" lid 5290 4xSDR */
      if (sscanf(line,
                 "[%"SCNu8"] \"H-%"SCNx64"\"[%"SCNu8"](%*x) # \"%64[^\"]\" lid %"SCNu16,
                 &sw_port, &hca_guid, &hca_port, hca_desc, &hca_lid) != 5)
        continue;

      host = chop(hca_desc, ' ');

      TRACE("sw_port %2"PRIu8", hca_guid "P_GUID", hca_port %2"PRIu8", "
            "host `%s', hca_lid %"PRIu16", line `%s'\n",
            sw_port, hca_guid, hca_port, host, hca_lid,
            chop(line, '\n'));

      struct ib_net_ent ne = {
        .ne_guid = use_hca ? hca_guid : sw_guid,
        .ne_lid = use_hca ? hca_lid : sw_lid,
        .ne_port = use_hca ? hca_port : sw_port,
        .ne_is_hca = use_hca,
      };

      if (ib_net_db_store(db, host, &ne) < 0)
        goto out;
    }
  }

  rc = 0;

 out:
  free(line);
  if (file != NULL && file_is_ours)
    fclose(file);

  return rc;
}

void ib_net_db_close(void *db)
{
  gdbm_close(db);
}

#ifdef IB_NET_DB_TEST
int main(int argc, char *argv[])
{
  const char *disc_path = NULL, *db_path = NULL;

  if (argc > 1)
    disc_path = argv[1];
  if (argc > 2)
    db_path = argv[2];

  void *db = ib_net_db_open(db_path, GDBM_NEWDB, 0666);
  if (db == NULL)
    exit(EXIT_FAILURE);

  int fastmode = 1;
  if (gdbm_setopt(db, GDBM_FASTMODE, &fastmode, sizeof(fastmode)) < 0)
    ERROR("cannot set db to fast mode: %s\n", gdbm_strerror(gdbm_errno));

  ib_net_db_fill(db, NULL, disc_path);

  gdbm_sync(db);

  const char *host = "i115-301";
  struct ib_net_ent ne;

  if (ib_net_db_fetch(db, host, &ne) > 0)
    printf("%s "
           "guid "P_GUID", lid %"PRIu16", port %2"PRIu8", is_hca %u\n",
           host, ne.ne_guid, ne.ne_lid, ne.ne_port, (unsigned) ne.ne_is_hca);
  else
    printf("%s NOT_FOUND\n", host);

  gdbm_close(db);

  return 0;
}
#endif
