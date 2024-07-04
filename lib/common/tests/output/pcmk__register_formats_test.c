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

static pcmk__output_t *
null_create_fn_2(char **argv) {
    return NULL;
}

static void
no_formats(void **state) {
    pcmk__register_formats(NULL, NULL);
    assert_null(pcmk__output_formatters());
}

static void
invalid_entries(void **state) {
    /* Here, we can only test that an empty name won't be added.  A NULL name is
     * the marker for the end of the format table.
     */
    pcmk__supported_format_t formats[] = {
        { "", null_create_fn, NULL },
        { NULL },
    };

    pcmk__assert_asserts(pcmk__register_formats(NULL, formats));
}

static void
valid_entries(void **state) {
    GHashTable *formatters = NULL;

    pcmk__supported_format_t formats[] = {
        { "fmt1", null_create_fn, NULL },
        { "fmt2", null_create_fn_2, NULL },
        { NULL },
    };

    pcmk__register_formats(NULL, formats);

    formatters = pcmk__output_formatters();
    assert_int_equal(g_hash_table_size(formatters), 2);
    assert_ptr_equal(g_hash_table_lookup(formatters, "fmt1"), null_create_fn);
    assert_ptr_equal(g_hash_table_lookup(formatters, "fmt2"), null_create_fn_2);

    pcmk__unregister_formats();
}

static void
duplicate_keys(void **state) {
    GHashTable *formatters = NULL;

    pcmk__supported_format_t formats[] = {
        { "fmt1", null_create_fn, NULL },
        { "fmt1", null_create_fn_2, NULL },
        { NULL },
    };

    pcmk__register_formats(NULL, formats);

    formatters = pcmk__output_formatters();
    assert_int_equal(g_hash_table_size(formatters), 1);
    assert_ptr_equal(g_hash_table_lookup(formatters, "fmt1"), null_create_fn_2);

    pcmk__unregister_formats();
}

static void
duplicate_values(void **state) {
    GHashTable *formatters = NULL;

    pcmk__supported_format_t formats[] = {
        { "fmt1", null_create_fn, NULL },
        { "fmt2", null_create_fn, NULL },
        { NULL },
    };

    pcmk__register_formats(NULL, formats);

    formatters = pcmk__output_formatters();
    assert_int_equal(g_hash_table_size(formatters), 2);
    assert_ptr_equal(g_hash_table_lookup(formatters, "fmt1"), null_create_fn);
    assert_ptr_equal(g_hash_table_lookup(formatters, "fmt2"), null_create_fn);

    pcmk__unregister_formats();
}

PCMK__UNIT_TEST(NULL, NULL,
                cmocka_unit_test(no_formats),
                cmocka_unit_test(invalid_entries),
                cmocka_unit_test(valid_entries),
                cmocka_unit_test(duplicate_keys),
                cmocka_unit_test(duplicate_values))
