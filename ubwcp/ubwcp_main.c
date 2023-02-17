// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/dma-buf.h>
#include <linux/slab.h>
#include <linux/cdev.h>
#include <linux/hashtable.h>
#include <linux/scatterlist.h>
#include <linux/types.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>
#include <linux/genalloc.h>
#include <linux/interrupt.h>
#include <linux/regulator/consumer.h>
#include <linux/numa.h>
#include <linux/memory_hotplug.h>
#include <asm/page.h>
#include <linux/delay.h>
#include <linux/ubwcp_dma_heap.h>
#include <linux/debugfs.h>
#include <linux/clk.h>
#include <linux/iommu.h>
#include <linux/set_memory.h>

MODULE_IMPORT_NS(DMA_BUF);

#include "include/kernel/ubwcp.h"
#include "ubwcp_hw.h"
#include "include/uapi/ubwcp_ioctl.h"
#define CREATE_TRACE_POINTS
#include "ubwcp_trace.h"

#define UBWCP_NUM_DEVICES 1
#define UBWCP_DEVICE_NAME "ubwcp"

#define UBWCP_BUFFER_DESC_OFFSET  64
#define UBWCP_BUFFER_DESC_COUNT  256


#define CACHE_ADDR(x) ((x) >> 6)
#define PAGE_ADDR(x)  ((x) >> 12)
#define UBWCP_ALIGN(_x, _y)  ((((_x) + (_y) - 1)/(_y))*(_y))


#define DBG_BUF_ATTR(fmt, args...) do { if (ubwcp_debug_trace_enable) \
					pr_err("ubwcp: %s(): " fmt "\n", __func__, ##args); \
					} while (0)
#define DBG(fmt, args...) do { if (ubwcp_debug_trace_enable) \
				pr_err("ubwcp: %s(): " fmt "\n", __func__, ##args); \
				} while (0)
#define ERR(fmt, args...) pr_err("ubwcp: %s(): ~~~ERROR~~~: " fmt "\n", __func__, ##args)
#define ERR_RATE_LIMIT(fmt, args...) pr_err_ratelimited("ubwcp: %s(): ~~~ERROR~~~: " fmt "\n",\
							__func__, ##args)

#define FENTRY() DBG("")


#define META_DATA_PITCH_ALIGN    64
#define META_DATA_HEIGHT_ALIGN   16
#define META_DATA_SIZE_ALIGN  4096
#define PIXEL_DATA_SIZE_ALIGN 4096

#define UBWCP_SYNC_GRANULE 0x4000000L /* 64 MB */

struct ubwcp_desc {
	int idx;
	void *ptr;
};

/* TBD: confirm size of width/height */
struct ubwcp_dimension {
	u16 width;
	u16 height;
};

struct ubwcp_plane_info {
	u16 pixel_bytes;
	u16 per_pixel;
	struct ubwcp_dimension tilesize_p;      /* pixels */
	struct ubwcp_dimension macrotilesize_p; /* pixels */
};

struct ubwcp_image_format_info {
	u16 planes;
	struct ubwcp_plane_info p_info[2];
};

enum ubwcp_std_image_format {
	RGBA   = 0,
	NV12   = 1,
	NV124R = 2,
	P010   = 3,
	TP10   = 4,
	P016   = 5,
	INFO_FORMAT_LIST_SIZE,
	STD_IMAGE_FORMAT_INVALID = 0xFF
};

struct ubwcp_driver {
	/* cdev related */
	dev_t devt;
	struct class *dev_class; //sysfs dev class
	struct device *dev_sys; //sysfs dev
	struct cdev cdev; //char dev

	/* debugfs */
	struct dentry *debugfs_root;
	bool read_err_irq_en;
	bool write_err_irq_en;
	bool decode_err_irq_en;
	bool encode_err_irq_en;

	/* ubwcp devices */
	struct device *dev; //ubwcp device
	struct device *dev_desc_cb; //smmu dev for descriptors
	struct device *dev_buf_cb; //smmu dev for ubwcp buffers

	void __iomem *base; //ubwcp base address
	struct regulator *vdd;

	struct clk **clocks;
	int num_clocks;

	/* interrupts */
	int irq_range_ck_rd;
	int irq_range_ck_wr;
	int irq_encode;
	int irq_decode;

	/* ula address pool */
	u64 ula_pool_base;
	u64 ula_pool_size;
	struct gen_pool *ula_pool;

	configure_mmap mmap_config_fptr;

	/* HW version */
	u32 hw_ver_major;
	u32 hw_ver_minor;

	/* keep track of all potential buffers.
	 * hash table index'ed using dma_buf ptr.
	 * 2**13 = 8192 hash values
	 */
	DECLARE_HASHTABLE(buf_table, 13);

	/* buffer descriptor */
	void       *buffer_desc_base;      /* CPU address */
	dma_addr_t buffer_desc_dma_handle; /* dma address */
	size_t buffer_desc_size;
	struct ubwcp_desc desc_list[UBWCP_BUFFER_DESC_COUNT];

	struct ubwcp_image_format_info format_info[INFO_FORMAT_LIST_SIZE];

	atomic_t num_non_lin_buffers;
	bool mem_online;

	struct mutex desc_lock;        /* allocate/free descriptors */
	spinlock_t buf_table_lock;     /* add/remove dma_buf into list of managed bufffers */
	struct mutex mem_hotplug_lock; /* memory hotplug lock */
	struct mutex ula_lock;         /* allocate/free ula */
	struct mutex ubwcp_flush_lock; /* ubwcp flush */
	struct mutex hw_range_ck_lock; /* range ck */
	struct list_head err_handler_list; /* error handler list */
	spinlock_t err_handler_list_lock;  /* err_handler_list lock */
};

struct ubwcp_buf {
	struct hlist_node hnode;
	struct ubwcp_driver *ubwcp;
	struct ubwcp_buffer_attrs buf_attr;
	bool perm;
	struct ubwcp_desc *desc;
	bool buf_attr_set;
	bool locked;
	enum dma_data_direction lock_dir;
	int lock_count;

	/* dma_buf info */
	struct dma_buf *dma_buf;
	struct dma_buf_attachment *attachment;
	struct sg_table *sgt;

	/* ula info */
	phys_addr_t ula_pa;
	size_t ula_size;

	/* meta metadata */
	struct ubwcp_hw_meta_metadata mmdata;
	struct mutex lock;
};


static struct ubwcp_driver *me;
static u32 ubwcp_debug_trace_enable;

static struct ubwcp_driver *ubwcp_get_driver(void)
{
	if (!me)
		WARN(1, "ubwcp: driver ptr requested but driver not initialized");

	return me;
}

static void image_format_init(struct ubwcp_driver *ubwcp)
{				/* planes, bytes/p,   Tp  ,    MTp    */
	ubwcp->format_info[RGBA]   = (struct ubwcp_image_format_info)
					{1, {{4, 1,  {16, 4}, {64, 16}}}};
	ubwcp->format_info[NV12]   = (struct ubwcp_image_format_info)
					{2, {{1,  1, {32, 8}, {128, 32}},
					     {2,  1, {16, 8}, { 64, 32}}}};
	ubwcp->format_info[NV124R] = (struct ubwcp_image_format_info)
					{2, {{1,  1, {64, 4}, {256, 16}},
					     {2,  1, {32, 4}, {128, 16}}}};
	ubwcp->format_info[P010]   = (struct ubwcp_image_format_info)
					{2, {{2,  1, {32, 4}, {128, 16}},
					     {4,  1, {16, 4}, { 64, 16}}}};
	ubwcp->format_info[TP10]   = (struct ubwcp_image_format_info)
					{2, {{4,  3, {48, 4}, {192, 16}},
					     {8,  3, {24, 4}, { 96, 16}}}};
	ubwcp->format_info[P016]   = (struct ubwcp_image_format_info)
					{2, {{2,  1, {32, 4}, {128, 16}},
					     {4,  1, {16, 4}, { 64, 16}}}};
}

static void ubwcp_buf_desc_list_init(struct ubwcp_driver *ubwcp)
{
	int idx;
	struct ubwcp_desc *desc_list = ubwcp->desc_list;

	for (idx = 0; idx < UBWCP_BUFFER_DESC_COUNT; idx++) {
		desc_list[idx].idx = -1;
		desc_list[idx].ptr = NULL;
	}
}

static int ubwcp_init_clocks(struct ubwcp_driver *ubwcp, struct device *dev)
{
	const char *cname;
	struct property *prop;
	int i;

	ubwcp->num_clocks =
		of_property_count_strings(dev->of_node, "clock-names");

	if (ubwcp->num_clocks < 1) {
		ubwcp->num_clocks = 0;
		return 0;
	}

	ubwcp->clocks = devm_kzalloc(dev,
		sizeof(*ubwcp->clocks) * ubwcp->num_clocks, GFP_KERNEL);
	if (!ubwcp->clocks)
		return -ENOMEM;

	i = 0;
	of_property_for_each_string(dev->of_node, "clock-names",
				prop, cname) {
		struct clk *c = devm_clk_get(dev, cname);

		if (IS_ERR(c)) {
			ERR("Couldn't get clock: %s\n", cname);
			return PTR_ERR(c);
		}

		ubwcp->clocks[i] = c;

		++i;
	}
	return 0;
}

static int ubwcp_enable_clocks(struct ubwcp_driver *ubwcp)
{
	int i, ret = 0;

	for (i = 0; i < ubwcp->num_clocks; ++i) {
		ret = clk_prepare_enable(ubwcp->clocks[i]);
		if (ret) {
			ERR("Couldn't enable clock #%d\n", i);
			while (i--)
				clk_disable_unprepare(ubwcp->clocks[i]);
			break;
		}
	}

	return ret;
}

static void ubwcp_disable_clocks(struct ubwcp_driver *ubwcp)
{
	int i;

	for (i = ubwcp->num_clocks; i; --i)
		clk_disable_unprepare(ubwcp->clocks[i - 1]);
}

/* UBWCP Power control */
static int ubwcp_power(struct ubwcp_driver *ubwcp, bool enable)
{
	int ret = 0;

	if (!ubwcp) {
		ERR("ubwcp ptr is NULL");
		return -1;
	}

	if (!ubwcp->vdd) {
		ERR("vdd is NULL");
		return -1;
	}

	if (enable) {
		ret = regulator_enable(ubwcp->vdd);
		if (ret < 0) {
			ERR("regulator_enable failed: %d", ret);
			ret = -1;
		} else {
			DBG("regulator_enable() success");
		}

		if (!ret) {
			ret = ubwcp_enable_clocks(ubwcp);
			if (ret) {
				ERR("enable clocks failed: %d", ret);
				regulator_disable(ubwcp->vdd);
			} else {
				DBG("enable clocks success");
			}
		}
	} else {
		ret = regulator_disable(ubwcp->vdd);
		if (ret < 0) {
			ERR("regulator_disable failed: %d", ret);
			ret = -1;
		} else {
			DBG("regulator_disable() success");
		}

		if (!ret) {
			ubwcp_disable_clocks(ubwcp);
			DBG("disable clocks success");
		}
	}

	return ret;
}


static int ubwcp_flush(struct ubwcp_driver *ubwcp)
{
	int ret = 0;

	mutex_lock(&ubwcp->ubwcp_flush_lock);
	ret = ubwcp_hw_flush(ubwcp->base);
	mutex_unlock(&ubwcp->ubwcp_flush_lock);
	if (ret != 0)
		WARN(1, "ubwcp_hw_flush() failed!");

	return ret;
}


/* get dma_buf ptr for the given dma_buf fd */
struct dma_buf *ubwcp_dma_buf_fd_to_dma_buf(int dma_buf_fd)
{
	struct dma_buf *dmabuf;

	/* TBD: dma_buf_get() results in taking ref to buf and it won't ever get
	 * free'ed until ref count goes to 0. So we must reduce the ref count
	 * immediately after we find our corresponding ubwcp_buf.
	 */
	dmabuf = dma_buf_get(dma_buf_fd);
	if (IS_ERR(dmabuf)) {
		ERR("dmabuf ptr not found for dma_buf_fd = %d", dma_buf_fd);
		return NULL;
	}

	dma_buf_put(dmabuf);

	return dmabuf;
}
EXPORT_SYMBOL(ubwcp_dma_buf_fd_to_dma_buf);


/* get ubwcp_buf corresponding to the given dma_buf */
static struct ubwcp_buf *dma_buf_to_ubwcp_buf(struct dma_buf *dmabuf)
{
	struct ubwcp_buf *buf = NULL;
	struct ubwcp_driver *ubwcp = ubwcp_get_driver();
	unsigned long flags;

	if (!dmabuf || !ubwcp)
		return NULL;

	spin_lock_irqsave(&ubwcp->buf_table_lock, flags);
	/* look up ubwcp_buf corresponding to this dma_buf */
	hash_for_each_possible(ubwcp->buf_table, buf, hnode, (u64)dmabuf) {
		if (buf->dma_buf == dmabuf)
			break;
	}
	spin_unlock_irqrestore(&ubwcp->buf_table_lock, flags);

	return buf;
}


/* return ubwcp hardware version */
int ubwcp_get_hw_version(struct ubwcp_ioctl_hw_version *ver)
{
	struct ubwcp_driver *ubwcp;

	FENTRY();

	if (!ver) {
		ERR("invalid version ptr");
		return -EINVAL;
	}

	ubwcp = ubwcp_get_driver();
	if (!ubwcp)
		return -1;

	ver->major = ubwcp->hw_ver_major;
	ver->minor = ubwcp->hw_ver_minor;
	return 0;
}
EXPORT_SYMBOL(ubwcp_get_hw_version);

static int add_ula_pa_memory(struct ubwcp_driver *ubwcp)
{
	int ret;
	int nid;

	nid = memory_add_physaddr_to_nid(ubwcp->ula_pool_base);
	DBG("calling add_memory()...");
	trace_ubwcp_add_memory_start(ubwcp->ula_pool_size);
	ret = add_memory(nid, ubwcp->ula_pool_base, ubwcp->ula_pool_size, MHP_NONE);
	trace_ubwcp_add_memory_end(ubwcp->ula_pool_size);

	if (ret) {
		ERR("add_memory() failed st:0x%lx sz:0x%lx err: %d",
			ubwcp->ula_pool_base,
			ubwcp->ula_pool_size,
			ret);
		/* Fix to put driver in invalid state */
	} else {
		DBG("add_memory() ula_pool_base:0x%llx, size:0x%zx, kernel addr:0x%p",
			ubwcp->ula_pool_base,
			ubwcp->ula_pool_size,
			page_to_virt(pfn_to_page(PFN_DOWN(ubwcp->ula_pool_base))));
	}

	return ret;
}

