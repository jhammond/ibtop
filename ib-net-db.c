#include <malloc.h>
#include <stdint.h>
#include <ctype.h>
#include <inttypes.h>
#include <sys/types.h>
#include "trace.h"
#include "string1.h"
#include "ibtop.h"
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

int ib_net_db_store(void *db, const char *ca, const struct ib_net_ent *ent)
{
  datum key = {
    .dptr = (char *) ca,
    .dsize = strlen(ca) + 1,
  }, val = {
    .dptr = (char *) ent,
    .dsize = sizeof(*ent),
  };

  if (gdbm_store(db, key, val, GDBM_REPLACE) < 0) {
    ERROR("cannot store IB net DB entry for CA `%s': %s\n", ca, gdbm_strerror(gdbm_errno));
    return -1;
  }

  return 0;
}

int ib_net_db_fetch(void *db, const char *ca, struct ib_net_ent *ent)
{
  int rc = 0;

  datum key = {
    .dptr = (char *) ca,
    .dsize = strlen(ca) + 1,
  };

  datum val = gdbm_fetch(db, key);
  if (val.dptr == NULL)
    goto out; /* Not found. */

  if (val.dsize != sizeof(*ent))
    FATAL("bad DB entry for CA `%s' size %zu, expected %zu\n",
          ca, (size_t) val.dsize, sizeof(*ent));

  memcpy(ent, val.dptr, sizeof(*ent));
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
    struct ib_net_ent ent;

    /* Scan for switch records. */
    if (sscanf(line, "Switch %*d \"S-%"SCNx64"\" # \"%*[^\"]\" %*s port %*d lid %"SCNu16,
               &ent.sw_guid, &ent.sw_lid) != 2)
      continue;

    TRACE("sw_guid %016"PRIx64", sw_lid %"PRIu16", line `%s'\n",
          ent.sw_guid, ent.sw_lid, chop(line, '\n'));

    /* OK, we have a switch record. */

    while (getline(&line, &line_size, file) >= 0) {
      char ca_desc[64];

      if (isspace(*line))
        break;

      if (sscanf(line,
                 "[%"SCNu8"] \"H-%"SCNx64"\"[%"SCNu8"](%*x) # \"%64[^\"]\" lid %"SCNu16,
                 &ent.sw_port, &ent.ca_guid, &ent.ca_port, ca_desc, &ent.ca_lid) != 5)
        continue;

      TRACE("sw_port %2"PRIu8", ca_guid %016"PRIx64", ca_port %2"PRIu8", "
            "ca_desc `%s', ca_lid %"PRIu16", line `%s'\n",
            ent.sw_port, ent.ca_guid, ent.ca_port, ca_desc, ent.ca_lid,
            chop(line, '\n'));

      if (ib_net_db_store(db, chop(ca_desc, ' '), &ent) < 0)
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


  const char *ca = "i115-301";
  struct ib_net_ent ent;

  if (ib_net_db_fetch(db, ca, &ent) > 0)
    printf("%s "
           "sw_guid %016"PRIx64", sw_lid %"PRIu16", sw_port %2"PRIu8", "
           "ca_guid %016"PRIx64", ca_lid %"PRIu16", ca_port %2"PRIu8"\n",
           ca,
           ent.sw_guid, ent.sw_lid, ent.sw_port,
           ent.ca_guid, ent.ca_lid, ent.ca_port);
  else
    printf("%s NOT_FOUND\n", ca);

  gdbm_close(db);

  return 0;
}
