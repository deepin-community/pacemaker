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

static int
null_message_fn(pcmk__output_t *out, va_list args) {
    return pcmk_rc_ok;
}

static bool
fake_text_init(pcmk__output_t *out) {
    return true;
}

static void
fake_text_free_priv(pcmk__output_t *out) {
    function_called();
    /* This function intentionally left blank */
}

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
no_messages(void **state) {
    pcmk__output_t *out = NULL;

    pcmk__output_new(&out, "text", NULL, NULL);

    expect_function_call(fake_text_free_priv);
    pcmk__output_free(out);
}

static void
messages(void **state) {
    pcmk__output_t *out = NULL;

    pcmk__output_new(&out, "text", NULL, NULL);
    pcmk__register_message(out, "fake", null_message_fn);

    expect_function_call(fake_text_free_priv);
    pcmk__output_free(out);
}

PCMK__UNIT_TEST(NULL, NULL,
                cmocka_unit_test_setup_teardown(no_messages, setup, teardown),
                cmocka_unit_test_setup_teardown(messages, setup, teardown))
