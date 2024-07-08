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

#include <glib.h>

static void
empty_list(void **state) {
    assert_false(pcmk__list_of_multiple(NULL));
}

static void
singleton_list(void **state) {
    GList *lst = NULL;

    lst = g_list_append(lst, strdup("abc"));
    assert_false(pcmk__list_of_multiple(lst));

    g_list_free_full(lst, free);
}

static void
longer_list(void **state) {
    GList *lst = NULL;

    lst = g_list_append(lst, strdup("abc"));
    lst = g_list_append(lst, strdup("xyz"));
    assert_true(pcmk__list_of_multiple(lst));

    g_list_free_full(lst, free);
}

PCMK__UNIT_TEST(NULL, NULL,
                cmocka_unit_test(empty_list),
                cmocka_unit_test(singleton_list),
                cmocka_unit_test(longer_list))
