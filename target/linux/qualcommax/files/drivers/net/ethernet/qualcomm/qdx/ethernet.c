// SPDX-License-Identifier: GPL-2.0-only
#include <linux/etherdevice.h>
#include <linux/if_vlan.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/reboot.h>
#include <linux/rtnetlink.h>
#include "qdx.h"

/* The two native providers can probe before the NSS platform device. */
static DEFINE_MUTEX(attachment_lock);
static DEFINE_MUTEX(detachment_lock);
static struct qdx *ethernet_instance;
static struct qdx_edma *native_edma;
static struct qdx_ppe *native_ppe;

static int qdx_port_command(struct qdx_port *port, u32 opcode,
			    const void *body, size_t size)
{
	struct qdx_reply reply = { .min_len = 0, .max_len = size };
	int ret;

	ret = qdx_command(&port->qdx->cores[0], port->ifnum, opcode, body, size,
			  &reply);
	if (reply.outcome == QDX_UNKNOWN)
		qdx_fail(port->qdx, ret);
	return ret;
}

/* The port mutex and native configuration lock cover this entire sequence. */
static int qdx_port_apply(struct qdx_port *port,
			  const struct qdx_port_state *state)
{
	struct qdx_ethernet *eth = port->qdx->ethernet;
	struct qdx_port_state old = port->confirmed;
	__le32 open[4] = { 0, 0, cpu_to_le32(QDX_IF_ETH_RX), 0 };
	bool mac_done = false, mtu_done = false;
	bool admin_done = false, link_done = false;
	bool old_opened = port->opened;
	u32 old_link = port->link_state;
	__le32 word;
	__le16 mtu;
	int ret, undo = 0;

	qdx_port_withdraw(port);
	if (!port->configured || !ether_addr_equal(old.mac, state->mac)) {
		ret = qdx_port_command(port, QDX_IF_MAC, state->mac, ETH_ALEN);
		if (ret)
			goto rollback;
		mac_done = true;
	}
	if (!port->configured || old.mtu != state->mtu) {
		ret = qdx_io_set_mtu(port->qdx, state->mtu);
		if (ret)
			goto rollback;
		mtu = cpu_to_le16(state->mtu);
		ret = qdx_port_command(port, QDX_IF_MTU, &mtu, sizeof(mtu));
		if (ret)
			goto rollback;
		mtu_done = true;
	}
	if (state->admin != old_opened) {
		word = 0;
		ret = state->admin ? qdx_port_command(port, QDX_IF_OPEN, open, sizeof(open)) :
			qdx_port_command(port, QDX_IF_CLOSE, &word, sizeof(word));
		if (ret)
			goto rollback;
		admin_done = true;
		port->opened = state->admin;
	}
	if (state->admin && (old_link != state->link_state || admin_done)) {
		word = cpu_to_le32(state->link_state);
		ret = qdx_port_command(port, QDX_IF_LINK, &word, sizeof(word));
		if (ret)
			goto rollback;
		link_done = true;
	}
	ret = eth->ppe->info.ops->reapply(eth->ppe->info.context, port->ifnum);
	if (ret)
		goto rollback;
	/* Firmware physical updates may also write the shared RX selector. */
	if (eth->transport == QDX_ETH_NATIVE || eth->transport == QDX_ETH_FIRMWARE) {
		ret = eth->edma->info.ops->select(eth->edma->info.context,
						eth->transport);
		if (ret) {
			qdx_fail(port->qdx, ret);
			return ret;
		}
	}
	port->confirmed = *state;
	port->configured = true;
	port->link_state = state->admin ? state->link_state : 0;
	qdx_port_publish(port);
	return 0;

rollback:
	/* A command with unknown execution cannot be safely reversed. */
	if (atomic_read(&port->qdx->failure))
		return ret;
	if (!port->configured) {
		/* Initial partial state is closed before terminal ownership recovery. */
		word = 0;
		if (port->opened)
			qdx_port_command(port, QDX_IF_CLOSE, &word, sizeof(word));
		qdx_fail(port->qdx, ret);
		return ret;
	}
	if (link_done) {
		word = cpu_to_le32(old_link);
		undo = qdx_port_command(port, QDX_IF_LINK, &word, sizeof(word));
	}
	if (admin_done && !undo) {
		word = 0;
		undo = old_opened ? qdx_port_command(port, QDX_IF_OPEN, open, sizeof(open)) :
			qdx_port_command(port, QDX_IF_CLOSE, &word, sizeof(word));
	}
	if (mtu_done && !undo) {
		mtu = cpu_to_le16(old.mtu);
		undo = qdx_port_command(port, QDX_IF_MTU, &mtu, sizeof(mtu));
	}
	if (mac_done && !undo)
		undo = qdx_port_command(port, QDX_IF_MAC, old.mac, ETH_ALEN);
	if (!undo && (eth->transport == QDX_ETH_NATIVE ||
		      eth->transport == QDX_ETH_FIRMWARE))
		undo = eth->edma->info.ops->select(eth->edma->info.context,
						 eth->transport);
	if (undo) {
		qdx_fail(port->qdx, undo);
		return ret;
	}
	port->opened = old_opened;
	port->link_state = old_link;
	return ret;
}