static int inc_num_non_lin_buffers(struct ubwcp_driver *ubwcp)
{
	int ret = 0;

	atomic_inc(&ubwcp->num_non_lin_buffers);
	mutex_lock(&ubwcp->mem_hotplug_lock);
	if (!ubwcp->mem_online) {
		if (atomic_read(&ubwcp->num_non_lin_buffers) == 0) {
			ret = -EINVAL;
			ERR("Bad state: num_non_lin_buffers should not be 0");
			/* Fix to put driver in invalid state */
			goto err_power_on;
		}

		ret = ubwcp_power(ubwcp, true);
		if (ret)
			goto err_power_on;

		ret = add_ula_pa_memory(ubwcp);
		if (ret)
			goto err_add_memory;

		ubwcp->mem_online = true;
	}
	mutex_unlock(&ubwcp->mem_hotplug_lock);
	return 0;

err_add_memory:
	ubwcp_power(ubwcp, false);
err_power_on:
	atomic_dec(&ubwcp->num_non_lin_buffers);
	mutex_unlock(&ubwcp->mem_hotplug_lock);

	return ret;
}

static int dec_num_non_lin_buffers(struct ubwcp_driver *ubwcp)
{
	int ret = 0;

	atomic_dec(&ubwcp->num_non_lin_buffers);
	mutex_lock(&ubwcp->mem_hotplug_lock);

	/* If this is the last buffer being freed, power off ubwcp */
	if (atomic_read(&ubwcp->num_non_lin_buffers) == 0) {
		unsigned long sync_remain = 0;
		unsigned long sync_offset = 0;
		unsigned long sync_size = 0;
		unsigned long sync_granule = UBWCP_SYNC_GRANULE;

		DBG("last buffer: ~~~~~~~~~~~");
		if (!ubwcp->mem_online) {
			ret = -EINVAL;
			ERR("Bad state: mem_online should not be false");
			/* Fix to put driver in invalid state */
			goto err_remove_mem;
		}

		DBG("set_direct_map_range_uncached() for ULA PA pool st:0x%lx num pages:%lu",
				ubwcp->ula_pool_base, ubwcp->ula_pool_size >> PAGE_SHIFT);
		trace_ubwcp_set_direct_map_range_uncached_start(ubwcp->ula_pool_size);
		ret = set_direct_map_range_uncached((unsigned long)phys_to_virt(
				ubwcp->ula_pool_base), ubwcp->ula_pool_size >> PAGE_SHIFT);
		trace_ubwcp_set_direct_map_range_uncached_end(ubwcp->ula_pool_size);
		if (ret) {
			ERR("set_direct_map_range_uncached failed st:0x%lx num pages:%lu err: %d",
				ubwcp->ula_pool_base,
				ubwcp->ula_pool_size >> PAGE_SHIFT, ret);
			goto err_remove_mem;
		} else {
			DBG("DONE: calling set_direct_map_range_uncached() for ULA PA pool");
		}

		DBG("Calling dma_sync_single_for_cpu() for ULA PA pool");
		trace_ubwcp_offline_sync_start(ubwcp->ula_pool_size);

		sync_remain = ubwcp->ula_pool_size;
		sync_offset = 0;
		while (sync_remain > 0) {
			if (atomic_read(&ubwcp->num_non_lin_buffers) > 0) {

				trace_ubwcp_offline_sync_end(ubwcp->ula_pool_size);
				DBG("Cancel memory offlining");

				DBG("Calling offline_and_remove_memory() for ULA PA pool");
				trace_ubwcp_offline_and_remove_memory_start(ubwcp->ula_pool_size);
				ret = offline_and_remove_memory(ubwcp->ula_pool_base,
						ubwcp->ula_pool_size);
				trace_ubwcp_offline_and_remove_memory_end(ubwcp->ula_pool_size);
				if (ret) {
					ERR("remove memory failed st:0x%lx sz:0x%lx err: %d",
						ubwcp->ula_pool_base,
						ubwcp->ula_pool_size, ret);
					goto err_remove_mem;
				} else {
					DBG("DONE: calling remove memory for ULA PA pool");
				}

				ret = add_ula_pa_memory(ubwcp);
				if (ret) {
					ERR("Bad state: failed to add back memory");
					/* Fix to put driver in invalid state */
					ubwcp->mem_online = false;
				}
				mutex_unlock(&ubwcp->mem_hotplug_lock);
				return ret;
			}

			if (sync_granule > sync_remain) {
				sync_size = sync_remain;
				sync_remain = 0;
			} else {
				sync_size = sync_granule;
				sync_remain -= sync_granule;
			}

			DBG("Partial sync offset:0x%lx size:0x%lx", sync_offset, sync_size);
			trace_ubwcp_dma_sync_single_for_cpu_start(sync_size);
			dma_sync_single_for_cpu(ubwcp->dev, ubwcp->ula_pool_base + sync_offset,
					sync_size, DMA_BIDIRECTIONAL);
			trace_ubwcp_dma_sync_single_for_cpu_end(sync_size);
			sync_offset += sync_size;
		}
		trace_ubwcp_offline_sync_end(ubwcp->ula_pool_size);

		DBG("Calling offline_and_remove_memory() for ULA PA pool");
		trace_ubwcp_offline_and_remove_memory_start(ubwcp->ula_pool_size);
		ret = offline_and_remove_memory(ubwcp->ula_pool_base, ubwcp->ula_pool_size);
		trace_ubwcp_offline_and_remove_memory_end(ubwcp->ula_pool_size);
		if (ret) {
			ERR("offline_and_remove_memory failed st:0x%lx sz:0x%lx err: %d",
				ubwcp->ula_pool_base,
				ubwcp->ula_pool_size, ret);
			/* Fix to put driver in invalid state */
			goto err_remove_mem;
		} else {
			DBG("DONE: calling offline_and_remove_memory() for ULA PA pool");
		}
		DBG("Calling power OFF ...");
		ubwcp_power(ubwcp, false);
		ubwcp->mem_online = false;
	}
	mutex_unlock(&ubwcp->mem_hotplug_lock);
	return 0;

err_remove_mem:
	atomic_inc(&ubwcp->num_non_lin_buffers);
	mutex_unlock(&ubwcp->mem_hotplug_lock);

	DBG("returning error: %d", ret);
	return ret;
}

/**
 *
 * Initialize ubwcp buffer for the given dma_buf. This
 * initializes ubwcp internal data structures and possibly hw to
 * use ubwcp for this buffer.
 *
 * @param dmabuf : ptr to the buffer to be configured for ubwcp
 *
 * @return int : 0 on success, otherwise error code
 */
static int ubwcp_init_buffer(struct dma_buf *dmabuf)
{
	struct ubwcp_buf *buf;
	struct ubwcp_driver *ubwcp = ubwcp_get_driver();
	unsigned long flags;

	FENTRY();
	trace_ubwcp_init_buffer_start(dmabuf);

	if (!ubwcp) {
		trace_ubwcp_init_buffer_end(dmabuf);
		return -1;
	}

	if (!dmabuf) {
		ERR("NULL dmabuf input ptr");
		trace_ubwcp_init_buffer_end(dmabuf);
		return -EINVAL;
	}

	if (dma_buf_to_ubwcp_buf(dmabuf)) {
		ERR("dma_buf already initialized for ubwcp");
		trace_ubwcp_init_buffer_end(dmabuf);
		return -EEXIST;
	}

	buf = kzalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf) {
		ERR("failed to alloc for new ubwcp_buf");
		trace_ubwcp_init_buffer_end(dmabuf);
		return -ENOMEM;
	}

	mutex_init(&buf->lock);
	buf->dma_buf = dmabuf;
	buf->ubwcp   = ubwcp;
	buf->buf_attr.image_format = UBWCP_LINEAR;

	spin_lock_irqsave(&ubwcp->buf_table_lock, flags);
	hash_add(ubwcp->buf_table, &buf->hnode, (u64)buf->dma_buf);
	spin_unlock_irqrestore(&ubwcp->buf_table_lock, flags);

	trace_ubwcp_init_buffer_end(dmabuf);
	return 0;
}

static void dump_attributes(struct ubwcp_buffer_attrs *attr)
{
	DBG_BUF_ATTR("");
	DBG_BUF_ATTR("image_format: %d", attr->image_format);
	DBG_BUF_ATTR("major_ubwc_ver: %d", attr->major_ubwc_ver);
	DBG_BUF_ATTR("minor_ubwc_ver: %d", attr->minor_ubwc_ver);
	DBG_BUF_ATTR("compression_type: %d", attr->compression_type);
	DBG_BUF_ATTR("lossy_params: %llu", attr->lossy_params);
	DBG_BUF_ATTR("width: %d", attr->width);
	DBG_BUF_ATTR("height: %d", attr->height);
	DBG_BUF_ATTR("stride: %d", attr->stride);
	DBG_BUF_ATTR("scanlines: %d", attr->scanlines);
	DBG_BUF_ATTR("planar_padding: %d", attr->planar_padding);
	DBG_BUF_ATTR("subsample: %d", attr->subsample);
	DBG_BUF_ATTR("sub_system_target: %d", attr->sub_system_target);
	DBG_BUF_ATTR("y_offset: %d", attr->y_offset);
	DBG_BUF_ATTR("batch_size: %d", attr->batch_size);
	DBG_BUF_ATTR("");
}

static enum ubwcp_std_image_format to_std_format(u16 ioctl_image_format)
{
	switch (ioctl_image_format) {
	case UBWCP_RGBA8888:
		return RGBA;
	case UBWCP_NV12:
	case UBWCP_NV12_Y:
	case UBWCP_NV12_UV:
		return NV12;
	case UBWCP_NV124R:
	case UBWCP_NV124R_Y:
	case UBWCP_NV124R_UV:
		return NV124R;
	case UBWCP_TP10:
	case UBWCP_TP10_Y:
	case UBWCP_TP10_UV:
		return TP10;
	case UBWCP_P010:
	case UBWCP_P010_Y:
	case UBWCP_P010_UV:
		return P010;
	case UBWCP_P016:
	case UBWCP_P016_Y:
	case UBWCP_P016_UV:
		return P016;
	default:
		WARN(1, "Fix this!!!");
		return STD_IMAGE_FORMAT_INVALID;
	}
}

static int get_stride_alignment(enum ubwcp_std_image_format format, u16 *align)
{
	switch (format) {
	case TP10:
		*align = 64;
		return 0;
	case NV12:
		*align = 128;
		return 0;
	case RGBA:
	case NV124R:
	case P010:
	case P016:
		*align = 256;
		return 0;
	default:
		return -1;
	}
}

/* returns stride of compressed image */
static u32 get_compressed_stride(struct ubwcp_driver *ubwcp,
					enum ubwcp_std_image_format format, u32 width)
{
	struct ubwcp_plane_info p_info;
	u16 macro_tile_width_p;
	u16 pixel_bytes;
	u16 per_pixel;

	p_info = ubwcp->format_info[format].p_info[0];
	macro_tile_width_p = p_info.macrotilesize_p.width;
	pixel_bytes = p_info.pixel_bytes;
	per_pixel = p_info.per_pixel;

	return UBWCP_ALIGN(width, macro_tile_width_p)*pixel_bytes/per_pixel;
}

/* check if linear stride conforms to hw limitations
 * always returns false for linear image
 */
static bool stride_is_valid(struct ubwcp_driver *ubwcp,
				u16 ioctl_img_fmt, u32 width, u32 lin_stride)
{
	u32 compressed_stride;
	enum ubwcp_std_image_format format = to_std_format(ioctl_img_fmt);

	if (format == STD_IMAGE_FORMAT_INVALID)
		return false;

	if ((lin_stride < width) || (lin_stride > 64*1024)) {
		ERR("stride is not valid (width <= stride <= 64K): %d", lin_stride);
		return false;
	}

	if (format == TP10) {
		if(!IS_ALIGNED(lin_stride, 64)) {
			ERR("stride must be aligned to 64: %d", lin_stride);
			return false;
		}
	} else {
		compressed_stride = get_compressed_stride(ubwcp, format, width);
		if (lin_stride != compressed_stride) {
			ERR("linear stride: %d must be same as compressed stride: %d",
				lin_stride, compressed_stride);
			return false;
		}
	}

	return true;
}

static bool ioctl_format_is_valid(u16 ioctl_image_format)
{
	switch (ioctl_image_format) {
	case UBWCP_LINEAR:
	case UBWCP_RGBA8888:
	case UBWCP_NV12:
	case UBWCP_NV12_Y:
	case UBWCP_NV12_UV:
	case UBWCP_NV124R:
	case UBWCP_NV124R_Y:
	case UBWCP_NV124R_UV:
	case UBWCP_TP10:
	case UBWCP_TP10_Y:
	case UBWCP_TP10_UV:
	case UBWCP_P010:
	case UBWCP_P010_Y:
	case UBWCP_P010_UV:
	case UBWCP_P016:
	case UBWCP_P016_Y:
	case UBWCP_P016_UV:
		return true;
	default:
		return false;
	}
}

/* validate buffer attributes */
static bool ubwcp_buf_attrs_valid(struct ubwcp_driver *ubwcp, struct ubwcp_buffer_attrs *attr)
{
	if (!ioctl_format_is_valid(attr->image_format)) {
		ERR("invalid image format: %d", attr->image_format);
		goto err;
	}

	if (attr->major_ubwc_ver || attr->minor_ubwc_ver) {
		ERR("major/minor ubwc ver must be 0. major: %d minor: %d",
			attr->major_ubwc_ver, attr->minor_ubwc_ver);
		goto err;
	}

	if (attr->compression_type != UBWCP_COMPRESSION_LOSSLESS) {
		ERR("compression_type is not valid: %d",
			attr->compression_type);
		goto err;
	}

	if (attr->lossy_params != 0) {
		ERR("lossy_params is not valid: %d", attr->lossy_params);
		goto err;
	}

	//TBD: some upper limit for width?
	if (attr->width > 10*1024) {
		ERR("width is invalid (above upper limit): %d", attr->width);
		goto err;
	}

	//TBD: some upper limit for height?
	if (attr->height > 10*1024) {
		ERR("height is invalid (above upper limit): %d", attr->height);
		goto err;
	}

	if (attr->image_format != UBWCP_LINEAR)
		if(!stride_is_valid(ubwcp, attr->image_format, attr->width, attr->stride)) {
			ERR("stride is invalid: %d", attr->stride);
			goto err;
		}

	if ((attr->scanlines < attr->height) ||
	    (attr->scanlines > attr->height + 32*1024)) {
		ERR("scanlines is not valid - height: %d scanlines: %d",
			attr->height, attr->scanlines);
		goto err;
	}

	if (attr->planar_padding > 4096) {
		ERR("planar_padding is not valid. (<= 4096): %d",
			attr->planar_padding);
		goto err;
	}

	if (attr->subsample != UBWCP_SUBSAMPLE_4_2_0)  {
		ERR("subsample is not valid: %d", attr->subsample);
		goto err;
	}

	if (attr->sub_system_target & ~UBWCP_SUBSYSTEM_TARGET_CPU) {
		ERR("sub_system_target other that CPU is not supported: %d",
			attr->sub_system_target);
		goto err;
	}

	if (!(attr->sub_system_target & UBWCP_SUBSYSTEM_TARGET_CPU)) {
		ERR("sub_system_target is not set to CPU: %d",
			attr->sub_system_target);
		goto err;
	}

	if (attr->y_offset != 0) {
		ERR("y_offset is not valid: %d", attr->y_offset);
		goto err;
	}

	if (attr->batch_size != 1) {
		ERR("batch_size is not valid: %d", attr->batch_size);
		goto err;
	}

	dump_attributes(attr);
	return true;
err:
	dump_attributes(attr);
	return false;
}


