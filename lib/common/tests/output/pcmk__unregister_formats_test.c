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

static pcmk__output_t *
null_create_fn(char **argv) {
    return NULL;
}

static void
invalid_params(void **state) {
    /* This is basically just here to make sure that calling pcmk__unregister_formats
     * with formatters=NULL doesn't segfault.
     */
    pcmk__unregister_formats();
    assert_null(pcmk__output_formatters());
}

static void
non_null_formatters(void **state) {
    pcmk__register_format(NULL, "fake", null_create_fn, NULL);

    pcmk__unregister_formats();
    assert_null(pcmk__output_formatters());
}

PCMK__UNIT_TEST(NULL, NULL,
                cmocka_unit_test(invalid_params),
                cmocka_unit_test(non_null_formatters))
