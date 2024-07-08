/*
 * Copyright 2021 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU General Public License version 2
 * or later (GPLv2+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>

#include <crm/common/unittest_internal.h>

#include "mock_private.h"

#include <sys/utsname.h>

static void
uname_succeeded_test(void **state)
{
    char *retval;

    // Set uname() return value and buf parameter node name
    pcmk__mock_uname = true;

    expect_any(__wrap_uname, buf);
    will_return(__wrap_uname, 0);
    will_return(__wrap_uname, "somename");

    retval = pcmk_hostname();
    assert_non_null(retval);
    assert_string_equal("somename", retval);

    free(retval);

    pcmk__mock_uname = false;
}

static void
uname_failed_test(void **state)
{
    // Set uname() return value and buf parameter node name
    pcmk__mock_uname = true;

    expect_any(__wrap_uname, buf);
    will_return(__wrap_uname, -1);
    will_return(__wrap_uname, NULL);

    assert_null(pcmk_hostname());

    pcmk__mock_uname = false;
}

PCMK__UNIT_TEST(NULL, NULL,
                cmocka_unit_test(uname_succeeded_test),
                cmocka_unit_test(uname_failed_test))
