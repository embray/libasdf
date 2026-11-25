/**
 * OOM regression tests
 *
 * This test module should be compiled with
 * ``-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=strdup,--wrap=strndup``
 * so that all allocation calls within the link unit can be intercepted.
 * The wrappers let us trigger a NULL return after a controlled number of
 * successful allocations, then verify that the library handles the OOM
 * gracefully instead of crashing.
 *
 * Note that the tests which fail one specific allocation can tend to be very
 * fragile, as they depend on counting the number of allocations performed by
 * the library down specific code paths.  If, after refactoring any of the
 * affected code, it is not sensible or feasible to update any of these tests
 * don't be afraid to delete them.  But it's useful for writing regression
 * tests for when such bugs are noticed.
 *
 * Tests using `malloc_fail_sweep` instead fail every allocation made by an
 * operation in turn, so they do not depend on the number of allocations.
 */
#include <stdbool.h>
#include <stdlib.h>

#include "munit.h"
#include "util.h"

#include "asdf/core/extension_metadata.h"
#include "asdf/core/ndarray.h"
#include "asdf/core/time.h"

#include "event.h"
#include "file.h"
#include "parser.h"
#include "tag.h"
#include "value.h"


/** malloc injection */

/** Default maximum number of iterations for `malloc_fail_sweep` */
#define SWEEP_MAX 200

static int fail_after = -1;
static int call_count = 0;
/** Set when an allocation was made to fail since the last `malloc_fail_after` */
static bool injected = false;


static void malloc_fail_after(int n) {
    call_count = 0;
    fail_after = n;
    injected = false;
}


static void malloc_fail_reset(void) {
    fail_after = -1;
}


static bool should_fail(void) {
    if (fail_after >= 0 && call_count++ >= fail_after) {
        injected = true;
        return true;
    }
    return false;
}


extern void *__real_malloc(size_t n);
extern void *__real_calloc(size_t n, size_t s);
extern void *__real_realloc(void *ptr, size_t n);
extern char *__real_strdup(const char *s);
extern char *__real_strndup(const char *s, size_t n);


void *__wrap_malloc(size_t n) {
    if (should_fail())
        return NULL;
    return __real_malloc(n);
}


void *__wrap_calloc(size_t n, size_t s) {
    if (should_fail())
        return NULL;
    return __real_calloc(n, s);
}


void *__wrap_realloc(void *ptr, size_t n) {
    if (should_fail())
        return NULL;
    return __real_realloc(ptr, n);
}


char *__wrap_strdup(const char *s) {
    if (should_fail())
        return NULL;
    return __real_strdup(s);
}


char *__wrap_strndup(const char *s, size_t n) {
    if (should_fail())
        return NULL;
    return __real_strndup(s, n);
}


typedef void (*sweep_setup_fn_t)(void *arg);
typedef bool (*sweep_fn_t)(void *arg);
typedef void (*sweep_teardown_fn_t)(void *arg);


/**
 * Run an operation repeatedly, with every allocation made from the Nth one
 * onward failing, for N = 0, 1, 2, ... until a run completes without any
 * allocation having been made to fail
 *
 * Each failed run is expected to fail cleanly; any crash or leak along the
 * way is caught by ASan / LSan.  The sweep ends when no failure was injected,
 * not when ``fn`` succeeds, as some code paths treat an allocation failure as
 * non-fatal and carry on.  The final run, with no failure injected, must
 * succeed.
 *
 * ``setup`` and ``teardown`` (both optional) run before and after each run of
 * ``fn``, with allocation failures disabled, so that only allocations made by
 * ``fn`` itself are failed.
 *
 * ``sweep_max`` is the maximum number of runs, or `SWEEP_MAX` if 0; the test
 * fails if the sweep does not complete within it.
 *
 * Returns the number of allocations made by the final run of ``fn``.
 */
