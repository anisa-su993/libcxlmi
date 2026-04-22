// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * This file is part of libcxlmi.
 *
 * VFIO transport: send CCI commands directly to a CXL device's Primary
 * Mailbox registers via userspace MMIO, bypassing the kernel CXL driver.
 *
 * Discovery of the CXL Device Register Block and its Primary Mailbox
 * capability is done with libpci (pciutils), using the CXL Register
 * Locator DVSEC (CXL r3.1 Section 8.1.9) followed by the Device
 * Capabilities Array walk (Section 8.2.8). The mailbox doorbell
 * protocol follows Section 8.2.8.4.
 *
 * This is intended for FM-owned access to a Multi-Headed Device (MHD),
 * where the primary mailbox of the FM-owned LD is the entry point for
 * issuing generic, memdev, and FM-API commands (the latter including
 * tunneled commands to the LD-pool CCI via DEFINE_CXLMI_TUNNEL_MHD).
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <ccan/endian/endian.h>

#include <libcxlmi.h>

#include "private.h"

#ifdef CONFIG_LIBPCI

#include <linux/vfio.h>
#include <pci/pci.h>

/* PCIe extended capability IDs / CXL DVSEC identifiers */
#define PCI_EXT_CAP_ID_DVSEC		0x0023
#define CXL_DVSEC_VENDOR_ID		0x1E98
#define CXL_DVSEC_REG_LOCATOR_ID	0x0008

/* Register Block Identifiers (CXL r3.1 Section 8.1.9.1) */
#define CXL_REGBLOCK_ID_DEVICE		0x03

/* Device Capability IDs (CXL r3.1 Section 8.2.8.2) */
#define CXL_DEVCAP_ID_ARRAY		0x0000
#define CXL_DEVCAP_ID_PRIMARY_MBOX	0x0002

/* MB Control */
#define CXL_MB_CTRL_DOORBELL		(1u << 0)

/* Command Register (64-bit) field shifts/masks */
#define CXL_MB_CMD_PAYLOAD_SHIFT	16
#define CXL_MB_CMD_PAYLOAD_MASK		0x0000000FFFFF0000ull /* 20 bits [35:16] */

/* MB Status (64-bit) */
#define CXL_MB_STS_BG_MASK		(1ull << 0)
#define CXL_MB_STS_RC_SHIFT		32
#define CXL_MB_STS_RC_MASK		0x0000FFFF00000000ull
#define CXL_MB_STS_VENEXT_SHIFT		48

/* Poll the doorbell at 100us intervals; use endpoint timeout as the cap. */
#define CXL_MB_POLL_INTERVAL_US		100

struct cxlmi_transport_vfio {
	char bdf[16];
	int container_fd;
	int group_fd;
	int device_fd;

	void *bar_map;
	size_t bar_len;

	/* Pointer into bar_map at the Primary Mailbox register set. */
	volatile uint8_t *mbox_regs;
	size_t payload_size;	/* 1 << MB Capabilities[4:0] */
};

/* ------------------------------------------------------------------ */
/* libpci discovery helpers                                           */
/* ------------------------------------------------------------------ */

static int parse_bdf(const char *bdf, int *dom, int *bus, int *dev, int *fn)
{
	if (sscanf(bdf, "%x:%x:%x.%x", dom, bus, dev, fn) == 4)
		return 0;
	*dom = 0;
	if (sscanf(bdf, "%x:%x.%x", bus, dev, fn) == 3)
		return 0;
	return -1;
}

/*
 * Walk the PCIe extended capability list looking for a DVSEC with the given
 * DVSEC Vendor ID and DVSEC ID. Returns the config-space offset of the DVSEC
 * header, or -1 if not found.
 */