/* return true if image format has only Y plane*/
bool ubwcp_image_y_only(u16 format)
{
	switch (format) {
	case UBWCP_NV12_Y:
	case UBWCP_NV124R_Y:
	case UBWCP_TP10_Y:
	case UBWCP_P010_Y:
	case UBWCP_P016_Y:
		return true;
	default:
		return false;
	}
}


/* return true if image format has only UV plane*/
bool ubwcp_image_uv_only(u16 format)
{
	switch (format) {
	case UBWCP_NV12_UV:
	case UBWCP_NV124R_UV:
	case UBWCP_TP10_UV:
	case UBWCP_P010_UV:
	case UBWCP_P016_UV:
		return true;
	default:
		return false;
	}
}

/* calculate and return metadata buffer size for a given plane
 * and buffer attributes
 * NOTE: in this function, we will only pass in NV12 format.
 * NOT NV12_Y or NV12_UV etc.
 * the Y or UV information is in the "plane"
 * "format" here purely means "encoding format" and no information
 * if some plane data is missing.
 */
static size_t metadata_buf_sz(struct ubwcp_driver *ubwcp,
				enum ubwcp_std_image_format format,
				u32 width, u32 height, u8 plane)
{
	size_t size;
	u64 pitch;
	u64 lines;
	u64 tile_width;
	u32 tile_height;

	struct ubwcp_image_format_info f_info;
	struct ubwcp_plane_info p_info;

	f_info = ubwcp->format_info[format];

	DBG_BUF_ATTR("");
	DBG_BUF_ATTR("");
	DBG_BUF_ATTR("Calculating metadata buffer size: format = %d, plane = %d", format, plane);

	if (plane >= f_info.planes) {
		ERR("Format does not have requested plane info: format: %d, plane: %d",
			format, plane);
		WARN(1, "Fix this!!!!!");
		return 0;
	}

	p_info = f_info.p_info[plane];

	/* UV plane */
	if (plane == 1) {
		width  = width/2;
		height = height/2;
	}

	tile_width  = p_info.tilesize_p.width;
	tile_height = p_info.tilesize_p.height;

	/* pitch: # of tiles in a row
	 * lines: # of tile rows
	 */
	pitch =  UBWCP_ALIGN((width  + tile_width  - 1)/tile_width,  META_DATA_PITCH_ALIGN);
	lines =  UBWCP_ALIGN((height + tile_height - 1)/tile_height, META_DATA_HEIGHT_ALIGN);

	DBG_BUF_ATTR("image params     : %d x %d (pixels)", width, height);
	DBG_BUF_ATTR("tile  params     : %d x %d (pixels)", tile_width, tile_height);
	DBG_BUF_ATTR("pitch            : %d (%d)", pitch, width/tile_width);
	DBG_BUF_ATTR("lines            : %d (%d)", lines, height);
	DBG_BUF_ATTR("size (p*l*bytes) : %d", pitch*lines*1);

	/* x1 below is only to clarify that we are multiplying by 1 bytes/tile */
	size = UBWCP_ALIGN(pitch*lines*1, META_DATA_SIZE_ALIGN);

	DBG_BUF_ATTR("size (aligned 4K): %zu (0x%zx)", size, size);
	return size;
}


/* calculate and return size of pixel data buffer for a given plane
 * and buffer attributes
 */
static size_t pixeldata_buf_sz(struct ubwcp_driver *ubwcp,
					u16 format, u32 width,
					u32 height, u8 plane)
{
	size_t size;
	u64 pitch;
	u64 lines;
	u16 pixel_bytes;
	u16 per_pixel;
	u64 macro_tile_width_p;
	u64 macro_tile_height_p;

	struct ubwcp_image_format_info f_info;
	struct ubwcp_plane_info p_info;

	f_info = ubwcp->format_info[format];

	DBG_BUF_ATTR("");
	DBG_BUF_ATTR("");
	DBG_BUF_ATTR("Calculating Pixeldata buffer size: format = %d, plane = %d", format, plane);

	if (plane >= f_info.planes) {
		ERR("Format does not have requested plane info: format: %d, plane: %d",
			format, plane);
		WARN(1, "Fix this!!!!!");
		return 0;
	}

	p_info = f_info.p_info[plane];

	pixel_bytes = p_info.pixel_bytes;
	per_pixel   = p_info.per_pixel;

	/* UV plane */
	if (plane == 1) {
		width  = width/2;
		height = height/2;
	}

	macro_tile_width_p  = p_info.macrotilesize_p.width;
	macro_tile_height_p = p_info.macrotilesize_p.height;

	/* align pixel width and height macro tile width and height */
	pitch = UBWCP_ALIGN(width, macro_tile_width_p);
	lines = UBWCP_ALIGN(height, macro_tile_height_p);

	DBG_BUF_ATTR("image params     : %d x %d (pixels)", width, height);
	DBG_BUF_ATTR("macro tile params: %d x %d (pixels)", macro_tile_width_p,
								macro_tile_height_p);
	DBG_BUF_ATTR("bytes_per_pixel  : %d/%d", pixel_bytes, per_pixel);
	DBG_BUF_ATTR("pitch            : %d", pitch);
	DBG_BUF_ATTR("lines            : %d", lines);
	DBG_BUF_ATTR("size (p*l*bytes) : %d", (pitch*lines*pixel_bytes)/per_pixel);

	size  = UBWCP_ALIGN((pitch*lines*pixel_bytes)/per_pixel, PIXEL_DATA_SIZE_ALIGN);

	DBG_BUF_ATTR("size (aligned 4K): %zu (0x%zx)", size, size);

	return size;
}

static int get_tile_height(struct ubwcp_driver *ubwcp, enum ubwcp_std_image_format format,
			   u8 plane)
{
	struct ubwcp_image_format_info f_info;
	struct ubwcp_plane_info p_info;

	f_info = ubwcp->format_info[format];
	p_info = f_info.p_info[plane];
	return p_info.tilesize_p.height;
}

/*
 * plane: must be 0 or 1 (1st plane == 0, 2nd plane == 1)
 */
static size_t ubwcp_ula_size(struct ubwcp_driver *ubwcp, u16 format,
					u32 stride_b, u32 scanlines, u8 plane,
					bool add_tile_pad)
{
	size_t size;

	DBG_BUF_ATTR("%s(format = %d, plane = %d)", __func__, format, plane);
	/* UV plane */
	if (plane == 1)
		scanlines = scanlines/2;

	if (add_tile_pad) {
		int tile_height = get_tile_height(ubwcp, format, plane);

		/* Align plane size to plane tile height */
		scanlines = ((scanlines + tile_height - 1) / tile_height) * tile_height;
	}
	size = stride_b*scanlines;
	DBG_BUF_ATTR("Size of plane-%u: (%u * %u) = %zu (0x%zx)",
		plane, stride_b, scanlines, size, size);
	return size;
}

int missing_plane_from_format(u16 ioctl_image_format)
{
	int missing_plane;

	switch (ioctl_image_format) {
	case UBWCP_NV12_Y:
		missing_plane = 2;
		break;
	case UBWCP_NV12_UV:
		missing_plane = 1;
		break;
	case UBWCP_NV124R_Y:
		missing_plane = 2;
		break;
	case UBWCP_NV124R_UV:
		missing_plane = 1;
		break;
	case UBWCP_TP10_Y:
		missing_plane = 2;
		break;
	case UBWCP_TP10_UV:
		missing_plane = 1;
		break;
	case UBWCP_P010_Y:
		missing_plane = 2;
		break;
	case UBWCP_P010_UV:
		missing_plane = 1;
		break;
	case UBWCP_P016_Y:
		missing_plane = 2;
		break;
	case UBWCP_P016_UV:
		missing_plane = 1;
		break;
	default:
		missing_plane = 0;
	}
	return missing_plane;
}

int planes_in_format(enum ubwcp_std_image_format format)
{
	if (format == RGBA)
		return 1;
	else
		return 2;
}

unsigned int ubwcp_get_hw_image_format_value(u16 ioctl_image_format)
{
	enum ubwcp_std_image_format format;

	format = to_std_format(ioctl_image_format);
	switch (format) {
	case RGBA:
		return HW_BUFFER_FORMAT_RGBA;
	case NV12:
		return HW_BUFFER_FORMAT_NV12;
	case NV124R:
		return HW_BUFFER_FORMAT_NV124R;
	case P010:
		return HW_BUFFER_FORMAT_P010;
	case TP10:
		return HW_BUFFER_FORMAT_TP10;
	case P016:
		return HW_BUFFER_FORMAT_P016;
	default:
		WARN(1, "Fix this!!!!!");
		return 0;
	}
}

static int ubwcp_validate_uv_align(struct ubwcp_driver *ubwcp,
					struct ubwcp_buffer_attrs *attr,
					size_t ula_y_plane_size,
					size_t uv_start_offset)
{
	int ret = 0;
	size_t ula_y_plane_size_align;
	size_t y_tile_align_bytes;
	int y_tile_height;
	int planes;

	/* Only validate UV align if there is both a Y and UV plane */
	planes = planes_in_format(to_std_format(attr->image_format));
	if (planes != 2)
		return 0;

	/* Check it is cache line size aligned */
	if ((uv_start_offset % 64) != 0) {
		ret = -EINVAL;
		ERR("uv_start_offset %zu not cache line aligned",
				uv_start_offset);
		goto err;
	}

	/*
	 * Check that UV plane does not overlap with any of the Y plane’s tiles
	 */
	y_tile_height = get_tile_height(ubwcp, to_std_format(attr->image_format), 0);
	y_tile_align_bytes = y_tile_height * attr->stride;
	ula_y_plane_size_align = ((ula_y_plane_size + y_tile_align_bytes - 1) /
					y_tile_align_bytes) * y_tile_align_bytes;

	if (uv_start_offset < ula_y_plane_size_align) {
		ret = -EINVAL;
		ERR("uv offset %zu less than y plane align %zu for y plane size %zu",
			uv_start_offset, ula_y_plane_size_align,
			ula_y_plane_size);
		goto err;
	}

	return 0;

err:
	return ret;
}

/* calculate ULA buffer parms
 * TBD: how do we make sure uv_start address (not the offset)
 * is aligned per requirement: cache line
 */
static int ubwcp_calc_ula_params(struct ubwcp_driver *ubwcp,
					struct ubwcp_buffer_attrs *attr,
					size_t *ula_size,
					size_t *ula_y_plane_size,
					size_t *uv_start_offset)
{
	size_t size;
	enum ubwcp_std_image_format format;
	int planes;
	int missing_plane;
	u32 stride;
	u32 scanlines;
	u32 planar_padding;

	stride         = attr->stride;
	scanlines      = attr->scanlines;
	planar_padding = attr->planar_padding;

	/* convert ioctl image format to standard image format */
	format = to_std_format(attr->image_format);


	/* Number of "expected" planes in "the standard defined" image format */
	planes = planes_in_format(format);

	/* any plane missing?
	 * valid missing_plane values:
	 *      0 == no plane missing
	 *      1 == 1st plane missing
	 *      2 == 2nd plane missing
	 */
	missing_plane = missing_plane_from_format(attr->image_format);

	DBG_BUF_ATTR("ioctl_image_format : %d, std_format: %d", attr->image_format, format);
	DBG_BUF_ATTR("planes_in_format   : %d", planes);
	DBG_BUF_ATTR("missing_plane      : %d", missing_plane);
	DBG_BUF_ATTR("Planar Padding     : %d", planar_padding);

	if (planes == 1) {
		/* uv_start beyond ULA range */
		size = ubwcp_ula_size(ubwcp, format, stride, scanlines, 0, true);
		*uv_start_offset = size;
		*ula_y_plane_size = size;
	} else {
		if (!missing_plane) {
			/* size for both planes and padding */

			/* Don't pad out Y plane as client would not expect this padding */
			size = ubwcp_ula_size(ubwcp, format, stride, scanlines, 0, false);
			*ula_y_plane_size = size;
			size += planar_padding;
			*uv_start_offset = size;
			size += ubwcp_ula_size(ubwcp, format, stride, scanlines, 1, true);
		} else  {
			if (missing_plane == 2) {
				/* Y-only image, set uv_start beyond ULA range */
				size = ubwcp_ula_size(ubwcp, format, stride, scanlines, 0, true);
				*uv_start_offset = size;
				*ula_y_plane_size = size;
			} else {
				/* first plane data is not there */
				size = ubwcp_ula_size(ubwcp, format, stride, scanlines, 1, true);
				*uv_start_offset = 0; /* uv data is at the beginning */
				*ula_y_plane_size = 0;
			}
		}
	}

	//TBD: cleanup
	*ula_size = size;
	DBG_BUF_ATTR("Before page align: Total ULA_Size: %d (0x%x) (planes + planar padding)",
									*ula_size, *ula_size);
	*ula_size = UBWCP_ALIGN(size, 4096);
	DBG_BUF_ATTR("After page align : Total ULA_Size: %d (0x%x) (planes + planar padding)",
									*ula_size, *ula_size);
	return 0;
}


