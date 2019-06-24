/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2017-2018 Nippon Telegraph and Telephone Corporation
 */

#include <rte_cycles.h>

#include "shared/secondary/spp_worker_th/vf_deps.h"
#include "shared/secondary/spp_worker_th/spp_port.h"
#include "spp_vf.h"
#include "spp_forward.h"

#define RTE_LOGTYPE_FORWARD RTE_LOGTYPE_USER1

/* A set of port info of rx and tx */
struct forward_rxtx {
	struct sppwk_port_info rx; /* rx port */
	struct sppwk_port_info tx; /* tx port */
};

/* Information on the path used for forward. */
struct forward_path {
	char name[STR_LEN_NAME];    /* component name */
	volatile enum sppwk_worker_type wk_type;
	int num_rx;                     /* number of receive ports */
	int num_tx;                     /* number of trans ports   */
	struct forward_rxtx ports[RTE_MAX_ETHPORTS];
					/* port used for transfer  */
};

/* Information for forward. */
struct forward_info {
	volatile int ref_index; /* index to reference area */
	volatile int upd_index; /* index to update area    */
	struct forward_path path[SPP_INFO_AREA_MAX];
				/* Information of data path */
};

struct forward_info g_forward_info[RTE_MAX_LCORE];

/* Clear info */
void
spp_forward_init(void)
{
	int cnt = 0;
	memset(&g_forward_info, 0x00, sizeof(g_forward_info));
	for (cnt = 0; cnt < RTE_MAX_LCORE; cnt++) {
		g_forward_info[cnt].ref_index = 0;
		g_forward_info[cnt].upd_index = 1;
	}
}

/* Update forward info */
int
spp_forward_update(struct sppwk_comp_info *component)
{
	int cnt = 0;
	int nof_rx = component->nof_rx;
	int nof_tx = component->nof_tx;
	int max = (nof_rx > nof_tx)?nof_rx*nof_tx:nof_tx;
	struct forward_info *info = &g_forward_info[component->comp_id];
	struct forward_path *path = &info->path[info->upd_index];

	/* Forward component allows only one receiving port. */
	if ((component->wk_type == SPPWK_TYPE_FWD) &&
			unlikely(nof_rx > 1)) {
		RTE_LOG(ERR, FORWARD,
			"Component[%d] Setting error. (type = %d, rx = %d)\n",
			component->comp_id, component->wk_type, nof_rx);
		return SPP_RET_NG;
	}

	/* Component allows only one transmit port. */
	if (unlikely(nof_tx != 0) && unlikely(nof_tx != 1)) {
		RTE_LOG(ERR, FORWARD,
			"Component[%d] Setting error. (type = %d, tx = %d)\n",
			component->comp_id, component->wk_type, nof_tx);
		return SPP_RET_NG;
	}

	memset(path, 0x00, sizeof(struct forward_path));

	RTE_LOG(INFO, FORWARD,
			"Component[%d] Start update component. "
			"(name = %s, type = %d)\n",
			component->comp_id,
			component->name,
			component->wk_type);

	memcpy(&path->name, component->name, STR_LEN_NAME);
	path->wk_type = component->wk_type;
	path->num_rx = component->nof_rx;
	path->num_tx = component->nof_tx;
	for (cnt = 0; cnt < nof_rx; cnt++)
		memcpy(&path->ports[cnt].rx, component->rx_ports[cnt],
				sizeof(struct sppwk_port_info));

	/* Transmit port is set according with larger nof_rx / nof_tx. */
	for (cnt = 0; cnt < max; cnt++)
		memcpy(&path->ports[cnt].tx, component->tx_ports[0],
				sizeof(struct sppwk_port_info));

	info->upd_index = info->ref_index;
	while (likely(info->ref_index == info->upd_index))
		rte_delay_us_block(SPP_CHANGE_UPDATE_INTERVAL);

	RTE_LOG(INFO, FORWARD,
			"Component[%d] Complete update component. "
			"(name = %s, type = %d)\n",
			component->comp_id,
			component->name,
			component->wk_type);

	return SPP_RET_OK;
}

