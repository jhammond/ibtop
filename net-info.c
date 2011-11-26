#include <stdio.h>
#include <stdint.h>
#include <malloc.h>
#include <ctype.h>
#include <inttypes.h>
#include <sys/types.h>
#include "trace.h"
#include "string1.h"

#define P_GUID "%016"PRIx64
#define P_TRID "%016"PRIx64

int net_disc_to_info(FILE *disc_file, FILE *info_file)
{
  int rc = -1;
  char *line = NULL;
  size_t line_size = 0;

  /* awk -v RS="\n\n" -v ORS="\n\n" '/i115-308/' current_net.out  */
  /* vendid=0x2c9
     devid=0xb924
     sysimgguid=0x144fa5eb880051
     switchguid=0x144fa5eb880050(144fa5eb880050)
     Switch  24 "S-00144fa5eb880050" # "MT47396 Infiniscale-III Mellanox Technologies" base port 0 lid 3229 lmc 0
     [1] "H-00144fa5eb88002c"[1](144fa5eb88002d) # "i115-312 HCA-1" lid 5290 4xSDR
     ... */

  while (getline(&line, &line_size, disc_file) >= 0) {
    uint64_t sw_guid;
    uint16_t sw_lid;

    /* Scan for switch records. */
    if (sscanf(line, "Switch %*d \"S-%"SCNx64"\" # \"%*[^\"]\" %*s port %*d lid %"SCNu16,
               &sw_guid, &sw_lid) != 2)
      continue;

    TRACE("sw_guid "P_GUID", sw_lid %"PRIu16", line `%s'\n",
          sw_guid, sw_lid, chop(line, '\n'));

    /* OK, we have a switch record.  Now extract all of the HCAs. */

    while (getline(&line, &line_size, disc_file) >= 0) {
      uint64_t hca_guid;
      uint16_t hca_lid;
      uint8_t sw_port, hca_port;
      char hca_desc[64], *host;
      unsigned int use_hca = 0; /* TODO */
      int link_width;
      char link_speed[4];

      if (isspace(*line))
        break;

      /* [1] "H-00144fa5eb88002c"[1](144fa5eb88002d) # "i115-312 HCA-1" lid 5290 4xSDR */
      if (sscanf(line,
                 "[%"SCNu8"] \"H-%"SCNx64"\"[%"SCNu8"](%*x) # \"%64[^\"]\" lid %"SCNu16" %dx%4s",
                 &sw_port, &hca_guid, &hca_port, hca_desc, &hca_lid,
		 &link_width, link_speed) != 7)
        continue;

      host = chop(hca_desc, ' ');

      TRACE("sw_port %2"PRIu8", hca_guid "P_GUID", hca_port %2"PRIu8", "
            "host `%s', hca_lid %"PRIu16", link_width %d, link_speed %.4s, "
	    "line `%s'\n",
            sw_port, hca_guid, hca_port, host, hca_lid,
	    link_width, link_speed,
            chop(line, '\n'));

      fprintf(info_file, "%s %d %"PRIx64" %"PRIx16" %"PRIx8" "
	      "%"PRIx64" %"PRIx16" %"PRIx8"\n",
	      host, use_hca, hca_guid, hca_lid, hca_port,
	      sw_guid, sw_lid, sw_port);
    }
  }

  free(line);

  return rc;
}

int main(int argc, char *argv[])
{
  const char *disc_path = "IB_NET_DISC.OUT";
  FILE *disc_file = fopen(disc_path, "r");

  net_disc_to_info(disc_file, stdout);

  fclose(disc_file);

  return 0;
}
