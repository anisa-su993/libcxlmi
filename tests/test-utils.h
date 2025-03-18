#include <CUnit/Basic.h>
#include <CUnit/CUnit.h>
#include <unistd.h>
#include <getopt.h>
#include <libcxlmi.h>

#define MAX_CHARS 1000
#define SHORT_OPTS "hls:t:"

#define MiB   (1024UL * 1024UL)
#define CXL_CAPACITY_MULTIPLIER   256 * MiB

#define FMAPI_DCD_MCTP_NID 11
#define FMAPI_DCD_MCTP_EID 10

static struct cxlmi_ctx* ctx;
static struct cxlmi_endpoint* ep;


enum {
    OPT_TEST_CASE = 't',
    OPT_TEST_SUITE = 's',
    OPT_LIST = 'l',
    OPT_HELP = 'h',
};

static const struct option long_opts[] = {
        {"test", required_argument, NULL, OPT_TEST_CASE},
        {"suite", required_argument, NULL, OPT_TEST_SUITE},
        {"list", no_argument, NULL, OPT_LIST},
        {"help", no_argument, NULL, OPT_HELP},
        {},
    };

static int setup_mctp()
{
    ctx = cxlmi_new_ctx(stdout, DEFAULT_LOGLEVEL);
    if (!ctx)
        return -1;

    ep = cxlmi_open_mctp(ctx, FMAPI_DCD_MCTP_NID, FMAPI_DCD_MCTP_EID);
    if (!ep) {
    ep = cxlmi_open_mctp(ctx, FMAPI_DCD_MCTP_NID, FMAPI_DCD_MCTP_EID);
        printf("Failed to open MCTP ep with NID:EID %d:%d\n",
               FMAPI_DCD_MCTP_NID, FMAPI_DCD_MCTP_EID);
        return -1;
    }

    return 0;
}

static int cleanup_mctp(void)
{
    if (ctx)
        cxlmi_free_ctx(ctx);
    if (ep)
        cxlmi_close(ep);

    return 0;
}

static void print_usage(void)
{
    printf("Usage: unit-test [OPTIONS]\n");
    printf("default (no opts) will run all unit tests\n");
    printf("\t-t --test\t\t run single test case\n");
    printf("\t-s --suite\t\t run all tests in the given suite\n");
    printf("\t-l --list\t\t list test suites and tests cases\n");
    printf("\t-h --help\t\t print usage options\n");
}

static void list_tests(void) {
    CU_pSuite suite;
	CU_pTest test;
	int sid = 1, tid;

    while ((suite = CU_get_suite_at_pos(sid)) != NULL) {
        printf("%s:\n", suite->pName);
        tid = 1;
        while((test = CU_get_test_at_pos(suite, tid))!= NULL) {
            printf("\t%s\n", test->pName);
            tid++;
        }
        sid++;
    }
}

typedef enum CxlExtentSelectionPolicy {
    CXL_EXTENT_SELECTION_POLICY_FREE,
    CXL_EXTENT_SELECTION_POLICY_CONTIGUOUS,
    CXL_EXTENT_SELECTION_POLICY_PRESCRIPTIVE,
    CXL_EXTENT_SELECTION_POLICY_ENABLE_SHARED_ACCESS,
    CXL_EXTENT_SELECTION_POLICY__MAX,
} CxlExtentSelectionPolicy;

typedef enum CxlExtentRemovalPolicy {
    CXL_EXTENT_REMOVAL_POLICY_TAG_BASED,
    CXL_EXTENT_REMOVAL_POLICY_PRESCRIPTIVE,
    CXL_EXTENT_REMOVAL_POLICY__MAX,
} CxlExtentRemovalPolicy;