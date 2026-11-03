// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * This file is part of libcxlmi.
 *
 * Print a memdev's Dynamic Capacity extent list as the device reports it
 * over the host mailbox (Get Dynamic Capacity Extent List, opcode 4801h),
 * one request per page, with each extent's tag and Shared Extent Sequence.
 *
 *   dc-extent-list <memN> [extents-per-request]
 *
 * extents-per-request defaults to 8, the most the library asks for at
 * once; use 1 to see how the device sizes a single-extent response.
 */
#include <inttypes.h>
#include <libcxlmi.h>
#include "examples.h"

#define MAX_EXTENTS_PER_REQ 8

static void print_tag(const uint8_t *t)
{
	printf("%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
	       t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7],
	       t[8], t[9], t[10], t[11], t[12], t[13], t[14], t[15]);
}

static int dump_extent_list(struct cxlmi_endpoint *ep, uint32_t per_req)
{
	struct cxlmi_cmd_memdev_get_dc_extent_list_req req = { 0 };
	struct cxlmi_cmd_memdev_get_dc_extent_list_rsp *rsp;
	uint32_t idx = 0, total = 0;
	int rc;

	rsp = calloc(1, sizeof(*rsp) +
		     MAX_EXTENTS_PER_REQ * sizeof(rsp->extents[0]));
	if (!rsp)
		return -1;

	do {
		uint32_t i;

		req.extent_cnt = per_req;
		req.start_extent_idx = idx;
		rc = cxlmi_cmd_memdev_get_dc_extent_list(ep, NULL, &req, rsp);
		if (rc) {
			if (rc > 0)
				fprintf(stderr, "get extent list: %s\n",
					cxlmi_cmd_retcode_tostr(rc));
			else
				fprintf(stderr, "get extent list failed\n");
			break;
		}

		printf("start %u: returned %u of %u, generation %u\n",
		       idx, rsp->num_extents_returned, rsp->total_num_extents,
		       rsp->generation_num);
		for (i = 0; i < rsp->num_extents_returned; i++) {
			printf("  extent %u: dpa 0x%" PRIx64 " len 0x%" PRIx64
			       " tag ", idx + i, rsp->extents[i].start_dpa,
			       rsp->extents[i].len);
			print_tag(rsp->extents[i].tag);
			printf(" shared_seq %u\n", rsp->extents[i].shared_seq);
		}

		total = rsp->total_num_extents;
		idx += rsp->num_extents_returned;
	} while (rsp->num_extents_returned && idx < total);

	free(rsp);
	return rc;
}

int main(int argc, char **argv)
{
	struct cxlmi_ctx *ctx;
	struct cxlmi_endpoint *ep;
	uint32_t per_req = MAX_EXTENTS_PER_REQ;
	int rc = EXIT_FAILURE;

	if (argc < 2 || argc > 3) {
		fprintf(stderr, "Usage: dc-extent-list <device> [extents-per-request]\n");
		goto exit;
	}
	if (argc == 3) {
		per_req = strtoul(argv[2], NULL, 0);
		if (!per_req || per_req > MAX_EXTENTS_PER_REQ) {
			fprintf(stderr, "extents-per-request must be 1..%d\n",
				MAX_EXTENTS_PER_REQ);
			goto exit;
		}
	}

	ctx = cxlmi_new_ctx(stdout, DEFAULT_LOGLEVEL);
	if (!ctx) {
		fprintf(stderr, "cannot create new context object\n");
		goto exit;
	}

	ep = cxlmi_open(ctx, argv[1]);
	if (!ep) {
		fprintf(stderr, "cannot open '%s' endpoint\n", argv[1]);
		goto exit_free_ctx;
	}

	if (!dump_extent_list(ep, per_req))
		rc = EXIT_SUCCESS;

	cxlmi_close(ep);
exit_free_ctx:
	cxlmi_free_ctx(ctx);
exit:
	return rc;
}
