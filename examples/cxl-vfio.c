// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * This file is part of libcxlmi.
 *
 * Example: open a CXL PCIe function via VFIO and send commands directly to
 * its Primary Mailbox registers. Targets Multi-Headed Devices, where the
 * endpoint is the FM-owned LD's primary mailbox. After an Identify, it
 * tunnels Get Multi-Headed Info to the LD Pool CCI.
 *
 * Before running, bind the function to vfio-pci, e.g.:
 *
 *   echo vfio-pci > /sys/bus/pci/devices/0000:3d:00.0/driver_override
 *   echo 0000:3d:00.0 > /sys/bus/pci/drivers/vfio-pci/bind
 *
 * Then:
 *
 *   cxl-vfio 0000:3d:00.0
 */
#include <libcxlmi.h>
#include "examples.h"

static int show_identify(struct cxlmi_endpoint *ep)
{
	struct cxlmi_cmd_identify_rsp id;
	int rc;

	rc = cxlmi_cmd_identify(ep, NULL, &id);
	if (rc) {
		if (rc > 0)
			fprintf(stderr, "identify failed: %s\n",
				cxlmi_cmd_retcode_tostr(rc));
		return rc;
	}

	printf("VID:%04x DID:%04x SubsysVID:%04x SubsysID:%04x\n",
	       id.vendor_id, id.device_id,
	       id.subsys_vendor_id, id.subsys_id);
	printf("serial: 0x%016lx  component type: 0x%02x\n",
	       (uint64_t)id.serial_num, id.component_type);
	return 0;
}

static int show_mhd_info(struct cxlmi_endpoint *ep)
{
	struct cxlmi_cmd_fmapi_get_multiheaded_info_req req = {
		.start_ld_id = 0,
		.ld_map_list_limit = 16,
	};
	struct cxlmi_cmd_fmapi_get_multiheaded_info_rsp *rsp;
	size_t rsp_sz = sizeof(*rsp) + 16;
	DEFINE_CXLMI_TUNNEL_MHD(ti);
	int rc, i;

	if (!cxlmi_endpoint_has_fmapi(ep)) {
		printf("FM-API not enabled; skipping MHD info\n");
		return 0;
	}

	rsp = calloc(1, rsp_sz);
	if (!rsp)
		return -1;

	rc = cxlmi_cmd_fmapi_get_multiheaded_info(ep, &ti, &req, rsp);
	if (rc) {
		if (rc > 0)
			fprintf(stderr, "get_multiheaded_info: %s\n",
				cxlmi_cmd_retcode_tostr(rc));
		goto out;
	}

	printf("MHD num_lds=%u num_heads=%u start_ld_id=%u ld_map_len=%u\n",
	       rsp->num_lds, rsp->num_heads,
	       rsp->start_ld_id, rsp->ld_map_len);
	for (i = 0; i < rsp->ld_map_len; i++)
		printf("  ld_map[%d] = 0x%02x\n", i, rsp->ld_map[i]);
out:
	free(rsp);
	return rc;
}

int main(int argc, char **argv)
{
	struct cxlmi_ctx *ctx;
	struct cxlmi_endpoint *ep;
	int rc = EXIT_FAILURE;

	if (argc != 2) {
		fprintf(stderr, "Usage: cxl-vfio <pci-bdf>\n");
		fprintf(stderr, "  e.g. cxl-vfio 0000:3d:00.0\n");
		return rc;
	}

	ctx = cxlmi_new_ctx(stdout, DEFAULT_LOGLEVEL);
	if (!ctx) {
		fprintf(stderr, "cannot create libcxlmi context\n");
		return rc;
	}

	ep = cxlmi_open_vfio(ctx, argv[1]);
	if (!ep) {
		fprintf(stderr, "cannot open VFIO endpoint %s: %m\n", argv[1]);
		goto free_ctx;
	}

	printf("opened VFIO endpoint for %s\n", argv[1]);

	if (show_identify(ep) == 0)
		(void)show_mhd_info(ep);

	rc = EXIT_SUCCESS;
	cxlmi_close(ep);
free_ctx:
	cxlmi_free_ctx(ctx);
	return rc;
}