/* Change index of forward info */
static inline void
change_forward_index(int id)
{
	struct forward_info *info = &g_forward_info[id];
	if (info->ref_index == info->upd_index) {
		/* Change reference index of port ability. */
		spp_port_ability_change_index(PORT_ABILITY_CHG_INDEX_REF,
									0, 0);

		info->ref_index = (info->upd_index+1)%SPP_INFO_AREA_MAX;
	}
}
/**
 * Forwarding packets as forwarder or merger
 *
 * Behavior of forwarding is defined as core_info->type which is given
 * as an argument of void and typecasted to spp_config_info.
 */
int
spp_forward(int id)
{
	int cnt, buf;
	int nb_rx = 0;
	int nb_tx = 0;
	struct forward_info *info = &g_forward_info[id];
	struct forward_path *path = NULL;
	struct sppwk_port_info *rx;
	struct sppwk_port_info *tx;
	struct rte_mbuf *bufs[MAX_PKT_BURST];

	change_forward_index(id);
	path = &info->path[info->ref_index];

	/* Practice condition check */
	if (path->wk_type == SPPWK_TYPE_MRG) {
		/* merger */
		if (!(path->num_tx == 1 && path->num_rx >= 1))
			return SPP_RET_OK;
	} else {
		/* forwarder */
		if (!(path->num_tx == 1 && path->num_rx == 1))
			return SPP_RET_OK;
	}

	for (cnt = 0; cnt < path->num_rx; cnt++) {
		rx = &path->ports[cnt].rx;
		tx = &path->ports[cnt].tx;

		/* Receive packets */
		nb_rx = spp_eth_rx_burst(rx->ethdev_port_id, 0,
				bufs, MAX_PKT_BURST);
		if (unlikely(nb_rx == 0))
			continue;

		/* Send packets */
		if (tx->ethdev_port_id >= 0)
			nb_tx = spp_eth_tx_burst(tx->ethdev_port_id,
					0, bufs, nb_rx);

		/* Discard remained packets to release mbuf */
		if (unlikely(nb_tx < nb_rx)) {
			for (buf = nb_tx; buf < nb_rx; buf++)
				rte_pktmbuf_free(bufs[buf]);
		}
	}
	return SPP_RET_OK;
}

/* Merge/Forward get component status */
int
spp_forward_get_component_status(
		unsigned int lcore_id, int id,
		struct spp_iterate_core_params *params)
{
	int ret = SPP_RET_NG;
	int cnt;
	const char *component_type = NULL;
	struct forward_info *info = &g_forward_info[id];
	struct forward_path *path = &info->path[info->ref_index];
	struct sppwk_port_idx rx_ports[RTE_MAX_ETHPORTS];
	struct sppwk_port_idx tx_ports[RTE_MAX_ETHPORTS];

	if (unlikely(path->wk_type == SPPWK_TYPE_NONE)) {
		RTE_LOG(ERR, FORWARD,
				"Component[%d] Not used. "
				"(status)(core = %d, type = %d)\n",
				id, lcore_id, path->wk_type);
		return SPP_RET_NG;
	}

	if (path->wk_type == SPPWK_TYPE_MRG)
		component_type = SPPWK_TYPE_MRG_STR;
	else
		component_type = SPPWK_TYPE_FWD_STR;

	memset(rx_ports, 0x00, sizeof(rx_ports));
	for (cnt = 0; cnt < path->num_rx; cnt++) {
		rx_ports[cnt].iface_type = path->ports[cnt].rx.iface_type;
		rx_ports[cnt].iface_no   = path->ports[cnt].rx.iface_no;
	}

	memset(tx_ports, 0x00, sizeof(tx_ports));
	for (cnt = 0; cnt < path->num_tx; cnt++) {
		tx_ports[cnt].iface_type = path->ports[cnt].tx.iface_type;
		tx_ports[cnt].iface_no   = path->ports[cnt].tx.iface_no;
	}

	/* Set the information with the function specified by the command. */
	ret = (*params->element_proc)(
		params, lcore_id,
		path->name, component_type,
		path->num_rx, rx_ports, path->num_tx, tx_ports);
	if (unlikely(ret != SPP_RET_OK))
		return SPP_RET_NG;

	return SPP_RET_OK;
}
