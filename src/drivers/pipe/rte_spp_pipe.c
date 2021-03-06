/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2015 Intel Corporation
 * Copyright(c) 2020 Nippon Telegraph and Telephone Corporation
 */

#include "rte_eth_ring.h"
#include <rte_mbuf.h>
#include <rte_ethdev_driver.h>
#include <rte_ethdev_vdev.h>
#include <rte_malloc.h>
#include <rte_memcpy.h>
#include <rte_string_fns.h>
#include <rte_bus_vdev.h>
#include <rte_kvargs.h>
#include <rte_errno.h>
#include <string.h>

#define ETH_PIPE_RX_ARG	"rx"
#define ETH_PIPE_TX_ARG	"tx"

/* TODO: define in config */
#define PMD_PIPE_MAX_RX_RINGS 1
#define PMD_PIPE_MAX_TX_RINGS 1

static const char * const valid_arguments[] = {
	ETH_PIPE_RX_ARG,
	ETH_PIPE_TX_ARG,
	NULL
};

struct ring_queue {
	struct rte_ring *rng;
	rte_atomic64_t rx_pkts;
	rte_atomic64_t tx_pkts;
	rte_atomic64_t err_pkts;
};

struct pipe_private {
	uint16_t nb_rx_queues;
	uint16_t nb_tx_queues;
	struct ring_queue rx_ring_queues[PMD_PIPE_MAX_RX_RINGS];
	struct ring_queue tx_ring_queues[PMD_PIPE_MAX_TX_RINGS];
};

static struct rte_eth_link pmd_pipe_link = {
	.link_speed = ETH_SPEED_NUM_10G,
	.link_duplex = ETH_LINK_FULL_DUPLEX,
	.link_status = ETH_LINK_DOWN,
	.link_autoneg = ETH_LINK_FIXED,
};

static int eth_pipe_logtype;