static int malloc_fail_sweep(
    sweep_setup_fn_t setup,
    sweep_fn_t fn,
    sweep_teardown_fn_t teardown,
    void *arg,
    int sweep_max) {

    if (sweep_max <= 0)
        sweep_max = SWEEP_MAX;

    for (int idx = 0; idx < sweep_max; idx++) {
        if (setup)
            setup(arg);

        malloc_fail_after(idx);
        bool ok = fn(arg);
        bool was_injected = injected;
        malloc_fail_reset();

        if (teardown)
            teardown(arg);

        if (!was_injected) {
            if (!ok)
                munit_errorf("operation failed with no allocation failure injected "
                             "(after %d allocations)", idx);

            munit_logf(MUNIT_LOG_INFO, "malloc sweep exercised %d allocations", idx);
            return idx;
        }
    }

    munit_errorf("malloc sweep did not complete within %d iterations", sweep_max);
}


/** Misc test helpers */

static asdf_parser_t *make_parser(const char *fixture) {
    const char *filename = get_fixture_file_path(fixture);
    asdf_parser_t *parser = asdf_parser_create(NULL);

    if (!parser)
        munit_error("failed to create parser");

    if (asdf_parser_set_input_file(parser, filename) != 0)
        munit_errorf("failed to set parser input file '%s'", filename);

    return parser;
}


/**
 * Consume exactly ``count`` events during test setup (no injection
 * active).
 */
static void consume_events(asdf_parser_t *parser, int count) {
    for (int idx = 0; idx < count; idx++) {
        asdf_event_t *event = asdf_event_iterate(parser);
        if (!event)
            munit_error("unexpected end of events during setup");
    }
}


/** Regression tests */

/**
 * ``parse_asdf_version`` -- ``malloc(sizeof(asdf_version_t))`` without NULL
 * check.
 *
 * First `asdf_event_iterate`: freelist is empty so `asdf_parse_event_alloc`
 * calls ``malloc`` (call 0).  fail_after=1 lets that succeed then fails
 * (call 1), which is the version-struct malloc inside ``parse_asdf_version``.
 */
MU_TEST(oom_parse_asdf_version) {
    asdf_parser_t *parser = make_parser("parse-minimal.asdf");

    malloc_fail_after(1);
    asdf_event_t *event = asdf_event_iterate(parser);
    malloc_fail_reset();

    assert_null(event);
    assert_true(asdf_parser_has_error(parser));

    asdf_parser_destroy(parser);
    return MUNIT_OK;
}


/**
 * ``parse_standard_version``--``malloc(sizeof(asdf_version_t))`` without NULL
 * check.
 *
 * Consume 1 event first (``ASDF_ASDF_VERSION_EVENT``).  On the second
 * iteration the previous event is recycled back to the freelist, so
 * ``asdf_parse_event_alloc`` takes it without calling malloc.
 * fail_after=0 therefore fails the first malloc call, which is the
 * version-struct allocation in ``parse_standard_version``.
 */
MU_TEST(oom_parse_standard_version) {
    asdf_parser_t *parser = make_parser("parse-minimal.asdf");

    consume_events(parser, 1);

    malloc_fail_after(0);
    asdf_event_t *event = asdf_event_iterate(parser);
    malloc_fail_reset();

    assert_null(event);
    assert_true(asdf_parser_has_error(parser));

    asdf_parser_destroy(parser);
    return MUNIT_OK;
}


/**
 * ``parse_comment`` -- ``strndup`` without NULL check.
 *
 * Consume 2 events first (``ASDF_ASDF_VERSION_EVENT``,
 * ``ASDF_STANDARD_VERSION_EVENT``).  The fixture has an extra comment
 * line, so the third iterate hits ``parse_comment``.  fail_after=0
 * fails call 0 = the strndup inside ``parse_comment``.
 */
MU_TEST(oom_parse_comment) {
    asdf_parser_t *parser = make_parser("parse-minimal-extra-comment.asdf");

    consume_events(parser, 2);

    malloc_fail_after(0);
    asdf_event_t *event = asdf_event_iterate(parser);
    malloc_fail_reset();

    assert_null(event);
    assert_true(asdf_parser_has_error(parser));

    asdf_parser_destroy(parser);
    return MUNIT_OK;
}


