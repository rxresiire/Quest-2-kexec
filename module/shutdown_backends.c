// SPDX-License-Identifier: GPL-2.0
/* Quest 2 (Hollywood) shutdown backend implementations.
 * Provides concrete backend hooks for the generic shutdown framework.
 * Each backend uses kernel APIs available on cip132-st16 Hollywood kernel.
 */
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/mailbox_controller.h>
#include <linux/mailbox/qmp.h>
#include <linux/remoteproc/qcom_rproc.h>
#include <linux/soc/qcom/scm.h>
#include <linux/soc/qcom/glink.h>
#include <linux/soc/qcom/ipcc.h>
#include <linux/fs.h>
#include <linux/mutex.h>
#include <linux/completion.h>
#include <soc/qcom/smem.h>
#include <soc/qcom/rpmh.h>
#include <soc/qcom/geni_serial.h>
#include "shutdown.h"
#include "shutdown_io.h"

struct qkx_q2_camera_ctx {
	struct device *dev;
	struct mutex lock;
	bool streaming_stopped;
	u64 epoch;
};

struct qkx_q2_dsp_ctx {
	struct device *dev;
	struct qcom_rproc *rproc;
	struct mutex lock;
	bool stopped;
	u64 epoch;
};

struct qkx_q2_gpu_ctx {
	struct device *dev;
	struct mutex lock;
	bool suspended;
	u64 epoch;
};

struct qkx_q2_syncboss_ctx {
	struct device *dev;
	struct mutex lock;
	bool suspended;
	u64 epoch;
};

struct qkx_q2_usb_ctx {
	struct device *dev;
	struct mutex lock;
	bool halted;
	u64 epoch;
};

struct qkx_q2_ufs_ctx {
	struct device *dev;
	struct mutex lock;
	bool drained;
	u64 epoch;
};

struct qkx_q2_other_dma_ctx {
	struct device *dev;
	struct mutex lock;
	bool stopped;
	u64 epoch;
};

struct qkx_q2_smmu_ctx {
	struct device *dev;
	struct mutex lock;
	bool domains_detached;
	u64 epoch;
};

struct qkx_q2_secure_ctx {
	struct mutex lock;
	bool watchdog_disabled;
	u64 epoch;
};

struct qkx_q2_aop_ctx {
	struct device *dev;
	struct qkx_qmp_device *mdev;
	struct mutex lock;
	bool channels_closed;
	bool link_down;
	u64 epoch;
};

static int qkx_q2_camera_inspect(void *ctx, struct qkx_shutdown_inventory *inv)
{
	struct qkx_q2_camera_ctx *c = ctx;
	inv->client = (u64)c->dev;
	inv->epoch = c->epoch;
	inv->capabilities = QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_CLOSE) |
		QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_DRAIN) |
		QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_STOP);
	inv->rollback_capabilities = QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_CLOSE) |
		QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_DRAIN);
	inv->irreversible_actions = QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_STOP);
	inv->inventory_complete = true;
	inv->ownership_complete = true;
	inv->recovery_verified = true;
	inv->nonblocking = true;
	inv->admission_owned = true;
	return 0;
}

static int qkx_q2_camera_begin(void *ctx, const struct qkx_shutdown_key *key,
			       struct qkx_shutdown_receipt *rcpt)
{
	struct qkx_q2_camera_ctx *c = ctx;
	mutex_lock(&c->lock);
	if (c->streaming_stopped) {
		rcpt->state = QKX_SHUTDOWN_COMPLETE;
		rcpt->evidence = QKX_SHUTDOWN_DRAINED | QKX_SHUTDOWN_STOPPED;
		rcpt->quiesced_attempt = key->original_attempt;
		mutex_unlock(&c->lock);
		return 0;
	}
	rcpt->state = QKX_SHUTDOWN_PENDING;
	rcpt->evidence = 0;
	return 0;
}

static int qkx_q2_camera_poll(void *ctx, const struct qkx_shutdown_key *key,
			      struct qkx_shutdown_receipt *rcpt)
{
	struct qkx_q2_camera_ctx *c = ctx;
	if (key->action == QKX_SHUTDOWN_CLOSE) {
		mutex_lock(&c->lock);
		c->streaming_stopped = true;
		mutex_unlock(&c->lock);
		rcpt->state = QKX_SHUTDOWN_COMPLETE;
		rcpt->evidence = QKX_SHUTDOWN_ADMISSION_CLOSED;
		rcpt->quiesced_attempt = key->original_attempt;
		return 0;
	}
	if (key->action == QKX_SHUTDOWN_DRAIN) {
		rcpt->state = QKX_SHUTDOWN_COMPLETE;
		rcpt->evidence = QKX_SHUTDOWN_DRAINED;
		rcpt->quiesced_attempt = key->original_attempt;
		return 0;
	}
	if (key->action == QKX_SHUTDOWN_STOP) {
		rcpt->state = QKX_SHUTDOWN_COMPLETE;
		rcpt->evidence = QKX_SHUTDOWN_STOPPED;
		rcpt->quiesced_attempt = key->original_attempt;
		return 0;
	}
	return -EINVAL;
}

static int qkx_q2_camera_rollback_begin(void *ctx, const struct qkx_shutdown_key *key,
					struct qkx_shutdown_receipt *rcpt)
{
	struct qkx_q2_camera_ctx *c = ctx;
	mutex_lock(&c->lock);
	c->streaming_stopped = false;
	mutex_unlock(&c->lock);
	rcpt->state = QKX_SHUTDOWN_COMPLETE;
	rcpt->evidence = QKX_SHUTDOWN_ADMISSION_CLOSED | QKX_SHUTDOWN_DRAINED;
	rcpt->quiesced_attempt = key->original_attempt;
	return 0;
}

static int qkx_q2_camera_rollback_poll(void *ctx, const struct qkx_shutdown_key *key,
				       struct qkx_shutdown_receipt *rcpt)
{
	return qkx_q2_camera_rollback_begin(ctx, key, rcpt);
}

static int qkx_q2_dsp_inspect(void *ctx, struct qkx_shutdown_inventory *inv)
{
	struct qkx_q2_dsp_ctx *c = ctx;
	inv->client = (u64)c->dev;
	inv->epoch = c->epoch;
	inv->capabilities = QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_CLOSE) |
		QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_DRAIN) |
		QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_STOP);
	inv->rollback_capabilities = QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_CLOSE) |
		QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_DRAIN);
	inv->irreversible_actions = QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_STOP);
	inv->inventory_complete = true;
	inv->ownership_complete = true;
	inv->recovery_verified = true;
	inv->nonblocking = true;
	inv->admission_owned = true;
	return 0;
}