/* calculate UBWCP buffer parms */
static int ubwcp_calc_ubwcp_buf_params(struct ubwcp_driver *ubwcp,
					struct ubwcp_buffer_attrs *attr,
					size_t *md_p0, size_t *pd_p0,
					size_t *md_p1, size_t *pd_p1,
					size_t *stride_tp10_b)
{
	int planes;
	int missing_plane;
	enum ubwcp_std_image_format format;
	size_t stride_tp10_p;

	FENTRY();

	/* convert ioctl image format to standard image format */
	format = to_std_format(attr->image_format);
	missing_plane = missing_plane_from_format(attr->image_format);
	planes = planes_in_format(format); //pass in 0 (RGB) should return 1

	DBG_BUF_ATTR("ioctl_image_format : %d, std_format: %d", attr->image_format, format);
	DBG_BUF_ATTR("planes_in_format   : %d", planes);
	DBG_BUF_ATTR("missing_plane      : %d", missing_plane);

	if (!missing_plane) {
		*md_p0 = metadata_buf_sz(ubwcp, format, attr->width, attr->height, 0);
		*pd_p0 = pixeldata_buf_sz(ubwcp, format, attr->width, attr->height, 0);
		if (planes == 2) {
			*md_p1 = metadata_buf_sz(ubwcp, format, attr->width, attr->height, 1);
			*pd_p1 = pixeldata_buf_sz(ubwcp, format, attr->width, attr->height, 1);
		}
	} else {
		if (missing_plane == 1) {
			*md_p0 = 0;
			*pd_p0 = 0;
			*md_p1 = metadata_buf_sz(ubwcp, format, attr->width, attr->height, 1);
			*pd_p1 = pixeldata_buf_sz(ubwcp, format, attr->width, attr->height, 1);
		} else {
			*md_p0 = metadata_buf_sz(ubwcp, format, attr->width, attr->height, 0);
			*pd_p0 = pixeldata_buf_sz(ubwcp, format, attr->width, attr->height, 0);
			*md_p1 = 0;
			*pd_p1 = 0;
		}
	}

	if (format == TP10) {
		stride_tp10_p = UBWCP_ALIGN(attr->width, 192);
		*stride_tp10_b = (stride_tp10_p/3) + stride_tp10_p;
	} else {
		*stride_tp10_b = 0;
	}

	return 0;
}


/* reserve ULA address space of the given size */
static phys_addr_t ubwcp_ula_alloc(struct ubwcp_driver *ubwcp, size_t size)
{
	phys_addr_t pa;

	mutex_lock(&ubwcp->ula_lock);
	pa = gen_pool_alloc(ubwcp->ula_pool, size);
	DBG("addr: %p, size: %zx", pa, size);
	mutex_unlock(&ubwcp->ula_lock);
	return pa;
}


/* free ULA address space of the given address and size */
static void ubwcp_ula_free(struct ubwcp_driver *ubwcp, phys_addr_t pa, size_t size)
{
	mutex_lock(&ubwcp->ula_lock);
	if (!gen_pool_has_addr(ubwcp->ula_pool, pa, size)) {
		ERR("Attempt to free mem not from gen_pool: pa: %p, size: %zx", pa, size);
		goto err;
	}
	DBG("addr: %p, size: %zx", pa, size);
	gen_pool_free(ubwcp->ula_pool, pa, size);
	mutex_unlock(&ubwcp->ula_lock);
	return;

err:
	mutex_unlock(&ubwcp->ula_lock);
}


/* free up or expand current_pa and return the new pa */
static phys_addr_t ubwcp_ula_realloc(struct ubwcp_driver *ubwcp,
					phys_addr_t pa,
					size_t size,
					size_t new_size)
{
	if (size == new_size)
		return pa;

	if (pa)
		ubwcp_ula_free(ubwcp, pa, size);

	return ubwcp_ula_alloc(ubwcp, new_size);
}


/* unmap dma buf */
static void ubwcp_dma_unmap(struct ubwcp_buf *buf)
{
	FENTRY();
	if (buf->dma_buf && buf->attachment) {
		DBG("Calling dma_buf_unmap_attachment()");
		dma_buf_unmap_attachment(buf->attachment, buf->sgt, DMA_BIDIRECTIONAL);
		buf->sgt = NULL;
		dma_buf_detach(buf->dma_buf, buf->attachment);
		buf->attachment = NULL;
	}
}


/* dma map ubwcp buffer */
static int ubwcp_dma_map(struct ubwcp_buf *buf,
				struct device *dev,
				size_t iova_min_size,
				dma_addr_t *iova)
{
	int ret = 0;
	struct dma_buf *dma_buf = buf->dma_buf;
	struct dma_buf_attachment *attachment;
	struct sg_table *sgt;
	size_t dma_len;

	/* Map buffer to SMMU and get IOVA */
	attachment = dma_buf_attach(dma_buf, dev);
	if (IS_ERR(attachment)) {
		ret = PTR_ERR(attachment);
		ERR("dma_buf_attach() failed: %d", ret);
		goto err;
	}

	dma_set_max_seg_size(dev, DMA_BIT_MASK(32));
	dma_set_seg_boundary(dev, (unsigned long)DMA_BIT_MASK(64));

	sgt = dma_buf_map_attachment(attachment, DMA_BIDIRECTIONAL);
	if (IS_ERR_OR_NULL(sgt)) {
		ret = PTR_ERR(sgt);
		ERR("dma_buf_map_attachment() failed: %d", ret);
		goto err_detach;
	}

	if (sgt->nents != 1) {
		ERR("nents = %d", sgt->nents);
		goto err_unmap;
	}

	/* ensure that dma_buf is big enough for the new attrs */
	dma_len = sg_dma_len(sgt->sgl);
	if (dma_len < iova_min_size) {
		ERR("dma len: %d is less than min ubwcp buffer size: %d",
							dma_len, iova_min_size);
		goto err_unmap;
	}

	*iova = sg_dma_address(sgt->sgl);
	buf->attachment = attachment;
	buf->sgt = sgt;
	return ret;

err_unmap:
	dma_buf_unmap_attachment(attachment, sgt, DMA_BIDIRECTIONAL);
err_detach:
	dma_buf_detach(dma_buf, attachment);
err:
	if (!ret)
		ret = -1;
	return ret;
}

static void
ubwcp_pixel_to_bytes(struct ubwcp_driver *ubwcp,
			enum ubwcp_std_image_format format,
			u32 width_p, u32 height_p,
			u32 *width_b, u32 *height_b)
{
	u16 pixel_bytes;
	u16 per_pixel;
	struct ubwcp_image_format_info f_info;
	struct ubwcp_plane_info p_info;

	f_info = ubwcp->format_info[format];
	p_info = f_info.p_info[0];

	pixel_bytes = p_info.pixel_bytes;
	per_pixel   = p_info.per_pixel;

	*width_b  = (width_p*pixel_bytes)/per_pixel;
	*height_b = (height_p*pixel_bytes)/per_pixel;
}

static void reset_buf_attrs(struct ubwcp_buf *buf)
{
	struct ubwcp_hw_meta_metadata *mmdata;
	struct ubwcp_driver *ubwcp;

	ubwcp = buf->ubwcp;
	mmdata = &buf->mmdata;

	ubwcp_dma_unmap(buf);

	/* reset ula params */
	if (buf->ula_size) {
		ubwcp_ula_free(ubwcp, buf->ula_pa, buf->ula_size);
		buf->ula_size = 0;
		buf->ula_pa = 0;
	}
	/* reset ubwcp params */
	memset(mmdata, 0, sizeof(*mmdata));
	buf->buf_attr_set = false;
	buf->buf_attr.image_format = UBWCP_LINEAR;
}

static void print_mmdata_desc(struct ubwcp_hw_meta_metadata *mmdata)
{
	DBG_BUF_ATTR("");
	DBG_BUF_ATTR("--------MM_DATA DESC ---------");
	DBG_BUF_ATTR("uv_start_addr   : 0x%08llx (cache addr) (actual: 0x%llx)",
					mmdata->uv_start_addr, mmdata->uv_start_addr << 6);
	DBG_BUF_ATTR("format          : 0x%08x", mmdata->format);
	DBG_BUF_ATTR("stride          : 0x%08x (cache addr) (actual: 0x%x)",
					mmdata->stride, mmdata->stride << 6);
	DBG_BUF_ATTR("stride_ubwcp    : 0x%08x (cache addr) (actual: 0x%zx)",
					mmdata->stride_ubwcp, mmdata->stride_ubwcp << 6);
	DBG_BUF_ATTR("metadata_base_y : 0x%08x (page addr)  (actual: 0x%llx)",
					mmdata->metadata_base_y,  mmdata->metadata_base_y << 12);
	DBG_BUF_ATTR("metadata_base_uv: 0x%08x (page addr)  (actual: 0x%zx)",
					mmdata->metadata_base_uv, mmdata->metadata_base_uv << 12);
	DBG_BUF_ATTR("buffer_y_offset : 0x%08x (page addr)  (actual: 0x%zx)",
					mmdata->buffer_y_offset,  mmdata->buffer_y_offset << 12);
	DBG_BUF_ATTR("buffer_uv_offset: 0x%08x (page addr)  (actual: 0x%zx)",
					mmdata->buffer_uv_offset, mmdata->buffer_uv_offset << 12);
	DBG_BUF_ATTR("width_height    : 0x%08x (width: 0x%x height: 0x%x)",
		mmdata->width_height, mmdata->width_height >> 16, mmdata->width_height & 0xFFFF);
	DBG_BUF_ATTR("");
}

/* set buffer attributes:
 * Failure:
 * If a call to ubwcp_set_buf_attrs() fails, any attributes set from a previously
 * successful ubwcp_set_buf_attrs() will be also removed. Thus,
 * ubwcp_set_buf_attrs() implicitly does "unset previous attributes" and
 * then "try to set these new attributes".
 *
 * The result of a failed call to ubwcp_set_buf_attrs() will leave the buffer
 * in a linear mode, NOT with attributes from earlier successful call.
 */
int ubwcp_set_buf_attrs(struct dma_buf *dmabuf, struct ubwcp_buffer_attrs *attr)
{
	int ret = 0;
	size_t ula_size = 0;
	size_t uv_start_offset = 0;
	size_t ula_y_plane_size = 0;
	phys_addr_t ula_pa = 0x0;
	struct ubwcp_buf *buf;
	struct ubwcp_driver *ubwcp;

	size_t metadata_p0;
	size_t pixeldata_p0;
	size_t metadata_p1;
	size_t pixeldata_p1;
	size_t iova_min_size;
	size_t stride_tp10_b;
	dma_addr_t iova_base;
	struct ubwcp_hw_meta_metadata *mmdata;
	u64 uv_start;
	u32 stride_b;
	u32 width_b;
	u32 height_b;
	enum ubwcp_std_image_format std_image_format;
	bool is_non_lin_buf;

	FENTRY();
	trace_ubwcp_set_buf_attrs_start(dmabuf);

	if (!dmabuf) {
		ERR("NULL dmabuf input ptr");
		trace_ubwcp_set_buf_attrs_end(dmabuf);
		return -EINVAL;
	}

	if (!attr) {
		ERR("NULL attr ptr");
		trace_ubwcp_set_buf_attrs_end(dmabuf);
		return -EINVAL;
	}

	buf = dma_buf_to_ubwcp_buf(dmabuf);
	if (!buf) {
		ERR("No corresponding ubwcp_buf for the passed in dma_buf");
		trace_ubwcp_set_buf_attrs_end(dmabuf);
		return -EINVAL;
	}

	mutex_lock(&buf->lock);

	if (buf->locked) {
		ERR("Cannot set attr when buffer is locked");
		ret = -EBUSY;
		goto unlock;
	}

	ubwcp  = buf->ubwcp;
	mmdata = &buf->mmdata;
	is_non_lin_buf = (buf->buf_attr.image_format != UBWCP_LINEAR);

	//TBD: now that we have single exit point for all errors,
	//we can limit this call to error only?
	//also see if this can be part of reset_buf_attrs()
	DBG_BUF_ATTR("resetting mmap to linear");
	/* remove any earlier dma buf mmap configuration */
	ret = ubwcp->mmap_config_fptr(buf->dma_buf, true, 0, 0);
	if (ret) {
		ERR("dma_buf_mmap_config() failed: %d", ret);
		goto unlock;
	}

	if (!ubwcp_buf_attrs_valid(ubwcp, attr)) {
		ERR("Invalid buf attrs");
		goto err;
	}

	if (attr->image_format == UBWCP_LINEAR) {
		DBG_BUF_ATTR("Linear format requested");


		/* linear format request with permanent range xlation doesn't
		 * make sense. need to define behavior if this happens.
		 * note: with perm set, desc is allocated to this buffer.
		 */
		//TBD: UBWCP_ASSERT(!buf->perm);

		if (buf->buf_attr_set)
			reset_buf_attrs(buf);

		if (is_non_lin_buf) {
			/*
			 * Changing buffer from ubwc to linear so decrement
			 * number of ubwc buffers
			 */
			ret = dec_num_non_lin_buffers(ubwcp);
		}

		mutex_unlock(&buf->lock);
		trace_ubwcp_set_buf_attrs_end(dmabuf);
		return ret;
	}

	std_image_format = to_std_format(attr->image_format);
	if (std_image_format == STD_IMAGE_FORMAT_INVALID) {
		ERR("Unable to map ioctl image format to std image format");
		goto err;
	}

	/* Calculate uncompressed-buffer size. */
	DBG_BUF_ATTR("");
	DBG_BUF_ATTR("");
	DBG_BUF_ATTR("Calculating ula params -->");
	ret = ubwcp_calc_ula_params(ubwcp, attr, &ula_size, &ula_y_plane_size, &uv_start_offset);
	if (ret) {
		ERR("ubwcp_calc_ula_params() failed: %d", ret);
		goto err;
	}

	ret = ubwcp_validate_uv_align(ubwcp, attr, ula_y_plane_size, uv_start_offset);
	if (ret) {
		ERR("ubwcp_validate_uv_align() failed: %d", ret);
		goto err;
	}

	DBG_BUF_ATTR("");
	DBG_BUF_ATTR("");
	DBG_BUF_ATTR("Calculating ubwcp params -->");
	ret = ubwcp_calc_ubwcp_buf_params(ubwcp, attr,
						&metadata_p0, &pixeldata_p0,
						&metadata_p1, &pixeldata_p1,
						&stride_tp10_b);
	if (ret) {
		ERR("ubwcp_calc_buf_params() failed: %d", ret);
		goto err;
	}

	iova_min_size = metadata_p0 + pixeldata_p0 + metadata_p1 + pixeldata_p1;

	DBG_BUF_ATTR("");
	DBG_BUF_ATTR("");
	DBG_BUF_ATTR("------Summary ULA  Calculated Params ------");
	DBG_BUF_ATTR("ULA Size        : %8zu (0x%8zx)", ula_size, ula_size);
	DBG_BUF_ATTR("UV Start Offset : %8zu (0x%8zx)", uv_start_offset, uv_start_offset);
	DBG_BUF_ATTR("------Summary UBCP Calculated Params ------");
	DBG_BUF_ATTR("metadata_p0     : %8d (0x%8zx)", metadata_p0, metadata_p0);
	DBG_BUF_ATTR("pixeldata_p0    : %8d (0x%8zx)", pixeldata_p0, pixeldata_p0);
	DBG_BUF_ATTR("metadata_p1     : %8d (0x%8zx)", metadata_p1, metadata_p1);
	DBG_BUF_ATTR("pixeldata_p1    : %8d (0x%8zx)", pixeldata_p1, pixeldata_p1);
	DBG_BUF_ATTR("stride_tp10     : %8d (0x%8zx)", stride_tp10_b, stride_tp10_b);
	DBG_BUF_ATTR("iova_min_size   : %8d (0x%8zx)", iova_min_size, iova_min_size);
	DBG_BUF_ATTR("");

	if (buf->buf_attr_set) {
		/* if buf attr were previously set, these must not be 0 */
		/* TBD: do we need this check in production code? */
		if (!buf->ula_pa) {
			WARN(1, "ula_pa cannot be 0 if buf_attr_set is true!!!");
			goto err;
		}
		if (!buf->ula_size) {
			WARN(1, "ula_size cannot be 0 if buf_attr_set is true!!!");
			goto err;
		}
	}

	/* assign ULA PA with uncompressed-size range */
	ula_pa = ubwcp_ula_realloc(ubwcp, buf->ula_pa, buf->ula_size, ula_size);
	if (!ula_pa) {
		ERR("ubwcp_ula_alloc/realloc() failed. running out of ULA PA space?");
		goto err;
	}

	buf->ula_size = ula_size;
	buf->ula_pa   = ula_pa;
	DBG_BUF_ATTR("Allocated ULA_PA: 0x%p of size: 0x%zx", ula_pa, ula_size);
	DBG_BUF_ATTR("");

	/* inform ULA-PA to dma-heap: needed for dma-heap to do CMOs later on */
	DBG_BUF_ATTR("Calling mmap_config(): ULA_PA: 0x%p size: 0x%zx", ula_pa, ula_size);
	ret = ubwcp->mmap_config_fptr(buf->dma_buf, false, buf->ula_pa,
								buf->ula_size);
	if (ret) {
		ERR("dma_buf_mmap_config() failed: %d", ret);
		goto err;
	}

	/* dma map only the first time attribute is set */
	if (!buf->buf_attr_set) {
		/* linear -> ubwcp. map ubwcp buffer */
		ret = ubwcp_dma_map(buf, ubwcp->dev_buf_cb, iova_min_size, &iova_base);
		if (ret) {
			ERR("ubwcp_dma_map() failed: %d", ret);
			goto err;
		}
		DBG_BUF_ATTR("dma_buf IOVA range: 0x%llx + min_size (0x%zx): 0x%llx",
					iova_base, iova_min_size, iova_base + iova_min_size);
	}

	uv_start = ula_pa + uv_start_offset;
	if (!IS_ALIGNED(uv_start, 64)) {
		ERR("ERROR: uv_start is NOT aligned to cache line");
		goto err;
	}

	/* Convert height and width to bytes for writing to mmdata */
	if (std_image_format != TP10) {
		ubwcp_pixel_to_bytes(ubwcp, std_image_format, attr->width,
					attr->height, &width_b, &height_b);
	} else {
		/* for tp10 image compression, we need to program p010 width/height */
		ubwcp_pixel_to_bytes(ubwcp, P010, attr->width,
					attr->height, &width_b, &height_b);
	}

	stride_b = attr->stride;

	/* create the mmdata descriptor */
	memset(mmdata, 0, sizeof(*mmdata));
	mmdata->uv_start_addr = CACHE_ADDR(uv_start);
	mmdata->format        = ubwcp_get_hw_image_format_value(attr->image_format);

	if (std_image_format != TP10) {
		mmdata->stride       = CACHE_ADDR(stride_b);      /* uncompressed stride */
	} else {
		mmdata->stride       = CACHE_ADDR(stride_tp10_b); /*   compressed stride */
		mmdata->stride_ubwcp = CACHE_ADDR(stride_b);      /* uncompressed stride */
	}

	mmdata->metadata_base_y  = PAGE_ADDR(iova_base);
	mmdata->metadata_base_uv = PAGE_ADDR(iova_base + metadata_p0 + pixeldata_p0);
	mmdata->buffer_y_offset  = PAGE_ADDR(metadata_p0);
	mmdata->buffer_uv_offset = PAGE_ADDR(metadata_p1);

	/* NOTE: For version 1.1, both width & height needs to be in bytes.
	 * For other versions, width in bytes & height in pixels.
	 */
	if ((ubwcp->hw_ver_major == 1) && (ubwcp->hw_ver_minor == 1))
		mmdata->width_height = width_b << 16 | height_b;
	else
		mmdata->width_height = width_b << 16 | attr->height;

	print_mmdata_desc(mmdata);
	if (!is_non_lin_buf) {
		/*
		 * Changing buffer from linear to ubwc so increment
		 * number of ubwc buffers
		 */
		ret = inc_num_non_lin_buffers(ubwcp);
	}
	if (ret) {
		ERR("inc_num_non_lin_buffers failed: %d", ret);
		goto err;
	}

	buf->buf_attr = *attr;
	buf->buf_attr_set = true;
	//TBD: UBWCP_ASSERT(!buf->perm);
	mutex_unlock(&buf->lock);
	trace_ubwcp_set_buf_attrs_end(dmabuf);
	return 0;

err:
	reset_buf_attrs(buf);
	if (is_non_lin_buf) {
		/*
		 * Changing buffer from ubwc to linear so decrement
		 * number of ubwc buffers
		 */
		dec_num_non_lin_buffers(ubwcp);
	}
unlock:
	mutex_unlock(&buf->lock);
	if (!ret)
		ret = -1;
	trace_ubwcp_set_buf_attrs_end(dmabuf);
	return ret;
}
EXPORT_SYMBOL(ubwcp_set_buf_attrs);