/**
 * ``emit_tree_start_event`` -- ``calloc(1, sizeof(asdf_tree_info_t))``
 * without NULL check.
 *
 * Consume 3 events first (``ASDF_ASDF_VERSION_EVENT``,
 * ``ASDF_STANDARD_VERSION_EVENT``, ``ASDF_BLOCK_INDEX_EVENT``) from a
 * fixture that has a full tree.  The fourth iterate enters
 * ``emit_tree_start_event``.  fail_after=0 fails call 0 = the calloc.
 */
MU_TEST(oom_emit_tree_start) {
    asdf_parser_t *parser = make_parser("255-padding-after-header.asdf");

    consume_events(parser, 3);

    malloc_fail_after(0);
    asdf_event_t *event = asdf_event_iterate(parser);
    malloc_fail_reset();

    assert_null(event);
    assert_true(asdf_parser_has_error(parser));

    asdf_parser_destroy(parser);
    return MUNIT_OK;
}


/**
 * ``asdf_tag_parse`` -- ``strdup`` of ``name`` without NULL check,
 * no-separator path.
 *
 * For a tag string with no ``-`` separator, ``asdf_tag_parse`` calls
 * ``calloc`` (call 0) then ``strdup`` for the name (call 1).
 * fail_after=1 lets the calloc succeed and fails the strdup.
 */
MU_TEST(oom_tag_parse_name_only) {
    malloc_fail_after(1);
    asdf_tag_t *tag = asdf_tag_parse("ndarray");
    malloc_fail_reset();

    assert_null(tag);
    return MUNIT_OK;
}


/**
 * ``asdf_tag_parse`` -- ``strndup`` of ``name`` without NULL check,
 * separator path.
 *
 * For a tag string with a ``-`` separator, ``asdf_tag_parse`` calls
 * ``calloc`` (0), ``strndup`` for the name (1), ``strdup`` for the
 * version (2).  fail_after=1 fails the strndup.
 */
MU_TEST(oom_tag_parse_name_strndup) {
    malloc_fail_after(1);
    asdf_tag_t *tag = asdf_tag_parse("ndarray-1.0.0");
    malloc_fail_reset();

    assert_null(tag);
    return MUNIT_OK;
}


/**
 * ``asdf_tag_parse`` -- ``strdup`` of ``version`` without NULL check.
 *
 * Same as above but fail_after=2 lets the calloc and strndup succeed
 * then fails the strdup for the version string.
 */
MU_TEST(oom_tag_parse_version_strdup) {
    malloc_fail_after(2);
    asdf_tag_t *tag = asdf_tag_parse("ndarray-1.0.0");
    malloc_fail_reset();

    assert_null(tag);
    return MUNIT_OK;
}


/**
 * State for sweeps over deserializing a single value from a fixture file
 *
 * The file is opened and the value looked up during the sweep's setup, so
 * that only the deserialization itself has allocations failed.  Each run
 * opens a fresh file and value to avoid extension-type caching.
 */
typedef struct {
    const char *fixture;
    const char *path;
    asdf_file_t *file;
    asdf_value_t *value;
} deserialize_sweep_t;


static void deserialize_sweep_setup(void *arg) {
    deserialize_sweep_t *sweep = arg;
    const char *filename = get_fixture_file_path(sweep->fixture);
    sweep->file = asdf_open(filename, "r");

    if (!sweep->file)
        munit_errorf("failed to open fixture '%s'", sweep->fixture);

    sweep->value = asdf_get_value(sweep->file, sweep->path);

    if (!sweep->value)
        munit_errorf("failed to get value at '%s'", sweep->path);
}


static void deserialize_sweep_teardown(void *arg) {
    deserialize_sweep_t *sweep = arg;
    asdf_value_destroy(sweep->value);
    asdf_close(sweep->file);
    sweep->value = NULL;
    sweep->file = NULL;
}