static void qdx_ports_lock(struct qdx_ethernet *eth)
{
	unsigned int i;

	for (i = 1; i < QDX_PHYSICAL_PORTS; i++) {
		if (!(eth->ppe->info.ports & BIT(i)))
			continue;
		eth->ppe->info.ops->lock(eth->ppe->info.context, i);
		mutex_lock(&eth->ports[i].lock);
	}
}

static void qdx_ports_unlock(struct qdx_ethernet *eth)
{
	int i;

	for (i = QDX_PHYSICAL_PORTS - 1; i > 0; i--) {
		if (!(eth->ppe->info.ports & BIT(i)))
			continue;
		mutex_unlock(&eth->ports[i].lock);
		eth->ppe->info.ops->unlock(eth->ppe->info.context, i);
	}
}

static void qdx_mac_work(struct work_struct *work)
{
	struct qdx_ethernet *eth = container_of(work, struct qdx_ethernet, mac_work);
	unsigned int i;

	rtnl_lock();
	if (!eth->ppe || eth->ppe->detaching || !eth->native_touched ||
	    READ_ONCE(eth->ppe->qdx->state) != QDX_READY ||
	    atomic_read(&eth->ppe->qdx->failure))
		goto out;
	for (i = 1; i < QDX_PHYSICAL_PORTS; i++) {
		struct qdx_port *port = &eth->ports[i];
		struct qdx_port_state state;
		int ret;

		if (!port->registered)
			continue;
		eth->ppe->info.ops->lock(eth->ppe->info.context, i);
		mutex_lock(&port->lock);
		ret = eth->ppe->info.ops->snapshot(eth->ppe->info.context, i, &state);
		if (!ret)
			ret = qdx_port_apply(port, &state);
		if (ret) {
			qdx_port_withdraw(port);
			qdx_fail(port->qdx, ret);
		}
		mutex_unlock(&port->lock);
		eth->ppe->info.ops->unlock(eth->ppe->info.context, i);
	}
	qdx_ethernet_wake(eth->edma->qdx);
out:
	rtnl_unlock();
}

static int qdx_netdev_event(struct notifier_block *nb, unsigned long event,
			    void *data)
{
	struct qdx_ethernet *eth = container_of(nb, struct qdx_ethernet, netdev_notifier);
	struct net_device *dev = netdev_notifier_info_to_dev(data);
	unsigned int i;

	if (event != NETDEV_CHANGEADDR)
		return NOTIFY_DONE;
	for (i = 1; i < QDX_PHYSICAL_PORTS; i++) {
		if (eth->ports[i].registered && eth->ports[i].netdev == dev) {
			schedule_work(&eth->mac_work);
			break;
		}
	}
	return NOTIFY_DONE;
}

int qdx_ethernet_register(struct qdx *qdx)
{
	struct qdx_ethernet *eth;
	int ret;

	eth = kzalloc(sizeof(*eth), GFP_KERNEL);
	if (!eth)
		return -ENOMEM;
	qdx->ethernet = eth;
	eth->transport = QDX_ETH_NATIVE;
	qdx_interfaces_init(qdx);
	INIT_WORK(&eth->mac_work, qdx_mac_work);
	eth->netdev_notifier.notifier_call = qdx_netdev_event;
	ret = register_netdevice_notifier(&eth->netdev_notifier);
	if (ret)
		goto free;
	mutex_lock(&attachment_lock);
	if (ethernet_instance) {
		mutex_unlock(&attachment_lock);
		unregister_netdevice_notifier(&eth->netdev_notifier);
		ret = -EBUSY;
		goto free;
	}
	ethernet_instance = qdx;
	mutex_unlock(&attachment_lock);
	qdx_schedule(qdx);
	return 0;
free:
	kfree(eth);
	qdx->ethernet = NULL;
	return ret;
}

