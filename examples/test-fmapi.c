#include "examples.h"
#define MAX_CHARS 50

/* Returns number of extents and ext_list */
static int parse_extents(extent *ext_list, bool add)
{
    int num_extents;
    char input[MAX_CHARS];
    uint64_t start, end;
    char err;
    char *errp = &err;
    int i;

    memset(input, 0, MAX_CHARS);
    printf("How many extents to %s? ", add ? "add" : "release");

    if (!fgets(input, MAX_CHARS, stdin)) {
        printf("Enter a valid number of extents.\n");
        return 0;
    }

    num_extents = atoi(input);

    if (!num_extents) {
        printf("Enter a valid number of extents.\n");
        return 0;
    }

    if (!ext_list) {
        printf("Failed to allocate extent list\n");
        return 0;
    }

    for (i = 0; i < num_extents; i++) {
        printf("Enter extent %d start-end in MB (ex: 0-128): ", i + 1);

        if (!fgets(input, MAX_CHARS, stdin)) {
            printf("Invalid length. Aborting.\n");
            return 0;
        }

        char *split = strchr(input, '-');
        if (!split) {
            printf("Invalid length. Aborting.\n");
            return 0;
        }
        *split = '\0';
        start = strtoull(input, &errp, 10) * CXL_CAPACITY_MULTIPLIER;
        if (*input == '\0' || err != '\0') {
            printf("Invalid start. Aborting.\n");
            return 0;
        }
        end = strtoull(split + 1, NULL, 10) * CXL_CAPACITY_MULTIPLIER;
        if (!end) {
            printf("Invalid end. Aborting.\n");
            return 0;
        }

        if (end - start == 0) {
            printf("Start and end cannot be equal. Aborting.\n");
            return 0;
        }

        ext_list[i].start_dpa =  start;
        ext_list[i].len = end - start;
    }

    return num_extents;
}

int send_add(int num_exts, extent *ext_list, struct cxlmi_endpoint *ep) {
    struct cxlmi_cmd_fmapi_initiate_dc_add_req* add_req = NULL;
    int i, rc;
    uint64_t total_len = 0;

    add_req = calloc(1, sizeof(*add_req) +
        num_exts * sizeof(add_req->extents[0]));

    if (!add_req) {
        free(ext_list);
        return -1;
    }

    add_req->host_id = 0;
	add_req->selection_policy = CXL_EXTENT_SELECTION_POLICY_PRESCRIPTIVE;
	add_req->ext_count = num_exts;

    for (i = 0; i < num_exts; i++) {
        add_req->extents[i].start_dpa = ext_list[i].start_dpa;
	    add_req->extents[i].len = ext_list[i].len;
        total_len += ext_list[i].len;
    }

    add_req->length = total_len;
    printf("Sending add request for %i extents\n", num_exts);

    rc = cxlmi_cmd_fmapi_initiate_dc_add(ep, NULL, add_req);
    free(add_req);
    return rc;
}

int send_release(int num_exts, extent *ext_list, struct cxlmi_endpoint *ep) {
    struct cxlmi_cmd_fmapi_initiate_dc_release_req* release_req = NULL;
    int i, rc;
    uint64_t total_len = 0;

    release_req = calloc(1, sizeof(*release_req) +
        num_exts * sizeof(release_req->extents[0]));

    if (!release_req) {
        free(ext_list);
        return -1;
    }

    release_req->host_id = 0;
	release_req->flags = CXL_EXTENT_REMOVAL_POLICY_PRESCRIPTIVE;
	release_req->ext_count = num_exts;

    for (i = 0; i < num_exts; i++) {
        release_req->extents[i].start_dpa = ext_list[i].start_dpa;
	    release_req->extents[i].len = ext_list[i].len;
        total_len += ext_list[i].len;
    }

    release_req->length = total_len;
    printf("Sending release request for %i extents\n", num_exts);

    rc = cxlmi_cmd_fmapi_initiate_dc_release(ep, NULL, release_req);

    free(release_req);
    return rc;
}

static int print_extents(struct cxlmi_endpoint *ep)
{
    int i, rc;
    struct cxlmi_cmd_fmapi_get_dc_region_ext_list_req req;
    struct cxlmi_cmd_fmapi_get_dc_region_ext_list_rsp *rsp;

    req.host_id = 0;
    req.extent_count = 10;
    req.start_ext_index = 0;

    rsp = calloc(1, sizeof(*rsp) + req.extent_count * sizeof(rsp->extents[0]));

    if (!rsp) {
        return -1;
    }

    rc = cxlmi_cmd_fmapi_get_dc_region_ext_list(ep, NULL, &req, rsp);
    if (rc) {
        rc = -1;
        goto free_out;
    }

    printf("\tHost Id: %hu\n", rsp->host_id);
    printf("\tStarting Extent Index: %u\n", rsp->start_ext_index);
    printf("\tNumber of Extents Returned: %u\n", rsp->extents_returned);
    printf("\tTotal Extents: %u\n", rsp->total_extents);
    printf("\tExtent List Generation Number: %u\n", rsp->list_generation_num);

    for (i = 0; i < rsp->extents_returned; i++) {
        printf("\t\tExtent %d Info:\n", i);
        printf("\t\t\tStart DPA: 0x%08lx\n", rsp->extents[i].start_dpa);
        printf("\t\t\tLength: 0x%08lx\n", rsp->extents[i].len);
    }

free_out:
    free(rsp);
    return rc;

}

int main(int argc, char **argv)
{
	struct cxlmi_ctx *ctx;
	struct cxlmi_endpoint *ep, *tmp;
    int cmd;
    char buf[MAX_CHARS];

	ctx = cxlmi_new_ctx(stdout, DEFAULT_LOGLEVEL);
	if (!ctx) {
		fprintf(stderr, "cannot create new context object\n");
		return EXIT_FAILURE;
	}

    int num_ep = cxlmi_scan_mctp(ctx);

    printf("scanning dbus...\n");

    if (num_ep < 0) {
        fprintf(stderr, "dbus scan error\n");
        goto exit_free_ctx;
    } else if (num_ep == 0) {
        printf("no endpoints found\n");
    } else
        printf("found %d endpoint(s)\n", num_ep);

    cxlmi_for_each_endpoint_safe(ctx, ep, tmp) {
        if (ep_supports_op(ep, 0x5600)) {
            extent *ext_list = calloc(10, sizeof(extent));
            while (true) {
                printf("Enter 1 (add), 2 (release), 3 (print). Otherwise, exit: ");
                memset(buf, 0, MAX_CHARS);
                if (!fgets(buf, MAX_CHARS, stdin)) {
                    goto exit_free_ctx;
                }
                cmd = atoi(buf);
                int num_extents;
                if (!cmd) {
                    goto exit_free_ctx;
                }

                switch (cmd) {
                    case 1:
                        num_extents = parse_extents(ext_list, 1);
                        if (num_extents != 0) {
                            send_add(num_extents, ext_list, ep);
                        }
                        break;
                    case 2:
                        num_extents = parse_extents(ext_list, 0);
                        if (num_extents != 0) {
                            send_release(num_extents, ext_list, ep);
                        }
                        break;
                    case 3:
                        print_extents(ep);
                        break;
                }

            }
            free(ext_list);
        }
    }

exit_free_ctx:
    cxlmi_free_ctx(ctx);
    return 0;
}