static int find_dvsec(struct pci_dev *pdev, uint16_t vendor, uint16_t id)
{
	uint16_t pos = 0x100;
	uint32_t header = pci_read_long(pdev, pos);

	if (header == 0 || header == 0xffffffff)
		return -1;

	while (pos) {
		uint16_t cap_id = header & 0xffff;
		uint16_t next = (header >> 20) & 0xffc;

		if (cap_id == PCI_EXT_CAP_ID_DVSEC) {
			uint32_t hdr1 = pci_read_long(pdev, pos + 4);
			uint32_t hdr2 = pci_read_long(pdev, pos + 8);
			uint16_t dv_vendor = hdr1 & 0xffff;
			uint16_t dv_id = hdr2 & 0xffff;

			if (dv_vendor == vendor && dv_id == id)
				return pos;
		}

		if (!next || next == pos)
			break;
		pos = next;
		header = pci_read_long(pdev, pos);
		if (header == 0 || header == 0xffffffff)
			break;
	}
	return -1;
}

/*
 * Parse the Register Block Entry array in the Register Locator DVSEC.
 * Each entry is 8 bytes: [bir:3][rsv:5][id:8][offset_lo:16] + offset_hi:32.
 * On match, returns the BIR and 64-bit BAR-relative offset.
 */
static int find_regblock(struct pci_dev *pdev, int dvsec_off,
			 uint8_t regblock_id, uint8_t *bir_out,
			 uint64_t *offset_out)
{
	/* DVSEC Header1: length in bits [31:20] */
	uint32_t hdr1 = pci_read_long(pdev, dvsec_off + 4);
	uint16_t dvsec_len = (hdr1 >> 20) & 0xfff;
	int entry_off = dvsec_off + 0x0C; /* skip 10B DVSEC + 2B reserved */

	if (dvsec_len < 0x10)
		return -1;

	while (entry_off + 8 <= dvsec_off + dvsec_len) {
		uint32_t lo = pci_read_long(pdev, entry_off);
		uint32_t hi = pci_read_long(pdev, entry_off + 4);
		uint8_t bir = lo & 0x7;
		uint8_t id = (lo >> 8) & 0xff;
		uint64_t offset =
			((uint64_t)hi << 32) | (uint64_t)(lo & 0xFFFF0000u);

		if (id == regblock_id) {
			*bir_out = bir;
			*offset_out = offset;
			return 0;
		}
		entry_off += 8;
	}
	return -1;
}

/*
 * Walk the Device Capabilities Array at dev_regs and find the capability
 * with the given id. Returns the offset (from dev_regs) and size of the
 * capability's register set. Caller must have mapped dev_regs already.
 */
static int find_dev_capability(volatile uint8_t *dev_regs, uint16_t cap_id,
			       uint32_t *cap_off, uint32_t *cap_size)
{
	uint64_t arr = le64_to_cpu(*(volatile uint64_t *)dev_regs);
	uint8_t count;
	uint8_t i;

	if ((arr & 0xffff) != CXL_DEVCAP_ID_ARRAY)
		return -1;
	count = (arr >> 32) & 0xff;

	for (i = 0; i < count; i++) {
		volatile uint8_t *hdr = dev_regs + 0x10 + (i * 0x10);
		uint64_t hdr0 = le64_to_cpu(*(volatile uint64_t *)hdr);
		uint64_t hdr1 = le64_to_cpu(*(volatile uint64_t *)(hdr + 8));

		if ((hdr0 & 0xffff) == cap_id) {
			*cap_off = (hdr0 >> 32) & 0xffffffffu;
			*cap_size = hdr1 & 0xffffffffu;
			return 0;
		}
	}
	return -1;
}

/* ------------------------------------------------------------------ */
/* VFIO setup / teardown                                              */
/* ------------------------------------------------------------------ */

static int read_iommu_group(const char *bdf)
{
	char path[PATH_MAX];
	char link[PATH_MAX];
	ssize_t n;
	char *slash;
	int group;

	snprintf(path, sizeof(path),
		 "/sys/bus/pci/devices/%s/iommu_group", bdf);
	n = readlink(path, link, sizeof(link) - 1);
	if (n < 0)
		return -1;
	link[n] = '\0';

	slash = strrchr(link, '/');
	if (!slash)
		return -1;
	if (sscanf(slash + 1, "%d", &group) != 1)
		return -1;
	return group;
}

static int vfio_setup(struct cxlmi_endpoint *ep, const char *bdf,
		      struct cxlmi_transport_vfio *vfio)
{
	char path[PATH_MAX];
	struct vfio_group_status gstatus = { .argsz = sizeof(gstatus) };
	int group_id;

