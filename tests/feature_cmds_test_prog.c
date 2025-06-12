#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <libcxlmi.h>
#include <string.h>

#define MAX_PAYLOAD_SIZE 4096

static inline void freep(void *p)
{
	free(*(void **)p);
}
#define _cleanup_free_ __attribute__((cleanup(freep)))

#define UUID(time_low, time_mid, time_hi_and_version,                    \
  clock_seq_hi_and_reserved, clock_seq_low, node0, node1, node2,         \
  node3, node4, node5)                                                   \
  { ((time_low) >> 24) & 0xff, ((time_low) >> 16) & 0xff,                \
    ((time_low) >> 8) & 0xff, (time_low) & 0xff,                         \
    ((time_mid) >> 8) & 0xff, (time_mid) & 0xff,                         \
    ((time_hi_and_version) >> 8) & 0xff, (time_hi_and_version) & 0xff,   \
    (clock_seq_hi_and_reserved), (clock_seq_low),                        \
    (node0), (node1), (node2), (node3), (node4), (node5)                 \
  }


#define ASSERT_EQUAL(expected, actual, field) \
    if ((expected).field != (actual)->field) { \
        printf("Assertion failed: %s.%s = %llu, %s->%s = %llu\n", \
               #expected, #field, (expected).field, \
               #actual, #field, (actual)->field); \
        rc = EXIT_FAILURE; \
    }

#define ASSERT_EQUAL_FATAL(expected, actual, field) \
    if ((expected).field != (actual)->field) { \
        printf("Assertion failed: %s.%s = %llu, %s->%s = %llu\n", \
               #expected, #field, (expected).field, \
               #actual, #field, (actual)->field); \
        rc = EXIT_FAILURE; \
        goto cleanup; \
    }

int main() {
    struct cxlmi_ctx *ctx;
    struct cxlmi_endpoint *ep, *tmp;
    void *buf = calloc(1, MAX_PAYLOAD_SIZE);
    int rc = EXIT_FAILURE;

    assert(buf != NULL);
    ctx = cxlmi_new_ctx(stdout, DEFAULT_LOGLEVEL);
    assert(ctx != NULL);

    ep = cxlmi_open(ctx, "mem0");
    if (!ep) {
        fprintf(stdout, "Failed to open device %s\n", "mem0");
        goto exit_free_ctx;
    }

    printf("Opened endpoint on device %s\n", "mem0");

    cxlmi_for_each_endpoint_safe(ctx, ep, tmp) {
            struct cxlmi_cmd_get_supported_features_req request_1 = {
            .count = 64,
            .starting_feature_index = 0,
        };

        _cleanup_free_ struct cxlmi_cmd_get_supported_features_rsp *expected_1 = calloc(1, sizeof(*expected_1) + 2 * sizeof(expected_1->supported_feature_entries[0]));
        expected_1->num_supported_feature_entries = 1;
        expected_1->device_supported_features = 2;

        struct cxlmi_cmd_get_supported_features_rsp *actual_1 = (struct cxlmi_cmd_get_supported_features_rsp *) buf;

        rc = cxlmi_cmd_get_supported_features(ep, NULL, &request_1, actual_1);
        if (rc != 0) {
            fprintf(stdout, "Error: Function cxlmi_cmd_get_supported_features returned non-zero rc: %d\n", rc);
            goto cleanup;
        }

        ASSERT_EQUAL(*expected_1, actual_1, num_supported_feature_entries);
        ASSERT_EQUAL(*expected_1, actual_1, device_supported_features);

        printf("Get Feature Size: %hu\n", actual_1->supported_feature_entries[0].get_feature_size);
        printf("Set Feature Size: %hu\n", actual_1->supported_feature_entries[0].set_feature_size);

        uint16_t get_feature_size = actual_1->supported_feature_entries[0].get_feature_size;

        struct cxlmi_cmd_get_feature_req request_2 = {
            .feature_id = UUID(0x96dad7d6, 0xfde8, 0x482b, 0xa7, 0x33,
                 0x75, 0x77, 0x4e, 0x06, 0xdb, 0x8a),
            .offset =  0 ,
            .count =  get_feature_size ,
            .selection =  0 ,
        };

        struct cxlmi_cmd_get_feature_rsp *actual_2 = (struct cxlmi_cmd_get_feature_rsp *) buf;

        rc = cxlmi_cmd_get_feature(ep, NULL, &request_2, actual_2);
        if (rc != 0) {
            fprintf(stdout, "Error: Function cxlmi_cmd_get_feature returned non-zero rc: %d\n", rc);
            goto cleanup;
        }

        struct cxlmi_cmd_set_feature *request_3 = calloc(1, sizeof(*request_3) + 7);

        uint8_t arr[16] = UUID(0xe5b13f22, 0x2328, 0x4a14, 0xb8, 0xba,
                 0xb9, 0x69, 0x1e, 0x89, 0x33, 0x86);
        memcpy(request_3->feature_id, arr, 0x10);
        request_3->offset = 0;
        request_3->version = 1;

        uint8_t feature_data[7] = {0};
        memcpy(request_3->feature_data, feature_data, 7);

        rc = cxlmi_cmd_set_feature(ep, NULL, request_3, 7);
        if (rc != 0) {
            fprintf(stdout, "Error: Function cxlmi_cmd_set_feature returned non-zero rc: %d\n", rc);
            goto cleanup;
        }


        cxlmi_close(ep);
    }

cleanup:
    cxlmi_for_each_endpoint_safe(ctx, ep, tmp) {
        cxlmi_close(ep);
    }
exit_free_ctx:
    free(buf);
    cxlmi_free_ctx(ctx);
    if (rc != 0) {
        fprintf(stdout, "Tests failed\n");
    } else {
        printf("All tests passed\n");
    }
    return rc;
}
