// SPDX-License-Identifier: GPL-2.0-only
/* NSS command results have a lifetime separate from their DMA carriers. */
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/list.h>
#include <linux/limits.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/xarray.h>

#include "qdx.h"

#define QDX_HOST_TAG		0x514458434f4d4d31ULL
#define QDX_COMMAND_TIMEOUT	msecs_to_jiffies(3000)
#define QDX_COMMAND_PAYLOAD	(QDX_COMMAND_SIZE - sizeof(struct qdx_cmn))
#define QDX_COMMAND_RETURN_MAX	5

struct qdx_request {
	refcount_t refs;
	struct completion done;
	struct list_head stopped;
	u64 id;
	u32 ifnum;
	u32 opcode;
	u16 min_len;
	u16 max_len;
	u16 capacity;
	u16 len;
	u32 firmware_error;
	enum qdx_command_outcome outcome;
	int error;
	u8 data[];
};

struct qdx_commands {
	struct xarray pending;
	wait_queue_head_t drained;
	unsigned int active;
	bool closed;
	atomic64_t notifications;
	atomic64_t malformed;
	atomic64_t late;
};

/* This ID namespace survives every per-core context for the module lifetime. */
static atomic64_t qdx_command_id = ATOMIC64_INIT(0);

static void qdx_request_put(struct qdx_request *request)
{
	if (refcount_dec_and_test(&request->refs))
		kfree(request);
}

int qdx_commands_init(struct qdx_core *core)
{
	struct qdx_commands *commands;

	commands = kzalloc(sizeof(*commands), GFP_KERNEL);
	if (!commands)
		return -ENOMEM;

	xa_init_flags(&commands->pending, XA_FLAGS_LOCK_IRQ);
	init_waitqueue_head(&commands->drained);
	atomic64_set(&commands->notifications, 0);
	atomic64_set(&commands->malformed, 0);
	atomic64_set(&commands->late, 0);
	core->commands = commands;
	return 0;
}

