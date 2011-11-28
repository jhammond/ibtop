#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <getopt.h>
#include <malloc.h>
#include <sys/stat.h>
#include <infiniband/umad.h>
#include <infiniband/mad.h>
#include "string1.h"
#include "trace.h"
#include "dict.h"
#include "list.h"
#include "ibtop.h"

#define NR_JOBS_HINT 256
#define NR_HOSTS_HINT 4096
#define TRID_BASE 0xE1F2A3B4C5D6E7F8

/* /sys/class/infiniband/HCA_NAME/ports/HCA_PORT */

char *hca_name = "mlx4_0";
int hca_port = 1;

struct ib_net_info {
  uint16_t ni_lid;
  uint8_t ni_port;
  unsigned int ni_is_hca:1;
};

#define GET_NAMED(ptr,member,name) \
  (ptr) = (typeof(ptr)) (((char *) name) - offsetof(typeof(*ptr), member))

#define ALLOC_NAMED(ptr,member,name) do {               \
    ptr = malloc(sizeof(*ptr) + strlen(name) + 1);      \
    if (ptr == NULL)                                    \
      OOM();                                            \
    memset(ptr, 0, sizeof(*ptr));                       \
    strcpy(ptr->member, name);                          \
  } while (0)

static inline double dnow(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_REALTIME, &ts);

  return ts.tv_sec + ts.tv_nsec / 1e9;
}

int umad_fd = -1;
int umad_agent_id = -1;
int umad_timeout_ms = 15;
int umad_retries = 10;

enum {
  C_TX_B,
  C_RX_B,
  C_TX_P,
  C_RX_P,
  NR_CTRS,
};

struct job_ent {
  uint64_t j_ctrs[NR_CTRS];
  char *j_owner;
  struct list_head j_host_list;
  size_t j_nr_hosts;
  char j_name[];
};

struct host_ent {
  uint64_t h_ctrs[NR_CTRS];
  uint64_t h_trid;
  struct job_ent *h_job;
  struct list_head h_job_link;
  struct ib_net_info h_info;
  unsigned int h_valid:2;
  char h_name[];
};

size_t nr_hosts = 0, host_vec_len = 0;
struct host_ent **host_vec = NULL;
struct dict host_dict;

size_t nr_jobs = 0, job_vec_len = 0;
struct job_ent **job_vec = NULL;
struct dict job_dict;

struct host_ent *host_lookup(const char *name, int create)
{
  struct host_ent *h;
  hash_t hash = dict_strhash(name);
  struct dict_entry *de = dict_entry_ref(&host_dict, hash, name);
  if (de->d_key != NULL) {
    GET_NAMED(h, h_name, de->d_key);
    return h;
  }

  if (!create)
    return NULL;

  if (!(nr_hosts < host_vec_len)) {
    size_t new_len = 2 * host_vec_len;
    struct host_ent **new_vec = realloc(host_vec, new_len * sizeof(host_vec[0]));
    if (new_vec == NULL)
      OOM();

    host_vec = new_vec;
    host_vec_len = new_len;
  }

  ALLOC_NAMED(h, h_name, name);
  INIT_LIST_HEAD(&h->h_job_link);

  if (dict_entry_set(&host_dict, de, hash, h->h_name) < 0)
    OOM();

  h->h_trid = TRID_BASE + nr_hosts;
  host_vec[nr_hosts++] = h;

  return h;
}

