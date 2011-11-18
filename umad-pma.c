#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <string.h>
#include <poll.h>
#include <time.h>
#include <infiniband/umad.h>
#include <infiniband/mad.h>
#include "trace.h"

int main(int argc, char *argv[])
{
  /* /sys/class/infiniband/HCA_NAME/ports/HCA_PORT */

  char *hca_name = "mlx4_0";
  int hca_port = 1;
  int sw_lid = 3229;
  int sw_port = 9;

  struct ibmad_port *mad_port = NULL;
  int mad_timeout = 15;
  // int mad_timeout_ms = 1000 * mad_timeout;
  // int mad_retries = 10;
  int mad_classes[] = { IB_SMI_DIRECT_CLASS, IB_PERFORMANCE_CLASS, };

  umad_debug(9);

  mad_port = mad_rpc_open_port(hca_name, hca_port, mad_classes, 2);
  if (mad_port == NULL) {
    ERROR("cannot open MAD port for HCA `%s' port %d\n", hca_name, hca_port);
    goto out;
  }

  ib_portid_t sw_port_id = {
    .lid = sw_lid,
  };

  char sw_pma[1024];
  memset(sw_pma, 0, sizeof(sw_pma));
  if (pma_query_via(sw_pma, &sw_port_id, sw_port, mad_timeout,
                    IB_GSI_PORT_COUNTERS_EXT, mad_port) == NULL) {
    ERROR("cannot query performance counters of switch LID %d, port %d: %m\n",
          sw_lid, sw_port);
    goto out;
  }

  uint64_t sw_rx_bytes, sw_rx_packets, sw_tx_bytes, sw_tx_packets;
  mad_decode_field(sw_pma, IB_PC_EXT_RCV_BYTES_F, &sw_rx_bytes);
  mad_decode_field(sw_pma, IB_PC_EXT_RCV_PKTS_F,  &sw_rx_packets);
  mad_decode_field(sw_pma, IB_PC_EXT_XMT_BYTES_F, &sw_tx_bytes);
  mad_decode_field(sw_pma, IB_PC_EXT_XMT_PKTS_F,  &sw_tx_packets);

  TRACE("sw_rx_bytes %lu, sw_rx_packets %lu, sw_tx_bytes %lu, sw_tx_packets %lu\n",
        sw_rx_bytes, sw_rx_packets, sw_tx_bytes, sw_tx_packets);

 out:
  if (mad_port != NULL)
    mad_rpc_close_port(mad_port);

  return 0;
}