bool qdx_ethernet_ready(struct qdx *qdx)
{
	struct device_node *edma, *ppe;
	bool ready = false;

	edma = of_parse_phandle(qdx->dev->of_node, "qcom,edma", 0);
	ppe = of_parse_phandle(qdx->dev->of_node, "qcom,ppe", 0);
	mutex_lock(&attachment_lock);
	if (native_edma && native_ppe && !native_edma->detaching &&
	    !native_ppe->detaching && native_edma->info.dev->of_node == edma &&
	    native_ppe->info.dev->of_node == ppe &&
	    native_edma->info.conduit == native_ppe->info.conduit) {
		qdx->ethernet->edma = native_edma;
		qdx->ethernet->ppe = native_ppe;
		native_edma->qdx = qdx;
		native_ppe->qdx = qdx;
		ready = true;
	}
	mutex_unlock(&attachment_lock);
	of_node_put(edma);
	of_node_put(ppe);
	return ready;
}

struct qdx_edma *qdx_edma_attach(const struct qdx_edma_info *info)
{
	struct qdx_edma *edma;

	if (!info->dev || !info->conduit || !info->ops || !info->ops->quiesce ||
	    !info->ops->restore || !info->ops->receive || !info->ops->activate ||
	    !info->ops->resume_shared || !info->ops->select ||
	    !info->ops->complete_tx || !info->ops->wake)
		return ERR_PTR(-EINVAL);
	edma = kzalloc(sizeof(*edma), GFP_KERNEL);
	if (!edma)
		return ERR_PTR(-ENOMEM);
	edma->info = *info;
	mutex_lock(&attachment_lock);
	if (native_edma) {
		mutex_unlock(&attachment_lock);
		kfree(edma);
		return ERR_PTR(-EBUSY);
	}
	get_device(info->dev);
	dev_hold(info->conduit);
	native_edma = edma;
	if (ethernet_instance)
		qdx_schedule(ethernet_instance);
	mutex_unlock(&attachment_lock);
	return edma;
}
EXPORT_SYMBOL_GPL(qdx_edma_attach);

struct qdx_ppe *qdx_ppe_attach(const struct qdx_ppe_info *info)
{
	struct qdx_ppe *ppe;

	if (!info->dev || !info->conduit || !info->ops || !info->ops->snapshot ||
	    !info->ops->reapply || !info->ops->restore || !info->ops->lock ||
	    !info->ops->unlock || !info->ports || info->ports & ~GENMASK(6, 1))
		return ERR_PTR(-EINVAL);
	ppe = kzalloc(sizeof(*ppe), GFP_KERNEL);
	if (!ppe)
		return ERR_PTR(-ENOMEM);
	ppe->info = *info;
	mutex_lock(&attachment_lock);
	if (native_ppe) {
		mutex_unlock(&attachment_lock);
		kfree(ppe);
		return ERR_PTR(-EBUSY);
	}
	get_device(info->dev);
	dev_hold(info->conduit);
	native_ppe = ppe;
	if (ethernet_instance)
		qdx_schedule(ethernet_instance);
	mutex_unlock(&attachment_lock);
	return ppe;
}
EXPORT_SYMBOL_GPL(qdx_ppe_attach);

/* Called without native locks, before the native driver's first destruction. */
static void qdx_native_drain(struct qdx *qdx)
{
	if (!qdx)
		return;
	qdx_fail(qdx, -ENODEV);
	wait_for_completion(&qdx->terminal_done);
	flush_work(&qdx->lifecycle);
	cancel_work_sync(&qdx->ethernet->mac_work);
	if (qdx->execution_possible && !qdx->access_ended)
		panic("qdx: native detach cannot release firmware-accessible resources");
	qdx_interfaces_unregister(qdx);
}

