#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <getopt.h>
#include <malloc.h>
#include <infiniband/umad.h>
#include <infiniband/mad.h>
#include <stdint.h>
#include "ib-net-db.h"
#include "string1.h"
#include "trace.h"

#define TRID_BASE 0xE1F2A3B4C5D6E7F8

static inline double dnow(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_REALTIME, &ts);

  return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* /sys/class/infiniband/HCA_NAME/ports/HCA_PORT */
char *hca_name = "mlx4_0";
int hca_port = 1;

int umad_fd = -1;
int umad_agent_id = -1;
int umad_timeout_ms = 15;
int umad_retries = 10;

struct host_ent {
  uint64_t h_rx_b;
  uint64_t h_rx_p;
  uint64_t h_tx_b;
  uint64_t h_tx_p;
  uint64_t h_trid;
  struct ib_net_ent h_ne;
  char h_name[];
};

struct host_ent *host_ent(struct ib_net_db *nd, const char *name, size_t i)
{
  struct host_ent *h;

  h = malloc(sizeof(*h) + strlen(name) + 1);
  memset(h, 0, sizeof(*h));
  strcpy(h->h_name, name);

  h->h_trid = TRID_BASE + i;

  if (ib_net_db_fetch(nd, h->h_name, &h->h_ne) <= 0)
    goto err;

  return h;

 err:
  free(h);
  return NULL;
}

static inline void dump_umad(void *um, size_t len)
{
#ifdef DEBUG
  unsigned char *p = um;
  unsigned int i, j;

  TRACE("umad dump, len %zu\n", len);

  for (i = 0; i < len; i += 16) {
    fprintf(stderr, "%4u", i);
    for (j = i; j < i + 16 && j < len; j++) {
      if (p[j] != 0)
        fprintf(stderr, " %02hhx", p[j]);
      else
        fprintf(stderr, " --");
    }
    fprintf(stderr, "\n");
  }
  fprintf(stderr, "\n");

#endif
}

int host_send_perf_umad(struct host_ent *h)
{
  char buf[1024];
  struct ib_user_mad *um;
  size_t um_size = umad_size() + IB_MAD_SIZE;
  void *m;

  memset(buf, 0, sizeof(buf));

  um = (struct ib_user_mad *) buf;
  umad_set_addr(um, h->h_ne.ne_lid, 1, 0, IB_DEFAULT_QP1_QKEY);

  um->agent_id   = umad_agent_id;
  um->timeout_ms = umad_timeout_ms;
  um->retries    = umad_retries;

  m = umad_get_mad(um);
  mad_set_field(m, 0, IB_MAD_METHOD_F, IB_MAD_METHOD_GET);
  /* mad_set_field(m, 0, IB_MAD_RESPONSE_F, 0); */
  /* mad_set_field(m, 0, IB_MAD_STATUS_F, 0 ); *//* rpc->rstatus */
  mad_set_field(m, 0, IB_MAD_CLASSVER_F, 1);
  mad_set_field(m, 0, IB_MAD_MGMTCLASS_F, IB_PERFORMANCE_CLASS);
  mad_set_field(m, 0, IB_MAD_BASEVER_F, 1);
  mad_set_field(m, 0, IB_MAD_ATTRID_F, IB_GSI_PORT_COUNTERS_EXT);
  /* mad_set_field(m, 0, IB_MAD_ATTRMOD_F, 0); *//* rpc->attr.mod */
  /* mad_set_field64(m, 0, IB_MAD_MKEY_F, 0); *//* rpc->mkey */

  mad_set_field64(m, 0, IB_MAD_TRID_F, h->h_trid);

  void *pc = (char *) m + IB_PC_DATA_OFFS;
  mad_set_field(pc, 0, IB_PC_PORT_SELECT_F, h->h_ne.ne_port);

  TRACE("sending perf umad for host `%s', "
        "lid %"PRIx16", port %"PRIx8", is_hca %u, trid "P_TRID"\n",
        h->h_name, h->h_ne.ne_lid, h->h_ne.ne_port,
        (unsigned int) h->h_ne.ne_is_hca, h->h_trid);

  dump_umad(um, um_size);

  ssize_t nw = write(umad_fd, um, um_size);
  if (nw < 0) {
    ERROR("error sending umad for host `%s': %m\n", h->h_name);
    return -1;
  } else if (nw < um_size) {
    /* ... */
  }

  return 0;
}

