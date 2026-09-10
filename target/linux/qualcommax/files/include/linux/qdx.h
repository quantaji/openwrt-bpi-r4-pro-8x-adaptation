/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_QDX_H
#define _LINUX_QDX_H

#include <linux/err.h>
#include <linux/if_ether.h>
#include <linux/kconfig.h>
#include <linux/netdevice.h>

struct qdx_edma;
struct qdx_ppe;

/* New host TX follows this selection; both completion paths remain usable. */
enum qdx_eth_transport {
	QDX_ETH_NATIVE,
	QDX_ETH_TRANSITION,
	QDX_ETH_FIRMWARE,
	QDX_ETH_UNAVAILABLE,
};

struct qdx_port_state {
	struct net_device *netdev;
	u32 mtu;
	u32 link_state;
	u8 mac[ETH_ALEN];
	bool admin;
};

struct qdx_edma_ops {
	int (*activate)(void *context);
	int (*quiesce)(void *context, u32 native_rx_entries);
	int (*resume_shared)(void *context);
	int (*select)(void *context, enum qdx_eth_transport transport);
	void (*wake)(void *context);
	void (*complete_tx)(void *context, unsigned int packets, unsigned int bytes);
	int (*restore)(void *context);
	void (*receive)(void *context, unsigned int port, struct sk_buff *skb,
			struct napi_struct *napi);
};

struct qdx_edma_info {
	struct device *dev;
	struct net_device *conduit;
	const struct qdx_edma_ops *ops;
	void *context;
	unsigned int max_frame;
	unsigned int tx_min_size;
};

struct qdx_ppe_ops {
	void (*lock)(void *context, unsigned int port);
	void (*unlock)(void *context, unsigned int port);
	int (*snapshot)(void *context, unsigned int port,
			struct qdx_port_state *state);
	int (*reapply)(void *context, unsigned int port);
	int (*restore)(void *context);
};

struct qdx_ppe_info {
	struct device *dev;
	struct net_device *conduit;
	const struct qdx_ppe_ops *ops;
	void *context;
	unsigned long ports;
};

#if IS_REACHABLE(CONFIG_QDX)
struct qdx_edma *qdx_edma_attach(const struct qdx_edma_info *info);
struct qdx_ppe *qdx_ppe_attach(const struct qdx_ppe_info *info);
void qdx_edma_detach(struct qdx_edma *edma);
void qdx_ppe_detach(struct qdx_ppe *ppe);
enum qdx_eth_transport qdx_edma_transport(struct qdx_edma *edma);
netdev_tx_t qdx_ethernet_xmit(struct qdx_edma *edma, struct sk_buff *skb,
			     unsigned int port);
int qdx_port_open(struct qdx_ppe *ppe, unsigned int port);
int qdx_port_close(struct qdx_ppe *ppe, unsigned int port);
int qdx_port_check_mtu(struct qdx_ppe *ppe, unsigned int port, unsigned int mtu);
int qdx_port_mtu(struct qdx_ppe *ppe, unsigned int port, unsigned int mtu);
/* A successful prepare holds port serialization until finish. */
int qdx_port_prepare(struct qdx_ppe *ppe, unsigned int port);
int qdx_port_finish(struct qdx_ppe *ppe, unsigned int port);
void qdx_port_failed(struct qdx_ppe *ppe, unsigned int port, int error);
#else
static inline struct qdx_edma *qdx_edma_attach(const struct qdx_edma_info *info)
{
	return NULL;
}
static inline struct qdx_ppe *qdx_ppe_attach(const struct qdx_ppe_info *info)
{
	return NULL;
}
static inline void qdx_edma_detach(struct qdx_edma *edma) {}
static inline void qdx_ppe_detach(struct qdx_ppe *ppe) {}
static inline enum qdx_eth_transport qdx_edma_transport(struct qdx_edma *edma)
{
	return QDX_ETH_NATIVE;
}
static inline netdev_tx_t qdx_ethernet_xmit(struct qdx_edma *edma,
		struct sk_buff *skb, unsigned int port)
{
	return NETDEV_TX_BUSY;
}
static inline int qdx_port_open(struct qdx_ppe *ppe, unsigned int port)
{
	return 0;
}
static inline int qdx_port_close(struct qdx_ppe *ppe, unsigned int port)
{
	return 0;
}
static inline int qdx_port_prepare(struct qdx_ppe *ppe, unsigned int port)
{
	return 0;
}
static inline int qdx_port_finish(struct qdx_ppe *ppe, unsigned int port)
{
	return 0;
}
static inline int qdx_port_check_mtu(struct qdx_ppe *ppe, unsigned int port,
				     unsigned int mtu)
{
	return 0;
}
static inline int qdx_port_mtu(struct qdx_ppe *ppe, unsigned int port, unsigned int mtu)
{
	return 0;
}
static inline void qdx_port_failed(struct qdx_ppe *ppe, unsigned int port, int error) {}
#endif
#endif