void qdx_edma_detach(struct qdx_edma *edma)
{
	if (!edma)
		return;
	mutex_lock(&detachment_lock);
	mutex_lock(&attachment_lock);
	edma->detaching = true;
	mutex_unlock(&attachment_lock);
	qdx_native_drain(edma->qdx);
	mutex_lock(&attachment_lock);
	if (edma->qdx)
		edma->qdx->ethernet->edma = NULL;
	native_edma = NULL;
	mutex_unlock(&attachment_lock);
	dev_put(edma->info.conduit);
	put_device(edma->info.dev);
	kfree(edma);
	mutex_unlock(&detachment_lock);
}
EXPORT_SYMBOL_GPL(qdx_edma_detach);

void qdx_ppe_detach(struct qdx_ppe *ppe)
{
	if (!ppe)
		return;
	mutex_lock(&detachment_lock);
	mutex_lock(&attachment_lock);
	ppe->detaching = true;
	mutex_unlock(&attachment_lock);
	qdx_native_drain(ppe->qdx);
	mutex_lock(&attachment_lock);
	if (ppe->qdx)
		ppe->qdx->ethernet->ppe = NULL;
	native_ppe = NULL;
	mutex_unlock(&attachment_lock);
	dev_put(ppe->info.conduit);
	put_device(ppe->info.dev);
	kfree(ppe);
	mutex_unlock(&detachment_lock);
}
EXPORT_SYMBOL_GPL(qdx_ppe_detach);

void qdx_ethernet_unregister(struct qdx *qdx)
{
	struct qdx_ethernet *eth;

	mutex_lock(&detachment_lock);
	eth = qdx->ethernet;
	if (!eth) {
		mutex_unlock(&detachment_lock);
		return;
	}
	unregister_netdevice_notifier(&eth->netdev_notifier);
	cancel_work_sync(&eth->mac_work);
	qdx_interfaces_unregister(qdx);
	/* Native configuration readers hold these locks before using QDX. */
	rtnl_lock();
	if (eth->ppe)
		qdx_ports_lock(eth);
	mutex_lock(&attachment_lock);
	if (eth->edma)
		eth->edma->qdx = NULL;
	if (eth->ppe)
		eth->ppe->qdx = NULL;
	ethernet_instance = NULL;
	mutex_unlock(&attachment_lock);
	if (eth->ppe)
		qdx_ports_unlock(eth);
	rtnl_unlock();
	synchronize_net();
	kfree(eth);
	qdx->ethernet = NULL;
	mutex_unlock(&detachment_lock);
}

int qdx_ethernet_prepare(struct qdx *qdx)
{
	struct qdx_ethernet *eth = qdx->ethernet;
	int ret;

	rtnl_lock();
	qdx_ports_lock(eth);
	if (eth->edma->detaching || eth->ppe->detaching || atomic_read(&qdx->failure)) {
		ret = -ENODEV;
		goto out;
	}
	ret = qdx_interfaces_register(qdx);
	if (ret)
		goto out;
	WRITE_ONCE(eth->transport, QDX_ETH_TRANSITION);
	eth->native_touched = true;
	ret = eth->edma->info.ops->quiesce(eth->edma->info.context,
					    qdx->limits.native_rx_entries);
	if (ret)
		goto out;
	eth->prepared = true;
	return 0;
out:
	qdx_ports_unlock(eth);
	rtnl_unlock();
	return ret;
}

void qdx_ethernet_unlock(struct qdx *qdx)
{
	struct qdx_ethernet *eth = qdx->ethernet;

	if (!eth->prepared)
		return;
	eth->prepared = false;
	qdx_ports_unlock(eth);
	rtnl_unlock();
}

int qdx_ethernet_start(struct qdx *qdx)
{
	struct qdx_ethernet *eth = qdx->ethernet;
	unsigned int i;
	int ret = 0;

	for (i = 1; i < QDX_PHYSICAL_PORTS; i++) {
		struct qdx_port_state state;

		if (!eth->ports[i].registered)
			continue;
		ret = eth->ppe->info.ops->snapshot(eth->ppe->info.context, i, &state);
		if (!ret)
			ret = qdx_port_apply(&eth->ports[i], &state);
		if (ret)
			break;
	}
	if (!ret)
		ret = eth->edma->info.ops->resume_shared(eth->edma->info.context);
	if (!ret)
		ret = eth->edma->info.ops->select(eth->edma->info.context,
						QDX_ETH_NATIVE);
	if (!ret) {
		eth->users = 0;
		WRITE_ONCE(eth->transport, QDX_ETH_NATIVE);
		qdx_ethernet_wake(qdx);
	}
	qdx_ethernet_unlock(qdx);
	return ret;
}

