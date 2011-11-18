#include <stdio.h>
#include <malloc.h>
#include <stdint.h>
#include <ctype.h>
#include <inttypes.h>
#include "trace.h"
#include "string1.h"

/* Return 0 if main() should look at *p_line, -1 otherwise. */
int parse_switch_ports(uint64_t sw_guid, int sw_lid,
                       FILE *file, char **p_line, size_t *p_line_size)
{
  /* [1] "H-00144fa5eb88002c"[1](144fa5eb88002d) # "i115-312 HCA-1" lid 5290 4xSDR */

  while (getline(p_line, p_line_size, file) >= 0) {
    int sw_port;
    uint64_t ca_guid;
    int ca_port;
    char ca_desc[64];
    int ca_lid;

    if (isspace(**p_line))
      return -1;

    if (sscanf(*p_line, "[%d] \"H-%"SCNx64"\"[%d](%*x) # \"%64[^\"]\" lid %d",
               &sw_port, &ca_guid, &ca_port, ca_desc, &ca_lid) != 5)
      continue;

    chop(*p_line, '\n');

    TRACE("sw_port %2d, ca_guid %016"PRIx64", ca_port %d, ca_desc `%s', ca_lid %d, line `%s'\n",
          sw_port, ca_guid, ca_port, ca_desc, ca_lid, *p_line);

    printf("%.64s\t%d\t%d\t%d\t%d\n", ca_desc, ca_lid, ca_port, sw_lid, sw_port);
  }

  return 0;
}

int main(int argc, char *argv[])
{
  const char *path = "/dev/stdin";
  FILE *file = NULL;
  char *line = NULL;
  size_t line_size = 0;

  file = fopen(path, "r");
  if (file == NULL) {
    ERROR("cannot open `%s': %m\n", path);
    goto out;
  }

  /* awk -v RS="\n\n" -v ORS="\n\n" '/i115-308/' current_net.out  */
  /* vendid=0x2c9
     devid=0xb924
     sysimgguid=0x144fa5eb880051
     switchguid=0x144fa5eb880050(144fa5eb880050)
     Switch  24 "S-00144fa5eb880050"        # "MT47396 Infiniscale-III Mellanox Technologies" base port 0 lid 3229 lmc 0
     ... */

  while (getline(&line, &line_size, file) >= 0) {
    uint64_t sw_guid;
    int sw_lid;

  skip_getline:
    /* Look for "Switch" records. */
    if (sscanf(line, "Switch %*d \"S-%"SCNx64"\" # \"%*[^\"]\" %*s port %*d lid %d",
               &sw_guid, &sw_lid) != 2)
      continue;

    chop(line, '\n');

    TRACE("sw_guid %016"PRIx64", sw_lid %d, line `%s'\n", sw_guid, sw_lid, line);

    if (parse_switch_ports(sw_guid, sw_lid, file, &line, &line_size) == 0)
      goto skip_getline;
  }

 out:
  free(line);
  if (file != NULL)
    fclose(file);

  return 0;
}