/* Set buffer attributes ioctl */
static int ubwcp_set_buf_attrs_ioctl(struct ubwcp_ioctl_buffer_attrs *attr_ioctl)
{
	struct dma_buf *dmabuf;

	dmabuf = ubwcp_dma_buf_fd_to_dma_buf(attr_ioctl->fd);

	return ubwcp_set_buf_attrs(dmabuf, &attr_ioctl->attr);
}


/* Free up the buffer descriptor */
static void ubwcp_buf_desc_free(struct ubwcp_driver *ubwcp, struct ubwcp_desc *desc)
{
	int idx = desc->idx;
	struct ubwcp_desc *desc_list = ubwcp->desc_list;

	mutex_lock(&ubwcp->desc_lock);
	desc_list[idx].idx = -1;
	desc_list[idx].ptr = NULL;
	DBG("freed descriptor_id: %d", idx);
	mutex_unlock(&ubwcp->desc_lock);
}


/* Allocate next available buffer descriptor. */
static struct ubwcp_desc *ubwcp_buf_desc_allocate(struct ubwcp_driver *ubwcp)
{
	int idx;
	struct ubwcp_desc *desc_list = ubwcp->desc_list;

	mutex_lock(&ubwcp->desc_lock);
	for (idx = 0; idx < UBWCP_BUFFER_DESC_COUNT; idx++) {
		if (desc_list[idx].idx == -1) {
			desc_list[idx].idx = idx;
			desc_list[idx].ptr = ubwcp->buffer_desc_base +
						idx*UBWCP_BUFFER_DESC_OFFSET;
			DBG("allocated descriptor_id: %d", idx);
			mutex_unlock(&ubwcp->desc_lock);
			return &desc_list[idx];
		}
	}
	mutex_unlock(&ubwcp->desc_lock);
	return NULL;
}

/**
 * Lock buffer for CPU access. This prepares ubwcp hw to allow
 * CPU access to the compressed buffer. It will perform
 * necessary address translation configuration and cache maintenance ops
 * so that CPU can safely access ubwcp buffer, if this call is
 * successful.
 * Allocate descriptor if not already,
 * perform CMO and then enable range check
 *
 * @param dmabuf : ptr to the dma buf
 * @param direction : direction of access
 *
 * @return int : 0 on success, otherwise error code
 */
static int ubwcp_lock(struct dma_buf *dmabuf, enum dma_data_direction dir)
{
	int ret = 0;
	struct ubwcp_buf *buf;
	struct ubwcp_driver *ubwcp;

	FENTRY();
	trace_ubwcp_lock_start(dmabuf);

	if (!dmabuf) {
		ERR("NULL dmabuf input ptr");
		trace_ubwcp_lock_end(dmabuf);
		return -EINVAL;
	}


	if (!valid_dma_direction(dir)) {
		ERR("invalid direction: %d", dir);
		trace_ubwcp_lock_end(dmabuf);
		return -EINVAL;
	}

	buf = dma_buf_to_ubwcp_buf(dmabuf);
	if (!buf) {
		ERR("ubwcp_buf ptr not found");
		trace_ubwcp_lock_end(dmabuf);
		return -1;
	}

	mutex_lock(&buf->lock);

	if (!buf->buf_attr_set) {
		ERR("lock() called on buffer, but attr not set");
		goto err;
	}

	if (buf->buf_attr.image_format == UBWCP_LINEAR) {
		ERR("lock() called on linear buffer");
		goto err;
	}

	if (!buf->locked) {
		DBG("first lock on buffer");
		ubwcp = buf->ubwcp;

		/* buf->desc could already be allocated because of perm range xlation */
		if (!buf->desc) {
			/* allocate a buffer descriptor */
			buf->desc = ubwcp_buf_desc_allocate(buf->ubwcp);
			if (!buf->desc) {
				ERR("ubwcp_allocate_buf_desc() failed");
				goto err;
			}

			memcpy(buf->desc->ptr, &buf->mmdata, sizeof(buf->mmdata));

			/* Flushing of updated mmdata:
			 * mmdata is iocoherent and ubwcp will get it from CPU cache -
			 * *as long as* it has not cached that itself during previous
			 * access to the same descriptor.
			 *
			 * During unlock of previous use of this descriptor,
			 * we do hw flush, which will get rid of this mmdata from
			 * ubwcp cache.
			 *
			 * In addition, we also do a hw flush after enable_range_ck().
			 * That will also get rid of any speculative fetch of mmdata
			 * by the ubwcp hw. At this time, the assumption is that ubwcp
			 * will cache mmdata only for active descriptor. But if ubwcp
			 * is speculatively fetching mmdata for all descriptors
			 * (irrespetive of enabled or not), the flush during lock
			 * will be necessary to make sure ubwcp sees updated mmdata
			 * that we just updated
			 */

			/* program ULA range for this buffer */
			DBG("setting range check: descriptor_id: %d, addr: %p, size: %zx",
							buf->desc->idx, buf->ula_pa, buf->ula_size);
			ubwcp_hw_set_range_check(ubwcp->base, buf->desc->idx, buf->ula_pa,
										buf->ula_size);
		}


		/* enable range check */
		DBG("enabling range check, descriptor_id: %d", buf->desc->idx);
		mutex_lock(&ubwcp->hw_range_ck_lock);
		ubwcp_hw_enable_range_check(ubwcp->base, buf->desc->idx);
		mutex_unlock(&ubwcp->hw_range_ck_lock);

		/* Flush/invalidate UBWCP caches */
		/* Why: cpu could have done a speculative fetch before
		 * enable_range_ck() and ubwcp in process of returning "default" data
		 * we don't want that stashing of default data pending.
		 * we force completion of that and then we also cpu invalidate which
		 * will get rid of that line.
		 */
		trace_ubwcp_hw_flush_start(buf->ula_size);
		ubwcp_flush(ubwcp);
		trace_ubwcp_hw_flush_end(buf->ula_size);

		/* Flush/invalidate ULA PA from CPU caches
		 * TBD: if (dir == READ or BIDIRECTION) //NOT for write
		 * -- Confirm with Chris if this can be skipped for write
		 */
		trace_ubwcp_dma_sync_single_for_cpu_start(buf->ula_size);
		dma_sync_single_for_cpu(ubwcp->dev, buf->ula_pa, buf->ula_size, dir);
		trace_ubwcp_dma_sync_single_for_cpu_end(buf->ula_size);
		buf->lock_dir = dir;
		buf->locked = true;
	} else {
		DBG("buf already locked");
		/* TBD: what if new buffer direction is not same as previous?
		 * must update the dir.
		 */
	}
	buf->lock_count++;
	DBG("new lock_count: %d", buf->lock_count);
	mutex_unlock(&buf->lock);
	trace_ubwcp_lock_end(dmabuf);
	return ret;

err:
	mutex_unlock(&buf->lock);
	if (!ret)
		ret = -1;
	trace_ubwcp_lock_end(dmabuf);
	return ret;
}

/* This can be called as a result of external unlock() call or
 * internally if free() is called without unlock().
 */
static int unlock_internal(struct ubwcp_buf *buf, enum dma_data_direction dir, bool free_buffer)
{
	int ret = 0;
	struct ubwcp_driver *ubwcp;

	DBG("current lock_count: %d", buf->lock_count);
	if (free_buffer) {
		buf->lock_count = 0;
		DBG("Forced lock_count: %d", buf->lock_count);
	} else {
		buf->lock_count--;
		DBG("new lock_count: %d", buf->lock_count);
		if (buf->lock_count) {
			DBG("more than 1 lock on buffer. waiting until last unlock");
			return 0;
		}
	}

	ubwcp = buf->ubwcp;

	/* Flush/invalidate ULA PA from CPU caches */
	//TBD: if (dir == WRITE or BIDIRECTION)
	trace_ubwcp_dma_sync_single_for_device_start(buf->ula_size);
	dma_sync_single_for_device(ubwcp->dev, buf->ula_pa, buf->ula_size, dir);
	trace_ubwcp_dma_sync_single_for_device_end(buf->ula_size);

	/* disable range check with ubwcp flush */
	DBG("disabling range check");
	//TBD: could combine these 2 locks into a single lock to make it simpler
	mutex_lock(&ubwcp->ubwcp_flush_lock);
	mutex_lock(&ubwcp->hw_range_ck_lock);
	trace_ubwcp_hw_flush_start(buf->ula_size);
	ret = ubwcp_hw_disable_range_check_with_flush(ubwcp->base, buf->desc->idx);
	trace_ubwcp_hw_flush_end(buf->ula_size);
	if (ret)
		ERR("disable_range_check_with_flush() failed: %d", ret);
	mutex_unlock(&ubwcp->hw_range_ck_lock);
	mutex_unlock(&ubwcp->ubwcp_flush_lock);

	/* release descriptor if perm range xlation is not set */
	if (!buf->perm) {
		ubwcp_buf_desc_free(buf->ubwcp, buf->desc);
		buf->desc = NULL;
	}
	buf->locked = false;
	return ret;
}


/**
 * Unlock buffer from CPU access. This prepares ubwcp hw to
 * safely allow for device access to the compressed buffer including any
 * necessary cache maintenance ops. It may also free up certain ubwcp
 * resources that could result in error when accessed by CPU in
 * unlocked state.
 *
 * @param dmabuf : ptr to the dma buf
 * @param direction : direction of access
 *
 * @return int : 0 on success, otherwise error code
 */