	group_id = read_iommu_group(bdf);
	if (group_id < 0) {
		cxlmi_msg(ep->ctx, LOG_ERR,
			  "%s: cannot read iommu_group (is vfio-pci bound?)\n",
			  bdf);
		return -1;
	}

	vfio->container_fd = open("/dev/vfio/vfio", O_RDWR | O_CLOEXEC);
	if (vfio->container_fd < 0) {
		cxlmi_msg(ep->ctx, LOG_ERR, "open(/dev/vfio/vfio) failed: %m\n");
		return -1;
	}

	snprintf(path, sizeof(path), "/dev/vfio/%d", group_id);
	vfio->group_fd = open(path, O_RDWR | O_CLOEXEC);
	if (vfio->group_fd < 0) {
		cxlmi_msg(ep->ctx, LOG_ERR, "open(%s) failed: %m\n", path);
		return -1;
	}

	if (ioctl(vfio->group_fd, VFIO_GROUP_GET_STATUS, &gstatus) ||
	    !(gstatus.flags & VFIO_GROUP_FLAGS_VIABLE)) {
		cxlmi_msg(ep->ctx, LOG_ERR,
			  "VFIO group %d not viable (all devices bound?)\n",
			  group_id);
		return -1;
	}

	if (ioctl(vfio->group_fd, VFIO_GROUP_SET_CONTAINER,
		  &vfio->container_fd)) {
		cxlmi_msg(ep->ctx, LOG_ERR,
			  "VFIO_GROUP_SET_CONTAINER failed: %m\n");
		return -1;
	}

	/*
	 * Best-effort: install a type-1 IOMMU on the container. Not strictly
	 * required for pure MMIO access, so failure is non-fatal.
	 */
	(void)ioctl(vfio->container_fd, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU);

	vfio->device_fd = ioctl(vfio->group_fd, VFIO_GROUP_GET_DEVICE_FD, bdf);
	if (vfio->device_fd < 0) {
		cxlmi_msg(ep->ctx, LOG_ERR,
			  "VFIO_GROUP_GET_DEVICE_FD(%s) failed: %m\n", bdf);
		return -1;
	}
	return 0;
}

static int vfio_map_bar(struct cxlmi_endpoint *ep,
			struct cxlmi_transport_vfio *vfio, uint8_t bir)
{
	struct vfio_region_info rinfo = {
		.argsz = sizeof(rinfo),
		.index = VFIO_PCI_BAR0_REGION_INDEX + bir,
	};
	void *map;

	if (ioctl(vfio->device_fd, VFIO_DEVICE_GET_REGION_INFO, &rinfo)) {
		cxlmi_msg(ep->ctx, LOG_ERR,
			  "VFIO_DEVICE_GET_REGION_INFO(BAR%u) failed: %m\n",
			  bir);
		return -1;
	}
	if (!(rinfo.flags & VFIO_REGION_INFO_FLAG_MMAP) || rinfo.size == 0) {
		cxlmi_msg(ep->ctx, LOG_ERR,
			  "BAR%u is not mmap-capable (flags=0x%x size=%llu)\n",
			  bir, rinfo.flags,
			  (unsigned long long)rinfo.size);
		return -1;
	}

	map = mmap(NULL, rinfo.size, PROT_READ | PROT_WRITE, MAP_SHARED,
		   vfio->device_fd, rinfo.offset);
	if (map == MAP_FAILED) {
		cxlmi_msg(ep->ctx, LOG_ERR, "mmap(BAR%u) failed: %m\n", bir);
		return -1;
	}
	vfio->bar_map = map;
	vfio->bar_len = rinfo.size;
	return 0;
}

static void vfio_teardown(struct cxlmi_transport_vfio *vfio)
{
	if (!vfio)
		return;
	if (vfio->bar_map && vfio->bar_map != MAP_FAILED)
		munmap(vfio->bar_map, vfio->bar_len);
	if (vfio->device_fd >= 0)
		close(vfio->device_fd);
	if (vfio->group_fd >= 0)
		close(vfio->group_fd);
	if (vfio->container_fd >= 0)
		close(vfio->container_fd);
}