#define PMD_PIPE_LOG(level, fmt, args...) \
	rte_log(RTE_LOG_ ## level, eth_pipe_logtype, \
		"%s(): " fmt "\n", __func__, ##args)

static uint16_t
eth_pipe_rx(void *q, struct rte_mbuf **bufs, uint16_t nb_bufs)
{
	void **ptrs = (void *)&bufs[0];
	struct ring_queue *r = q;
	uint16_t nb_rx;

	if (!q)
		return 0;

	nb_rx = (uint16_t)rte_ring_dequeue_burst(r->rng, ptrs, nb_bufs, NULL);

	if (r->rng->flags & RING_F_SC_DEQ)
		r->rx_pkts.cnt += nb_rx;
	else
		rte_atomic64_add(&(r->rx_pkts), nb_rx);

	return nb_rx;
}

static uint16_t
eth_pipe_tx(void *q, struct rte_mbuf **bufs, uint16_t nb_bufs)
{
	void **ptrs = (void *)&bufs[0];
	struct ring_queue *r = q;
	uint16_t nb_tx;

	if (!q)
		return 0;

	nb_tx = (uint16_t)rte_ring_enqueue_burst(r->rng, ptrs, nb_bufs, NULL);

	if (r->rng->flags & RING_F_SP_ENQ) {
		r->tx_pkts.cnt += nb_tx;
		r->err_pkts.cnt += nb_bufs - nb_tx;
	} else {
		rte_atomic64_add(&(r->tx_pkts), nb_tx);
		rte_atomic64_add(&(r->err_pkts), nb_bufs - nb_tx);
	}

	return nb_tx;
}

static int
eth_dev_configure(struct rte_eth_dev *dev __rte_unused)
{
	return 0;
}

static int
eth_dev_start(struct rte_eth_dev *dev)
{
	dev->data->dev_link.link_status = ETH_LINK_UP;
	return 0;
}

static void
eth_dev_stop(struct rte_eth_dev *dev)
{
	dev->data->dev_link.link_status = ETH_LINK_DOWN;
}

static int
eth_rx_queue_setup(struct rte_eth_dev *dev,
		uint16_t rx_queue_id,
		uint16_t nb_rx_desc __rte_unused,
		unsigned int socket_id __rte_unused,
		const struct rte_eth_rxconf *rx_conf __rte_unused,
		struct rte_mempool *mb_pool __rte_unused)
{
	struct pipe_private *pipe_priv = dev->data->dev_private;
	dev->data->rx_queues[rx_queue_id] =
				&pipe_priv->rx_ring_queues[rx_queue_id];
	return 0;
}

static int
eth_tx_queue_setup(struct rte_eth_dev *dev,
		uint16_t tx_queue_id,
		uint16_t nb_tx_desc __rte_unused,
		unsigned int socket_id __rte_unused,
		const struct rte_eth_txconf *tx_conf __rte_unused)
{
	struct pipe_private *pipe_priv = dev->data->dev_private;
	dev->data->tx_queues[tx_queue_id] =
				&pipe_priv->tx_ring_queues[tx_queue_id];
	return 0;
}

static int
eth_dev_info(struct rte_eth_dev *dev, struct rte_eth_dev_info *dev_info)
{
	struct pipe_private *pipe_priv = dev->data->dev_private;
	dev_info->max_mac_addrs = 1;
	dev_info->max_rx_pktlen = (uint32_t)-1;
	dev_info->max_rx_queues = pipe_priv->nb_rx_queues;
	dev_info->max_tx_queues = pipe_priv->nb_tx_queues;
	dev_info->min_rx_bufsize = 0;

	return 0;
}

static int
eth_stats_get(struct rte_eth_dev *dev, struct rte_eth_stats *stats)
{
	unsigned int i;
	unsigned long rx_total = 0, tx_total = 0, tx_err_total = 0;
	const struct pipe_private *pipe_priv = dev->data->dev_private;

	for (i = 0; i < RTE_ETHDEV_QUEUE_STAT_CNTRS &&
			i < dev->data->nb_rx_queues; i++) {
		stats->q_ipackets[i] = pipe_priv->rx_ring_queues[i].rx_pkts.cnt;
		rx_total += stats->q_ipackets[i];
	}

	for (i = 0; i < RTE_ETHDEV_QUEUE_STAT_CNTRS &&
			i < dev->data->nb_tx_queues; i++) {
		stats->q_opackets[i] = pipe_priv->tx_ring_queues[i].tx_pkts.cnt;
		stats->q_errors[i] = pipe_priv->tx_ring_queues[i].err_pkts.cnt;
		tx_total += stats->q_opackets[i];
		tx_err_total += stats->q_errors[i];
	}

	stats->ipackets = rx_total;
	stats->opackets = tx_total;
	stats->oerrors = tx_err_total;

	return 0;
}

static int
eth_stats_reset(struct rte_eth_dev *dev)
{
	unsigned int i;
	struct pipe_private *pipe_priv = dev->data->dev_private;
	for (i = 0; i < dev->data->nb_rx_queues; i++)
		pipe_priv->rx_ring_queues[i].rx_pkts.cnt = 0;
	for (i = 0; i < dev->data->nb_tx_queues; i++) {
		pipe_priv->tx_ring_queues[i].tx_pkts.cnt = 0;
		pipe_priv->tx_ring_queues[i].err_pkts.cnt = 0;
	}

	return 0;
}

static void
eth_queue_release(void *q __rte_unused)
{
}

static int
eth_link_update(struct rte_eth_dev *dev __rte_unused,
		int wait_to_complete __rte_unused)
{
	return 0;
}

static const struct eth_dev_ops ops = {
	.dev_start = eth_dev_start,
	.dev_stop = eth_dev_stop,
	.dev_configure = eth_dev_configure,
	.dev_infos_get = eth_dev_info,
	.rx_queue_setup = eth_rx_queue_setup,
	.tx_queue_setup = eth_tx_queue_setup,
	.rx_queue_release = eth_queue_release,
	.tx_queue_release = eth_queue_release,
	.link_update = eth_link_update,
	.stats_get = eth_stats_get,
	.stats_reset = eth_stats_reset,
};

static char *get_rx_queue_name(unsigned int id)
{
	static char buffer[32];

	snprintf(buffer, sizeof(buffer) - 1, "eth_ring%u", id);

	return buffer;
}

static int
validate_ring_name(const char *value, unsigned int *num)
{
	const char *ring_name = "ring:";
	size_t len = strlen(ring_name);
	const char *num_start = value + len;
	char *end;

	if (value == NULL || strncmp(ring_name, value, len) != 0)
		return -1;

	*num = (unsigned int)strtoul(num_start, &end, 10);
	if (*num_start == '\0' || *end != '\0')
		return -1;

	return 0;
}

static int
parse_rings(const char *key, const char *value, void *data)
{
	struct pipe_private *pipe_priv = data;
	unsigned int num;
	struct rte_ring *r;

	if (validate_ring_name(value, &num) == -1) {
		PMD_PIPE_LOG(ERR, "invalid ring name %s", value);
		return -1;
	}

	r = rte_ring_lookup(get_rx_queue_name(num));
	if (r == NULL) {
		PMD_PIPE_LOG(ERR, "ring %s does not exist", value);
		return -1;
	}

	PMD_PIPE_LOG(DEBUG, "%s %s cons.head: %u cons.tail: %u "
			"prod.head: %u prod.tail: %u",
			key, value, r->cons.head, r->cons.tail,
			r->prod.head, r->prod.tail);

	if (strcmp(key, ETH_PIPE_RX_ARG) == 0) {
		if (pipe_priv->nb_rx_queues >= PMD_PIPE_MAX_RX_RINGS) {
			PMD_PIPE_LOG(ERR, "rx rings exceeds max(%d)",
					PMD_PIPE_MAX_RX_RINGS);
			return -1;
		}
		pipe_priv->rx_ring_queues[pipe_priv->nb_rx_queues].rng = r;
		pipe_priv->nb_rx_queues++;
	} else { /* ETH_PIPE_TX_ARG */
		if (pipe_priv->nb_tx_queues >= PMD_PIPE_MAX_TX_RINGS) {
			PMD_PIPE_LOG(ERR, "tx rings exceeds max(%d)",
					PMD_PIPE_MAX_TX_RINGS);
			return -1;
		}
		pipe_priv->tx_ring_queues[pipe_priv->nb_tx_queues].rng = r;
		pipe_priv->nb_tx_queues++;
	}

	return 0;
}

static int
rte_pmd_pipe_probe(struct rte_vdev_device *dev)
{
	const char *name = rte_vdev_device_name(dev);
	struct rte_kvargs *kvlist;
	int ret;
	struct pipe_private *pipe_priv;
	struct rte_eth_dev *eth_dev;
	struct rte_ether_addr *eth_addr;

	PMD_PIPE_LOG(INFO, "Initializing pmd_pipe for %s", name);

	if (rte_eal_process_type() == RTE_PROC_SECONDARY) {
		PMD_PIPE_LOG(DEBUG, "secondary");
		eth_dev = rte_eth_dev_attach_secondary(name);
		if (!eth_dev) {
			PMD_PIPE_LOG(ERR, "device not found");
			return -EINVAL;
		}

		eth_dev->dev_ops = &ops;
		eth_dev->rx_pkt_burst = eth_pipe_rx;
		eth_dev->tx_pkt_burst = eth_pipe_tx;
		eth_dev->device = &dev->device;

		rte_eth_dev_probing_finish(eth_dev);

		PMD_PIPE_LOG(DEBUG, "%s created", name);
		PMD_PIPE_LOG(DEBUG, "port_id = %d", eth_dev->data->port_id);

		return 0;
	}

	kvlist = rte_kvargs_parse(rte_vdev_device_args(dev), valid_arguments);
	if (kvlist == NULL) {
		PMD_PIPE_LOG(ERR, "invalid parameter");
		return -EINVAL;
	}

	eth_addr = rte_zmalloc_socket(NULL, sizeof(*eth_addr), 0,
		       rte_socket_id());
	if (eth_addr == NULL) {
		PMD_PIPE_LOG(ERR, "can't alloc memory");
		rte_kvargs_free(kvlist);
		return -ENOMEM;
	}

	eth_dev = rte_eth_vdev_allocate(dev, sizeof(*pipe_priv));
	if (eth_dev == NULL) {
		PMD_PIPE_LOG(ERR, "can't alloc memory");
		rte_kvargs_free(kvlist);
		return -ENOMEM;
	}

	pipe_priv = eth_dev->data->dev_private;

	ret = rte_kvargs_process(kvlist, NULL, parse_rings, pipe_priv);
	rte_kvargs_free(kvlist);
	if (ret == -1) {
		ret = -EINVAL;
	} else if (pipe_priv->nb_rx_queues == 0) {
		PMD_PIPE_LOG(ERR, "no rx ring specified");
		ret = -EINVAL;
	} else if (pipe_priv->nb_tx_queues == 0) {
		PMD_PIPE_LOG(ERR, "no tx ring specified");
		ret = -EINVAL;
	}
	if (ret != 0) {
		rte_eth_dev_release_port(eth_dev);
		return ret;
	}

	eth_dev->data->dev_link = pmd_pipe_link;
	eth_dev->data->mac_addrs = eth_addr;

	eth_dev->dev_ops = &ops;
	eth_dev->rx_pkt_burst = eth_pipe_rx;
	eth_dev->tx_pkt_burst = eth_pipe_tx;

	rte_eth_dev_probing_finish(eth_dev);

	PMD_PIPE_LOG(DEBUG, "%s created", name);
	PMD_PIPE_LOG(DEBUG, "port_id = %d", eth_dev->data->port_id);

	return 0;
}

static int
rte_pmd_pipe_remove(struct rte_vdev_device *dev)
{
	const char *name = rte_vdev_device_name(dev);
	struct rte_eth_dev *eth_dev = NULL;

	if (name == NULL)
		return -EINVAL;

	PMD_PIPE_LOG(INFO, "Un-Initializing pmd_pipe for %s", name);

	eth_dev = rte_eth_dev_allocated(name);
	if (eth_dev == NULL) {
		PMD_PIPE_LOG(ERR, "no eth_dev allocated for %s", name);
		return -ENODEV;
	}

	eth_dev_stop(eth_dev);

	rte_eth_dev_release_port(eth_dev);

	PMD_PIPE_LOG(DEBUG, "%s removed", name);

	return 0;
}

static struct rte_vdev_driver pmd_pipe_drv = {
	.probe = rte_pmd_pipe_probe,
	.remove = rte_pmd_pipe_remove,
};

RTE_PMD_REGISTER_VDEV(spp_pipe, pmd_pipe_drv);
RTE_PMD_REGISTER_PARAM_STRING(spp_pipe, "rx=<rx_ring> tx=<tx_ring>");

RTE_INIT(eth_pipe_init_log)
{
	eth_pipe_logtype = rte_log_register("pmd.spp.pipe");
	if (eth_pipe_logtype >= 0)
		rte_log_set_level(eth_pipe_logtype, RTE_LOG_INFO);
}