static bool extension_metadata_deserialize(void *arg) {
    deserialize_sweep_t *sweep = arg;
    asdf_extension_metadata_t *metadata = NULL;
    asdf_value_err_t err = asdf_value_as_extension_metadata(sweep->value, &metadata);
    asdf_extension_metadata_destroy(metadata);
    return err == ASDF_VALUE_OK;
}


/**
 * ``asdf_extension_metadata_deserialize`` -- ``asdf_software_destroy(package)``
 * missing on ``calloc(metadata)`` failure.
 *
 * The fixture has a ``package`` field so that ``asdf_value_as_software``
 * allocates the package before the metadata struct calloc is attempted.
 * Without the fix, the iteration that fails exactly at ``calloc(metadata)``
 * while ``package != NULL`` leaks the package; LSAN catches it at subprocess
 * exit.
 */
MU_TEST(oom_extension_metadata_deserialize) {
    deserialize_sweep_t sweep = {
        .fixture = "extension-metadata-with-package.asdf",
        .path = "history/extensions/0",
    };
    malloc_fail_sweep(
        deserialize_sweep_setup, extension_metadata_deserialize, deserialize_sweep_teardown,
        &sweep, 0);
    return MUNIT_OK;
}


static bool time_deserialize(void *arg) {
    deserialize_sweep_t *sweep = arg;
    asdf_time_t *time = NULL;
    asdf_value_err_t err = asdf_value_as_time(sweep->value, &time);
    asdf_time_destroy(time);
    return err == ASDF_VALUE_OK;
}


/**
 * ``asdf_time_deserialize`` -- ``asdf_value_destroy(prop)`` missing on
 * ``calloc(time->value)`` failure when the input is a mapping.
 *
 * Uses a time value that is a flow mapping (``{format: ..., value: ...}``)
 * so that ``asdf_mapping_get(mapping, "value")`` allocates ``prop`` before
 * the ``time->value`` calloc is attempted.  Without the fix that iteration
 * leaks ``prop``; LSAN catches it.
 */
MU_TEST(oom_time_deserialize) {
    /* t_byear is a flow mapping: {format: byear, value: '...'} */
    deserialize_sweep_t sweep = {.fixture = "time.asdf", .path = "t_byear"};
    malloc_fail_sweep(
        deserialize_sweep_setup, time_deserialize, deserialize_sweep_teardown, &sweep, 0);
    return MUNIT_OK;
}


static char *oom_read_full_file_fixtures[] = {
    "datatypes.asdf",
    "ndarray-inline.asdf",
    "compressed.asdf",
    "time.asdf",
    NULL
};


static char *oom_read_full_file_modes[] = {"file", "mem", NULL};


static MunitParameterEnum oom_read_full_file_params[] = {
    {"fixture", oom_read_full_file_fixtures},
    {"mode", oom_read_full_file_modes},
    {NULL, NULL}
};


typedef struct {
    const char *fixture;
    size_t len;
    bool is_filename;
} read_full_file_sweep_t;