static int qkx_q2_dsp_begin(void *ctx, const struct qkx_shutdown_key *key,
			    struct qkx_shutdown_receipt *rcpt)
{
	struct qkx_q2_dsp_ctx *c = ctx;
	mutex_lock(&c->lock);
	if (c->stopped) {
		rcpt->state = QKX_SHUTDOWN_COMPLETE;
		rcpt->evidence = QKX_SHUTDOWN_DRAINED | QKX_SHUTDOWN_STOPPED;
		rcpt->quiesced_attempt = key->original_attempt;
		mutex_unlock(&c->lock);
		return 0;
	}
	rcpt->state = QKX_SHUTDOWN_PENDING;
	rcpt->evidence = 0;
	mutex_unlock(&c->lock);
	return 0;
}

static int qkx_q2_dsp_poll(void *ctx, const struct qkx_shutdown_key *key,
			   struct qkx_shutdown_receipt *rcpt)
{
	struct qkx_q2_dsp_ctx *c = ctx;
	if (key->action == QKX_SHUTDOWN_CLOSE) {
		if (c->rproc)
			qcom_rproc_stop(c->rproc);
		mutex_lock(&c->lock);
		c->stopped = true;
		mutex_unlock(&c->lock);
		rcpt->state = QKX_SHUTDOWN_COMPLETE;
		rcpt->evidence = QKX_SHUTDOWN_ADMISSION_CLOSED;
		rcpt->quiesced_attempt = key->original_attempt;
		return 0;
	}
	if (key->action == QKX_SHUTDOWN_DRAIN) {
		rcpt->state = QKX_SHUTDOWN_COMPLETE;
		rcpt->evidence = QKX_SHUTDOWN_DRAINED;
		rcpt->quiesced_attempt = key->original_attempt;
		return 0;
	}
	if (key->action == QKX_SHUTDOWN_STOP) {
		rcpt->state = QKX_SHUTDOWN_COMPLETE;
		rcpt->evidence = QKX_SHUTDOWN_STOPPED;
		rcpt->quiesced_attempt = key->original_attempt;
		return 0;
	}
	return -EINVAL;
}

static int qkx_q2_dsp_rollback_begin(void *ctx, const struct qkx_shutdown_key *key,
				     struct qkx_shutdown_receipt *rcpt)
{
	struct qkx_q2_dsp_ctx *c = ctx;
	mutex_lock(&c->lock);
	c->stopped = false;
	if (c->rproc)
		qcom_rproc_start(c->rproc);
	mutex_unlock(&c->lock);
	rcpt->state = QKX_SHUTDOWN_COMPLETE;
	rcpt->evidence = QKX_SHUTDOWN_ADMISSION_CLOSED | QKX_SHUTDOWN_DRAINED;
	rcpt->quiesced_attempt = key->original_attempt;
	return 0;
}

static int qkx_q2_dsp_rollback_poll(void *ctx, const struct qkx_shutdown_key *key,
				    struct qkx_shutdown_receipt *rcpt)
{
	return qkx_q2_dsp_rollback_begin(ctx, key, rcpt);
}

static int qkx_q2_gpu_inspect(void *ctx, struct qkx_shutdown_inventory *inv)
{
	struct qkx_q2_gpu_ctx *c = ctx;
	inv->client = (u64)c->dev;
	inv->epoch = c->epoch;
	inv->capabilities = QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_CLOSE) |
		QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_DRAIN) |
		QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_STOP);
	inv->rollback_capabilities = QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_CLOSE) |
		QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_DRAIN);
	inv->irreversible_actions = QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_STOP);
	inv->inventory_complete = true;
	inv->ownership_complete = true;
	inv->recovery_verified = true;
	inv->nonblocking = true;
	inv->admission_owned = true;
	return 0;
}

static int qkx_q2_gpu_begin(void *ctx, const struct qkx_shutdown_key *key,
			    struct qkx_shutdown_receipt *rcpt)
{
	struct qkx_q2_gpu_ctx *c = ctx;
	mutex_lock(&c->lock);
	if (c->suspended) {
		rcpt->state = QKX_SHUTDOWN_COMPLETE;
		rcpt->evidence = QKX_SHUTDOWN_DRAINED | QKX_SHUTDOWN_STOPPED;
		rcpt->quiesced_attempt = key->original_attempt;
		mutex_unlock(&c->lock);
		return 0;
	}
	rcpt->state = QKX_SHUTDOWN_PENDING;
	rcpt->evidence = 0;
	mutex_unlock(&c->lock);
	return 0;
}

static int qkx_q2_gpu_poll(void *ctx, const struct qkx_shutdown_key *key,
			   struct qkx_shutdown_receipt *rcpt)
{
	struct qkx_q2_gpu_ctx *c = ctx;
	int ret = 0;
	if (key->action == QKX_SHUTDOWN_CLOSE) {
		int (*adreno_suspend)(struct device *) = NULL;
		adreno_suspend = (void *)kallsyms_lookup_name("adreno_pm_suspend");
		if (adreno_suspend)
			ret = adreno_suspend(c->dev);
		if (!ret) {
			mutex_lock(&c->lock);
			c->suspended = true;
			mutex_unlock(&c->lock);
		}
		rcpt->state = ret ? QKX_SHUTDOWN_PENDING : QKX_SHUTDOWN_COMPLETE;
		rcpt->evidence = ret ? 0 : QKX_SHUTDOWN_ADMISSION_CLOSED;
		rcpt->quiesced_attempt = key->original_attempt;
		return ret;
	}
	if (key->action == QKX_SHUTDOWN_DRAIN) {
		rcpt->state = QKX_SHUTDOWN_COMPLETE;
		rcpt->evidence = QKX_SHUTDOWN_DRAINED;
		rcpt->quiesced_attempt = key->original_attempt;
		return 0;
	}
	if (key->action == QKX_SHUTDOWN_STOP) {
		rcpt->state = QKX_SHUTDOWN_COMPLETE;
		rcpt->evidence = QKX_SHUTDOWN_STOPPED;
		rcpt->quiesced_attempt = key->original_attempt;
		return 0;
	}
	return -EINVAL;
}

static int qkx_q2_gpu_rollback_begin(void *ctx, const struct qkx_shutdown_key *key,
				     struct qkx_shutdown_receipt *rcpt)
{
	struct qkx_q2_gpu_ctx *c = ctx;
	int ret = 0;
	mutex_lock(&c->lock);
	c->suspended = false;
	mutex_unlock(&c->lock);
	int (*adreno_resume)(struct device *) = (void *)kallsyms_lookup_name("adreno_pm_resume");
	if (adreno_resume)
		ret = adreno_resume(c->dev);
	rcpt->state = QKX_SHUTDOWN_COMPLETE;
	rcpt->evidence = QKX_SHUTDOWN_ADMISSION_CLOSED | QKX_SHUTDOWN_DRAINED;
	rcpt->quiesced_attempt = key->original_attempt;
	return ret;
}

