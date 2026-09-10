// SPDX-License-Identifier: GPL-2.0-only
/* Diagnostic callers and observations of the same production operations. */
#include <linux/debugfs.h>
#include <linux/module.h>
#include <linux/rtnetlink.h>
#include <linux/seq_file.h>

#include "qdx.h"

static int qdx_wired_use_get(void *data, u64 *value)
{
	struct qdx *qdx = data;

	mutex_lock(&qdx->diagnosis.lock);
	rtnl_lock();
	*value = qdx->diagnosis.wired_use;
	rtnl_unlock();
	mutex_unlock(&qdx->diagnosis.lock);
	return 0;
}

static int qdx_wired_use_set(void *data, u64 value)
{
	struct qdx *qdx = data;
	int ret = 0;

	if (value > 1)
		return -EINVAL;
	mutex_lock(&qdx->diagnosis.lock);
	rtnl_lock();
	if (value && !qdx->diagnosis.wired_use) {
		ret = qdx_ethernet_acquire(qdx);
		if (!ret)
			qdx->diagnosis.wired_use = true;
	} else if (!value && qdx->diagnosis.wired_use) {
		ret = qdx_ethernet_release(qdx);
		/* The formal release consumes this reference even on failure. */
		qdx->diagnosis.wired_use = false;
	}
	rtnl_unlock();
	mutex_unlock(&qdx->diagnosis.lock);
	return ret;
}
DEFINE_DEBUGFS_ATTRIBUTE(qdx_wired_use_fops, qdx_wired_use_get,
			 qdx_wired_use_set, "%llu\n");

static int qdx_diagnostic_state_show(struct seq_file *seq, void *unused)
{
	static const char * const states[] = {
		"waiting", "starting", "ready", "stopping", "terminal"
	};
	static const char * const transports[] = {
		"native", "transition", "firmware", "unavailable"
	};
	struct qdx *qdx = seq->private;
	struct qdx_ethernet *eth;
	enum qdx_state state;
	unsigned int i;
	int ret = 0;

	mutex_lock(&qdx->diagnosis.lock);
	rtnl_lock();
	eth = qdx->ethernet;
	state = READ_ONCE(qdx->state);
	seq_printf(seq,
		   "state=%s error=%d execution_possible=%u access_ended=%u\n",
		   states[state], atomic_read(&qdx->failure),
		   READ_ONCE(qdx->execution_possible), READ_ONCE(qdx->access_ended));
	seq_printf(seq, "transport=%s users=%u diagnostic_use=%u\n",
		   transports[READ_ONCE(eth->transport)], eth->users,
		   qdx->diagnosis.wired_use);
	seq_printf(seq, "native_rx_entries=%u tx_slots_per_core=%u\n",
		   qdx->limits.native_rx_entries, qdx->limits.tx_slots);
	seq_printf(seq, "rx_dma_limit_bytes=%lu rx_dma_used_bytes=%ld\n",
		   qdx->limits.rx_dma_bytes, atomic_long_read(&qdx->rx_dma_used));
	seq_printf(seq, "rx_memory_limit_bytes=%lu rx_memory_charged_bytes=%ld\n",
		   qdx->limits.rx_memory_bytes,
		   atomic_long_read(&qdx->rx_memory_charged));
	for (i = 0; i < QDX_CORES; i++) {
		struct qdx_core *core = &qdx->cores[i];

		seq_printf(seq, "core=%u map=%u common=%u peer=%u\n", i,
			   READ_ONCE(core->map_ready), READ_ONCE(core->common_ready),
			   READ_ONCE(core->peer_ready));
		qdx_io_diagnose(core, seq);
	}
	/* Detach can free the native attachment after terminal completion. */
	if (state == QDX_READY && eth->edma && !READ_ONCE(eth->edma->detaching))
		ret = eth->edma->info.ops->diagnose(eth->edma->info.context, seq);
	else
		seq_puts(seq, "native_observation=unavailable\n");
	rtnl_unlock();
	mutex_unlock(&qdx->diagnosis.lock);
	return ret;
}
DEFINE_SHOW_ATTRIBUTE(qdx_diagnostic_state);