static int ubwcp_unlock(struct dma_buf *dmabuf, enum dma_data_direction dir)
{
	struct ubwcp_buf *buf;
	int ret;

	FENTRY();
	trace_ubwcp_unlock_start(dmabuf);
	if (!dmabuf) {
		ERR("NULL dmabuf input ptr");
		trace_ubwcp_unlock_end(dmabuf);
		return -EINVAL;
	}

	if (!valid_dma_direction(dir)) {
		ERR("invalid direction: %d", dir);
		trace_ubwcp_unlock_end(dmabuf);
		return -EINVAL;
	}

	buf = dma_buf_to_ubwcp_buf(dmabuf);
	if (!buf) {
		ERR("ubwcp_buf not found");
		trace_ubwcp_unlock_end(dmabuf);
		return -1;
	}

	if (!buf->locked) {
		ERR("unlock() called on buffer which not in locked state");
		trace_ubwcp_unlock_end(dmabuf);
		return -1;
	}

	mutex_lock(&buf->lock);
	ret = unlock_internal(buf, dir, false);
	mutex_unlock(&buf->lock);
	trace_ubwcp_unlock_end(dmabuf);
	return ret;
}


/* Return buffer attributes for the given buffer */
int ubwcp_get_buf_attrs(struct dma_buf *dmabuf, struct ubwcp_buffer_attrs *attr)
{
	int ret = 0;
	struct ubwcp_buf *buf;

	FENTRY();

	if (!dmabuf) {
		ERR("NULL dmabuf input ptr");
		return -EINVAL;
	}

	if (!attr) {
		ERR("NULL attr ptr");
		return -EINVAL;
	}

	buf = dma_buf_to_ubwcp_buf(dmabuf);
	if (!buf) {
		ERR("ubwcp_buf ptr not found");
		return -1;
	}

	mutex_lock(&buf->lock);
	if (!buf->buf_attr_set) {
		ERR("buffer attributes not set");
		mutex_unlock(&buf->lock);
		return -1;
	}

	*attr = buf->buf_attr;

	mutex_unlock(&buf->lock);
	return ret;
}
EXPORT_SYMBOL(ubwcp_get_buf_attrs);


/* Set permanent range translation.
 * enable: Descriptor will be reserved for this buffer until disabled,
 *         making lock/unlock quicker.
 * disable: Descriptor will not be reserved for this buffer. Instead,
 *          descriptor will be allocated and released for each lock/unlock.
 *          If currently allocated but not being used, descriptor will be
 *          released.
 */
int ubwcp_set_perm_range_translation(struct dma_buf *dmabuf, bool enable)
{
	int ret = 0;
	struct ubwcp_buf *buf;

	FENTRY();

	if (!dmabuf) {
		ERR("NULL dmabuf input ptr");
		return -EINVAL;
	}

	buf = dma_buf_to_ubwcp_buf(dmabuf);
	if (!buf) {
		ERR("ubwcp_buf not found");
		return -1;
	}

	/* not implemented */
	if (1) {
		ERR("API not implemented yet");
		return -1;
	}

	/* TBD: make sure we acquire buf lock while setting this so there is
	 * no race condition with attr_set/lock/unlock
	 */
	buf->perm = enable;

	/* if "disable" and we have allocated a desc and it is not being
	 * used currently, release it
	 */
	if (!enable && buf->desc && !buf->locked) {
		ubwcp_buf_desc_free(buf->ubwcp, buf->desc);
		buf->desc = NULL;

		/* Flush/invalidate UBWCP caches */
		//TBD: need to do anything?
	}

	return ret;
}
EXPORT_SYMBOL(ubwcp_set_perm_range_translation);

/**
 * Free up ubwcp resources for this buffer.
 *
 * @param dmabuf : ptr to the dma buf
 *
 * @return int : 0 on success, otherwise error code
 */
static int ubwcp_free_buffer(struct dma_buf *dmabuf)
{
	int ret = 0;
	struct ubwcp_buf *buf;
	struct ubwcp_driver *ubwcp;
	unsigned long flags;
	bool is_non_lin_buf;

	FENTRY();
	trace_ubwcp_free_buffer_start(dmabuf);

	if (!dmabuf) {
		ERR("NULL dmabuf input ptr");
		trace_ubwcp_free_buffer_end(dmabuf);
		return -EINVAL;
	}

	buf = dma_buf_to_ubwcp_buf(dmabuf);
	if (!buf) {
		ERR("ubwcp_buf ptr not found");
		trace_ubwcp_free_buffer_end(dmabuf);
		return -1;
	}

	mutex_lock(&buf->lock);
	ubwcp = buf->ubwcp;
	is_non_lin_buf = (buf->buf_attr.image_format != UBWCP_LINEAR);

	if (buf->locked) {
		DBG("free() called without unlock. unlock()'ing first...");
		ret = unlock_internal(buf, buf->lock_dir, true);
		if (ret)
			ERR("unlock_internal(): failed : %d, but continuing free()", ret);
	}

	/* if we are still holding a desc, release it. this can happen only if perm == true */
	if (buf->desc) {
		WARN_ON(!buf->perm); /* TBD: change to BUG() later...*/
		ubwcp_buf_desc_free(buf->ubwcp, buf->desc);
		buf->desc = NULL;
	}

	if (buf->buf_attr_set)
		reset_buf_attrs(buf);

	spin_lock_irqsave(&ubwcp->buf_table_lock, flags);
	hash_del(&buf->hnode);
	spin_unlock_irqrestore(&ubwcp->buf_table_lock, flags);

	kfree(buf);

	if (is_non_lin_buf)
		dec_num_non_lin_buffers(ubwcp);

	trace_ubwcp_free_buffer_end(dmabuf);
	return 0;
}


/* file open: TBD: increment ref count? */
static int ubwcp_open(struct inode *i, struct file *f)
{
	return 0;
}


/* file open: TBD: decrement ref count? */
static int ubwcp_close(struct inode *i, struct file *f)
{
	return 0;
}

/* handle IOCTLs */
static long ubwcp_ioctl(struct file *file, unsigned int ioctl_num, unsigned long ioctl_param)
{
	struct ubwcp_ioctl_buffer_attrs buf_attr_ioctl;
	struct ubwcp_ioctl_hw_version hw_ver;
	struct ubwcp_ioctl_validate_stride validate_stride_ioctl;
	struct ubwcp_ioctl_stride_align stride_align_ioctl;
	enum ubwcp_std_image_format format;
	struct ubwcp_driver *ubwcp;

	switch (ioctl_num) {
	case UBWCP_IOCTL_SET_BUF_ATTR:
		if (copy_from_user(&buf_attr_ioctl, (const void __user *) ioctl_param,
				   sizeof(buf_attr_ioctl))) {
			ERR("ERROR: copy_from_user() failed");
			return -EFAULT;
		}
		DBG("IOCTL : SET_BUF_ATTR: fd = %d", buf_attr_ioctl.fd);
		return ubwcp_set_buf_attrs_ioctl(&buf_attr_ioctl);

	case UBWCP_IOCTL_GET_HW_VER:
		DBG("IOCTL : GET_HW_VER");
		ubwcp_get_hw_version(&hw_ver);
		if (copy_to_user((void __user *)ioctl_param, &hw_ver, sizeof(hw_ver))) {
			ERR("ERROR: copy_to_user() failed");
			return -EFAULT;
		}
		break;

	case UBWCP_IOCTL_GET_STRIDE_ALIGN:
		DBG("IOCTL : GET_STRIDE_ALIGN");
		if (copy_from_user(&stride_align_ioctl, (const void __user *) ioctl_param,
				   sizeof(stride_align_ioctl))) {
			ERR("ERROR: copy_from_user() failed");
			return -EFAULT;
		}

		format = to_std_format(stride_align_ioctl.image_format);
		if (format == STD_IMAGE_FORMAT_INVALID)
			return -EINVAL;

		if (stride_align_ioctl.unused != 0)
			return -EINVAL;

		if (get_stride_alignment(format, &stride_align_ioctl.stride_align)) {
			ERR("ERROR: copy_to_user() failed");
			return -EFAULT;
		}

		if (copy_to_user((void __user *)ioctl_param, &stride_align_ioctl,
				sizeof(stride_align_ioctl))) {
			ERR("ERROR: copy_to_user() failed");
			return -EFAULT;
		}
		break;

	case UBWCP_IOCTL_VALIDATE_STRIDE:
		DBG("IOCTL : VALIDATE_STRIDE");
		ubwcp = ubwcp_get_driver();
		if (!ubwcp)
			return -EINVAL;

		if (copy_from_user(&validate_stride_ioctl, (const void __user *) ioctl_param,
				   sizeof(validate_stride_ioctl))) {
			ERR("ERROR: copy_from_user() failed");
			return -EFAULT;
		}

		format = to_std_format(validate_stride_ioctl.image_format);
		if (format == STD_IMAGE_FORMAT_INVALID) {
			ERR("ERROR: invalid format: %d", validate_stride_ioctl.image_format);
			return -EINVAL;
		}

		if (validate_stride_ioctl.unused1 || validate_stride_ioctl.unused2) {
			ERR("ERROR: unused values must be set to 0");
			return -EINVAL;
		}

		validate_stride_ioctl.valid = stride_is_valid(ubwcp,
						validate_stride_ioctl.image_format,
						validate_stride_ioctl.width,
						validate_stride_ioctl.stride);

		if (copy_to_user((void __user *)ioctl_param, &validate_stride_ioctl,
			sizeof(validate_stride_ioctl))) {
			ERR("ERROR: copy_to_user() failed");
			return -EFAULT;
		}
		break;

	default:
		ERR("Invalid ioctl_num = %d", ioctl_num);
		return -EINVAL;
	}
	return 0;
}


static const struct file_operations ubwcp_fops = {
	.owner = THIS_MODULE,
	.open           = ubwcp_open,
	.release        = ubwcp_close,
	.unlocked_ioctl = ubwcp_ioctl,
};

static int read_err_r_op(void *data, u64 *value)
{
	struct ubwcp_driver *ubwcp = data;
	*value = ubwcp->read_err_irq_en;
	return 0;
}

static int read_err_w_op(void *data, u64 value)
{
	struct ubwcp_driver *ubwcp = data;

	if (ubwcp_power(ubwcp, true))
		return -1;

	ubwcp_hw_interrupt_enable(ubwcp->base, INTERRUPT_READ_ERROR, value);
	ubwcp->read_err_irq_en = value;

	if (ubwcp_power(ubwcp, false))
		return -1;

	return 0;
}

static int write_err_r_op(void *data, u64 *value)
{
	struct ubwcp_driver *ubwcp = data;
	*value = ubwcp->write_err_irq_en;
	return 0;
}

static int write_err_w_op(void *data, u64 value)
{
	struct ubwcp_driver *ubwcp = data;

	if (ubwcp_power(ubwcp, true))
		return -1;

	ubwcp_hw_interrupt_enable(ubwcp->base, INTERRUPT_WRITE_ERROR, value);
	ubwcp->write_err_irq_en = value;

	if (ubwcp_power(ubwcp, false))
		return -1;

	return 0;
}

static int decode_err_r_op(void *data, u64 *value)
{
	struct ubwcp_driver *ubwcp = data;
	*value = ubwcp->decode_err_irq_en;
	return 0;
}

static int decode_err_w_op(void *data, u64 value)
{
	struct ubwcp_driver *ubwcp = data;

	if (ubwcp_power(ubwcp, true))
		return -1;

	ubwcp_hw_interrupt_enable(ubwcp->base, INTERRUPT_DECODE_ERROR, value);
	ubwcp->decode_err_irq_en = value;

	if (ubwcp_power(ubwcp, false))
		return -1;

	return 0;
}

static int encode_err_r_op(void *data, u64 *value)
{
	struct ubwcp_driver *ubwcp = data;
	*value = ubwcp->encode_err_irq_en;
	return 0;
}

static int encode_err_w_op(void *data, u64 value)
{
	struct ubwcp_driver *ubwcp = data;

	if (ubwcp_power(ubwcp, true))
		return -1;

	ubwcp_hw_interrupt_enable(ubwcp->base, INTERRUPT_ENCODE_ERROR, value);
	ubwcp->encode_err_irq_en = value;

	if (ubwcp_power(ubwcp, false))
		return -1;

	return 0;
}

static int reg_rw_trace_w_op(void *data, u64 value)
{
	ubwcp_hw_trace_set(value);
	return 0;
}