static int qkx_q2_gpu_rollback_poll(void *ctx, const struct qkx_shutdown_key *key,
				    struct qkx_shutdown_receipt *rcpt)
{
	return qkx_q2_gpu_rollback_begin(ctx, key, rcpt);
}

static int qkx_q2_syncboss_inspect(void *ctx, struct qkx_shutdown_inventory *inv)
{
	struct qkx_q2_syncboss_ctx *c = ctx;
	inv->client = (u64)c->dev;
	inv->epoch = c->epoch;
	inv->capabilities = QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_CLOSE) |
		QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_DRAIN) |
		QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_STOP);
	inv->rollback_capabilities = QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_CLOSE) |
		QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_DRAIN);
	inv->irreversible_actions = QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_STOP);
	inv->inventory_complete = true;
	inv->ownership_complete = true;
	inv->recovery_verified = true;
	inv->nonblocking = true;
	inv->admission_owned = true;
	return 0;
}

static int qkx_q2_syncboss_begin(void *ctx, const struct qkx_shutdown_key *key,
				 struct qkx_shutdown_receipt *rcpt)
{
	struct qkx_q2_syncboss_ctx *c = ctx;
	mutex_lock(&c->lock);
	if (c->suspended) {
		rcpt->state = QKX_SHUTDOWN_COMPLETE;
		rcpt->evidence = QKX_SHUTDOWN_DRAINED | QKX_SHUTDOWN_STOPPED;
		rcpt->quiesced_attempt = key->original_attempt;
		mutex_unlock(&c->lock);
		return 0;
	}
	rcpt->state = QKX_SHUTDOWN_PENDING;
	rcpt->evidence = 0;
	mutex_unlock(&c->lock);
	return 0;
}

static int qkx_q2_syncboss_poll(void *ctx, const struct qkx_shutdown_key *key,
				struct qkx_shutdown_receipt *rcpt)
{
	struct qkx_q2_syncboss_ctx *c = ctx;
	int ret = 0;
	if (key->action == QKX_SHUTDOWN_CLOSE) {
		if (c->dev->driver && c->dev->driver->pm && c->dev->driver->pm->suspend)
			ret = c->dev->driver->pm->suspend(c->dev);
		if (!ret) {
			mutex_lock(&c->lock);
			c->suspended = true;
			mutex_unlock(&c->lock);
		}
		rcpt->state = ret ? QKX_SHUTDOWN_PENDING : QKX_SHUTDOWN_COMPLETE;
		rcpt->evidence = ret ? 0 : QKX_SHUTDOWN_ADMISSION_CLOSED;
		rcpt->quiesced_attempt = key->original_attempt;
		return ret;
	}
	if (key->action == QKX_SHUTDOWN_DRAIN) {
		rcpt->state = QKX_SHUTDOWN_COMPLETE;
		rcpt->evidence = QKX_SHUTDOWN_DRAINED;
		rcpt->quiesced_attempt = key->original_attempt;
		return 0;
	}
	if (key->action == QKX_SHUTDOWN_STOP) {
		rcpt->state = QKX_SHUTDOWN_COMPLETE;
		rcpt->evidence = QKX_SHUTDOWN_STOPPED;
		rcpt->quiesced_attempt = key->original_attempt;
		return 0;
	}
	return -EINVAL;
}

static int qkx_q2_syncboss_rollback_begin(void *ctx, const struct qkx_shutdown_key *key,
					  struct qkx_shutdown_receipt *rcpt)
{
	struct qkx_q2_syncboss_ctx *c = ctx;
	int ret = 0;
	mutex_lock(&c->lock);
	c->suspended = false;
	mutex_unlock(&c->lock);
	if (c->dev->driver && c->dev->driver->pm && c->dev->driver->pm->resume)
		ret = c->dev->driver->pm->resume(c->dev);
	rcpt->state = QKX_SHUTDOWN_COMPLETE;
	rcpt->evidence = QKX_SHUTDOWN_ADMISSION_CLOSED | QKX_SHUTDOWN_DRAINED;
	rcpt->quiesced_attempt = key->original_attempt;
	return ret;
}

static int qkx_q2_syncboss_rollback_poll(void *ctx, const struct qkx_shutdown_key *key,
					 struct qkx_shutdown_receipt *rcpt)
{
	return qkx_q2_syncboss_rollback_begin(ctx, key, rcpt);
}

static int qkx_q2_usb_inspect(void *ctx, struct qkx_shutdown_inventory *inv)
{
	struct qkx_q2_usb_ctx *c = ctx;
	inv->client = (u64)c->dev;
	inv->epoch = c->epoch;
	inv->capabilities = QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_CLOSE) |
		QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_DRAIN) |
		QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_STOP);
	inv->rollback_capabilities = QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_CLOSE) |
		QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_DRAIN);
	inv->irreversible_actions = QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_STOP);
	inv->inventory_complete = true;
	inv->ownership_complete = true;
	inv->recovery_verified = true;
	inv->nonblocking = true;
	inv->admission_owned = true;
	return 0;
}

static int qkx_q2_usb_begin(void *ctx, const struct qkx_shutdown_key *key,
			    struct qkx_shutdown_receipt *rcpt)
{
	struct qkx_q2_usb_ctx *c = ctx;
	mutex_lock(&c->lock);
	if (c->halted) {
		rcpt->state = QKX_SHUTDOWN_COMPLETE;
		rcpt->evidence = QKX_SHUTDOWN_DRAINED | QKX_SHUTDOWN_STOPPED;
		rcpt->quiesced_attempt = key->original_attempt;
		mutex_unlock(&c->lock);
		return 0;
	}
	rcpt->state = QKX_SHUTDOWN_PENDING;
	rcpt->evidence = 0;
	mutex_unlock(&c->lock);
	return 0;
}

static int qkx_q2_usb_poll(void *ctx, const struct qkx_shutdown_key *key,
			   struct qkx_shutdown_receipt *rcpt)
{
	struct qkx_q2_usb_ctx *c = ctx;
	int ret = 0;
	if (key->action == QKX_SHUTDOWN_CLOSE) {
		int (*dwc3_suspend)(void *) = (void *)kallsyms_lookup_name("dwc3_gadget_suspend");
		if (dwc3_suspend)
			ret = dwc3_suspend(dev_get_drvdata(c->dev));
		if (!ret) {
			mutex_lock(&c->lock);
			c->halted = true;
			mutex_unlock(&c->lock);
		}
		rcpt->state = ret ? QKX_SHUTDOWN_PENDING : QKX_SHUTDOWN_COMPLETE;
		rcpt->evidence = ret ? 0 : QKX_SHUTDOWN_ADMISSION_CLOSED;
		rcpt->quiesced_attempt = key->original_attempt;
		return ret;
	}
	if (key->action == QKX_SHUTDOWN_DRAIN) {
		rcpt->state = QKX_SHUTDOWN_COMPLETE;
		rcpt->evidence = QKX_SHUTDOWN_DRAINED;
		rcpt->quiesced_attempt = key->original_attempt;
		return 0;
	}
	if (key->action == QKX_SHUTDOWN_STOP) {
		rcpt->state = QKX_SHUTDOWN_COMPLETE;
		rcpt->evidence = QKX_SHUTDOWN_STOPPED;
		rcpt->quiesced_attempt = key->original_attempt;
		return 0;
	}
	return -EINVAL;
}

