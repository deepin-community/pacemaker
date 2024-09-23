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
#include <crm/pengine/internal.h>

#include "mock_private.h"

static void
calloc_fails(void **state) {
    pcmk__mock_calloc = true;   // calloc() will return NULL

    expect_value(__wrap_calloc, nmemb, 1);
    expect_value(__wrap_calloc, size, sizeof(pe_working_set_t));
    assert_null(pe_new_working_set());

    pcmk__mock_calloc = false;  // Use real calloc()
}

static void
calloc_succeeds(void **state) {
    pe_working_set_t *data_set = pe_new_working_set();

    /* Nothing else to test about this function, as all it does is call
     * set_working_set_defaults which is also a public function and should
     * get its own unit test.
     */
    assert_non_null(data_set);

    /* Avoid calling pe_free_working_set here so we don't artificially
     * inflate the coverage numbers.
     */
    free(data_set);
}

PCMK__UNIT_TEST(NULL, NULL,
                cmocka_unit_test(calloc_fails),
                cmocka_unit_test(calloc_succeeds))