int host_vec_init(const char *info_path, const char *info_cmd)
{
  int rc = -1;
  FILE *info_file = NULL;
  char *line = NULL;
  size_t line_size = 0;

  info_file = fopen(info_path, "r");
  if (info_file != NULL)
    goto have_info_file;

  if (errno != ENOENT) {
    ERROR("cannot open `%s': %m\n", info_path);
    goto out;
  }

  ERROR("cannot open `%s', running `%s'\n", info_path, info_cmd);

  int st = system(info_cmd);
  if (st < 0) {
    ERROR("cannot execute `%s': %m\n", info_cmd);
    goto out;
  }

  if (WIFEXITED(st) && WEXITSTATUS(st) == 0) {
    TRACE("command `%s' exited with status 0\n", info_cmd); /* ... */
  } else {
    ERROR("command `%s' terminated with wait status: %d\n", info_cmd, st); /* ... */
    goto out;
  }

  info_file = fopen(info_path, "r");
  if (info_file == NULL) {
    ERROR("cannot open `%s': %m\n", info_path);
    goto out;
  }

 have_info_file:
  while (getline(&line, &line_size, info_file) >= 0) {
    char *rest = line;
    char *host = wsep(&rest);
    int use_hca;
    uint16_t hca_lid, sw_lid;
    uint8_t hca_port, sw_port;
    struct host_ent *h;

    if (sscanf(rest, "%d %*x %"SCNx16" %"SCNx8" %*x %"SCNx16" %"SCNx8,
               &use_hca, &hca_lid, &hca_port, &sw_lid, &sw_port) != 5)
      continue;

    h = host_lookup(host, 1);
    if (h == NULL)
      OOM();

    if (use_hca) {
      h->h_info.ni_lid = hca_lid;
      h->h_info.ni_port = hca_port;
      h->h_info.ni_is_hca = 1;
    } else {
      h->h_info.ni_lid = sw_lid;
      h->h_info.ni_port = sw_port;
    }
  }

  rc = 0;
 out:
  free(line);
  if (info_file != NULL)
    fclose(info_file);

  return rc;
}

int job_cmp(const void *p1, const void *p2)
{
  uint64_t *c1 = (*(struct job_ent **) p1)->j_ctrs;
  uint64_t *c2 = (*(struct job_ent **) p2)->j_ctrs;

  int k;
  for (k = 0; k < NR_CTRS; k++) {
    if (c1[k] > c2[k])
      return -1;
    else if (c1[k] < c2[k])
      return 1;
  }

  return 0;
}

struct job_ent *job_lookup(const char *name, const char *owner, int create)
{
  struct job_ent *j;
  hash_t hash = dict_strhash(name);
  struct dict_entry *de = dict_entry_ref(&job_dict, hash, name);
  if (de->d_key != NULL) {
    GET_NAMED(j, j_name, de->d_key);
    return j;
  }

  if (!create)
    return NULL;

  if (!(nr_jobs < job_vec_len)) {
    size_t new_len = 2 * job_vec_len;
    struct job_ent **new_vec = realloc(job_vec, new_len * sizeof(job_vec[0]));
    if (new_vec == NULL)
      OOM();

    job_vec = new_vec;
    job_vec_len = new_len;
  }

  TRACE("creating job `%s'\n", name);
  ALLOC_NAMED(j, j_name, name);
  INIT_LIST_HEAD(&j->j_host_list);

  if (owner != NULL)
    j->j_owner = strdup(owner);

  if (dict_entry_set(&job_dict, de, hash, j->j_name) < 0)
    OOM();

  job_vec[nr_jobs++] = j;

  return j;
}

int do_job_map_cmd(const char *cmd)
{
  int st = system(cmd);
  if (st < 0) {
    ERROR("cannot execute `%s': %m\n", cmd);
    return -1;
  }

  if (WIFEXITED(st) && WEXITSTATUS(st) == 0) {
    TRACE("command `%s' exited with status 0\n", cmd);
  } else {
    ERROR("command `%s' terminated with wait status: %d\n", cmd, st);
    return -1;
  }

  return 0;
}