void vfio_close_transport(struct cxlmi_endpoint *ep)
{
	struct cxlmi_transport_vfio *vfio = ep->vfio_data;

	vfio_teardown(vfio);
	free(vfio);
	ep->vfio_data = NULL;
}

/* ------------------------------------------------------------------ */
/* Mailbox send path                                                  */
/* ------------------------------------------------------------------ */

static int mbox_wait_doorbell(struct cxlmi_endpoint *ep,
			      volatile uint32_t *ctrl)
{
	unsigned int elapsed_us = 0;
	unsigned int max_us =
		(ep->timeout_ms > 0 ? ep->timeout_ms : 2000) * 1000u;

	while (le32_to_cpu(*ctrl) & CXL_MB_CTRL_DOORBELL) {
		struct timespec ts = {
			.tv_nsec = CXL_MB_POLL_INTERVAL_US * 1000,
		};

		if (elapsed_us >= max_us) {
			cxlmi_msg(ep->ctx, LOG_ERR,
				  "mailbox doorbell timeout\n");
			errno = ETIMEDOUT;
			return -1;
		}
		nanosleep(&ts, NULL);
		elapsed_us += CXL_MB_POLL_INTERVAL_US;
	}
	return 0;
}

/*
 * Copy a payload region into (or out of) MMIO with aligned 32-bit
 * accesses when possible; tail bytes fall back to byte writes/reads.
 * Values are stored little-endian in the mailbox payload register.
 */
static void mmio_copy_to(volatile uint8_t *dst, const uint8_t *src, size_t n)
{
	size_t i = 0;

	for (; i + 4 <= n; i += 4) {
		uint32_t v;
		memcpy(&v, src + i, 4);
		*(volatile uint32_t *)(dst + i) = cpu_to_le32(v);
	}
	for (; i < n; i++)
		dst[i] = src[i];
}

static void mmio_copy_from(uint8_t *dst, volatile uint8_t *src, size_t n)
{
	size_t i = 0;

	for (; i + 4 <= n; i += 4) {
		uint32_t v = le32_to_cpu(*(volatile uint32_t *)(src + i));
		memcpy(dst + i, &v, 4);
	}
	for (; i < n; i++)
		dst[i] = src[i];
}

