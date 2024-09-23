/*
 * Copyright 2022 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU General Public License version 2
 * or later (GPLv2+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>

#include <crm/common/unittest_internal.h>
#include <crm/common/output_internal.h>

#include "mock_private.h"

static bool init_succeeds = true;

static bool
fake_text_init(pcmk__output_t *out) {
    return init_succeeds;
}

static void
fake_text_free_priv(pcmk__output_t *out) {
    /* This function intentionally left blank */
}

/* "text" is the default for pcmk__output_new. */
static pcmk__output_t *
mk_fake_text_output(char **argv) {
    pcmk__output_t *retval = calloc(1, sizeof(pcmk__output_t));

    if (retval == NULL) {
        return NULL;
    }

    retval->fmt_name = "text";
    retval->init = fake_text_init;
    retval->free_priv = fake_text_free_priv;

    retval->register_message = pcmk__register_message;
    retval->message = pcmk__call_message;

    return retval;
}

static int
setup(void **state) {
    pcmk__register_format(NULL, "text", mk_fake_text_output, NULL);
    return 0;
}

static int
teardown(void **state) {
    pcmk__unregister_formats();
    return 0;
}

static void
empty_formatters(void **state) {
    pcmk__output_t *out = NULL;

    pcmk__assert_asserts(pcmk__output_new(&out, "fake", NULL, NULL));
}

static void
invalid_params(void **state) {
    /* This must be called with the setup/teardown functions so formatters is not NULL. */
    pcmk__assert_asserts(pcmk__output_new(NULL, "fake", NULL, NULL));
}

static void
no_such_format(void **state) {
    pcmk__output_t *out = NULL;

    assert_int_equal(pcmk__output_new(&out, "fake", NULL, NULL), pcmk_rc_unknown_format);
}

static void
create_fails(void **state) {
    pcmk__output_t *out = NULL;

    pcmk__mock_calloc = true;   // calloc() will return NULL

    expect_value(__wrap_calloc, nmemb, 1);
    expect_value(__wrap_calloc, size, sizeof(pcmk__output_t));
    assert_int_equal(pcmk__output_new(&out, "text", NULL, NULL), ENOMEM);

    pcmk__mock_calloc = false;  // Use real calloc()
}

static void
fopen_fails(void **state) {
    pcmk__output_t *out = NULL;

    pcmk__mock_fopen = true;
    expect_string(__wrap_fopen, pathname, "destfile");
    expect_string(__wrap_fopen, mode, "w");
    will_return(__wrap_fopen, EPERM);

    assert_int_equal(pcmk__output_new(&out, "text", "destfile", NULL), EPERM);

    pcmk__mock_fopen = false;
}

static void
init_fails(void **state) {
    pcmk__output_t *out = NULL;

    init_succeeds = false;
    assert_int_equal(pcmk__output_new(&out, "text", NULL, NULL), ENOMEM);
    init_succeeds = true;
}

static void
everything_succeeds(void **state) {
    pcmk__output_t *out = NULL;

    assert_int_equal(pcmk__output_new(&out, "text", NULL, NULL), pcmk_rc_ok);
    assert_string_equal(out->fmt_name, "text");
    assert_ptr_equal(out->dest, stdout);
    assert_false(out->quiet);
    assert_non_null(out->messages);
    assert_string_equal(getenv("OCF_OUTPUT_FORMAT"), "text");

    pcmk__output_free(out);
}

static void
no_fmt_name_given(void **state) {
    pcmk__output_t *out = NULL;

    assert_int_equal(pcmk__output_new(&out, NULL, NULL, NULL), pcmk_rc_ok);
    assert_string_equal(out->fmt_name, "text");

    pcmk__output_free(out);
}

PCMK__UNIT_TEST(NULL, NULL,
                cmocka_unit_test(empty_formatters),
                cmocka_unit_test_setup_teardown(invalid_params, setup, teardown),
                cmocka_unit_test_setup_teardown(no_such_format, setup, teardown),
                cmocka_unit_test_setup_teardown(create_fails, setup, teardown),
                cmocka_unit_test_setup_teardown(init_fails, setup, teardown),
                cmocka_unit_test_setup_teardown(fopen_fails, setup, teardown),
                cmocka_unit_test_setup_teardown(everything_succeeds, setup, teardown),
                cmocka_unit_test_setup_teardown(no_fmt_name_given, setup, teardown))