void qdx_ethernet_stop(struct qdx *qdx)
{
	struct qdx_ethernet *eth = qdx->ethernet;
	unsigned int i;

	cancel_work_sync(&eth->mac_work);
	if (eth->native_touched)
		WRITE_ONCE(eth->transport, QDX_ETH_UNAVAILABLE);
	for (i = 1; i < QDX_PHYSICAL_PORTS; i++)
		qdx_port_withdraw(&eth->ports[i]);
	if (eth->edma && eth->native_touched)
		netif_tx_disable(eth->edma->info.conduit);
	/* Finish native writes already in progress before hardware holds. New
	 * destructive changes now fail prepare while retaining desired state.
	 */
	if (eth->ppe && eth->native_touched) {
		rtnl_lock();
		qdx_ports_lock(eth);
		qdx_ports_unlock(eth);
		rtnl_unlock();
	}
}

int qdx_ethernet_restore(struct qdx *qdx)
{
	struct qdx_ethernet *eth = qdx->ethernet;
	int ret;

	if (!eth->native_touched)
		return 0;
	if (!eth->edma || !eth->ppe)
		return -ENODEV;
	rtnl_lock();
	qdx_ports_lock(eth);
	ret = eth->edma->info.ops->restore(eth->edma->info.context);
	if (!ret)
		ret = eth->ppe->info.ops->restore(eth->ppe->info.context);
	if (!ret)
		eth->native_restored = true;
	qdx_ports_unlock(eth);
	rtnl_unlock();
	return ret;
}

int qdx_ethernet_activate(struct qdx *qdx)
{
	struct qdx_ethernet *eth = qdx->ethernet;
	int ret;

	if (!eth->native_touched || !eth->native_restored)
		return 0;
	rtnl_lock();
	ret = eth->edma->info.ops->activate(eth->edma->info.context);
	if (ret) {
		rtnl_unlock();
		return ret;
	}
	WRITE_ONCE(eth->transport, QDX_ETH_NATIVE);
	eth->native_touched = false;
	if (!eth->edma->detaching && netif_running(eth->edma->info.conduit))
		netif_wake_queue(eth->edma->info.conduit);
	rtnl_unlock();
	return 0;
}

enum qdx_eth_transport qdx_edma_transport(struct qdx_edma *edma)
{
	if (!edma || !READ_ONCE(edma->qdx))
		return QDX_ETH_NATIVE;
	return READ_ONCE(edma->qdx->ethernet->transport);
}
EXPORT_SYMBOL_GPL(qdx_edma_transport);

