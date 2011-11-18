#ifndef _IBTOP_H_
#define _IBTOP_H_
#include <stdint.h>

struct ib_net_ent {
  uint64_t ca_guid;
  uint64_t sw_guid;
  uint16_t ca_lid;
  uint16_t sw_lid;
  uint8_t ca_port;
  uint8_t sw_port;
};

#endif