static int qdx_command_check_show(struct seq_file *seq, void *unused)
{
	struct qdx_pool_info request = {}, response;
	struct qdx *qdx = seq->private;
	struct qdx_reply reply;
	unsigned int i;
	u32 pool, low, high;
	int ret;

	mutex_lock(&qdx->diagnosis.lock);
	if (READ_ONCE(qdx->state) != QDX_READY || atomic_read(&qdx->failure)) {
		mutex_unlock(&qdx->diagnosis.lock);
		return -EBUSY;
	}
	for (i = 0; i < QDX_CORES; i++) {
		reply = (struct qdx_reply) {
			.data = &response,
			.min_len = sizeof(response),
			.max_len = sizeof(response),
		};
		ret = qdx_command(&qdx->cores[i], QDX_IF_N2H, QDX_N2H_GET_POOL,
				  &request, sizeof(request), &reply);
		if (ret) {
			seq_printf(seq, "core=%u error=%d outcome=%u firmware_error=%u\n",
				   i, ret, reply.outcome, reply.firmware_error);
			continue;
		}
		pool = be32_to_cpu(response.pool);
		low = be32_to_cpu(response.low);
		high = be32_to_cpu(response.high);
		ret = low > high ? -EPROTO : 0;
		seq_printf(seq,
			   "core=%u error=%d outcome=%u firmware_error=%u pool=%u low=%u high=%u\n",
			   i, ret, reply.outcome, reply.firmware_error, pool, low, high);
	}
	mutex_unlock(&qdx->diagnosis.lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(qdx_command_check);

static int qdx_diagnostic_fail_set(void *data, u64 value)
{
	struct qdx *qdx = data;
	int ret = 0;

	if (value != 1)
		return -EINVAL;
	mutex_lock(&qdx->diagnosis.lock);
	if (READ_ONCE(qdx->state) != QDX_READY || atomic_read(&qdx->failure))
		ret = -EBUSY;
	else
		qdx_fail(qdx, -EIO);
	mutex_unlock(&qdx->diagnosis.lock);
	return ret;
}
DEFINE_DEBUGFS_ATTRIBUTE(qdx_diagnostic_fail_fops, NULL,
			 qdx_diagnostic_fail_set, "%llu\n");

static int qdx_pause_returns_set(void *data, u64 value)
{
	struct qdx *qdx = data;
	int ret;

	if (!value || value > 250)
		return -EINVAL;
	/* Refuse overlapping pauses rather than queueing another service stop. */
	if (!mutex_trylock(&qdx->diagnosis.lock))
		return -EBUSY;
	rtnl_lock();
	ret = qdx_io_pause_returns(qdx, value);
	rtnl_unlock();
	mutex_unlock(&qdx->diagnosis.lock);
	return ret;
}
DEFINE_DEBUGFS_ATTRIBUTE(qdx_pause_returns_fops, NULL,
			 qdx_pause_returns_set, "%llu\n");

void qdx_diagnosis_register(struct qdx *qdx)
{
	struct dentry *root;

	/* Formal registration already enforces one live QDX instance. */
	root = debugfs_create_dir("qdx", NULL);
	if (IS_ERR(root))
		return;
	qdx->diagnosis.root = root;
	debugfs_create_file("wired_use", 0600, root, qdx, &qdx_wired_use_fops);
	debugfs_create_file("state", 0400, root, qdx, &qdx_diagnostic_state_fops);
	debugfs_create_file("command_check", 0400, root, qdx, &qdx_command_check_fops);
	debugfs_create_file("fail", 0600, root, qdx, &qdx_diagnostic_fail_fops);
	debugfs_create_file("pause_returns_ms", 0600, root, qdx, &qdx_pause_returns_fops);
}

void qdx_diagnosis_remove(struct qdx *qdx)
{
	/* Drain proxy-protected handlers before taking the locks they use. */
	debugfs_remove(qdx->diagnosis.root);
	qdx->diagnosis.root = NULL;
	mutex_lock(&qdx->diagnosis.lock);
	rtnl_lock();
	if (qdx->diagnosis.wired_use) {
		qdx_ethernet_release(qdx);
		qdx->diagnosis.wired_use = false;
	}
	rtnl_unlock();
	mutex_unlock(&qdx->diagnosis.lock);
}
