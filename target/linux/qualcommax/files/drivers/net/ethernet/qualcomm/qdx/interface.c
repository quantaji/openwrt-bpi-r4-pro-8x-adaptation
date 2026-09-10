// SPDX-License-Identifier: GPL-2.0-only
#include <linux/etherdevice.h>
#include <linux/rtnetlink.h>
#include "qdx.h"

void qdx_interfaces_init(struct qdx *qdx)
{
	unsigned int i;

	for (i = 0; i < QDX_PHYSICAL_PORTS; i++) {
		struct qdx_port *port = &qdx->ethernet->ports[i];

		port->qdx = qdx;
		port->ifnum = i;
		mutex_init(&port->lock);
		refcount_set(&port->refs, 1);
		init_waitqueue_head(&port->drained);
	}
}

int qdx_interfaces_register(struct qdx *qdx)
{
	struct qdx_ethernet *eth = qdx->ethernet;
	unsigned int i;
	int ret;

	ASSERT_RTNL();
	for (i = 1; i < QDX_PHYSICAL_PORTS; i++) {
		struct qdx_port *port = &eth->ports[i];
		struct qdx_port_state state;

		if (!(eth->ppe->info.ports & BIT(i)) || port->registered)
			continue;
		ret = eth->ppe->info.ops->snapshot(eth->ppe->info.context, i, &state);
		if (ret)
			return ret;
		port->netdev = state.netdev;
		port->conduit = eth->edma->info.conduit;
		dev_hold(port->netdev);
		dev_hold(port->conduit);
		/* One immutable physical table serves either firmware core. */
		smp_store_release(&port->registered, true);
	}
	return 0;
}

void qdx_port_publish(struct qdx_port *port)
{
	if (port->registered && port->opened && port->link_state &&
	    !READ_ONCE(port->changing) &&
	    !atomic_read(&port->qdx->failure))
		smp_store_release(&port->available, true);
}

void qdx_port_withdraw(struct qdx_port *port)
{
	qdx_io_port_close(port);
}

struct qdx_port *qdx_port_get(struct qdx *qdx, unsigned int core, u32 ifnum)
{
	struct qdx_port *port;

	if (core >= 2 || ifnum == 0 || ifnum >= QDX_PHYSICAL_PORTS)
		return NULL;
	port = &qdx->ethernet->ports[ifnum];
	rcu_read_lock();
	if (!smp_load_acquire(&port->registered) ||
	    !smp_load_acquire(&port->available)) {
		rcu_read_unlock();
		return NULL;
	}
	refcount_inc(&port->refs);
	rcu_read_unlock();
	return port;
}

void qdx_port_put(struct qdx_port *port)
{
	refcount_dec(&port->refs);
	wake_up_all(&port->drained);
}

void qdx_interfaces_unregister(struct qdx *qdx)
{
	unsigned int i;

	for (i = 1; i < QDX_PHYSICAL_PORTS; i++) {
		struct qdx_port *port = &qdx->ethernet->ports[i];

		qdx_port_withdraw(port);
	}
	synchronize_net();
	for (i = 1; i < QDX_PHYSICAL_PORTS; i++) {
		struct qdx_port *port = &qdx->ethernet->ports[i];

		if (!port->registered)
			continue;
		wait_event(port->drained, refcount_read(&port->refs) == 1);
		smp_store_release(&port->registered, false);
		dev_put(port->netdev);
		dev_put(port->conduit);
		port->netdev = NULL;
		port->conduit = NULL;
	}
}