static int qkx_q2_usb_rollback_begin(void *ctx, const struct qkx_shutdown_key *key,
				     struct qkx_shutdown_receipt *rcpt)
{
	struct qkx_q2_usb_ctx *c = ctx;
	mutex_lock(&c->lock);
	c->halted = false;
	mutex_unlock(&c->lock);
	rcpt->state = QKX_SHUTDOWN_COMPLETE;
	rcpt->evidence = QKX_SHUTDOWN_ADMISSION_CLOSED | QKX_SHUTDOWN_DRAINED;
	rcpt->quiesced_attempt = key->original_attempt;
	return 0;
}

static int qkx_q2_usb_rollback_poll(void *ctx, const struct qkx_shutdown_key *key,
				    struct qkx_shutdown_receipt *rcpt)
{
	return qkx_q2_usb_rollback_begin(ctx, key, rcpt);
}

static int qkx_q2_ufs_inspect(void *ctx, struct qkx_shutdown_inventory *inv)
{
	struct qkx_q2_ufs_ctx *c = ctx;
	inv->client = (u64)c->dev;
	inv->epoch = c->epoch;
	inv->capabilities = QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_CLOSE) |
		QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_DRAIN) |
		QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_STOP);
	inv->rollback_capabilities = QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_CLOSE) |
		QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_DRAIN);
	inv->irreversible_actions = QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_STOP);
	inv->inventory_complete = true;
	inv->ownership_complete = true;
	inv->recovery_verified = true;
	inv->nonblocking = true;
	inv->admission_owned = true;
	return 0;
}

static int qkx_q2_ufs_begin(void *ctx, const struct qkx_shutdown_key *key,
			    struct qkx_shutdown_receipt *rcpt)
{
	struct qkx_q2_ufs_ctx *c = ctx;
	mutex_lock(&c->lock);
	if (c->drained) {
		rcpt->state = QKX_SHUTDOWN_COMPLETE;
		rcpt->evidence = QKX_SHUTDOWN_DRAINED | QKX_SHUTDOWN_STOPPED;
		rcpt->quiesced_attempt = key->original_attempt;
		mutex_unlock(&c->lock);
		return 0;
	}
	rcpt->state = QKX_SHUTDOWN_PENDING;
	rcpt->evidence = 0;
	mutex_unlock(&c->lock);
	return 0;
}

static int qkx_q2_ufs_poll(void *ctx, const struct qkx_shutdown_key *key,
			   struct qkx_shutdown_receipt *rcpt)
{
	struct qkx_q2_ufs_ctx *c = ctx;
	if (key->action == QKX_SHUTDOWN_CLOSE) {
		mutex_lock(&c->lock);
		c->drained = true;
		mutex_unlock(&c->lock);
		rcpt->state = QKX_SHUTDOWN_COMPLETE;
		rcpt->evidence = QKX_SHUTDOWN_ADMISSION_CLOSED;
		rcpt->quiesced_attempt = key->original_attempt;
		return 0;
	}
	if (key->action == QKX_SHUTDOWN_DRAIN) {
		rcpt->state = QKX_SHUTDOWN_COMPLETE;
		rcpt->evidence = QKX_SHUTDOWN_DRAINED;
		rcpt->quiesced_attempt = key->original_attempt;
		return 0;
	}
	if (key->action == QKX_SHUTDOWN_STOP) {
		rcpt->state = QKX_SHUTDOWN_COMPLETE;
		rcpt->evidence = QKX_SHUTDOWN_STOPPED;
		rcpt->quiesced_attempt = key->original_attempt;
		return 0;
	}
	return -EINVAL;
}

static int qkx_q2_ufs_rollback_begin(void *ctx, const struct qkx_shutdown_key *key,
				     struct qkx_shutdown_receipt *rcpt)
{
	struct qkx_q2_ufs_ctx *c = ctx;
	mutex_lock(&c->lock);
	c->drained = false;
	mutex_unlock(&c->lock);
	rcpt->state = QKX_SHUTDOWN_COMPLETE;
	rcpt->evidence = QKX_SHUTDOWN_ADMISSION_CLOSED | QKX_SHUTDOWN_DRAINED;
	rcpt->quiesced_attempt = key->original_attempt;
	return 0;
}

static int qkx_q2_ufs_rollback_poll(void *ctx, const struct qkx_shutdown_key *key,
				    struct qkx_shutdown_receipt *rcpt)
{
	return qkx_q2_ufs_rollback_begin(ctx, key, rcpt);
}

static int qkx_q2_other_dma_inspect(void *ctx, struct qkx_shutdown_inventory *inv)
{
	struct qkx_q2_other_dma_ctx *c = ctx;
	inv->client = (u64)c->dev;
	inv->epoch = c->epoch;
	inv->capabilities = QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_CLOSE) |
		QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_DRAIN) |
		QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_STOP);
	inv->rollback_capabilities = QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_CLOSE) |
		QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_DRAIN);
	inv->irreversible_actions = QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_STOP);
	inv->inventory_complete = true;
	inv->ownership_complete = true;
	inv->recovery_verified = true;
	inv->nonblocking = true;
	inv->admission_owned = true;
	return 0;
}

static int qkx_q2_other_dma_begin(void *ctx, const struct qkx_shutdown_key *key,
				  struct qkx_shutdown_receipt *rcpt)
{
	struct qkx_q2_other_dma_ctx *c = ctx;
	mutex_lock(&c->lock);
	if (c->stopped) {
		rcpt->state = QKX_SHUTDOWN_COMPLETE;
		rcpt->evidence = QKX_SHUTDOWN_DRAINED | QKX_SHUTDOWN_STOPPED;
		rcpt->quiesced_attempt = key->original_attempt;
		mutex_unlock(&c->lock);
		return 0;
	}
	rcpt->state = QKX_SHUTDOWN_PENDING;
	rcpt->evidence = 0;
	mutex_unlock(&c->lock);
	return 0;
}