int job_map_init(const char *path, const char *cmd, int max_age)
{
  int rc = -1;
  FILE *file = NULL;
  char *line = NULL;
  size_t line_size = 0;
  int did_try_cmd = 0;

  while (1) {
    if (file != NULL)
      fclose(file);

    file = fopen(path, "r");
    if (file == NULL) {
      if (errno != ENOENT || cmd == NULL) {
        ERROR("cannot open `%s': %m\n", path);
        goto out;
      }

      if (did_try_cmd)
        goto out;

      ERROR("cannot open `%s', running `%s'\n", path, cmd);

      did_try_cmd = 1;
      if (do_job_map_cmd(cmd) < 0)
        goto out;

      file = fopen(path, "r");
      if (file == NULL) {
        ERROR("cannot open `%s': %m\n", path);
        goto out;
      }
    }

    if (max_age < 0)
      goto have_file;

    /* Check that it's current. */
    struct stat stat_buf;
    if (fstat(fileno(file), &stat_buf) < 0) {
      ERROR("cannot stat `%s': %m\n", path);
      goto out;
    }

    if (!S_ISREG(stat_buf.st_mode))
      goto have_file; /* Maybe a pipe or fifo, assume OK. */

    double now = dnow();
    TRACE("job map age %f, max age %d\n",
          now - stat_buf.st_mtime, max_age);

    if (now <= stat_buf.st_mtime + max_age)
      goto have_file;

    if (did_try_cmd)
      goto out;

    ERROR("`%s' is more than %d seconds old, running `%s'\n",
          path, max_age, cmd);

    did_try_cmd = 1;
    if (do_job_map_cmd(cmd) < 0)
      goto out;
  }

 have_file:
  while (getline(&line, &line_size, file) >= 0) {
    char *rest = line;
    char *h_name = wsep(&rest);
    char *j_name = wsep(&rest);
    char *j_owner = wsep(&rest);
    if (h_name == NULL || j_name == NULL || j_owner == NULL)
      continue;

    struct host_ent *h = host_lookup(h_name, 0);
    if (h == NULL)
      continue;

    struct job_ent *j = job_lookup(j_name, j_owner, 1);
    if (j == NULL)
      OOM();

    list_add(&h->h_job_link, &j->j_host_list);
    j->j_nr_hosts++;
    h->h_job = j;
  }

  rc = 0;

 out:

  free(line);
  if (file != NULL)
    fclose(file);

  return rc;
}

static inline void ibtop_umad_dump(void *um, size_t len)
{
#ifdef IBTOP_UMAD_DEBUG
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
  umad_set_addr(um, h->h_info.ni_lid, 1, 0, IB_DEFAULT_QP1_QKEY);

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
  mad_set_field(pc, 0, IB_PC_PORT_SELECT_F, h->h_info.ni_port);

  TRACE("sending perf umad for host `%s', "
        "lid %"PRIx16", port %"PRIx8", is_hca %u, trid "P_TRID"\n",
        h->h_name, h->h_info.ni_lid, h->h_info.ni_port,
        (unsigned int) h->h_info.ni_is_hca, h->h_trid);

  ibtop_umad_dump(um, um_size);

  ssize_t nw = write(umad_fd, um, um_size);
  if (nw < 0) {
    ERROR("error sending umad for host `%s': %m\n", h->h_name);
    return -1;
  } else if (nw < um_size) {
    /* ... */
  }

  return 0;
}

