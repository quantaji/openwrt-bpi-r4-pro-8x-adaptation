/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _QDX_H
#define _QDX_H

#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/dma-mapping.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/notifier.h>
#include <linux/platform_device.h>
#include <linux/qdx.h>
#include <linux/refcount.h>
#include <linux/sizes.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#include "abi.h"

#define QDX_PHYSICAL_PORTS 8
#define QDX_REQUESTS 64
#define QDX_SLOTS 65536U

struct dentry;
struct seq_file;
struct qdx;
struct qdx_hw;
struct qdx_mem;
struct qdx_io;
struct qdx_commands;

enum qdx_state {
	QDX_WAITING, QDX_STARTING, QDX_READY, QDX_STOPPING, QDX_TERMINAL,
};

enum qdx_command_outcome {
	QDX_NOT_SUBMITTED, QDX_ACK, QDX_REJECTED, QDX_UNKNOWN,
};

struct qdx_reply {
	enum qdx_command_outcome outcome;
	void *data;
	u16 min_len;
	u16 max_len;
	u16 len;
	u32 firmware_error;
};

struct qdx_core {
	struct qdx *qdx;
	unsigned int id;
	struct qdx_mem *mem;
	struct qdx_io *io;
	struct qdx_commands *commands;
	void __iomem *csm;
	phys_addr_t csm_phys;
	void __iomem *imem;
	phys_addr_t imem_phys;
	size_t imem_size;
	void __iomem *image;
	phys_addr_t image_phys;
	size_t image_size;
	struct qdx_ifmap *map;
	struct qdx_h2n_desc *h2n_desc[QDX_H2N_RINGS];
	struct qdx_n2h_desc *n2h_desc[QDX_N2H_RINGS];
	dma_addr_t c2c_dma;
	bool boot_signaled;
	bool map_ready;
	bool common_ready;
	bool peer_ready;
};

struct qdx_edma {
	struct qdx_edma_info info;
	struct qdx *qdx;
	bool detaching;
};

struct qdx_ppe {
	struct qdx_ppe_info info;
	struct qdx *qdx;
	bool detaching;
};

struct qdx_port {
	struct qdx *qdx;
	struct net_device *netdev;
	struct net_device *conduit;
	struct mutex lock;
	refcount_t refs;
	wait_queue_head_t drained;
	u32 ifnum;
	bool registered;
	bool available;
	bool configured;
	bool opened;
	bool changing;
	u32 link_state;
	struct qdx_port_state confirmed;
};

struct qdx_ethernet {
	struct qdx_edma *edma;
	struct qdx_ppe *ppe;
	struct qdx_port ports[QDX_PHYSICAL_PORTS];
	enum qdx_eth_transport transport;
	unsigned int users;
	bool prepared;
	bool native_touched;
	bool native_restored;
	struct notifier_block netdev_notifier;
	struct work_struct mac_work;
};

struct qdx_limits {
	u32 native_rx_entries;
	u32 tx_slots;
	unsigned long rx_dma_bytes;
	unsigned long rx_memory_bytes;
};

struct qdx_diagnosis {
	struct dentry *root;
	struct mutex lock;
	bool wired_use;
};

struct qdx {
	struct device *dev;
	struct platform_device *pdev;
	struct qdx_hw *hw;
	struct qdx_core cores[QDX_CORES];
	struct qdx_ethernet *ethernet;
	enum qdx_state state;
	atomic_t failure;
	struct work_struct lifecycle;
	struct completion terminal_done;
	struct qdx_diagnosis diagnosis;
	struct qdx_limits limits;
	atomic_long_t rx_dma_used;
	atomic_long_t rx_memory_charged;
	bool execution_possible;
	bool access_ended;
};

void qdx_diagnosis_register(struct qdx *qdx);
void qdx_diagnosis_remove(struct qdx *qdx);
void qdx_io_diagnose(struct qdx_core *core, struct seq_file *seq);
int qdx_io_pause_returns(struct qdx *qdx, unsigned int msecs);

void qdx_fail(struct qdx *qdx, int error);
void qdx_schedule(struct qdx *qdx);
int qdx_hw_get(struct qdx *qdx);
int qdx_hw_prepare(struct qdx *qdx);
int qdx_hw_start_core(struct qdx_core *core);
int qdx_hw_stop(struct qdx *qdx);
int qdx_hw_notify(struct qdx_core *core, unsigned int channel);
void qdx_hw_unprepare(struct qdx *qdx);
int qdx_mem_prepare(struct qdx *qdx);
int qdx_mem_validate_map(struct qdx_core *core);
void qdx_mem_release(struct qdx *qdx, bool access_ended);

int qdx_io_init(struct qdx_core *core);
void qdx_io_enable(struct qdx_core *core);
void qdx_io_close(struct qdx_core *core);
void qdx_io_stop(struct qdx_core *core);
void qdx_io_release(struct qdx_core *core, bool access_ended);
int qdx_io_bootstrap(struct qdx_core *core);
int qdx_io_set_pool(struct qdx_core *core, bool paged, u32 pool, u32 low, u32 high);
int qdx_io_check_mtu(struct qdx *qdx, unsigned int mtu);
int qdx_io_set_mtu(struct qdx *qdx, unsigned int mtu);
bool qdx_io_has_space(struct qdx *qdx);
void qdx_io_port_close(struct qdx_port *port);
int qdx_io_send(struct qdx_port *port, struct sk_buff *skb,
		struct netdev_queue *queue, unsigned int bytes);
int qdx_io_command(struct qdx_core *core, void *buffer, size_t length, u64 id);

int qdx_commands_init(struct qdx_core *core);
void qdx_commands_stop(struct qdx_core *core, int error);
void qdx_commands_release(struct qdx_core *core);
int qdx_command(struct qdx_core *core, u32 ifnum, u32 opcode,
		const void *body, size_t length, struct qdx_reply *reply);
void qdx_command_receive(struct qdx_core *core, u32 ifnum,
			 const void *buffer, size_t length);
void qdx_command_return(struct qdx_core *core, u64 id, u8 response);

int qdx_ethernet_register(struct qdx *qdx);
void qdx_ethernet_unregister(struct qdx *qdx);
bool qdx_ethernet_ready(struct qdx *qdx);
int qdx_ethernet_prepare(struct qdx *qdx);
int qdx_ethernet_start(struct qdx *qdx);
void qdx_ethernet_unlock(struct qdx *qdx);
int qdx_ethernet_acquire(struct qdx *qdx);
int qdx_ethernet_release(struct qdx *qdx);
void qdx_ethernet_stop(struct qdx *qdx);
int qdx_ethernet_restore(struct qdx *qdx);
int qdx_ethernet_activate(struct qdx *qdx);
void qdx_ethernet_receive(struct qdx *qdx, unsigned int core, u32 ifnum,
			  struct sk_buff *skb, struct napi_struct *napi);
void qdx_ethernet_wake(struct qdx *qdx);
void qdx_port_publish(struct qdx_port *port);
void qdx_port_withdraw(struct qdx_port *port);
struct qdx_port *qdx_port_get(struct qdx *qdx, unsigned int core, u32 ifnum);
void qdx_port_put(struct qdx_port *port);
void qdx_interfaces_init(struct qdx *qdx);
int qdx_interfaces_register(struct qdx *qdx);
void qdx_interfaces_unregister(struct qdx *qdx);

#endif
