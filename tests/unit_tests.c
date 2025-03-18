#include <stdlib.h>
#include <libcxlmi.h>
#include "test-utils.h"

/* Opcode 5600h */
static void test_fmapi_get_dcd_info(void)
{
	struct cxlmi_cmd_fmapi_get_dcd_info *out;
    uint64_t total_capacity = 4096 * MiB;
    uint64_t block_size = 1 << (int) log2(2 * MiB);
    int rc;

	out = calloc(1, sizeof(*out));
	if (!out) {
        CU_FAIL_FATAL("Failed to allocate output payload");
    }

    rc = cxlmi_cmd_fmapi_get_dcd_info(ep, NULL, out);
    if (rc){
        free(out);
        CU_FAIL_FATAL("Nonzero rc");
    }

    CU_ASSERT_EQUAL(out->num_hosts, 1);
    CU_ASSERT_EQUAL(out->num_supported_dc_regions, 2);
    CU_ASSERT_EQUAL(out->capacity_selection_policies,
                    (1 << CXL_EXTENT_SELECTION_POLICY_PRESCRIPTIVE));
    CU_ASSERT_EQUAL(out->capacity_removal_policies,
                    (1 << CXL_EXTENT_REMOVAL_POLICY_PRESCRIPTIVE));
    CU_ASSERT_EQUAL(out->sanitize_on_release_config_mask, 0);
    CU_ASSERT_EQUAL(out->total_dynamic_capacity, total_capacity);
    CU_ASSERT_EQUAL(out->region_0_supported_blk_sz_mask, block_size);
    CU_ASSERT_EQUAL(out->region_1_supported_blk_sz_mask, block_size);

	free(out);
}

static CU_TestInfo fm_dcd_tests[] = {
    {"5600h", test_fmapi_get_dcd_info},
    CU_TEST_INFO_NULL,
};

int main(int argc, char** argv)
{
    CU_pSuite suite = NULL;
    CU_pTest test = NULL;
    char* suitename = NULL;
    char* testname = NULL;
    int rc = 0, opt;

    /* Initialize CUnit test registry */
    rc = CU_initialize_registry();
    if (rc) {
        printf(CU_get_error_msg());
        return rc;
    }

    CU_SuiteInfo suites[] = {
        {"56h FM DCD Management", setup_mctp, cleanup_mctp, NULL, NULL, fm_dcd_tests},
        CU_SUITE_INFO_NULL,
    };

    rc = CU_register_suites(suites);
    if (rc) {
        printf(CU_get_error_msg());
        goto exit;
    }

    while((opt = getopt_long(argc, argv, SHORT_OPTS, long_opts, NULL)) != -1) {
        switch(opt) {
            case OPT_HELP:
                print_usage();
                return 0;
            case OPT_TEST_CASE:
                testname = optarg;
                break;
            case OPT_TEST_SUITE:
                suitename = optarg;
                break;
            case OPT_LIST:
                list_tests();
                return 0;
            case '?':
			    rc = -EINVAL;
        }
    }

    CU_basic_set_mode(CU_BRM_VERBOSE);

    if (suitename) {
        suite = CU_get_suite(suitename);
    }

    if (testname) {
        /* Go through all suites to find the test if unspecified */
        if (!suite) {
            suite = CU_get_suite_at_pos(1);

            while(suite->pNext != NULL) {
                test = CU_get_test(suite, testname);
                if (test) {
                    break;
                }
                suite = suite->pNext;
            }
        }
        test = CU_get_test(suite, testname);
    }

    if (test) {
        rc = CU_basic_run_test(suite, test);
    } else if (suite) {
        rc = CU_basic_run_suite(suite);
    } else {
        rc = CU_basic_run_tests();
    }

exit:
    CU_cleanup_registry();
    return rc;
}