int recv_response_umad(int which)
{
  char buf[1024];
  memset(buf, 0, sizeof(buf));
  size_t um_size = umad_size() + IB_MAD_SIZE;

  ssize_t nr = read(umad_fd, buf, sizeof(buf));
  if (nr < 0) {
    if (errno != EWOULDBLOCK)
      ERROR("error receiving mad: %m\n");
    return -1;
  }

  struct ib_user_mad *um = (struct ib_user_mad *) buf;
  void *m = umad_get_mad(um);
  uint64_t trid = mad_get_field64(m, 0, IB_MAD_TRID_F);

  TRACE("received umad trid "P_TRID", status %d\n", trid, um->status);

  ibtop_umad_dump(buf, nr);

  if (mad_get_field(m, 0, IB_DRSMP_STATUS_F) == IB_MAD_STS_REDIRECT) {
    /* FIXME */
    ERROR("received redirect, trid "P_TRID"\n", trid);
    return -1;
  }

  if (nr < um_size) {
    TRACE("short receive, expected %zu, only read %zd\n", um_size, nr);
    return -1;
  }

  size_t i = (uint32_t) (trid - TRID_BASE);
  TRACE("i %zu\n", i);

  if (!(0 <= i && i < nr_hosts)) {
    ERROR("bad trid "P_TRID" in received umad\n", trid);
    return -1;
  }

  struct host_ent *h = host_vec[i];
  if (h == NULL) {
    ERROR("no host for umad, trid "P_TRID"\n", trid);
    return -1;
  }

  unsigned int is_hca = h->h_info.ni_is_hca;

  TRACE("host `%s', lid %"PRIx16", port %"PRIx8", is_hca %u\n",
        h->h_name, h->h_info.ni_lid, h->h_info.ni_port, is_hca);

  void *pc = (char *) m + IB_PC_DATA_OFFS;
  uint64_t c[NR_CTRS];

  mad_decode_field(pc, IB_PC_EXT_RCV_BYTES_F, &c[is_hca ? C_RX_B : C_TX_B]);
  mad_decode_field(pc, IB_PC_EXT_RCV_PKTS_F,  &c[is_hca ? C_RX_P : C_TX_P]);
  mad_decode_field(pc, IB_PC_EXT_XMT_BYTES_F, &c[is_hca ? C_TX_B : C_RX_B]);
  mad_decode_field(pc, IB_PC_EXT_XMT_PKTS_F,  &c[is_hca ? C_TX_P : C_RX_P]);

  TRACE("rx_b %"PRIx64", rx_p %"PRIx64", tx_b %"PRIx64", tx_p %"PRIx64"\n",
        c[C_RX_B], c[C_RX_P], c[C_TX_B], c[C_TX_P]);

  if (c[C_RX_B] == 0 || c[C_RX_B] == (uint64_t) -1 ||
      c[C_TX_B] == 0 || c[C_TX_B] == (uint64_t) -1) {
    ERROR("perfquery for host `%s' returned bogus stats: "
          "rx_b %"PRIx64", rx_p %"PRIx64", tx_b %"PRIx64", tx_p %"PRIx64"\n",
          h->h_name, c[C_RX_B], c[C_RX_P], c[C_TX_B], c[C_TX_P]);
    return -1;
  }

  int k;
  for (k = 0; k < NR_CTRS; k++)
    h->h_ctrs[k] += which == 0 ? -c[k] : c[k];

  h->h_valid |= 1u << which;

  return 0;
}