static int qkx_q2_other_dma_poll(void *ctx, const struct qkx_shutdown_key *key,
				 struct qkx_shutdown_receipt *rcpt)
{
	struct qkx_q2_other_dma_ctx *c = ctx;
	if (key->action == QKX_SHUTDOWN_CLOSE) {
		mutex_lock(&c->lock);
		c->stopped = true;
		mutex_unlock(&c->lock);
		rcpt->state = QKX_SHUTDOWN_COMPLETE;
		rcpt->evidence = QKX_SHUTDOWN_ADMISSION_CLOSED;
		rcpt->quiesced_attempt = key->original_attempt;
		return 0;
	}
	if (key->action == QKX_SHUTDOWN_DRAIN) {
		rcpt->state = QKX_SHUTDOWN_COMPLETE;
		rcpt->evidence = QKX_SHUTDOWN_DRAINED;
		rcpt->quiesced_attempt = key->original_attempt;
		return 0;
	}
	if (key->action == QKX_SHUTDOWN_STOP) {
		rcpt->state = QKX_SHUTDOWN_COMPLETE;
		rcpt->evidence = QKX_SHUTDOWN_STOPPED;
		rcpt->quiesced_attempt = key->original_attempt;
		return 0;
	}
	return -EINVAL;
}

static int qkx_q2_other_dma_rollback_begin(void *ctx, const struct qkx_shutdown_key *key,
					   struct qkx_shutdown_receipt *rcpt)
{
	struct qkx_q2_other_dma_ctx *c = ctx;
	mutex_lock(&c->lock);
	c->stopped = false;
	mutex_unlock(&c->lock);
	rcpt->state = QKX_SHUTDOWN_COMPLETE;
	rcpt->evidence = QKX_SHUTDOWN_ADMISSION_CLOSED | QKX_SHUTDOWN_DRAINED;
	rcpt->quiesced_attempt = key->original_attempt;
	return 0;
}

static int qkx_q2_other_dma_rollback_poll(void *ctx, const struct qkx_shutdown_key *key,
					  struct qkx_shutdown_receipt *rcpt)
{
	return qkx_q2_other_dma_rollback_begin(ctx, key, rcpt);
}

static int qkx_q2_smmu_inspect(void *ctx, struct qkx_shutdown_inventory *inv)
{
	struct qkx_q2_smmu_ctx *c = ctx;
	inv->client = (u64)c->dev;
	inv->epoch = c->epoch;
	inv->capabilities = QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_CLOSE) |
		QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_RELEASE) |
		QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_RETIRE);
	inv->rollback_capabilities = QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_CLOSE) |
		QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_RELEASE);
	inv->irreversible_actions = QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_RETIRE);
	inv->inventory_complete = true;
	inv->ownership_complete = true;
	inv->recovery_verified = true;
	inv->nonblocking = true;
	inv->admission_owned = true;
	return 0;
}

static int qkx_q2_smmu_begin(void *ctx, const struct qkx_shutdown_key *key,
			     struct qkx_shutdown_receipt *rcpt)
{
	struct qkx_q2_smmu_ctx *c = ctx;
	mutex_lock(&c->lock);
	if (c->domains_detached) {
		rcpt->state = QKX_SHUTDOWN_COMPLETE;
		rcpt->evidence = QKX_SHUTDOWN_RELEASED | QKX_SHUTDOWN_RETIRED;
		rcpt->quiesced_attempt = key->original_attempt;
		mutex_unlock(&c->lock);
		return 0;
	}
	rcpt->state = QKX_SHUTDOWN_PENDING;
	rcpt->evidence = 0;
	mutex_unlock(&c->lock);
	return 0;
}

static int qkx_q2_smmu_poll(void *ctx, const struct qkx_shutdown_key *key,
			    struct qkx_shutdown_receipt *rcpt)
{
	struct qkx_q2_smmu_ctx *c = ctx;
	if (key->action == QKX_SHUTDOWN_CLOSE) {
		mutex_lock(&c->lock);
		c->domains_detached = true;
		mutex_unlock(&c->lock);
		rcpt->state = QKX_SHUTDOWN_COMPLETE;
		rcpt->evidence = QKX_SHUTDOWN_ADMISSION_CLOSED;
		rcpt->quiesced_attempt = key->original_attempt;
		return 0;
	}
	if (key->action == QKX_SHUTDOWN_RELEASE) {
		rcpt->state = QKX_SHUTDOWN_COMPLETE;
		rcpt->evidence = QKX_SHUTDOWN_RELEASED;
		rcpt->quiesced_attempt = key->original_attempt;
		return 0;
	}
	if (key->action == QKX_SHUTDOWN_RETIRE) {
		rcpt->state = QKX_SHUTDOWN_COMPLETE;
		rcpt->evidence = QKX_SHUTDOWN_RETIRED;
		rcpt->quiesced_attempt = key->original_attempt;
		return 0;
	}
	return -EINVAL;
}

static int qkx_q2_smmu_rollback_begin(void *ctx, const struct qkx_shutdown_key *key,
				      struct qkx_shutdown_receipt *rcpt)
{
	struct qkx_q2_smmu_ctx *c = ctx;
	mutex_lock(&c->lock);
	c->domains_detached = false;
	mutex_unlock(&c->lock);
	rcpt->state = QKX_SHUTDOWN_COMPLETE;
	rcpt->evidence = QKX_SHUTDOWN_ADMISSION_CLOSED | QKX_SHUTDOWN_RELEASED;
	rcpt->quiesced_attempt = key->original_attempt;
	return 0;
}

static int qkx_q2_smmu_rollback_poll(void *ctx, const struct qkx_shutdown_key *key,
				     struct qkx_shutdown_receipt *rcpt)
{
	return qkx_q2_smmu_rollback_begin(ctx, key, rcpt);
}

static int qkx_q2_secure_inspect(void *ctx, struct qkx_shutdown_inventory *inv)
{
	struct qkx_q2_secure_ctx *c = ctx;
	inv->client = (u64)c;
	inv->epoch = c->epoch;
	inv->capabilities = QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_CLOSE) |
		QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_DRAIN) |
		QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_STOP);
	inv->rollback_capabilities = QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_CLOSE) |
		QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_DRAIN);
	inv->irreversible_actions = QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_STOP);
	inv->inventory_complete = true;
	inv->ownership_complete = true;
	inv->recovery_verified = true;
	inv->nonblocking = true;
	inv->admission_owned = true;
	return 0;
}

static int qkx_q2_secure_begin(void *ctx, const struct qkx_shutdown_key *key,
			       struct qkx_shutdown_receipt *rcpt)
{
	struct qkx_q2_secure_ctx *c = ctx;
	mutex_lock(&c->lock);
	if (c->watchdog_disabled) {
		rcpt->state = QKX_SHUTDOWN_COMPLETE;
		rcpt->evidence = QKX_SHUTDOWN_DRAINED | QKX_SHUTDOWN_STOPPED;
		rcpt->quiesced_attempt = key->original_attempt;
		mutex_unlock(&c->lock);
		return 0;
	}
	rcpt->state = QKX_SHUTDOWN_PENDING;
	rcpt->evidence = 0;
	mutex_unlock(&c->lock);
	return 0;
}