static int reg_rw_trace_r_op(void *data, u64 *value)
{
	bool trace_status;
	ubwcp_hw_trace_get(&trace_status);
	*value = trace_status;
	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(read_err_fops, read_err_r_op, read_err_w_op, "%d\n");
DEFINE_DEBUGFS_ATTRIBUTE(decode_err_fops, decode_err_r_op, decode_err_w_op, "%d\n");
DEFINE_DEBUGFS_ATTRIBUTE(write_err_fops, write_err_r_op, write_err_w_op, "%d\n");
DEFINE_DEBUGFS_ATTRIBUTE(encode_err_fops, encode_err_r_op, encode_err_w_op, "%d\n");
DEFINE_DEBUGFS_ATTRIBUTE(reg_rw_trace_fops, reg_rw_trace_r_op, reg_rw_trace_w_op, "%d\n");

static void ubwcp_debugfs_init(struct ubwcp_driver *ubwcp)
{
	struct dentry *debugfs_root;
	struct dentry *dfile;

	debugfs_root = debugfs_create_dir("ubwcp", NULL);
	if (IS_ERR_OR_NULL(debugfs_root)) {
		ERR("Failed to create debugfs for ubwcp\n");
		return;
	}

	debugfs_create_u32("debug_trace_enable", 0644, debugfs_root, &ubwcp_debug_trace_enable);

	dfile = debugfs_create_file("reg_rw_trace_en", 0644, debugfs_root, ubwcp, &reg_rw_trace_fops);
	if (IS_ERR_OR_NULL(dfile)) {
		ERR("failed to create reg_rw_trace_en debugfs file");
		goto err;
	}

	dfile = debugfs_create_file("read_err_irq_en", 0644, debugfs_root, ubwcp, &read_err_fops);
	if (IS_ERR_OR_NULL(dfile)) {
		ERR("failed to create read_err_irq debugfs file");
		goto err;
	}

	dfile = debugfs_create_file("write_err_irq_en", 0644, debugfs_root, ubwcp, &write_err_fops);
	if (IS_ERR_OR_NULL(dfile)) {
		ERR("failed to create write_err_irq debugfs file");
		goto err;
	}

	dfile = debugfs_create_file("decode_err_irq_en", 0644, debugfs_root, ubwcp,
					&decode_err_fops);
	if (IS_ERR_OR_NULL(dfile)) {
		ERR("failed to create decode_err_irq debugfs file");
		goto err;
	}

	dfile = debugfs_create_file("encode_err_irq_en", 0644, debugfs_root, ubwcp,
					&encode_err_fops);
	if (IS_ERR_OR_NULL(dfile)) {
		ERR("failed to create encode_err_irq debugfs file");
		goto err;
	}

	ubwcp->debugfs_root = debugfs_root;
	return;

err:
	debugfs_remove_recursive(ubwcp->debugfs_root);
	ubwcp->debugfs_root = NULL;
}

static void ubwcp_debugfs_deinit(struct ubwcp_driver *ubwcp)
{
	debugfs_remove_recursive(ubwcp->debugfs_root);
}

/* ubwcp char device initialization */
static int ubwcp_cdev_init(struct ubwcp_driver *ubwcp)
{
	int ret;
	dev_t devt;
	struct class *dev_class;
	struct device *dev_sys;

	/* allocate major device number (/proc/devices -> major_num ubwcp) */
	ret = alloc_chrdev_region(&devt, 0, UBWCP_NUM_DEVICES, UBWCP_DEVICE_NAME);
	if (ret) {
		ERR("alloc_chrdev_region() failed: %d", ret);
		return ret;
	}

	/* create device class  (/sys/class/ubwcp_class) */
	dev_class = class_create(THIS_MODULE, "ubwcp_class");
	if (IS_ERR(dev_class)) {
		ERR("class_create() failed");
		return -1;
	}

	/* Create device and register with sysfs
	 * (/sys/class/ubwcp_class/ubwcp/... -> dev/power/subsystem/uevent)
	 */
	dev_sys = device_create(dev_class, NULL, devt, NULL,
			UBWCP_DEVICE_NAME);
	if (IS_ERR(dev_sys)) {
		ERR("device_create() failed");
		return -1;
	}

	/* register file operations and get cdev */
	cdev_init(&ubwcp->cdev, &ubwcp_fops);

	/* associate cdev and device major/minor with file system
	 * can do file ops on /dev/ubwcp after this
	 */
	ret = cdev_add(&ubwcp->cdev, devt, 1);
	if (ret) {
		ERR("cdev_add() failed");
		return -1;
	}

	ubwcp->devt = devt;
	ubwcp->dev_class = dev_class;
	ubwcp->dev_sys = dev_sys;
	return 0;
}

static void ubwcp_cdev_deinit(struct ubwcp_driver *ubwcp)
{
	device_destroy(ubwcp->dev_class, ubwcp->devt);
	class_destroy(ubwcp->dev_class);
	cdev_del(&ubwcp->cdev);
	unregister_chrdev_region(ubwcp->devt, UBWCP_NUM_DEVICES);
}

struct handler_node {
	struct list_head list;
	u32 client_id;
	ubwcp_error_handler_t handler;
	void *data;
};

int ubwcp_register_error_handler(u32 client_id, ubwcp_error_handler_t handler,
				void *data)
{
	struct handler_node *node;
	unsigned long flags;
	struct ubwcp_driver *ubwcp = ubwcp_get_driver();

	if (!ubwcp)
		return -EINVAL;

	if (client_id != -1)
		return -EINVAL;

	if (!handler)
		return -EINVAL;

	node = kzalloc(sizeof(*node), GFP_KERNEL);
	if (!node)
		return -ENOMEM;

	node->client_id = client_id;
	node->handler = handler;
	node->data = data;

	spin_lock_irqsave(&ubwcp->err_handler_list_lock, flags);
	list_add_tail(&node->list, &ubwcp->err_handler_list);
	spin_unlock_irqrestore(&ubwcp->err_handler_list_lock, flags);

	return 0;
}
EXPORT_SYMBOL(ubwcp_register_error_handler);

static void ubwcp_notify_error_handlers(struct ubwcp_err_info *err)
{
	struct handler_node *node;
	unsigned long flags;
	struct ubwcp_driver *ubwcp = ubwcp_get_driver();

	if (!ubwcp)
		return;

	spin_lock_irqsave(&ubwcp->err_handler_list_lock, flags);
	list_for_each_entry(node, &ubwcp->err_handler_list, list)
		node->handler(err, node->data);

	spin_unlock_irqrestore(&ubwcp->err_handler_list_lock, flags);
}

int ubwcp_unregister_error_handler(u32 client_id)
{
	int ret = -EINVAL;
	struct handler_node *node;
	unsigned long flags;
	struct ubwcp_driver *ubwcp = ubwcp_get_driver();

	if (!ubwcp)
		return -EINVAL;

	spin_lock_irqsave(&ubwcp->err_handler_list_lock, flags);
	list_for_each_entry(node, &ubwcp->err_handler_list, list)
		if (node->client_id == client_id) {
			list_del(&node->list);
			kfree(node);
			ret = 0;
			break;
		}
	spin_unlock_irqrestore(&ubwcp->err_handler_list_lock, flags);

	return ret;
}
EXPORT_SYMBOL(ubwcp_unregister_error_handler);

/* get ubwcp_buf corresponding to the ULA PA*/
static struct dma_buf *get_dma_buf_from_ulapa(phys_addr_t addr)
{
	struct ubwcp_buf *buf = NULL;
	struct dma_buf *ret_buf = NULL;
	struct ubwcp_driver *ubwcp = ubwcp_get_driver();
	unsigned long flags;
	u32 i;

	if (!ubwcp)
		return NULL;

	spin_lock_irqsave(&ubwcp->buf_table_lock, flags);
	hash_for_each(ubwcp->buf_table, i, buf, hnode) {
		if (buf->ula_pa <= addr && addr < buf->ula_pa + buf->ula_size) {
			ret_buf = buf->dma_buf;
			break;
		}
	}
	spin_unlock_irqrestore(&ubwcp->buf_table_lock, flags);

	return ret_buf;
}

/* get ubwcp_buf corresponding to the IOVA*/
static struct dma_buf *get_dma_buf_from_iova(unsigned long addr)
{
	struct ubwcp_buf *buf = NULL;
	struct dma_buf *ret_buf = NULL;
	struct ubwcp_driver *ubwcp = ubwcp_get_driver();
	unsigned long flags;
	u32 i;

	if (!ubwcp)
		return NULL;

	spin_lock_irqsave(&ubwcp->buf_table_lock, flags);
	hash_for_each(ubwcp->buf_table, i, buf, hnode) {
		unsigned long iova_base;
		unsigned int iova_size;

		if (!buf->sgt)
			continue;

		iova_base = sg_dma_address(buf->sgt->sgl);
		iova_size = sg_dma_len(buf->sgt->sgl);

		if (iova_base <= addr && addr < iova_base + iova_size) {
			ret_buf = buf->dma_buf;
			break;
		}
	}
	spin_unlock_irqrestore(&ubwcp->buf_table_lock, flags);

	return ret_buf;
}

int ubwcp_iommu_fault_handler(struct iommu_domain *domain, struct device *dev,
		unsigned long iova, int flags, void *data)
{
	int ret = 0;
	struct ubwcp_err_info err;
	struct ubwcp_driver *ubwcp = ubwcp_get_driver();
	struct device *cb_dev = (struct device *)data;

	if (!ubwcp) {
		ret = -EINVAL;
		goto err;
	}

	err.err_code = UBWCP_SMMU_FAULT;

	if (cb_dev == ubwcp->dev_desc_cb)
		err.smmu_err.iommu_dev_id = UBWCP_DESC_CB_ID;
	else if (cb_dev == ubwcp->dev_buf_cb)
		err.smmu_err.iommu_dev_id = UBWCP_BUF_CB_ID;
	else
		err.smmu_err.iommu_dev_id = UBWCP_UNKNOWN_CB_ID;
	err.smmu_err.dmabuf = get_dma_buf_from_iova(iova);
	err.smmu_err.iova = iova;
	err.smmu_err.iommu_fault_flags = flags;
	ERR_RATE_LIMIT("ubwcp_err: err code: %d (smmu), iommu_dev_id: %d, iova: 0x%llx, flags: 0x%x",
		err.err_code, err.smmu_err.iommu_dev_id, err.smmu_err.iova,
		err.smmu_err.iommu_fault_flags);
	ubwcp_notify_error_handlers(&err);

err:
	return ret;
}

irqreturn_t ubwcp_irq_handler(int irq, void *ptr)
{
	struct ubwcp_driver *ubwcp;
	void __iomem *base;
	phys_addr_t addr;
	struct ubwcp_err_info err;

	ubwcp = (struct ubwcp_driver *) ptr;
	base = ubwcp->base;

	if (irq == ubwcp->irq_range_ck_rd) {
		addr = ubwcp_hw_interrupt_src_address(base, 0) << 6;
		err.err_code = UBWCP_RANGE_TRANSLATION_ERROR;
		err.translation_err.dmabuf = get_dma_buf_from_ulapa(addr);
		err.translation_err.ula_pa = addr;
		err.translation_err.read = true;
		ERR_RATE_LIMIT("ubwcp_err: err code: %d (range), dmabuf: 0x%llx, read: %d, addr: 0x%llx",
			err.err_code, err.translation_err.dmabuf, err.translation_err.read, addr);
		ubwcp_notify_error_handlers(&err);
		ubwcp_hw_interrupt_clear(ubwcp->base, 0);

	} else if (irq == ubwcp->irq_range_ck_wr) {
		addr = ubwcp_hw_interrupt_src_address(base, 1) << 6;
		err.err_code = UBWCP_RANGE_TRANSLATION_ERROR;
		err.translation_err.dmabuf = get_dma_buf_from_ulapa(addr);
		err.translation_err.ula_pa = addr;
		err.translation_err.read = false;
		ERR_RATE_LIMIT("ubwcp_err: err code: %d (range), dmabuf: 0x%llx, read: %d, addr: 0x%llx",
			err.err_code, err.translation_err.dmabuf, err.translation_err.read, addr);
		ubwcp_notify_error_handlers(&err);
		ubwcp_hw_interrupt_clear(ubwcp->base, 1);
	} else if (irq == ubwcp->irq_encode) {
		addr = ubwcp_hw_interrupt_src_address(base, 3) << 6;
		err.err_code = UBWCP_ENCODE_ERROR;
		err.enc_err.dmabuf = get_dma_buf_from_ulapa(addr);
		err.enc_err.ula_pa = addr;
		ERR_RATE_LIMIT("ubwcp_err: err code: %d (encode), dmabuf: 0x%llx, addr: 0x%llx",
				err.err_code, err.enc_err.dmabuf, addr);
		ubwcp_notify_error_handlers(&err);
		ubwcp_hw_interrupt_clear(ubwcp->base, 3);
	} else if (irq == ubwcp->irq_decode) {
		addr = ubwcp_hw_interrupt_src_address(base, 2) << 6;
		err.err_code = UBWCP_DECODE_ERROR;
		err.dec_err.dmabuf = get_dma_buf_from_ulapa(addr);
		err.dec_err.ula_pa = addr;
		ERR_RATE_LIMIT("ubwcp_err: err code: %d (decode), dmabuf: 0x%llx, addr: 0x%llx",
				err.err_code, err.enc_err.dmabuf, addr);
		ubwcp_notify_error_handlers(&err);
		ubwcp_hw_interrupt_clear(ubwcp->base, 2);
	} else {
		ERR("unknown irq: %d", irq);
		return IRQ_NONE;
	}

	return IRQ_HANDLED;
}

static int ubwcp_interrupt_register(struct platform_device *pdev, struct ubwcp_driver *ubwcp)
{
	int ret = 0;
	struct device *dev = &pdev->dev;

	FENTRY();

	ubwcp->irq_range_ck_rd = platform_get_irq(pdev, 0);
	if (ubwcp->irq_range_ck_rd < 0)
		return ubwcp->irq_range_ck_rd;

	ubwcp->irq_range_ck_wr = platform_get_irq(pdev, 1);
	if (ubwcp->irq_range_ck_wr < 0)
		return ubwcp->irq_range_ck_wr;

	ubwcp->irq_encode = platform_get_irq(pdev, 2);
	if (ubwcp->irq_encode < 0)
		return ubwcp->irq_encode;

	ubwcp->irq_decode = platform_get_irq(pdev, 3);
	if (ubwcp->irq_decode < 0)
		return ubwcp->irq_decode;

	DBG("got irqs: %d %d %d %d", ubwcp->irq_range_ck_rd,
					ubwcp->irq_range_ck_wr,
					ubwcp->irq_encode,
					ubwcp->irq_decode);

	ret = devm_request_irq(dev, ubwcp->irq_range_ck_rd, ubwcp_irq_handler, 0, "ubwcp", ubwcp);
	if (ret) {
		ERR("request_irq() failed. irq: %d ret: %d",
						ubwcp->irq_range_ck_rd, ret);
		return ret;
	}

	ret = devm_request_irq(dev, ubwcp->irq_range_ck_wr, ubwcp_irq_handler, 0, "ubwcp", ubwcp);
	if (ret) {
		ERR("request_irq() failed. irq: %d ret: %d",
						ubwcp->irq_range_ck_wr, ret);
		return ret;
	}

	ret = devm_request_irq(dev, ubwcp->irq_encode, ubwcp_irq_handler, 0, "ubwcp", ubwcp);
	if (ret) {
		ERR("request_irq() failed. irq: %d ret: %d",
							ubwcp->irq_encode, ret);
		return ret;
	}

	ret = devm_request_irq(dev, ubwcp->irq_decode, ubwcp_irq_handler, 0, "ubwcp", ubwcp);
	if (ret) {
		ERR("request_irq() failed. irq: %d ret: %d",
							ubwcp->irq_decode, ret);
		return ret;
	}

	return ret;
}

/* ubwcp device probe */
static int qcom_ubwcp_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct ubwcp_driver *ubwcp;
	struct device *ubwcp_dev = &pdev->dev;

	FENTRY();

	ubwcp = devm_kzalloc(ubwcp_dev, sizeof(*ubwcp), GFP_KERNEL);
	if (!ubwcp) {
		ERR("devm_kzalloc() failed");
		return -ENOMEM;
	}

	ubwcp->dev = &pdev->dev;

	ret = dma_set_mask_and_coherent(ubwcp->dev, DMA_BIT_MASK(64));

#ifdef UBWCP_USE_SMC
	{
		struct resource res;

		of_address_to_resource(ubwcp_dev->of_node, 0, &res);
		ubwcp->base = (void __iomem *) res.start;
		DBG("Using SMC calls. base: %p", ubwcp->base);
	}
#else
	ubwcp->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(ubwcp->base)) {
		ERR("devm ioremap() failed: %d", PTR_ERR(ubwcp->base));
		return PTR_ERR(ubwcp->base);
	}
	DBG("ubwcp->base: %p", ubwcp->base);