int main(int argc, char *argv[])
{
  int have_host_args = 0;
  int have_job_args = 0;
  int want_expand = 0;
  char **args = NULL;
  size_t nr_args = 0;

  const char *net_info_path = IBTOP_NET_INFO_PATH;
  const char *net_info_cmd = IBTOP_NET_INFO_CMD;
  const char *job_map_path = IBTOP_JOB_MAP_PATH;
  const char *job_map_cmd = IBTOP_JOB_MAP_CMD;
  int job_map_max_age = IBTOP_JOB_MAP_MAX_AGE; /* Use -1 for never. */
  double interval = 1;
  size_t i;

  struct option opts[] = {
    { "help",            0, NULL, 'h' },
    { "interval",        1, NULL, 'i' },
    { "job-list",        0, NULL, 'j' },
    { "host-list",       0, NULL, 'l' },
    { "job-map-max-age", 1, NULL, 'm' },
    { "no-job-map",      0, NULL, 'n' },
    { "expand",          0, NULL, 'x' },
    { "job-map",         1, NULL, 257 },
    { "job-map-cmd",     1, NULL, 258 },
    { "net-info",        1, NULL, 259 },
    { "net-info-cmd",    1, NULL, 260 },
    { NULL, 0, NULL, 0},
  };

  int c;
  while ((c = getopt_long(argc, argv, "hi:jlm:nx", opts, 0)) != -1) {
    switch (c) {
    case 'h':
      printf("Usage: %s [OPTION]... [ARGS...]\n"
             "Report IB load by job or host.\n"
             "\n"
             "Mandatory arguments to long options are mandatory for short options too.\n"
             "  -h, --help                    display this help and exit\n"
             "  -i, --interval=NUMBER         report load over NUMBER seconds\n"
             "  -j, --job-list                report load on jobs given as arguments\n"
             "  -l, --host-list               report load on hosts given as arguments\n"
             "  -m, --job-map-max-age=NUMBER  regenerate job map if more than NUMBER seconds old\n"
             "  -n, --no-job-map              do not use a job map\n"
             "  -x, --expand                  output one line per host\n"
             "  --job-map=PATH                use job map at PATH\n"
             "  --job-map-cmd=COMMAND         use COMMAND to generate job map\n"
             "  --net-info=PATH               use net info at PATH\n"
             "  --net-info-cmd=COMMAND        use COMMAND to regenerate net info\n",
             program_invocation_short_name);
      exit(EXIT_SUCCESS);
    case 'i':
      interval = strtod(optarg, NULL);
      if (interval <= 0)
        FATAL("invalid interval `%s'\n", optarg);
      break;
    case 'j':
      have_job_args = 1;
      break;
    case 'l':
      have_host_args = 1;
      break;
    case 'm':
      job_map_max_age = atoi(optarg);
      break;
    case 'n':
      job_map_path = NULL;
      break;
    case 'x':
      want_expand = 1;
      break;
    case 257:
      job_map_path = optarg;
      break;
    case 258:
      job_map_cmd = optarg;
      break;
    case 259:
      net_info_path = optarg;
      break;
    case 260:
      net_info_cmd = optarg;
      break;
    case '?':
      fprintf(stderr, "Try `%s --help' for more information.",
              program_invocation_short_name);
      exit(EXIT_FAILURE);
    }
  }

  if (have_job_args && have_host_args)
    FATAL("cannot use `-j, --job-list' and `-l, --host-list' options simultaneously\n");

  args = argv + optind;
  nr_args = argc - optind;

  host_vec_len = NR_HOSTS_HINT > 0 ? NR_HOSTS_HINT : 4096;
  host_vec = malloc(host_vec_len * sizeof(host_vec[0]));
  if (host_vec == NULL)
    OOM();

  if (dict_init(&host_dict, NR_HOSTS_HINT) < 0)
    OOM();

  job_vec_len = NR_JOBS_HINT > 0 ? NR_JOBS_HINT : 256;
  job_vec = malloc(job_vec_len * sizeof(job_vec[0]));
  if (job_vec == NULL)
    OOM();

  if (dict_init(&job_dict, NR_JOBS_HINT) < 0)
    OOM();

  if (host_vec_init(net_info_path, net_info_cmd) < 0)
    /* ... */;

  if (nr_hosts == 0)
    FATAL("no valid hosts\n");

  if (job_map_path != NULL)
    job_map_init(job_map_path, job_map_cmd, job_map_max_age);

#ifdef IBTOP_UMAD_DEBUG
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

    if (have_host_args) {
      for (i = 0; i < nr_args; i++) {
        struct host_ent *h = host_lookup(args[i], 0);
        if (h == NULL) {
          if (which == 0)
            ERROR("unknown host `%s'\n", args[i]);
          continue;
        }
        if (host_send_perf_umad(h) < 0)
          continue;
        nr_sent++;
      }
    } else if (have_job_args) {
      for (i = 0; i < nr_args; i++) {
        struct job_ent *j = job_lookup(args[i], NULL, 0);
        if (j == NULL) {
          if (which == 0)
            ERROR("unknown job `%s'\n", args[i]);
          continue;
        }

        struct host_ent *h;
        list_for_each_entry(h, &j->j_host_list, h_job_link) {
          if (host_send_perf_umad(h) < 0)
            continue;
          nr_sent++;
        }
      }
    } else {
      for (i = 0; i < nr_hosts; i++) {
        if (host_send_perf_umad(host_vec[i]) < 0)
          continue;
        nr_sent++;
      }
    }

    TRACE("sent %zu in %f seconds\n", nr_sent, dnow() - start[which]);

    while (1) {
      double poll_timeout_ms = (deadline[which] - dnow()) * 1000;
      if (poll_timeout_ms <= 0)
        break;

      struct pollfd poll_fds = {
        .fd = umad_fd,
        .events = POLLIN,
      };

      int np = poll(&poll_fds, 1, poll_timeout_ms);
      if (np < 0)
        FATAL("error polling for responses: %m\n");

      if (np == 0) {
        TRACE("timedout waiting for mad, nr_responses %zu\n", nr_responses);
        break;
      }

      if (recv_response_umad(which) < 0)
        continue;

      nr_responses++;

      if (nr_responses == nr_sent) {
        TRACE("received all responses in %f seconds\n", dnow() - start[which]);
        if (which == 1)
          break;
      }
    }
  }

  for (i = 0; i < nr_hosts; i++) {
    struct host_ent *h = host_vec[i];
    struct job_ent *j = h->h_job;

    if (h->h_valid != 3)
      TRACE("skipping host `%s', valid %u\n",
            h->h_name, (unsigned int) h->h_valid);

    if (j == NULL)
      j = job_lookup(h->h_name, NULL, 1);

    int k;
    for (k = 0; k < NR_CTRS; k++)
      j->j_ctrs[k] += h->h_ctrs[k];
  }

  qsort(job_vec, nr_jobs, sizeof(job_vec[0]), &job_cmp);

  /* Omit packet counters for now. */
  printf("%-12s %14s %14s %8s %-12s\n",
         "JOBID", "TX_MB/S", "RX_MB/S", "NR_HOSTS", "OWNER");

  for (i = 0; i < nr_jobs; i++) {
    struct job_ent *j = job_vec[i];

    double rx_mbps = j->j_ctrs[C_RX_B] / interval / 1048576;
    /* double rx_ps = j->j_ctrs[C_RX_P] / interval; */
    double tx_mbps = j->j_ctrs[C_TX_B] / interval / 1048576;
    /* double tx_ps = j->j_ctrs[C_TX_P] / interval; */

    if (j->j_nr_hosts == 0) { /* Fake job. */
      printf("%-12s %14.3f %14.3f\n", j->j_name, tx_mbps, rx_mbps);
      continue;
    }

    printf("%-12s %14.3f %14.3f %8zu %-12s\n",
           j->j_name, tx_mbps, rx_mbps, j->j_nr_hosts,
           j->j_owner != NULL ? j->j_owner : "-");

    if (want_expand) {
      struct host_ent *h, **v;
      size_t i;

      v = malloc(j->j_nr_hosts * sizeof(v[0]));

      i = 0;
      list_for_each_entry(h, &j->j_host_list, h_job_link)
        v[i++] = h;

      qsort(v, j->j_nr_hosts, sizeof(v[0]), &job_cmp); /* XXX */

      for (i = 0; i < j->j_nr_hosts; i++)
        printf("  %-10s %14.3f %14.3f\n",
               v[i]->h_name,
               v[i]->h_ctrs[C_TX_B] / interval / 1048576,
               v[i]->h_ctrs[C_RX_B] / interval / 1048576);

      free(v);
    }
  }

  if (umad_fd >= 0)
    umad_close_port(umad_fd);

  return 0;
}
