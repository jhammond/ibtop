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
  const char *hca_name = "mlx4_0";
  int hca_port = 1;
  int sw_lid = -1, sw_port = -1;

  struct ibmad_port *mad_port = NULL;
  int mad_fd = -1;
  int mad_timeout = 15;
  int mad_classes[] = { IB_PERFORMANCE_CLASS, };
  int nr_mad_classes = sizeof(mad_classes) / sizeof(mad_classes[0]);
  int mad_timeout_ms = 1000 * mad_timeout;

  /* /sys/class/infiniband/HCA_NAME/ports/HCA_PORT */

  mad_port = mad_rpc_open_port(hca_name, hca_port, mad_classes, nr_mad_classes);
  if (mad_port == NULL) {
    ERROR("cannot open MAD port for HCA `%s' port %d\n", hca_name, hca_port);
    goto out;
  }

  mad_fd = mad_port->port_id;

#ifdef CALL_PMA_QUERY_VIA
  ib_portid_t sw_id = {
    .lid = sw_lid
    .qp = 1,
    .qkey = IB_DEFAULT_QP1_QKEY,
  };

  char sw_pma_buf[1024];

  if (pma_query_via(sw_pma_buf, &sw_id, sw_port, mad_timeout,
		    IB_GSI_PORT_COUNTERS_EXT, mad_port) == NULL) {
    ERROR("cannot query performance counters of switch LID %d, port %d: %m\n", sw_lid, sw_port);
    goto out;
  }
#endif

#ifdef CALL_MAD_RPC
  ib_rpc_t sw_pma_rpc = {
    .mgtclass = IB_PERFORMANCE_CLASS,
    .method = IB_MAD_METHOD_GET,
    .attr = {
      .id = IB_GSI_PORT_COUNTERS_EXT,
      .mod = 0,
    },
    .timeout = mad_timeout,
    .datasz = IB_PC_DATA_SZ,
    .dataoffs = IB_PC_DATA_OFFS,
  };

  mad_set_field(sw_pma_buf, 0, IB_PC_PORT_SELECT_F, sw_port);
  if (mad_rpc(mad_port, &sw_pma_rpc, &sw_id, sw_pma_buf, sw_pma_buf) == NULL) {
    /* ... */
  }
#endif

  uint64_t s_trid = 0x1010cafebabe8008;

  char s_buf[1024], r_buf[1024];
  memset(s_buf, 0, sizeof(s_buf));
  memset(r_buf, 0, sizeof(r_buf));

  struct ib_user_mad *su = (struct ib_user_mad *) s_buf;
  su->agent_id = mad_port->class_agents[IB_PERFORMANCE_CLASS];
  su->timeout_ms = mad_timeout_ms;
  su->retries = mad_retries;
  /* length? */
  su->addr.qpn = 1;
  su->addr.qkey = IB_DEFAULT_QP1_QKEY;
  su->addr.lid = sw_lid;

  void *sm = umad_get_mad(su);
  /* char *p = mad_encode(mad, &sw_pma_rpc, 0, sw_pma_buf); */

  mad_set_field(sm, 0, IB_MAD_METHOD_F, IB_MAD_METHOD_GET);
  mad_set_field(sm, 0, IB_MAD_RESPONSE_F, 0);
  mad_set_field(sm, 0, IB_MAD_CLASSVER_F, 2);
  mad_set_field(sm, 0, IB_MAD_MGMTCLASS_F, IB_PERFORMANCE_CLASS);
  mad_set_field(sm, 0, IB_MAD_BASEVER_F, 1);

  mad_set_field(sm, 0, IB_MAD_STATUS_F, 0 /* rpc->rstatus */);
  mad_set_field64(sm, 0, IB_MAD_TRID_F, s_trid);
  mad_set_field(sm, 0, IB_MAD_ATTRID_F, IB_GSI_PORT_COUNTERS_EXT);
  mad_set_field(sm, 0, IB_MAD_ATTRMOD_F, 0 /* rpc->attr.mod */);
  mad_set_field64(sm, 0, IB_MAD_MKEY_F, 0 /* rpc->mkey */);

  void *s_data = (char *) sm + IB_PC_DATA_OFFS; 
  mad_set_field(s_data, 0, IB_PC_PORT_SELECT_F, sw_port);

  /* memcpy(mad + IB_PC_DATA_OFFS, sw_pma_buf, IB_PC_DATA_SZ); */
  /* p = mda + IB_MAD_SIZE. */
  /* len = p - mad; */

  // ns = _do_madrpc(mad_port->port_id, umad_snd_buf, umad_rcv_buf,
  // mad_port->class_agents[IB_PERFORMANCE_CLASS],
  // mad_len, mad_timeout, mad_retries);

  /* BEGIN umad_send() */
  // ((struct ib_user_mad *) umad)->timeout_ms = mad_timeout_ms;
  // ((struct ib_user_mad *) umad)->retries = mad_retries;
  // ((struct ib_user_mad *) umad)->agent_id = mad_agent_id;

  ssize_t ns = write(mad_fd, su, umad_size() + IB_MAD_SIZE);
  if (ns < 0) {
    ERROR("cannot send mad: %m\n");
    goto out;
  }

  struct pollfd fds = {
    .fd = mad_fd, 
    .events = POLLIN,
  };

  struct timespec poll_ts = {
    .tv_sec = timeout,
  };

  while (1) {
    int np = ppoll(&fds, 1, &poll_ts, NULL);
    if (np < 0) {
      ERROR("error polling for mads: %m\n");
      goto out;
    } else if (np == 0) {
      ERROR("timedout waiting for mad\n");
      goto out;
    }

    ssize_t nr = read(mad_fd, r_buf, sizeof(r_buf));
    if (nr < 0) {
      if (errno == EWOULDBLOCK)
	continue;
      ERROR("error receiving mad: %m\n");
      goto out;
    }
  }

  struct ib_user_mad *ru = (struct ib_user_mad *) r_buf;
  TRACE("ru status %d\n", ru->status);
  void *rm = umad_get_mad(ru);
  uint64_t r_trid = mad_get_field64(rm, 0, IB_MAD_TRID_F);
  TRACE("r_trid %lu\n", (unsigned long) r_trid);


  if (mad_get_field(rm, 0, IB_DRSMP_STATUS_F) == IB_MAD_STS_REDIRECT) {
    ERROR("received redirect\n");
    goto out;
  }

  void *r_data = (char *) rm + IB_PC_DATA_OFFS;

  uint64_t sw_rx_bytes, sw_rx_packets, sw_tx_bytes, sw_tx_packets;
  mad_decode_field(r_data, IB_PC_EXT_RCV_BYTES_F, &sw_rx_bytes);
  mad_decode_field(r_data, IB_PC_EXT_RCV_PKTS_F,  &sw_rx_packets);
  mad_decode_field(r_data, IB_PC_EXT_XMT_BYTES_F, &sw_tx_bytes);
  mad_decode_field(r_data, IB_PC_EXT_XMT_PKTS_F,  &sw_tx_packets);

  TRACE("sw_rx_bytes %lu, sw_rx_packets %lu, sw_tx_bytes %lu, sw_tx_packets %lu\n",
        sw_rx_bytes, sw_rx_packets, sw_tx_bytes, sw_tx_packets);

 out:
  if (mad_port != NULL)
    mad_rpc_close_port(mad_port);

  return 0;
}
