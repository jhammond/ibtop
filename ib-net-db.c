#include <malloc.h>
#include <stdint.h>
#include <ctype.h>
#include <inttypes.h>
#include <sys/types.h>
#include "trace.h"
#include "string1.h"
#include "ib-net-db.h"
#include "gdbm.h"

int ib_net_db_open(struct ib_net_db *nd, const char *path, const char *disc_cmd,
                   int flags, mode_t mode)
{
  int rc = -1;
  FILE *pfile = NULL;

  memset(nd, 0, sizeof(*nd));

  /* gdbm runs very slowly on Lustre if you allow it to use a 2M block size. */
  nd->nd_db = gdbm_open((char*) path, 4096, flags, mode, NULL /* fatal_func() */);
  if (nd->nd_db != NULL) {
    rc = 0;
    goto out;
  }

  if (disc_cmd == NULL)
    goto out;

  nd->nd_db = gdbm_open((char *) path, 4096, GDBM_NEWDB, 0666, NULL);
  if (nd->nd_db == NULL) {
    ERROR("cannot open IB net DB `%s': %s\n", path, gdbm_strerror(gdbm_errno));
    goto out;
  }

  TRACE("running `%s'\n", disc_cmd);

  pfile = popen(disc_cmd, "r");
  if (pfile == NULL) {
    ERROR("cannot execute `%s': %m\n", disc_cmd);
    goto out;
  }

  if (ib_net_db_fill(nd, pfile, "PIPE") < 0)
    goto out;

  int ps = pclose(pfile);
  pfile = NULL;
  if (ps < 0) {
    ERROR("cannot obtain termination status of `%s': %m\n", disc_cmd);
    goto out;
  }

  if (!(WIFEXITED(ps) && WEXITSTATUS(ps) == 0)) {
    ERROR("command `%s' terminated with wait status %d\n", disc_cmd, ps);
    goto out;
  }

  TRACE("command `%s' exited with status 0\n", disc_cmd);
  rc = 0;

 out:
  if (pfile != NULL)
    pclose(pfile);

  if (rc < 0)
    ib_net_db_close(nd);

  return rc;
}

int ib_net_db_store(struct ib_net_db *nd, const char *host, const struct ib_net_ent *ne)
{
  datum key = {
    .dptr = (char *) host,
    .dsize = strlen(host) + 1,
  }, val = {
    .dptr = (char *) ne,
    .dsize = sizeof(*ne),
  };

  if (gdbm_store(nd->nd_db, key, val, GDBM_REPLACE) < 0) {
    ERROR("cannot store IB net DB entry for host `%s': %s\n",
          host, gdbm_strerror(gdbm_errno));
    return -1;
  }

  return 0;
}

int ib_net_db_fetch(struct ib_net_db *nd, const char *host, struct ib_net_ent *ne)
{
  int rc = 0;

  datum key = {
    .dptr = (char *) host,
    .dsize = strlen(host) + 1,
  };

  datum val = gdbm_fetch(nd->nd_db, key);
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

int ib_net_db_fill(struct ib_net_db *nd, FILE *file, const char *path)
{
  int rc = -1;
  int file_is_ours = 0;
  char *line = NULL;
  size_t line_size = 0;

  int fastmode = 1;
  if (gdbm_setopt(nd->nd_db, GDBM_FASTMODE, &fastmode, sizeof(fastmode)) < 0)
    ERROR("cannot set db to fast mode: %s\n", gdbm_strerror(gdbm_errno));

  if (file == NULL && path == NULL)
    return -1;

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

      if (ib_net_db_store(nd, host, &ne) < 0)
        goto out;
    }
  }

  gdbm_sync(nd->nd_db); /* XXX */
  rc = 0;

 out:
  free(line);
  if (file != NULL && file_is_ours)
    fclose(file);

  return rc;
}

int ib_net_db_iter(struct ib_net_db *nd, char **name, size_t *size)
{
  datum key;

  if (*name == NULL) {
    key = gdbm_firstkey(nd->nd_db);
  } else {
    datum prev = {
      .dptr = *name,
      .dsize = *size,
    };
    key = gdbm_nextkey(nd->nd_db, prev);
    free(*name);
  }

  *name = key.dptr;
  *size = key.dsize;

  return *name != NULL;
}

void ib_net_db_close(struct ib_net_db *nd)
{
  if (nd->nd_db != NULL)
    gdbm_close(nd->nd_db);
  memset(nd, 0, sizeof(nd));
}
