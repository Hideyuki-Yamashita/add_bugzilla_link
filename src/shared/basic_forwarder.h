/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2019 Nippon Telegraph and Telephone Corporation
 */

#ifndef __SHARED_FORWARDER_H__
#define __SHARED_FORWARDER_H__

struct port_map port_map[RTE_MAX_ETHPORTS];
struct port ports_fwd_array[RTE_MAX_ETHPORTS];

void forward(void);

#endif