int send_vfio_direct(struct cxlmi_endpoint *ep,
		     struct cxlmi_cci_msg *req_msg, size_t req_msg_sz,
		     struct cxlmi_cci_msg *rsp_msg, size_t rsp_msg_sz,
		     size_t rsp_msg_sz_min)
{
	struct cxlmi_transport_vfio *vfio = ep->vfio_data;
	volatile uint8_t *mb = vfio->mbox_regs;
	volatile uint32_t *ctrl = (volatile uint32_t *)(mb + 0x04);
	volatile uint64_t *cmd_reg = (volatile uint64_t *)(mb + 0x08);
	volatile uint64_t *status = (volatile uint64_t *)(mb + 0x10);
	volatile uint8_t *payload_reg = mb + 0x20;
	size_t in_pl_sz = req_msg_sz - sizeof(*req_msg);
	size_t out_pl_max = rsp_msg_sz - sizeof(*rsp_msg);
	uint64_t cmd, st;
	uint32_t out_pl_sz;
	uint16_t ret_code;

	if (in_pl_sz > vfio->payload_size ||
	    out_pl_max > vfio->payload_size) {
		cxlmi_msg(ep->ctx, LOG_ERR,
			  "payload too large for mailbox "
			  "(in=%zu out=%zu max=%zu)\n",
			  in_pl_sz, out_pl_max, vfio->payload_size);
		errno = EMSGSIZE;
		return -1;
	}

	if (mbox_wait_doorbell(ep, ctrl))
		return -1;

	mmio_copy_to(payload_reg, req_msg->payload, in_pl_sz);

	cmd = ((uint64_t)req_msg->command_set << 8) | req_msg->command;
	cmd |= ((uint64_t)in_pl_sz << CXL_MB_CMD_PAYLOAD_SHIFT) &
	       CXL_MB_CMD_PAYLOAD_MASK;
	*cmd_reg = cpu_to_le64(cmd);

	/* Ring the doorbell */
	*ctrl = cpu_to_le32(CXL_MB_CTRL_DOORBELL);

	if (mbox_wait_doorbell(ep, ctrl))
		return -1;

	st = le64_to_cpu(*status);
	ret_code = (st & CXL_MB_STS_RC_MASK) >> CXL_MB_STS_RC_SHIFT;

	/* Rebuild a minimal CCI response header for callers. */
	memset(rsp_msg, 0, rsp_msg_sz);
	rsp_msg->category = 1; /* CXL_MCTP_CATEGORY_RSP equivalent */
	rsp_msg->tag = req_msg->tag;
	rsp_msg->command = req_msg->command;
	rsp_msg->command_set = req_msg->command_set;
	rsp_msg->return_code = cpu_to_le16(ret_code);
	rsp_msg->vendor_ext_status =
		cpu_to_le16((st >> CXL_MB_STS_VENEXT_SHIFT) & 0xffff);

	if (ret_code != 0 && ret_code != CXLMI_RET_BACKGROUND) {
		cxlmi_msg(ep->ctx, LOG_ERR,
			  "mailbox command 0x%02x%02x returned %u\n",
			  req_msg->command_set, req_msg->command, ret_code);
		return ret_code;
	}

	cmd = le64_to_cpu(*cmd_reg);
	out_pl_sz = (cmd & CXL_MB_CMD_PAYLOAD_MASK) >> CXL_MB_CMD_PAYLOAD_SHIFT;
	if (out_pl_sz > out_pl_max)
		out_pl_sz = out_pl_max;
	rsp_msg->pl_length[0] = out_pl_sz & 0xff;
	rsp_msg->pl_length[1] = (out_pl_sz >> 8) & 0xff;
	rsp_msg->pl_length[2] = (out_pl_sz >> 16) & 0x0f;

	mmio_copy_from(rsp_msg->payload, payload_reg, out_pl_sz);

	if (rsp_msg_sz_min > sizeof(*rsp_msg) + out_pl_sz) {
		cxlmi_msg(ep->ctx, LOG_WARNING,
			  "response smaller than caller expected "
			  "(got %zu, want >=%zu)\n",
			  sizeof(*rsp_msg) + out_pl_sz, rsp_msg_sz_min);
	}

	return ret_code;
}

/* ------------------------------------------------------------------ */
/* Public open                                                        */
/* ------------------------------------------------------------------ */