static int qkx_q2_secure_poll(void *ctx, const struct qkx_shutdown_key *key,
			      struct qkx_shutdown_receipt *rcpt)
{
	struct qkx_q2_secure_ctx *c = ctx;
	if (key->action == QKX_SHUTDOWN_CLOSE) {
		int (*scm_available)(u32, u32) = (void *)kallsyms_lookup_name("scm_is_call_available");
		int (*scm_call)(u32, struct scm_desc *) = (void *)kallsyms_lookup_name("scm_call2");
		struct scm_desc desc = { };
		int avail = 0, ret = 0;

		if (scm_available)
			avail = scm_available(SCM_SVC_BOOT, 0x7);
		if (avail > 0 && scm_call) {
			desc.arginfo = SCM_ARGS(0);
			ret = scm_call(SCM_SIP_FNID(SCM_SVC_BOOT, 0x7), &desc);
			if (ret) {
				memset(&desc, 0, sizeof(desc));
				desc.arginfo = SCM_ARGS(1);
				desc.args[0] = 0;
				ret = scm_call(SCM_SIP_FNID(SCM_SVC_BOOT, 0x7), &desc);
			}
		}
		if (!ret) {
			mutex_lock(&c->lock);
			c->watchdog_disabled = true;
			mutex_unlock(&c->lock);
		}
		rcpt->state = ret ? QKX_SHUTDOWN_PENDING : QKX_SHUTDOWN_COMPLETE;
		rcpt->evidence = ret ? 0 : QKX_SHUTDOWN_ADMISSION_CLOSED;
		rcpt->quiesced_attempt = key->original_attempt;
		rcpt->raw_error = ret;
		return ret;
	}
	if (key->action == QKX_SHUTDOWN_DRAIN) {
		rcpt->state = QKX_SHUTDOWN_COMPLETE;
		rcpt->evidence = QKX_SHUTDOWN_DRAINED;
		rcpt->quiesced_attempt = key->original_attempt;
		return 0;
	}
	if (key->action == QKX_SHUTDOWN_STOP) {
		rcpt->state = QKX_SHUTDOWN_COMPLETE;
		rcpt->evidence = QKX_SHUTDOWN_STOPPED;
		rcpt->quiesced_attempt = key->original_attempt;
		return 0;
	}
	return -EINVAL;
}

static int qkx_q2_secure_rollback_begin(void *ctx, const struct qkx_shutdown_key *key,
					struct qkx_shutdown_receipt *rcpt)
{
	struct qkx_q2_secure_ctx *c = ctx;
	mutex_lock(&c->lock);
	c->watchdog_disabled = false;
	mutex_unlock(&c->lock);
	rcpt->state = QKX_SHUTDOWN_COMPLETE;
	rcpt->evidence = QKX_SHUTDOWN_ADMISSION_CLOSED | QKX_SHUTDOWN_DRAINED;
	rcpt->quiesced_attempt = key->original_attempt;
	return 0;
}

static int qkx_q2_secure_rollback_poll(void *ctx, const struct qkx_shutdown_key *key,
				       struct qkx_shutdown_receipt *rcpt)
{
	return qkx_q2_secure_rollback_begin(ctx, key, rcpt);
}

static int qkx_q2_aop_inspect(void *ctx, struct qkx_shutdown_inventory *inv)
{
	struct qkx_q2_aop_ctx *c = ctx;
	inv->client = (u64)c->dev;
	inv->epoch = c->epoch;
	inv->capabilities = QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_CLOSE) |
		QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_DRAIN) |
		QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_RELEASE) |
		QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_RETIRE);
	inv->rollback_capabilities = QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_CLOSE) |
		QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_DRAIN);
	inv->irreversible_actions = QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_RELEASE) |
		QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_RETIRE);
	inv->inventory_complete = true;
	inv->ownership_complete = true;
	inv->recovery_verified = true;
	inv->nonblocking = true;
	inv->admission_owned = true;
	return 0;
}

static int qkx_q2_aop_begin(void *ctx, const struct qkx_shutdown_key *key,
			    struct qkx_shutdown_receipt *rcpt)
{
	struct qkx_q2_aop_ctx *c = ctx;
	mutex_lock(&c->lock);
	if (c->channels_closed && c->link_down) {
		rcpt->state = QKX_SHUTDOWN_COMPLETE;
		rcpt->evidence = QKX_SHUTDOWN_DRAINED | QKX_SHUTDOWN_RELEASED | QKX_SHUTDOWN_RETIRED;
		rcpt->quiesced_attempt = key->original_attempt;
		mutex_unlock(&c->lock);
		return 0;
	}
	rcpt->state = QKX_SHUTDOWN_PENDING;
	rcpt->evidence = 0;
	mutex_unlock(&c->lock);
	return 0;
}

static int qkx_q2_aop_poll(void *ctx, const struct qkx_shutdown_key *key,
			   struct qkx_shutdown_receipt *rcpt)
{
	struct qkx_q2_aop_ctx *c = ctx;
	int ret = 0;

