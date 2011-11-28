#include <stdio.h>
#include <stdint.h>
#include <malloc.h>
#include <ctype.h>
#include <inttypes.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "trace.h"
#include "string1.h"
#include "ibtop.h"

#define P_GUID "%016"PRIx64

int net_disc_to_info(FILE *disc_file, FILE *info_file)
{
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
    if (sscanf(line,
               "Switch %*d \"S-%"SCNx64"\" # \"%*[^\"]\" %*s port %*d lid %"SCNu16,
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
                 "[%"SCNu8"] \"H-%"SCNx64"\"[%"SCNu8"](%*x) # \"%64[^\"]\" "
                 "lid %"SCNu16" %dx%4s",
                 &sw_port, &hca_guid, &hca_port, hca_desc,
                 &hca_lid, &link_width, link_speed) != 7)
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

  return 0;
}

int make_net_info(const char *disc_cmd, const char *info_path)
{
  int rc = -1;
  char *stmp_path = NULL;
  mode_t stmp_mode = 0644;
  int stmp_fd = -1;
  FILE *stmp_file = NULL;
  FILE *disc_file = NULL;

  size_t stmp_path_size = strlen(info_path) + 10;
  stmp_path = malloc(stmp_path_size);
  if (stmp_path == NULL)
    OOM();

  snprintf(stmp_path, stmp_path_size, "%s.XXXXXXXX", info_path);

  stmp_fd = mkstemp(stmp_path);
  if (stmp_fd < 0) {
    ERROR("cannot open temporary file `%s': %m\n", stmp_path);
    goto out;
  }

  if (fchmod(stmp_fd, stmp_mode) < 0) {
    ERROR("cannot chmod `%s': %m\n", stmp_path);
    goto out;
  }

  stmp_file = fdopen(stmp_fd, "w");
  if (stmp_file == NULL) {
    ERROR("cannot open `%s': %m\n", stmp_path);
    goto out;
  }
  stmp_fd = -1;

  disc_file = popen(disc_cmd, "r");
  if (disc_file == NULL) {
    ERROR("cannot execute `%s': %m\n", disc_cmd);
    goto out;
  }

  if (net_disc_to_info(disc_file, stmp_file) < 0)
    goto out;

  rc = 0;

 out:
  if (disc_file != NULL) {
    int st = pclose(disc_file);
    if (st != 0) {
      ERROR("command `%s' exited with status %d\n", disc_cmd, st);
      rc = -1;
    }
  }

  if (stmp_file != NULL) {
    if (fclose(stmp_file) != 0) {
      ERROR("error closing `%s': %m\n", stmp_path);
      rc = -1;
    }
  }

  if (rc == 0 && rename(stmp_path, info_path) < 0) {
    ERROR("cannot rename `%s' to `%s': %m\n", stmp_path, info_path);
    rc = -1;
  }

  if (stmp_fd >= 0)
    close(stmp_fd);

  free(stmp_path);

  return rc;
}

int main(int argc, char *argv[])
{
  const char *disc_cmd = IBNETDISCOVER_PATH;
  const char *info_path = IBTOP_NET_INFO_PATH;

  if (make_net_info(disc_cmd, info_path) < 0)
    return 1;

  return 0;
}