CXLMI_EXPORT struct cxlmi_endpoint *cxlmi_open_vfio(struct cxlmi_ctx *ctx,
						    const char *pci_bdf)
{
	struct cxlmi_endpoint *ep = NULL, *tmp;
	struct cxlmi_transport_vfio *vfio = NULL;
	struct pci_access *pacc = NULL;
	struct pci_dev *pdev = NULL;
	char bdf[16];
	int dom, bus, dev, fn;
	int dvsec_off;
	uint8_t bir;
	uint64_t dev_regblock_offset;
	uint32_t mbox_off = 0, mbox_size = 0;
	volatile uint8_t *dev_regs;
	uint64_t mb_caps;
	int errno_save = 0;

	if (!ctx || !pci_bdf) {
		errno = EINVAL;
		return NULL;
	}

	if (parse_bdf(pci_bdf, &dom, &bus, &dev, &fn)) {
		cxlmi_msg(ctx, LOG_ERR, "invalid PCI BDF: %s\n", pci_bdf);
		errno = EINVAL;
		return NULL;
	}
	snprintf(bdf, sizeof(bdf), "%04x:%02x:%02x.%x", dom, bus, dev, fn);

	cxlmi_for_each_endpoint(ctx, tmp) {
		if (cxlmi_is_vfio_endpoint(tmp)) {
			struct cxlmi_transport_vfio *ex = tmp->vfio_data;

			if (!strcmp(ex->bdf, bdf)) {
				cxlmi_msg(ctx, LOG_ERR,
					  "vfio endpoint %s already opened\n",
					  bdf);
				errno = EEXIST;
				return NULL;
			}
		}
	}

	pacc = pci_alloc();
	if (!pacc) {
		cxlmi_msg(ctx, LOG_ERR, "pci_alloc() failed\n");
		return NULL;
	}
	pci_init(pacc);
	pdev = pci_get_dev(pacc, dom, bus, dev, fn);
	if (!pdev) {
		cxlmi_msg(ctx, LOG_ERR, "pci_get_dev(%s) failed\n", bdf);
		goto err;
	}
	pci_fill_info(pdev, PCI_FILL_IDENT | PCI_FILL_EXT_CAPS);

	dvsec_off = find_dvsec(pdev, CXL_DVSEC_VENDOR_ID,
			       CXL_DVSEC_REG_LOCATOR_ID);
	if (dvsec_off < 0) {
		cxlmi_msg(ctx, LOG_ERR,
			  "%s: CXL Register Locator DVSEC not found\n", bdf);
		goto err;
	}

	if (find_regblock(pdev, dvsec_off, CXL_REGBLOCK_ID_DEVICE,
			  &bir, &dev_regblock_offset) < 0) {
		cxlmi_msg(ctx, LOG_ERR,
			  "%s: CXL Device Register Block not found\n", bdf);
		goto err;
	}

	ep = init_endpoint(ctx);
	if (!ep)
		goto err;

	vfio = calloc(1, sizeof(*vfio));
	if (!vfio)
		goto err_close_ep;
	vfio->container_fd = -1;
	vfio->group_fd = -1;
	vfio->device_fd = -1;
	strncpy(vfio->bdf, bdf, sizeof(vfio->bdf) - 1);
	ep->vfio_data = vfio;

	if (vfio_setup(ep, bdf, vfio))
		goto err_close_ep;

	if (vfio_map_bar(ep, vfio, bir))
		goto err_close_ep;

	if (dev_regblock_offset >= vfio->bar_len) {
		cxlmi_msg(ctx, LOG_ERR,
			  "%s: device reg block offset 0x%llx beyond BAR%u (0x%zx)\n",
			  bdf, (unsigned long long)dev_regblock_offset, bir,
			  vfio->bar_len);
		goto err_close_ep;
	}
	dev_regs = (volatile uint8_t *)vfio->bar_map + dev_regblock_offset;

	if (find_dev_capability(dev_regs, CXL_DEVCAP_ID_PRIMARY_MBOX,
				&mbox_off, &mbox_size) < 0) {
		cxlmi_msg(ctx, LOG_ERR,
			  "%s: Primary Mailbox capability not found\n", bdf);
		goto err_close_ep;
	}
	if (mbox_size < 0x20) {
		cxlmi_msg(ctx, LOG_ERR,
			  "%s: Primary Mailbox region too small (%u)\n",
			  bdf, mbox_size);
		goto err_close_ep;
	}

	vfio->mbox_regs = dev_regs + mbox_off;
	mb_caps = le64_to_cpu(*(volatile uint64_t *)vfio->mbox_regs);
	/* MB Capabilities: Payload Size (bits 4:0) gives log2(size) */
	vfio->payload_size = (size_t)1 << (mb_caps & 0x1f);

	pci_free_dev(pdev);
	pci_cleanup(pacc);

	ep->timeout_ms = 2000;
	endpoint_probe(ep);
	return ep;

err_close_ep:
	errno_save = errno;
	cxlmi_close(ep);
	ep = NULL;
err:
	if (pdev)
		pci_free_dev(pdev);
	if (pacc)
		pci_cleanup(pacc);
	if (errno_save)
		errno = errno_save;
	return NULL;
}

#else /* !CONFIG_LIBPCI */

/*
 * libpci is not available at build time. We still export cxlmi_open_vfio()
 * so callers can link against libcxlmi unconditionally; it simply fails.
 * The send/close paths are guarded by CONFIG_LIBPCI in cxlmi.c so they are
 * never reached when vfio_data is always NULL.
 */
CXLMI_EXPORT struct cxlmi_endpoint *cxlmi_open_vfio(struct cxlmi_ctx *ctx,
						    const char *pci_bdf)
{
	(void)pci_bdf;
	if (ctx)
		cxlmi_msg(ctx, LOG_ERR,
			  "VFIO transport not available: "
			  "libcxlmi built without libpci\n");
	errno = ENOTSUP;
	return NULL;
}

#endif /* CONFIG_LIBPCI */