int recv_response_umad(int which, struct host_ent **host_list, size_t nr_hosts)
{
  char buf[1024];
  size_t um_size = umad_size() + IB_MAD_SIZE;
  /* memset(buf, 0, sizeof(buf)); */

  ssize_t nr = read(umad_fd, buf, sizeof(buf));
  if (nr < 0) {
    if (errno != EWOULDBLOCK)
      ERROR("error receiving mad: %m\n");
    return -1;
  } else if (nr < um_size) {
    ERROR("short receive, expected %zu, only read %zd\n", um_size, nr);
    return -1;
  }

  struct ib_user_mad *um = (struct ib_user_mad *) buf;
  void *m = umad_get_mad(um);
  uint64_t trid = mad_get_field64(m, 0, IB_MAD_TRID_F);

  TRACE("received umad trid "P_TRID", status %d\n", trid, um->status);

  dump_umad(buf, nr);

  if (mad_get_field(m, 0, IB_DRSMP_STATUS_F) == IB_MAD_STS_REDIRECT) {
    /* FIXME */
    ERROR("received redirect, trid "P_TRID"\n", trid);
    return -1;
  }

  size_t i = (uint32_t) (trid - TRID_BASE);
  TRACE("i %zu\n", i);

  if (!(0 <= i && i < nr_hosts)) {
    ERROR("bad trid "P_TRID" in received umad\n", trid);
    return -1;
  }

  struct host_ent *h = host_list[i];
  if (h == NULL) {
    ERROR("no host for umad, trid "P_TRID"\n", trid);
    return -1;
  }

  unsigned int is_hca = h->h_ne.ne_is_hca;

  TRACE("host `%s', lid %"PRIx16", port %"PRIx8", is_hca %u\n",
        h->h_name, h->h_ne.ne_lid, h->h_ne.ne_port, is_hca);

  void *pc = (char *) m + IB_PC_DATA_OFFS;
  uint64_t rx_b, rx_p, tx_b, tx_p;

  mad_decode_field(pc, IB_PC_EXT_RCV_BYTES_F, is_hca ? &rx_b : &tx_b);
  mad_decode_field(pc, IB_PC_EXT_RCV_PKTS_F,  is_hca ? &rx_p : &tx_p);
  mad_decode_field(pc, IB_PC_EXT_XMT_BYTES_F, is_hca ? &tx_b : &rx_b);
  mad_decode_field(pc, IB_PC_EXT_XMT_PKTS_F,  is_hca ? &tx_p : &rx_p);

  TRACE("rx_b %"PRIx64", rx_p %"PRIx64", tx_b %"PRIx64", tx_p %"PRIx64"\n",
        rx_b, rx_p, tx_b, tx_p);

  h->h_rx_b += which == 0 ? -rx_b : rx_b;
  h->h_rx_p += which == 0 ? -rx_p : rx_p;
  h->h_tx_b += which == 0 ? -tx_b : tx_b;
  h->h_tx_p += which == 0 ? -tx_p : tx_p;

  return 0;
}