int qdx_command(struct qdx_core *core, u32 ifnum, u32 opcode,
		const void *body, size_t length, struct qdx_reply *reply)
{
	struct qdx_commands *commands = core->commands;
	struct qdx_request *request;
	struct qdx_request *removed;
	struct qdx_cmn *message;
	unsigned long flags, deadline, now, remaining;
	s64 previous_id;
	u16 capacity;
	bool drained;
	int error;

	if (!reply)
		return -EINVAL;

	reply->outcome = QDX_NOT_SUBMITTED;
	reply->len = 0;
	reply->firmware_error = 0;
	if (!commands)
		return -ENODEV;
	if ((length && !body) || length > QDX_COMMAND_PAYLOAD ||
	    reply->max_len > QDX_COMMAND_PAYLOAD ||
	    reply->min_len > reply->max_len || ifnum > 0xffffff)
		return -EINVAL;

	/* Count callers before allocation, including those not yet published. */
	xa_lock_irqsave(&commands->pending, flags);
	if (commands->closed)
		error = -ESHUTDOWN;
	else if (commands->active == QDX_REQUESTS)
		error = -EAGAIN;
	else {
		commands->active++;
		error = 0;
	}
	xa_unlock_irqrestore(&commands->pending, flags);
	if (error)
		return error;

	capacity = max_t(size_t, length, reply->max_len);
	request = kzalloc(struct_size(request, data, capacity), GFP_KERNEL);
	if (!request) {
		error = -ENOMEM;
		goto out_active;
	}
	refcount_set(&request->refs, 1);
	init_completion(&request->done);
	INIT_LIST_HEAD(&request->stopped);
	request->ifnum = ifnum;
	request->opcode = opcode;
	request->min_len = reply->min_len;
	request->max_len = reply->max_len;
	request->capacity = capacity;
	request->outcome = QDX_UNKNOWN;
	request->error = -EINPROGRESS;

	previous_id = atomic64_read(&qdx_command_id);
	do {
		if ((u64)previous_id == U64_MAX) {
			error = -EOVERFLOW;
			goto out_request;
		}
	} while (!atomic64_try_cmpxchg(&qdx_command_id, &previous_id,
				      (u64)previous_id + 1));
	request->id = (u64)previous_id + 1;

	message = kzalloc(QDX_COMMAND_SIZE, GFP_KERNEL);
	if (!message) {
		error = -ENOMEM;
		goto out_request;
	}
	message->version = cpu_to_le16(QDX_MESSAGE_VERSION);
	message->length = cpu_to_le16(length);
	message->interface = cpu_to_le32(ifnum);
	message->type = cpu_to_le32(opcode);
	message->host_tag = cpu_to_le64(QDX_HOST_TAG);
	message->request_id = cpu_to_le64(request->id);
	if (length)
		memcpy(message + 1, body, length);

	xa_lock_irqsave(&commands->pending, flags);
	if (commands->closed) {
		error = -ESHUTDOWN;
	} else {
		error = __xa_insert(&commands->pending, request->id, request,
				    GFP_ATOMIC);
		if (!error)
			refcount_inc(&request->refs);
	}
	xa_unlock_irqrestore(&commands->pending, flags);
	if (error) {
		kfree(message);
		goto out_request;
	}

	/* Do not restart this deadline if publication or local notification fails. */
	deadline = jiffies + QDX_COMMAND_TIMEOUT;
	error = qdx_io_command(core, message, sizeof(*message) + length,
			       request->id);
	if (error) {
		xa_lock_irqsave(&commands->pending, flags);
		removed = __xa_erase(&commands->pending, request->id);
		xa_unlock_irqrestore(&commands->pending, flags);
		if (removed)
			qdx_request_put(removed);
		kfree(message);
		goto out_request;
	}
	/* IO now owns the buffer, including on a later notification failure. */

	now = jiffies;
	remaining = time_before(now, deadline) ? deadline - now : 0;
	if (!wait_for_completion_timeout(&request->done, remaining)) {
		xa_lock_irqsave(&commands->pending, flags);
		removed = __xa_erase(&commands->pending, request->id);
		if (removed) {
			request->outcome = QDX_UNKNOWN;
			request->error = -ETIMEDOUT;
		}
		xa_unlock_irqrestore(&commands->pending, flags);
		if (removed)
			qdx_request_put(removed);
	}

	/* Erasure under the same lock decides reply, stop or timeout exactly once. */
	xa_lock_irqsave(&commands->pending, flags);
	reply->outcome = request->outcome;
	reply->firmware_error = request->firmware_error;
	reply->len = request->len;
	error = request->error;
	xa_unlock_irqrestore(&commands->pending, flags);
	if (reply->data && request->len && request->len <= reply->max_len)
		memcpy(reply->data, request->data, request->len);

out_request:
	qdx_request_put(request);
out_active:
	xa_lock_irqsave(&commands->pending, flags);
	drained = !--commands->active;
	if (drained)
		wake_up_all(&commands->drained);
	xa_unlock_irqrestore(&commands->pending, flags);
	return error;
}