/* RTNL serializes resource users and the shared wired transport. */
static int qdx_ethernet_select(struct qdx *qdx, enum qdx_eth_transport target)
{
	struct qdx_ethernet *eth = qdx->ethernet;
	enum qdx_eth_transport old;
	unsigned int i;
	int ret = 0, undo;

	ASSERT_RTNL();
	if (!eth->edma || !eth->ppe)
		return -ENODEV;
	qdx_ports_lock(eth);
	if (eth->edma->detaching || eth->ppe->detaching ||
	    READ_ONCE(qdx->state) != QDX_READY || atomic_read(&qdx->failure)) {
		ret = -EIO;
		goto out;
	}
	old = eth->transport;
	if (old == target)
		goto out;
	if ((old != QDX_ETH_NATIVE && old != QDX_ETH_FIRMWARE) ||
	    (target != QDX_ETH_NATIVE && target != QDX_ETH_FIRMWARE)) {
		ret = -EAGAIN;
		goto out;
	}

	/* A queued MAC notifier must not leave the first user with stale state. */
	for (i = 1; i < QDX_PHYSICAL_PORTS; i++) {
		struct qdx_port *port = &eth->ports[i];
		struct qdx_port_state state;

		if (!port->registered)
			continue;
		ret = eth->ppe->info.ops->snapshot(eth->ppe->info.context, i, &state);
		if (!ret && !ether_addr_equal(port->confirmed.mac, state.mac))
			ret = qdx_port_apply(port, &state);
		if (ret) {
			qdx_port_withdraw(port);
			qdx_fail(qdx, ret);
			goto out;
		}
	}
	for (i = 1; i < QDX_PHYSICAL_PORTS; i++)
		WRITE_ONCE(eth->ports[i].changing, true);
	WRITE_ONCE(eth->transport, QDX_ETH_TRANSITION);
	netif_tx_disable(eth->edma->info.conduit);
	for (i = 1; i < QDX_PHYSICAL_PORTS; i++)
		qdx_port_withdraw(&eth->ports[i]);

	ret = eth->edma->info.ops->select(eth->edma->info.context, target);
	if (ret) {
		undo = eth->edma->info.ops->select(eth->edma->info.context, old);
		if (undo || target == QDX_ETH_NATIVE) {
			WRITE_ONCE(eth->transport, QDX_ETH_UNAVAILABLE);
			qdx_fail(qdx, undo ? undo : ret);
		} else {
			WRITE_ONCE(eth->transport, old);
		}
	} else {
		WRITE_ONCE(eth->transport, target);
	}
	for (i = 1; i < QDX_PHYSICAL_PORTS; i++)
		WRITE_ONCE(eth->ports[i].changing, false);
	if (!atomic_read(&qdx->failure)) {
		for (i = 1; i < QDX_PHYSICAL_PORTS; i++)
			qdx_port_publish(&eth->ports[i]);
		qdx_ethernet_wake(qdx);
	}
out:
	qdx_ports_unlock(eth);
	return ret;
}

int qdx_ethernet_acquire(struct qdx *qdx)
{
	struct qdx_ethernet *eth = qdx->ethernet;
	int ret;

	ASSERT_RTNL();
	if (READ_ONCE(qdx->state) != QDX_READY || atomic_read(&qdx->failure))
		return -EAGAIN;
	if (!eth->edma || !eth->ppe || eth->edma->detaching || eth->ppe->detaching)
		return -ENODEV;
	if (eth->users == UINT_MAX)
		return -EOVERFLOW;
	if (eth->users) {
		eth->users++;
		return 0;
	}
	ret = qdx_ethernet_select(qdx, QDX_ETH_FIRMWARE);
	if (!ret)
		eth->users = 1;
	return ret;
}

int qdx_ethernet_release(struct qdx *qdx)
{
	struct qdx_ethernet *eth = qdx->ethernet;
	int ret;

	ASSERT_RTNL();
	if (!eth->users)
		return -EINVAL;
	if (--eth->users)
		return 0;
	if (READ_ONCE(qdx->state) != QDX_READY || atomic_read(&qdx->failure))
		return -EIO;
	ret = qdx_ethernet_select(qdx, QDX_ETH_NATIVE);
	if (ret) {
		WRITE_ONCE(eth->transport, QDX_ETH_UNAVAILABLE);
		if (eth->edma)
			netif_tx_disable(eth->edma->info.conduit);
		qdx_fail(qdx, ret);
	}
	return ret;
}

int qdx_port_prepare(struct qdx_ppe *ppe, unsigned int number)
{
	struct qdx_port *port;
	__le32 down = 0;
	int ret;

	if (!ppe || !ppe->qdx || number == 0)
		return 0;
	if (number >= QDX_PHYSICAL_PORTS)
		return -EINVAL;
	port = &ppe->qdx->ethernet->ports[number];
	mutex_lock(&port->lock);
	if (!ppe->qdx->ethernet->native_touched)
		return 0;
	if (atomic_read(&ppe->qdx->failure)) {
		mutex_unlock(&port->lock);
		return -EIO;
	}
	WRITE_ONCE(port->changing, true);
	qdx_port_withdraw(port);
	if (!port->opened || !port->link_state)
		return 0;
	ret = qdx_port_command(port, QDX_IF_LINK, &down, sizeof(down));
	if (ret) {
		/* A rejected/unsubmitted command leaves the confirmed link intact. */
		if (!atomic_read(&ppe->qdx->failure)) {
			WRITE_ONCE(port->changing, false);
			qdx_port_publish(port);
		}
		mutex_unlock(&port->lock);
		if (!atomic_read(&ppe->qdx->failure))
			qdx_ethernet_wake(ppe->qdx);
		return ret;
	}
	port->link_state = 0;
	return 0;
}
EXPORT_SYMBOL_GPL(qdx_port_prepare);