#endif

	ret = of_property_read_u64_index(ubwcp_dev->of_node, "ula_range", 0, &ubwcp->ula_pool_base);
	if (ret) {
		ERR("failed reading ula_range (base): %d", ret);
		return ret;
	}
	DBG("ubwcp: ula_range: base = 0x%lx", ubwcp->ula_pool_base);

	ret = of_property_read_u64_index(ubwcp_dev->of_node, "ula_range", 1, &ubwcp->ula_pool_size);
	if (ret) {
		ERR("failed reading ula_range (size): %d", ret);
		return ret;
	}
	DBG("ubwcp: ula_range: size = 0x%lx", ubwcp->ula_pool_size);

	INIT_LIST_HEAD(&ubwcp->err_handler_list);

	atomic_set(&ubwcp->num_non_lin_buffers, 0);
	ubwcp->mem_online = false;

	mutex_init(&ubwcp->desc_lock);
	spin_lock_init(&ubwcp->buf_table_lock);
	mutex_init(&ubwcp->mem_hotplug_lock);
	mutex_init(&ubwcp->ula_lock);
	mutex_init(&ubwcp->ubwcp_flush_lock);
	mutex_init(&ubwcp->hw_range_ck_lock);
	spin_lock_init(&ubwcp->err_handler_list_lock);

	/* Regulator */
	ubwcp->vdd = devm_regulator_get(ubwcp_dev, "vdd");
	if (IS_ERR_OR_NULL(ubwcp->vdd)) {
		ret = PTR_ERR(ubwcp->vdd);
		ERR("devm_regulator_get() failed: %d", ret);
		return -1;
	}

	ret = ubwcp_init_clocks(ubwcp, ubwcp_dev);
	if (ret) {
		ERR("failed to initialize ubwcp clocks err: %d", ret);
		return ret;
	}

	if (ubwcp_power(ubwcp, true))
		return -1;

	if (ubwcp_cdev_init(ubwcp))
		return -1;

	/* disable all interrupts (reset value has some interrupts enabled by default) */
	ubwcp_hw_interrupt_enable(ubwcp->base, INTERRUPT_READ_ERROR, false);
	ubwcp_hw_interrupt_enable(ubwcp->base, INTERRUPT_WRITE_ERROR, false);
	ubwcp_hw_interrupt_enable(ubwcp->base, INTERRUPT_ENCODE_ERROR, false);
	ubwcp_hw_interrupt_enable(ubwcp->base, INTERRUPT_DECODE_ERROR, false);

	if (ubwcp_interrupt_register(pdev, ubwcp))
		return -1;

	ubwcp_debugfs_init(ubwcp);

	/* create ULA pool */
	ubwcp->ula_pool = gen_pool_create(12, -1);
	if (!ubwcp->ula_pool) {
		ERR("failed gen_pool_create()");
		ret = -1;
		goto err_pool_create;
	}

	ret = gen_pool_add(ubwcp->ula_pool, ubwcp->ula_pool_base, ubwcp->ula_pool_size, -1);
	if (ret) {
		ERR("failed gen_pool_add(): %d", ret);
		ret = -1;
		goto err_pool_add;
	}

	/* register the default config mmap function. */
	ubwcp->mmap_config_fptr = msm_ubwcp_dma_buf_configure_mmap;

	hash_init(ubwcp->buf_table);
	ubwcp_buf_desc_list_init(ubwcp);
	image_format_init(ubwcp);

	/* one time hw init */
	ubwcp_hw_one_time_init(ubwcp->base);
	ubwcp_hw_version(ubwcp->base, &ubwcp->hw_ver_major, &ubwcp->hw_ver_minor);
	pr_err("ubwcp: hw version: major %d, minor %d\n", ubwcp->hw_ver_major, ubwcp->hw_ver_minor);
	if (ubwcp->hw_ver_major == 0) {
		ERR("Failed to read HW version");
		ret = -1;
		goto err_pool_add;
	}

	/* set pdev->dev->driver_data = ubwcp */
	platform_set_drvdata(pdev, ubwcp);


	/* enable interrupts */
	if (ubwcp->read_err_irq_en)
		ubwcp_hw_interrupt_enable(ubwcp->base, INTERRUPT_READ_ERROR,  true);
	if (ubwcp->write_err_irq_en)
		ubwcp_hw_interrupt_enable(ubwcp->base, INTERRUPT_WRITE_ERROR,  true);
	if (ubwcp->decode_err_irq_en)
		ubwcp_hw_interrupt_enable(ubwcp->base, INTERRUPT_DECODE_ERROR, true);
	if (ubwcp->encode_err_irq_en)
		ubwcp_hw_interrupt_enable(ubwcp->base, INTERRUPT_ENCODE_ERROR, true);

	/* Turn OFF until buffers are allocated */
	if (ubwcp_power(ubwcp, false)) {
		ret = -1;
		goto err_power_off;
	}

	ret = msm_ubwcp_set_ops(ubwcp_init_buffer, ubwcp_free_buffer, ubwcp_lock, ubwcp_unlock);
	if (ret) {
		ERR("msm_ubwcp_set_ops() failed: %d, but IGNORED", ret);
		/* TBD: ignore return error during testing phase.
		 * This allows us to rmmod/insmod for faster dev cycle.
		 * In final version: return error and de-register driver if set_ops fails.
		 */
		ret = 0;
		//goto err_power_off;
	} else {
		DBG("msm_ubwcp_set_ops(): success"); }

	me = ubwcp;
	return ret;

err_power_off:
	ubwcp_hw_interrupt_enable(ubwcp->base, INTERRUPT_READ_ERROR,   false);
	ubwcp_hw_interrupt_enable(ubwcp->base, INTERRUPT_WRITE_ERROR,  false);
	ubwcp_hw_interrupt_enable(ubwcp->base, INTERRUPT_ENCODE_ERROR, false);
	ubwcp_hw_interrupt_enable(ubwcp->base, INTERRUPT_DECODE_ERROR, false);
err_pool_add:
	gen_pool_destroy(ubwcp->ula_pool);
err_pool_create:
	ubwcp_cdev_deinit(ubwcp);
	return ret;
}


/* buffer context bank device probe */
static int ubwcp_probe_cb_buf(struct platform_device *pdev)
{
	struct ubwcp_driver *ubwcp;
	struct iommu_domain *domain = NULL;

	FENTRY();

	ubwcp = dev_get_drvdata(pdev->dev.parent);
	if (!ubwcp) {
		ERR("failed to get ubwcp ptr");
		return -EINVAL;
	}

	/* save the buffer cb device */
	ubwcp->dev_buf_cb = &pdev->dev;

	domain = iommu_get_domain_for_dev(ubwcp->dev_buf_cb);
	if (domain)
		iommu_set_fault_handler(domain, ubwcp_iommu_fault_handler, ubwcp->dev_buf_cb);

	return 0;
}

/* descriptor context bank device probe */
static int ubwcp_probe_cb_desc(struct platform_device *pdev)
{
	int ret = 0;
	struct ubwcp_driver *ubwcp;
	struct iommu_domain *domain = NULL;

	FENTRY();

	ubwcp = dev_get_drvdata(pdev->dev.parent);
	if (!ubwcp) {
		ERR("failed to get ubwcp ptr");
		return -EINVAL;
	}

	ubwcp->buffer_desc_size = UBWCP_BUFFER_DESC_OFFSET *
					UBWCP_BUFFER_DESC_COUNT;

	ubwcp->dev_desc_cb = &pdev->dev;

	dma_set_max_seg_size(ubwcp->dev_desc_cb, DMA_BIT_MASK(32));
	dma_set_seg_boundary(ubwcp->dev_desc_cb, (unsigned long)DMA_BIT_MASK(64));

	/* Allocate buffer descriptors. UBWCP is iocoherent device.
	 * Thus we don't need to flush after updates to buffer descriptors.
	 */
	ubwcp->buffer_desc_base = dma_alloc_coherent(ubwcp->dev_desc_cb,
					ubwcp->buffer_desc_size,
					&ubwcp->buffer_desc_dma_handle,
					GFP_KERNEL);
	if (!ubwcp->buffer_desc_base) {
		ERR("failed to allocate desc buffer");
		return -ENOMEM;
	}

	DBG("desc_base = %p size = %zu", ubwcp->buffer_desc_base,
						ubwcp->buffer_desc_size);

	ret = ubwcp_power(ubwcp, true);
	if (ret) {
		ERR("failed to power on");
		goto err;
	}
	ubwcp_hw_set_buf_desc(ubwcp->base, (u64) ubwcp->buffer_desc_dma_handle,
						UBWCP_BUFFER_DESC_OFFSET);

	ret = ubwcp_power(ubwcp, false);
	if (ret) {
		ERR("failed to power off");
		goto err;
	}

	domain = iommu_get_domain_for_dev(ubwcp->dev_desc_cb);
	if (domain)
		iommu_set_fault_handler(domain, ubwcp_iommu_fault_handler, ubwcp->dev_desc_cb);

	return ret;

err:
	dma_free_coherent(ubwcp->dev_desc_cb,
				ubwcp->buffer_desc_size,
				ubwcp->buffer_desc_base,
				ubwcp->buffer_desc_dma_handle);
	ubwcp->buffer_desc_base = NULL;
	ubwcp->buffer_desc_dma_handle = 0;
	ubwcp->dev_desc_cb = NULL;
	return -1;
}

/* buffer context bank device remove */
static int ubwcp_remove_cb_buf(struct platform_device *pdev)
{
	struct ubwcp_driver *ubwcp;

	FENTRY();

	ubwcp = dev_get_drvdata(pdev->dev.parent);
	if (!ubwcp) {
		ERR("failed to get ubwcp ptr");
		return -EINVAL;
	}

	/* remove buf_cb reference */
	ubwcp->dev_buf_cb = NULL;
	return 0;
}

/* descriptor context bank device remove */
static int ubwcp_remove_cb_desc(struct platform_device *pdev)
{
	struct ubwcp_driver *ubwcp;

	FENTRY();

	ubwcp = dev_get_drvdata(pdev->dev.parent);
	if (!ubwcp) {
		ERR("failed to get ubwcp ptr");
		return -EINVAL;
	}

	if (!ubwcp->dev_desc_cb) {
		ERR("ubwcp->dev_desc_cb == NULL");
		return -1;
	}

	ubwcp_power(ubwcp, true);
	ubwcp_hw_set_buf_desc(ubwcp->base, 0x0, 0x0);
	ubwcp_power(ubwcp, false);

	dma_free_coherent(ubwcp->dev_desc_cb,
				ubwcp->buffer_desc_size,
				ubwcp->buffer_desc_base,
				ubwcp->buffer_desc_dma_handle);
	ubwcp->buffer_desc_base = NULL;
	ubwcp->buffer_desc_dma_handle = 0;
	return 0;
}

/* ubwcp device remove */
static int qcom_ubwcp_remove(struct platform_device *pdev)
{
	size_t avail;
	size_t psize;
	struct ubwcp_driver *ubwcp;

	FENTRY();

	/* get pdev->dev->driver_data = ubwcp */
	ubwcp = platform_get_drvdata(pdev);
	if (!ubwcp) {
		ERR("ubwcp == NULL");
		return -1;
	}

	ubwcp_power(ubwcp, true);
	ubwcp_hw_interrupt_enable(ubwcp->base, INTERRUPT_READ_ERROR,   false);
	ubwcp_hw_interrupt_enable(ubwcp->base, INTERRUPT_WRITE_ERROR,  false);
	ubwcp_hw_interrupt_enable(ubwcp->base, INTERRUPT_ENCODE_ERROR, false);
	ubwcp_hw_interrupt_enable(ubwcp->base, INTERRUPT_DECODE_ERROR, false);
	ubwcp_power(ubwcp, false);

	/* before destroying, make sure pool is empty. otherwise pool_destroy() panics.
	 * TBD: remove this check for production code and let it panic
	 */
	avail = gen_pool_avail(ubwcp->ula_pool);
	psize = gen_pool_size(ubwcp->ula_pool);
	if (psize != avail) {
		ERR("gen_pool is not empty! avail: %zx size: %zx", avail, psize);
		ERR("skipping pool destroy....cause it will PANIC. Fix this!!!!");
		WARN(1, "Fix this!");
	} else {
		gen_pool_destroy(ubwcp->ula_pool);
	}
	ubwcp_debugfs_deinit(ubwcp);
	ubwcp_cdev_deinit(ubwcp);

	return 0;
}


/* top level ubwcp device probe function */
static int ubwcp_probe(struct platform_device *pdev)
{
	const char *compatible = "";

	FENTRY();
	trace_ubwcp_probe(pdev);

	if (of_device_is_compatible(pdev->dev.of_node, "qcom,ubwcp"))
		return qcom_ubwcp_probe(pdev);
	else if (of_device_is_compatible(pdev->dev.of_node, "qcom,ubwcp-context-bank-desc"))
		return ubwcp_probe_cb_desc(pdev);
	else if (of_device_is_compatible(pdev->dev.of_node, "qcom,ubwcp-context-bank-buf"))
		return ubwcp_probe_cb_buf(pdev);

	of_property_read_string(pdev->dev.of_node, "compatible", &compatible);
	ERR("unknown device: %s", compatible);

	WARN_ON(1);
	return -EINVAL;
}

/* top level ubwcp device remove function */
static int ubwcp_remove(struct platform_device *pdev)
{
	const char *compatible = "";

	FENTRY();
	trace_ubwcp_remove(pdev);

	/* TBD: what if buffers are still allocated? locked? etc.
	 *  also should turn off power?
	 */

	if (of_device_is_compatible(pdev->dev.of_node, "qcom,ubwcp"))
		return qcom_ubwcp_remove(pdev);
	else if (of_device_is_compatible(pdev->dev.of_node, "qcom,ubwcp-context-bank-desc"))
		return ubwcp_remove_cb_desc(pdev);
	else if (of_device_is_compatible(pdev->dev.of_node, "qcom,ubwcp-context-bank-buf"))
		return ubwcp_remove_cb_buf(pdev);

	of_property_read_string(pdev->dev.of_node, "compatible", &compatible);
	ERR("unknown device: %s", compatible);

	WARN_ON(1);
	return -EINVAL;
}


static const struct of_device_id ubwcp_dt_match[] = {
	{.compatible = "qcom,ubwcp"},
	{.compatible = "qcom,ubwcp-context-bank-desc"},
	{.compatible = "qcom,ubwcp-context-bank-buf"},
	{}
};

struct platform_driver ubwcp_platform_driver = {
	.probe = ubwcp_probe,
	.remove = ubwcp_remove,
	.driver = {
		.name = "qcom,ubwcp",
		.of_match_table = ubwcp_dt_match,
	},
};

int ubwcp_init(void)
{
	int ret = 0;

	DBG("+++++++++++");

	ret = platform_driver_register(&ubwcp_platform_driver);
	if (ret)
		ERR("platform_driver_register() failed: %d", ret);

	return ret;
}

void ubwcp_exit(void)
{
	platform_driver_unregister(&ubwcp_platform_driver);

	DBG("-----------");
}

module_init(ubwcp_init);
module_exit(ubwcp_exit);

MODULE_LICENSE("GPL");