int main(int argc, char *argv[])
{
  struct host_ent **host_list = NULL;
  size_t nr_hosts = 0, i;
  double interval = 10;
  struct ib_net_db nd;

  struct option opts[] = {
    { "interval", 1, NULL, 'i' },
    { NULL, 0, NULL, 0},
  };

  int c;
  while ((c = getopt_long(argc, argv, "i:", opts, 0)) != -1) {
    switch (c) {
    case 'i':
      interval = strtod(optarg, NULL);
      if (interval <= 0)
        FATAL("invalid interval `%s'\n", optarg);
      break;
    case '?':
      fprintf(stderr, "Try `%s --help' for more information.",
              program_invocation_short_name);
      exit(EXIT_FAILURE);
    }
  }

  if (ib_net_db_open(&nd, NULL, 0, 0) < 0)
    FATAL("cannot open IB net DB\n");

  if (argc > optind) {
    size_t nr_host_args = argc - optind;
    host_list = calloc(nr_host_args, sizeof(host_list[0]));

    for (i = 0; i < nr_host_args; i++) {
      char *name = argv[optind + i];
      struct host_ent *h = host_ent(&nd, name, nr_hosts);

      if (h == NULL) {
        ERROR("unknown host `%s'\n", name);
        continue;
      }

      host_list[nr_hosts++] = h;
    }

    if (nr_hosts == 0)
      FATAL("no good hosts out of %zu\n", nr_host_args);
  } else {
    char *name = NULL;
    size_t name_size = 0;

    host_list = calloc(nd.nd_count, sizeof(host_list[0]));

    while (nr_hosts < nd.nd_count) {
      ib_net_db_iter(&nd, &name, &name_size);
      if (name == NULL)
        break;

      struct host_ent *h = host_ent(&nd, name, nr_hosts);

      if (h == NULL) {
        ERROR("unknown host `%s'\n", name);
        continue;
      }

      host_list[nr_hosts++] = h;
    }

    free(name);
  }

  ib_net_db_close(&nd);

#ifdef DEBUG
  umad_debug(9);
#endif

  if (umad_init() < 0)
    FATAL("cannot init libibumad: %m\n");

  umad_fd = umad_open_port(hca_name, hca_port);
  if (umad_fd < 0)
    FATAL("cannot open umad port: %m\n");

  umad_agent_id = umad_register(umad_fd, IB_PERFORMANCE_CLASS, 1, 0, 0);
  if (umad_agent_id < 0)
    FATAL("cannot register umad agent: %m\n");

  double start[2];
  double deadline[2];
  deadline[0] = dnow() + interval;
  deadline[1] = deadline[0] + 1;

  int which;
  for (which = 0; which < 2; which++) {

    size_t nr_sent = 0, nr_responses = 0;

    start[which] = dnow();

    for (i = 0; i < nr_hosts; i++) {
      if (host_list[i] == NULL)
        continue;
      if (host_send_perf_umad(host_list[i]) < 0)
        continue;
      nr_sent++;
    }

    TRACE("sent %zu in %f seconds\n", nr_sent, dnow() - start[which]);

    while (1) {
      int poll_timeout_ms = 1000 * (deadline[which] - dnow());
      struct pollfd poll_fds = {
        .fd = umad_fd,
        .events = POLLIN,
      };

      int np = poll(&poll_fds, 1, poll_timeout_ms);
      if (np < 0)
        FATAL("error polling for mads: %m\n");

      if (np == 0) {
        TRACE("timedout waiting for mad, nr_responses %zu\n", nr_responses);
        break;
      }

      if (recv_response_umad(which, host_list, nr_hosts) < 0)
        continue;

      nr_responses++;

      if (nr_responses == nr_sent) {
        TRACE("received all responses in %f seconds\n", dnow() - start[which]);
        if (which == 1)
          break;
      }
    }
  }

  printf("%-12s %12s %12s %12s %12s\n",
         "HOST", "RX_KB/S", "RX_P/S", "TX_KB/S", "TX_P/S");

  double t_rx_kbs = 0, t_rx_ps = 0, t_tx_kbs = 0, t_tx_ps = 0;

  for (i = 0; i < nr_hosts; i++) {
    struct host_ent *h = host_list[i];
    double rx_kbs = h->h_rx_b / interval / 1024;
    double rx_ps = h->h_rx_p / interval;
    double tx_kbs = h->h_tx_b / interval / 1024;
    double tx_ps = h->h_tx_p / interval;

    t_rx_kbs += rx_kbs;
    t_rx_ps += rx_ps;
    t_tx_kbs += tx_kbs;
    t_tx_ps += tx_ps;

    printf("%-12s %12.3f %12.3f %12.3f %12.3f\n",
           h->h_name, rx_kbs, rx_ps, tx_kbs, tx_ps);
  }

  printf("%-12s %12.3f %12.3f %12.3f %12.3f\n",
         "TOTAL", t_rx_kbs, t_rx_ps, t_tx_kbs, t_tx_ps);

  if (umad_fd >= 0)
    umad_close_port(umad_fd);

  return 0;
}
