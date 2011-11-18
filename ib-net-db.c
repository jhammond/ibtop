#include <malloc.h>
#include <stdint.h>
#include <ctype.h>
#include <inttypes.h>
#include <sys/types.h>
#include "trace.h"
#include "string1.h"
#include "ibtop.h"
#include "gdbm.h"

#define IB_NET_DB_PATH "ib-net.db"
#define IB_NET_DISC_OUT "ib-net-disc.out"

void *ib_net_db_open(const char *path, int flags, mode_t mode)
{
  GDBM_FILE dbf = NULL;

  if (path == NULL)
    path = IB_NET_DB_PATH;

  dbf = gdbm_open((char*) path, 0, flags, mode, NULL /* fatal_func() */);

  return dbf;
}

int ib_net_db_store(void *db, const char *name, struct ib_net_ent *ent)
{

  return 0;
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
    if (sscanf(line, "Switch %*d \"S-%"SCNx64"\" # \"%*[^\"]\" %*s port %*d lid %"SCNx16,
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

      if (ib_net_db_store(db, chop(ca_desc, ' '), &ent) < 0) {
        ERROR("cannot store net db entry for ca `%s': %m\n", ca_desc);
        goto out;
      }
    }
  }

  rc = 0;

 out:
  free(line);
  if (file != NULL && file_is_ours)
    fclose(file);

  return rc;
}


int main(int argc, char *argv[])
{
  void *db = ib_net_db_open(NULL, GDBM_WRCREAT, 0666);

  ib_net_db_fill(db, NULL, NULL);

  gdbm_close(db);

  return 0;
}