static bool read_full_file_sweep(void *args) {
    read_full_file_sweep_t *sweep_args = args;
    asdf_file_t *file = NULL;
    asdf_value_t *root = NULL;
    asdf_find_iter_t *iter = NULL;
    const asdf_extension_t *ext = NULL;
    void *ext_obj = NULL;
    asdf_config_t config = {.decomp = {.mode = ASDF_BLOCK_DECOMP_MODE_EAGER}};
    bool ret = false;

    if (sweep_args->is_filename)
        file = asdf_open_file_ex(sweep_args->fixture, "r", &config);
    else
        file = asdf_open_mem_ex(sweep_args->fixture, sweep_args->len, &config);

    if (!file)
        goto cleanup;

    root = asdf_get_value(file, "");

    if (!root)
        goto cleanup;

    // Passing NULL as the predicate means just iterate *all* values
    iter = asdf_find_iter_init(root, NULL);

    if (!iter)
        goto cleanup;

    while (iter && asdf_value_find_iter_next(&iter)) {
        asdf_value_t *value = iter->value;
        // Round-about way to force the value to deserialize to its detected
        // scalar type
        asdf_value_type_t type = asdf_value_get_type(value);

        if (type != ASDF_VALUE_EXTENSION) {
            if (asdf_value_as_type(value, type, NULL) != ASDF_VALUE_OK)
                goto cleanup;
        } else {
            // Should be an easier way to do this too; see gh-296
            const char *tag = asdf_value_tag(value);
            ext = asdf_extension_get(value->file, tag);

            if (!ext)
                goto cleanup;

            if (asdf_value_as_extension_type(value, ext, &ext_obj) != ASDF_VALUE_OK)
                goto cleanup;

            // I feel like there should also be an easier way to test a value
            // against a known registered extension without also looking that
            // extension up by tag name, but maybe that is the simplest way...
            const asdf_extension_t *ndarray_ext = asdf_extension_get(
                value->file, ASDF_CORE_NDARRAY_TAG);

            // Is an ndarray?
            if (ext == ndarray_ext) {
                asdf_ndarray_t *ndarray = (asdf_ndarray_t *)ext_obj;
                void *buf = NULL;
                asdf_ndarray_err_t err = asdf_ndarray_read_all(
                    ndarray, ASDF_DATATYPE_SOURCE, &buf);

                asdf_free(buf);

                if (err != ASDF_NDARRAY_OK)
                    goto cleanup;
            }

            // There should be a generic method for destroying arbitrary
            // extension objs, given that asdf_<ext>_destroy is just exactly
            // this but type safe.
            if (ext->vtab && ext->vtab->deinit)
                ext->vtab->deinit(ext_obj);

            asdf_free(ext_obj);
            ext_obj = NULL;
        }
    }

    ret = true;
cleanup:
    if (ext_obj && ext) {
        if (ext->vtab && ext->vtab->deinit)
            ext->vtab->deinit(ext_obj);

        asdf_free(ext_obj);
    }
    asdf_find_iter_destroy(iter);
    asdf_value_destroy(root);
    asdf_close(file);
    return ret;
}
/**
 * Comprehensive malloc failure test over several major code paths against
 * test fixtures chosen for the range of paths they exercise
 *
 * Opens each of the given files (either from a memory buffer or from a file in
 * order to test different I/O stream modes), walks the entire tree,
 * deserializing each tree value, and in the case of ndarrays opens and reads
 * the full ndarray data.
 */
MU_TEST(oom_read_full_file) {
    const char *fixture_filename = munit_parameters_get(params, "fixture");
    const char *mode = munit_parameters_get(params, "mode");
    const char *fixture_path = get_fixture_file_path(fixture_filename);
    bool is_file = (strcmp(mode, "file") == 0);

    read_full_file_sweep_t sweep_args = {0};
    if (is_file) {
        sweep_args.fixture = fixture_path;
        sweep_args.is_filename = true;
    } else {
        sweep_args.fixture = read_file(fixture_path, &sweep_args.len);
        sweep_args.is_filename = false;

        if (!sweep_args.fixture)
            munit_errorf("failed to read fixture file %s", fixture_path);
    }

    // 500 chosen empirically; in practice this test exercised <=273
    // allocations last checked, so 500 gives some room to grow
    malloc_fail_sweep(NULL, read_full_file_sweep, NULL, &sweep_args, 500);

    if (!is_file && sweep_args.fixture)
        free((void *)sweep_args.fixture);

    return MUNIT_OK;
}


MU_TEST_SUITE(
    malloc_fail,
    MU_RUN_TEST(oom_parse_asdf_version),
    MU_RUN_TEST(oom_parse_standard_version),
    MU_RUN_TEST(oom_parse_comment),
    MU_RUN_TEST(oom_emit_tree_start),
    MU_RUN_TEST(oom_tag_parse_name_only),
    MU_RUN_TEST(oom_tag_parse_name_strndup),
    MU_RUN_TEST(oom_tag_parse_version_strdup),
    MU_RUN_TEST(oom_extension_metadata_deserialize),
    MU_RUN_TEST(oom_time_deserialize),
    MU_RUN_TEST(oom_read_full_file, oom_read_full_file_params)
);


MU_RUN_SUITE(malloc_fail);
