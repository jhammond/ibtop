#include <malloc.h>
#include <stdint.h>
#include <ctype.h>
#include <inttypes.h>
#include <sys/types.h>
#include "trace.h"
#include "string1.h"
#include "ib-net-db.h"
#include "gdbm.h"

int main(int argc, char *argv[])
{
  const char *disc_path = NULL, *db_path = NULL;
  struct ib_net_db nd;

  if (argc > 1)
    disc_path = argv[1];
  if (argc > 2)
    db_path = argv[2];

  if (ib_net_db_open(&nd, db_path, GDBM_NEWDB, 0666) < 0)
    exit(EXIT_FAILURE);

  ib_net_db_fill(&nd, NULL, disc_path);

  const char *host = "i115-301";
  struct ib_net_ent ne;

  if (ib_net_db_fetch(&nd, host, &ne) > 0)
    printf("%s "
           "guid "P_GUID", lid %"PRIu16", port %2"PRIu8", is_hca %u\n",
           host, ne.ne_guid, ne.ne_lid, ne.ne_port, (unsigned) ne.ne_is_hca);
  else
    printf("%s NOT_FOUND\n", host);

  ib_net_db_close(&nd);

  return 0;
}