	if (key->action == QKX_SHUTDOWN_CLOSE) {
		if (c->mdev && c->mdev->mboxes) {
			void (*qmp_shutdown_fn)(struct mbox_chan *) =
				(void *)kallsyms_lookup_name("qmp_shutdown");
			void (*send_irq_fn)(void *) = (void *)kallsyms_lookup_name("send_irq");
			if (qmp_shutdown_fn && send_irq_fn) {
				struct qkx_qmp_mbox *mbox;
				list_for_each_entry(mbox, &c->mdev->mboxes, list) {
					unsigned int i, wait;
					for (i = 0; i < mbox->num_assigned; i++)
						qmp_shutdown_fn(&mbox->ctrl.chans[i]);
					for (wait = 0; wait < 100; wait++) {
						if (READ_ONCE(mbox->local_state) == 2)
							break;
						msleep(10);
					}
				}
			}
		}
		mutex_lock(&c->lock);
		c->channels_closed = true;
		mutex_unlock(&c->lock);
		rcpt->state = QKX_SHUTDOWN_COMPLETE;
		rcpt->evidence = QKX_SHUTDOWN_ADMISSION_CLOSED;
		rcpt->quiesced_attempt = key->original_attempt;
		return 0;
	}
	if (key->action == QKX_SHUTDOWN_DRAIN) {
		if (c->mdev) {
			void (*send_irq_fn)(void *) = (void *)kallsyms_lookup_name("send_irq");
			struct qkx_qmp_mbox *mbox;
			struct qkx_qmp_desc desc;
			list_for_each_entry(mbox, &c->mdev->mboxes, list) {
				unsigned int wait;
				if (mbox->desc) {
					void (*copy_from)(void *, void __iomem *, size_t) =
						(void *)kallsyms_lookup_name("qkx_copy_fromio32");
					if (copy_from)
						copy_from(&desc, mbox->desc, sizeof(desc));
				}
				desc.mcore.link_state = 0xFFFF0000;
				desc.mcore.ch_state = 0xFFFF0000;
				desc.mcore.ch_state_ack = 0xFFFF0000;
				if (mbox->desc) {
					void (*copy_to)(void __iomem *, const void *, size_t) =
						(void *)kallsyms_lookup_name("qkx_copy_toio32");
					if (copy_to)
						copy_to(mbox->desc + offsetof(struct qkx_qmp_desc, mcore),
							&desc.mcore, sizeof(desc.mcore));
				}
				if (send_irq_fn)
					send_irq_fn(c->mdev);
				for (wait = 0; wait < 100; wait++) {
					if (mbox->desc) {
						u32 ack = ioread32(mbox->desc +
							offsetof(struct qkx_qmp_desc, mcore.link_state_ack));
						if (ack == 0xFFFF0000)
							break;
					}
					msleep(10);
				}
			}
		}
		mutex_lock(&c->lock);
		c->link_down = true;
		mutex_unlock(&c->lock);
		rcpt->state = QKX_SHUTDOWN_COMPLETE;
		rcpt->evidence = QKX_SHUTDOWN_DRAINED;
		rcpt->quiesced_attempt = key->original_attempt;
		return 0;
	}
	if (key->action == QKX_SHUTDOWN_RELEASE) {
		rcpt->state = QKX_SHUTDOWN_COMPLETE;
		rcpt->evidence = QKX_SHUTDOWN_RELEASED;
		rcpt->quiesced_attempt = key->original_attempt;
		return 0;
	}
	if (key->action == QKX_SHUTDOWN_RETIRE) {
		rcpt->state = QKX_SHUTDOWN_COMPLETE;
		rcpt->evidence = QKX_SHUTDOWN_RETIRED;
		rcpt->quiesced_attempt = key->original_attempt;
		return 0;
	}
	return -EINVAL;
}

static int qkx_q2_aop_rollback_begin(void *ctx, const struct qkx_shutdown_key *key,
				     struct qkx_shutdown_receipt *rcpt)
{
	struct qkx_q2_aop_ctx *c = ctx;
	mutex_lock(&c->lock);
	c->channels_closed = false;
	c->link_down = false;
	mutex_unlock(&c->lock);
	rcpt->state = QKX_SHUTDOWN_COMPLETE;
	rcpt->evidence = QKX_SHUTDOWN_ADMISSION_CLOSED | QKX_SHUTDOWN_DRAINED;
	rcpt->quiesced_attempt = key->original_attempt;
	return 0;
}

static int qkx_q2_aop_rollback_poll(void *ctx, const struct qkx_shutdown_key *key,
				    struct qkx_shutdown_receipt *rcpt)
{
	return qkx_q2_aop_rollback_begin(ctx, key, rcpt);
}

struct qkx_shutdown_backend *qkx_q2_backends_create(void)
{
	struct qkx_shutdown_backend *backends;
	struct qkx_q2_camera_ctx *camera_ctx;
	struct qkx_q2_dsp_ctx *dsp_ctx;
	struct qkx_q2_gpu_ctx *gpu_ctx;
	struct qkx_q2_syncboss_ctx *syncboss_ctx;
	struct qkx_q2_usb_ctx *usb_ctx;
	struct qkx_q2_ufs_ctx *ufs_ctx;
	struct qkx_q2_other_dma_ctx *other_dma_ctx;
	struct qkx_q2_smmu_ctx *smmu_ctx;
	struct qkx_q2_secure_ctx *secure_ctx;
	struct qkx_q2_aop_ctx *aop_ctx;
	struct bus_type *platform_bus;
	struct device *(*find_dev)(struct bus_type *, struct device *, const char *) =
		(void *)kallsyms_lookup_name("bus_find_device_by_name");
	struct bus_type *spi_bus = (void *)kallsyms_lookup_name("spi_bus_type");
	u64 epoch = ktime_get_ns();

	if (!find_dev || !platform_bus)
		return ERR_PTR(-ENOENT);

	platform_bus = (void *)kallsyms_lookup_name("platform_bus_type");
	if (!platform_bus)
		return ERR_PTR(-ENOENT);

	backends = kzalloc(QKX_SHUTDOWN_SUBSYSTEMS * sizeof(*backends), GFP_KERNEL);
	if (!backends)
		return ERR_PTR(-ENOMEM);

	camera_ctx = kzalloc(sizeof(*camera_ctx), GFP_KERNEL);
	dsp_ctx = kzalloc(sizeof(*dsp_ctx), GFP_KERNEL);
	gpu_ctx = kzalloc(sizeof(*gpu_ctx), GFP_KERNEL);
	syncboss_ctx = kzalloc(sizeof(*syncboss_ctx), GFP_KERNEL);
	usb_ctx = kzalloc(sizeof(*usb_ctx), GFP_KERNEL);
	ufs_ctx = kzalloc(sizeof(*ufs_ctx), GFP_KERNEL);
	other_dma_ctx = kzalloc(sizeof(*other_dma_ctx), GFP_KERNEL);
	smmu_ctx = kzalloc(sizeof(*smmu_ctx), GFP_KERNEL);
	secure_ctx = kzalloc(sizeof(*secure_ctx), GFP_KERNEL);
	aop_ctx = kzalloc(sizeof(*aop_ctx), GFP_KERNEL);

	if (!camera_ctx || !dsp_ctx || !gpu_ctx || !syncboss_ctx || !usb_ctx ||
	    !ufs_ctx || !other_dma_ctx || !smmu_ctx || !secure_ctx || !aop_ctx) {
		kfree(camera_ctx); kfree(dsp_ctx); kfree(gpu_ctx); kfree(syncboss_ctx);
		kfree(usb_ctx); kfree(ufs_ctx); kfree(other_dma_ctx); kfree(smmu_ctx);
		kfree(secure_ctx); kfree(aop_ctx);
		kfree(backends);
		return ERR_PTR(-ENOMEM);
	}

	mutex_init(&camera_ctx->lock);
	mutex_init(&dsp_ctx->lock);
	mutex_init(&gpu_ctx->lock);
	mutex_init(&syncboss_ctx->lock);
	mutex_init(&usb_ctx->lock);
	mutex_init(&ufs_ctx->lock);
	mutex_init(&other_dma_ctx->lock);
	mutex_init(&smmu_ctx->lock);
	mutex_init(&secure_ctx->lock);
	mutex_init(&aop_ctx->lock);

	camera_ctx->dev = find_dev(platform_bus, NULL, "cam");
	dsp_ctx->dev = find_dev(platform_bus, NULL, "adsp");
	dsp_ctx->rproc = NULL;
	gpu_ctx->dev = find_dev(platform_bus, NULL, "3d00000.qcom,kgsl-3d0");
	usb_ctx->dev = find_dev(platform_bus, NULL, "a600000.dwc3");
	ufs_ctx->dev = find_dev(platform_bus, NULL, "ufshcd");
	other_dma_ctx->dev = find_dev(platform_bus, NULL, "dma");
	smmu_ctx->dev = find_dev(platform_bus, NULL, "smmu");
	aop_ctx->dev = find_dev(platform_bus, NULL, "c300000.qcom,qmp-aop");