int qdx_port_finish(struct qdx_ppe *ppe, unsigned int number)
{
	struct qdx_port_state state;
	struct qdx_port *port;
	int ret = 0;

	if (!ppe || !ppe->qdx || number == 0)
		return 0;
	port = &ppe->qdx->ethernet->ports[number];
	if (atomic_read(&ppe->qdx->failure) &&
	    ppe->qdx->ethernet->native_touched) {
		ret = -EIO;
	} else if (ppe->qdx->ethernet->native_touched) {
		ret = ppe->info.ops->snapshot(ppe->info.context, number, &state);
		if (!ret)
			ret = qdx_port_apply(port, &state);
	}
	WRITE_ONCE(port->changing, false);
	if (!ret)
		qdx_port_publish(port);
	mutex_unlock(&port->lock);
	qdx_ethernet_wake(ppe->qdx);
	return ret;
}
EXPORT_SYMBOL_GPL(qdx_port_finish);

int qdx_port_check_mtu(struct qdx_ppe *ppe, unsigned int number, unsigned int mtu)
{
	struct qdx_port *port;
	int ret = 0;

	if (!ppe || !ppe->qdx || !number)
		return 0;
	if (number >= QDX_PHYSICAL_PORTS)
		return -EINVAL;
	port = &ppe->qdx->ethernet->ports[number];
	mutex_lock(&port->lock);
	if (ppe->qdx->ethernet->native_touched) {
		ret = atomic_read(&ppe->qdx->failure);
		if (!ret)
			ret = qdx_io_check_mtu(ppe->qdx, mtu);
	}
	mutex_unlock(&port->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(qdx_port_check_mtu);

int qdx_port_mtu(struct qdx_ppe *ppe, unsigned int number, unsigned int mtu)
{
	struct qdx_port *port;
	__le16 wire_mtu = cpu_to_le16(mtu);
	int ret;

	if (!ppe || !ppe->qdx || number == 0 ||
	    !ppe->qdx->ethernet->native_touched)
		return 0;
	port = &ppe->qdx->ethernet->ports[number];
	lockdep_assert_held(&port->lock);
	ret = qdx_io_set_mtu(ppe->qdx, mtu);
	if (!ret)
		ret = qdx_port_command(port, QDX_IF_MTU, &wire_mtu, sizeof(wire_mtu));
	if (!ret)
		port->confirmed.mtu = mtu;
	return ret;
}
EXPORT_SYMBOL_GPL(qdx_port_mtu);

void qdx_port_failed(struct qdx_ppe *ppe, unsigned int number, int error)
{
	if (!ppe || !ppe->qdx || number >= QDX_PHYSICAL_PORTS)
		return;
	qdx_port_withdraw(&ppe->qdx->ethernet->ports[number]);
	qdx_fail(ppe->qdx, error);
}
EXPORT_SYMBOL_GPL(qdx_port_failed);

int qdx_port_open(struct qdx_ppe *ppe, unsigned int number)
{
	int ret = qdx_port_prepare(ppe, number);

	if (ret)
		return ret;
	return qdx_port_finish(ppe, number);
}
EXPORT_SYMBOL_GPL(qdx_port_open);

int qdx_port_close(struct qdx_ppe *ppe, unsigned int number)
{
	int ret = qdx_port_prepare(ppe, number);

	if (!ret)
		ret = qdx_port_finish(ppe, number);
	if (ret && ppe && ppe->qdx)
		qdx_fail(ppe->qdx, ret);
	return ret;
}
EXPORT_SYMBOL_GPL(qdx_port_close);

void qdx_ethernet_wake(struct qdx *qdx)
{
	struct qdx_ethernet *eth = qdx->ethernet;
	struct net_device *dev;
	unsigned int i;

	if (!eth->edma || eth->edma->detaching)
		return;
	for (i = 1; i < QDX_PHYSICAL_PORTS; i++)
		if (READ_ONCE(eth->ports[i].changing))
			return;
	if (READ_ONCE(eth->transport) == QDX_ETH_NATIVE) {
		eth->edma->info.ops->wake(eth->edma->info.context);
		return;
	}
	if (READ_ONCE(eth->transport) != QDX_ETH_FIRMWARE ||
	    atomic_read(&qdx->failure) || !qdx_io_has_space(qdx))
		return;
	dev = eth->edma->info.conduit;
	if (!netif_running(dev))
		return;
	for (i = 1; i < QDX_PHYSICAL_PORTS; i++) {
		if (READ_ONCE(eth->ports[i].available)) {
			netif_wake_queue(dev);
			return;
		}
	}
}

netdev_tx_t qdx_ethernet_xmit(struct qdx_edma *edma, struct sk_buff *skb,
			     unsigned int number)
{
	struct qdx_port *port;
	struct sk_buff *packet = skb;
	struct net_device *dev = edma->info.conduit;
	unsigned int bytes = skb->len;
	int ret;

	if (qdx_edma_transport(edma) == QDX_ETH_TRANSITION) {
		netif_stop_queue(dev);
		smp_mb();
		if (qdx_edma_transport(edma) != QDX_ETH_TRANSITION)
			netif_wake_queue(dev);
		return NETDEV_TX_BUSY;
	}
	if (qdx_edma_transport(edma) != QDX_ETH_FIRMWARE || edma->detaching)
		goto drop;
	port = qdx_port_get(edma->qdx, 0, number);
	if (!port) {
		if (number < QDX_PHYSICAL_PORTS &&
		    READ_ONCE(edma->qdx->ethernet->ports[number].changing)) {
			netif_stop_queue(dev);
			smp_mb();
			qdx_ethernet_wake(edma->qdx);
			return NETDEV_TX_BUSY;
		}
		goto drop;
	}
	if (unlikely(skb->len < ETH_HLEN || skb_is_gso(skb) ||
		     skb->len > edma->info.max_frame))
		goto put_drop;
	if (!qdx_io_has_space(edma->qdx)) {
		netif_stop_queue(dev);
		smp_mb();
		if (!qdx_io_has_space(edma->qdx)) {
			qdx_port_put(port);
			return NETDEV_TX_BUSY;
		}
		netif_wake_queue(dev);
	}
	if (skb_is_nonlinear(skb) || skb->ip_summed == CHECKSUM_PARTIAL ||
	    skb->len < edma->info.tx_min_size) {
		packet = skb_copy_expand(skb, skb_headroom(skb),
					max_t(unsigned int, skb_tailroom(skb),
					      edma->info.tx_min_size), GFP_ATOMIC);
		if (!packet)
			goto put_drop;
		if (packet->ip_summed == CHECKSUM_PARTIAL && skb_checksum_help(packet))
			goto free_copy;
		if (skb_put_padto(packet, edma->info.tx_min_size)) {
			packet = NULL; /* skb_put_padto consumes allocation on failure. */
			goto put_drop;
		}
	}
	ret = qdx_io_send(port, packet, netdev_get_tx_queue(dev, 0), bytes);
	if (!ret) {
		if (packet != skb)
			dev_consume_skb_any(skb);
		qdx_port_put(port);
		return NETDEV_TX_OK;
	}
	if (packet != skb)
		dev_kfree_skb_any(packet);
	if (ret == -ENOSPC || ret == -EAGAIN ||
	    (ret == -ESHUTDOWN && READ_ONCE(port->changing) &&
	     !atomic_read(&edma->qdx->failure))) {
		netif_stop_queue(dev);
		smp_mb();
		qdx_ethernet_wake(edma->qdx);
		qdx_port_put(port);
		return NETDEV_TX_BUSY;
	}
	goto put_drop;
free_copy:
	dev_kfree_skb_any(packet);
put_drop:
	qdx_port_put(port);
drop:
	dev->stats.tx_dropped++;
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}
EXPORT_SYMBOL_GPL(qdx_ethernet_xmit);

void qdx_ethernet_receive(struct qdx *qdx, unsigned int core, u32 ifnum,
			struct sk_buff *skb, struct napi_struct *napi)
{
	struct qdx_port *port = qdx_port_get(qdx, core, ifnum);
	struct qdx_edma *edma = qdx->ethernet->edma;

	if (!port || !edma || edma->detaching || !netif_running(port->netdev)) {
		if (port)
			qdx_port_put(port);
		dev_kfree_skb_any(skb);
		return;
	}
	edma->info.ops->receive(edma->info.context, ifnum, skb, napi);
	qdx_port_put(port);
}