void qdx_command_receive(struct qdx_core *core, u32 ifnum,
			 const void *buffer, size_t length)
{
	struct qdx_commands *commands = core->commands;
	struct qdx_request *request;
	struct qdx_cmn message;
	unsigned long flags;
	u64 id;
	u32 response;
	u16 payload;
	bool valid;

	if (!commands)
		return;
	if (length < sizeof(message))
		goto malformed;

	/* Returned data need not have the alignment of a host structure. */
	memcpy(&message, buffer, sizeof(message));
	payload = le16_to_cpu(message.length);
	response = le32_to_cpu(message.response);
	valid = le16_to_cpu(message.version) == QDX_MESSAGE_VERSION &&
		ifnum <= 0xffffff && le32_to_cpu(message.interface) == ifnum &&
		payload <= length - sizeof(message) &&
		payload <= QDX_COMMAND_PAYLOAD && response < QDX_RESPONSE_MAX;

	if (response == QDX_RESPONSE_NOTIFY) {
		/* Phase 1 consumes no policy notifications or firmware callbacks. */
		if (valid)
			atomic64_inc(&commands->notifications);
		else
			atomic64_inc(&commands->malformed);
		return;
	}

	id = le64_to_cpu(message.request_id);
	if (!id || le64_to_cpu(message.host_tag) != QDX_HOST_TAG)
		goto malformed;

	xa_lock_irqsave(&commands->pending, flags);
	request = xa_load(&commands->pending, id);
	if (!request) {
		xa_unlock_irqrestore(&commands->pending, flags);
		atomic64_inc(&commands->late);
		return;
	}
	valid = valid && request->ifnum == ifnum &&
		request->opcode == le32_to_cpu(message.type);
	if (response == QDX_RESPONSE_ACK)
		valid = valid && payload >= request->min_len &&
			payload <= request->max_len;
	else
		/* A rejection may omit its body or echo the submitted request. */
		valid = valid && payload <= request->capacity;
	__xa_erase(&commands->pending, id);
	if (!valid) {
		request->outcome = QDX_UNKNOWN;
		request->error = -EPROTO;
	} else {
		request->len = payload;
		request->firmware_error = le32_to_cpu(message.error);
		if (payload)
			memcpy(request->data, (const u8 *)buffer + sizeof(message),
			       payload);
		if (response == QDX_RESPONSE_ACK) {
			request->outcome = QDX_ACK;
			request->error = 0;
		} else {
			request->outcome = QDX_REJECTED;
			request->error = -EREMOTEIO;
		}
	}
	xa_unlock_irqrestore(&commands->pending, flags);
	complete(&request->done);
	qdx_request_put(request);
	if (!valid)
		goto malformed;
	return;

malformed:
	atomic64_inc(&commands->malformed);
	qdx_fail(core->qdx, -EPROTO);
}

void qdx_command_return(struct qdx_core *core, u64 id, u8 response)
{
	struct qdx_commands *commands = core->commands;
	struct qdx_request *request;
	unsigned long flags;
	int error;

	/* Returning the carrier successfully says nothing about the command ACK. */
	if (!commands || !response)
		return;

	error = response > QDX_COMMAND_RETURN_MAX ? -EPROTO : -EIO;
	xa_lock_irqsave(&commands->pending, flags);
	request = __xa_erase(&commands->pending, id);
	if (request) {
		request->outcome = QDX_UNKNOWN;
		request->error = error;
	}
	xa_unlock_irqrestore(&commands->pending, flags);
	if (request) {
		complete(&request->done);
		qdx_request_put(request);
	}
	qdx_fail(core->qdx, error);
}

void qdx_commands_stop(struct qdx_core *core, int error)
{
	struct qdx_commands *commands = core->commands;
	struct qdx_request *request, *next;
	unsigned long flags, id;
	LIST_HEAD(stopped);

	if (!commands)
		return;

	xa_lock_irqsave(&commands->pending, flags);
	commands->closed = true;
	xa_for_each(&commands->pending, id, request) {
		__xa_erase(&commands->pending, id);
		request->outcome = QDX_UNKNOWN;
		request->error = error < 0 ? error : -ESHUTDOWN;
		list_add_tail(&request->stopped, &stopped);
	}
	xa_unlock_irqrestore(&commands->pending, flags);

	list_for_each_entry_safe(request, next, &stopped, stopped) {
		list_del_init(&request->stopped);
		complete(&request->done);
		qdx_request_put(request);
	}
}

void qdx_commands_release(struct qdx_core *core)
{
	struct qdx_commands *commands = core->commands;
	unsigned long flags;

	if (!commands)
		return;

	/* Native entry points and RX callbacks must be closed/drained by caller. */
	qdx_commands_stop(core, -ESHUTDOWN);
	wait_event(commands->drained, !READ_ONCE(commands->active));
	/* Wait for the last caller to leave its wakeup and final lock section. */
	xa_lock_irqsave(&commands->pending, flags);
	xa_unlock_irqrestore(&commands->pending, flags);
	dev_dbg(core->qdx->dev,
		"core %u control: notifications=%lld malformed=%lld late=%lld\n",
		core->id, atomic64_read(&commands->notifications),
		atomic64_read(&commands->malformed), atomic64_read(&commands->late));
	xa_destroy(&commands->pending);
	core->commands = NULL;
	kfree(commands);
}