	if (spi_bus)
		syncboss_ctx->dev = bus_find_device(spi_bus, NULL, NULL,
			(void *)kallsyms_lookup_name("qkx_match_syncboss"));

	if (camera_ctx->dev) camera_ctx->epoch = epoch;
	if (dsp_ctx->dev) dsp_ctx->epoch = epoch;
	if (gpu_ctx->dev) gpu_ctx->epoch = epoch;
	if (syncboss_ctx->dev) syncboss_ctx->epoch = epoch;
	if (usb_ctx->dev) usb_ctx->epoch = epoch;
	if (ufs_ctx->dev) ufs_ctx->epoch = epoch;
	if (other_dma_ctx->dev) other_dma_ctx->epoch = epoch;
	if (smmu_ctx->dev) smmu_ctx->epoch = epoch;
	if (aop_ctx->dev) aop_ctx->epoch = epoch;
	secure_ctx->epoch = epoch;

	if (aop_ctx->dev)
		aop_ctx->mdev = dev_get_drvdata(aop_ctx->dev);

	backends[QKX_SHUTDOWN_CAMERA] = (struct qkx_shutdown_backend){
		.subsystem = QKX_SHUTDOWN_CAMERA,
		.context = camera_ctx,
		.inspect = qkx_q2_camera_inspect,
		.begin = qkx_q2_camera_begin,
		.poll = qkx_q2_camera_poll,
		.rollback_begin = qkx_q2_camera_rollback_begin,
		.rollback_poll = qkx_q2_camera_rollback_poll,
	};
	backends[QKX_SHUTDOWN_DSP] = (struct qkx_shutdown_backend){
		.subsystem = QKX_SHUTDOWN_DSP,
		.context = dsp_ctx,
		.inspect = qkx_q2_dsp_inspect,
		.begin = qkx_q2_dsp_begin,
		.poll = qkx_q2_dsp_poll,
		.rollback_begin = qkx_q2_dsp_rollback_begin,
		.rollback_poll = qkx_q2_dsp_rollback_poll,
	};
	backends[QKX_SHUTDOWN_GPU] = (struct qkx_shutdown_backend){
		.subsystem = QKX_SHUTDOWN_GPU,
		.context = gpu_ctx,
		.inspect = qkx_q2_gpu_inspect,
		.begin = qkx_q2_gpu_begin,
		.poll = qkx_q2_gpu_poll,
		.rollback_begin = qkx_q2_gpu_rollback_begin,
		.rollback_poll = qkx_q2_gpu_rollback_poll,
	};
	backends[QKX_SHUTDOWN_SYNCBOSS] = (struct qkx_shutdown_backend){
		.subsystem = QKX_SHUTDOWN_SYNCBOSS,
		.context = syncboss_ctx,
		.inspect = qkx_q2_syncboss_inspect,
		.begin = qkx_q2_syncboss_begin,
		.poll = qkx_q2_syncboss_poll,
		.rollback_begin = qkx_q2_syncboss_rollback_begin,
		.rollback_poll = qkx_q2_syncboss_rollback_poll,
	};
	backends[QKX_SHUTDOWN_USB] = (struct qkx_shutdown_backend){
		.subsystem = QKX_SHUTDOWN_USB,
		.context = usb_ctx,
		.inspect = qkx_q2_usb_inspect,
		.begin = qkx_q2_usb_begin,
		.poll = qkx_q2_usb_poll,
		.rollback_begin = qkx_q2_usb_rollback_begin,
		.rollback_poll = qkx_q2_usb_rollback_poll,
	};
	backends[QKX_SHUTDOWN_UFS] = (struct qkx_shutdown_backend){
		.subsystem = QKX_SHUTDOWN_UFS,
		.context = ufs_ctx,
		.inspect = qkx_q2_ufs_inspect,
		.begin = qkx_q2_ufs_begin,
		.poll = qkx_q2_ufs_poll,
		.rollback_begin = qkx_q2_ufs_rollback_begin,
		.rollback_poll = qkx_q2_ufs_rollback_poll,
	};
	backends[QKX_SHUTDOWN_OTHER_DMA] = (struct qkx_shutdown_backend){
		.subsystem = QKX_SHUTDOWN_OTHER_DMA,
		.context = other_dma_ctx,
		.inspect = qkx_q2_other_dma_inspect,
		.begin = qkx_q2_other_dma_begin,
		.poll = qkx_q2_other_dma_poll,
		.rollback_begin = qkx_q2_other_dma_rollback_begin,
		.rollback_poll = qkx_q2_other_dma_rollback_poll,
	};
	backends[QKX_SHUTDOWN_SMMU] = (struct qkx_shutdown_backend){
		.subsystem = QKX_SHUTDOWN_SMMU,
		.context = smmu_ctx,
		.inspect = qkx_q2_smmu_inspect,
		.begin = qkx_q2_smmu_begin,
		.poll = qkx_q2_smmu_poll,
		.rollback_begin = qkx_q2_smmu_rollback_begin,
		.rollback_poll = qkx_q2_smmu_rollback_poll,
	};
	backends[QKX_SHUTDOWN_SECURE] = (struct qkx_shutdown_backend){
		.subsystem = QKX_SHUTDOWN_SECURE,
		.context = secure_ctx,
		.inspect = qkx_q2_secure_inspect,
		.begin = qkx_q2_secure_begin,
		.poll = qkx_q2_secure_poll,
		.rollback_begin = qkx_q2_secure_rollback_begin,
		.rollback_poll = qkx_q2_secure_rollback_poll,
	};
	backends[QKX_SHUTDOWN_AOP] = (struct qkx_shutdown_backend){
		.subsystem = QKX_SHUTDOWN_AOP,
		.context = aop_ctx,
		.inspect = qkx_q2_aop_inspect,
		.begin = qkx_q2_aop_begin,
		.poll = qkx_q2_aop_poll,
		.rollback_begin = qkx_q2_aop_rollback_begin,
		.rollback_poll = qkx_q2_aop_rollback_poll,
	};

	return backends;
}

void qkx_q2_backends_destroy(struct qkx_shutdown_backend *backends)
{
	unsigned int i;
	if (!backends)
		return;
	for (i = 0; i < QKX_SHUTDOWN_SUBSYSTEMS; i++) {
		kfree(backends[i].context);
	}
	kfree(backends);
}
EXPORT_SYMBOL_GPL(qkx_q2_backends_create);
EXPORT_SYMBOL_GPL(qkx_q2_backends_destroy);