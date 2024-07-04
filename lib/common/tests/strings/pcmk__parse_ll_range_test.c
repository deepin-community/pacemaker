/*
 * Copyright 2020-2023 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU General Public License version 2
 * or later (GPLv2+) WITHOUT ANY WARRANTY.
 */

#include "crm/common/results.h"
#include <crm_internal.h>

#include <crm/common/unittest_internal.h>

static void
empty_input_string(void **state)
{
    long long start, end;

    assert_int_equal(pcmk__parse_ll_range(NULL, &start, &end), ENODATA);
    assert_int_equal(pcmk__parse_ll_range("", &start, &end), ENODATA);
}

static void
null_input_variables(void **state)
{
    long long start, end;

    pcmk__assert_asserts(pcmk__parse_ll_range("1234", NULL, &end));
    pcmk__assert_asserts(pcmk__parse_ll_range("1234", &start, NULL));
}

static void
missing_separator(void **state)
{
    long long start, end;

    assert_int_equal(pcmk__parse_ll_range("1234", &start, &end), pcmk_rc_ok);
    assert_int_equal(start, 1234);
    assert_int_equal(end, 1234);
}

static void
only_separator(void **state)
{
    long long start, end;

    assert_int_equal(pcmk__parse_ll_range("-", &start, &end), pcmk_rc_bad_input);
    assert_int_equal(start, PCMK__PARSE_INT_DEFAULT);
    assert_int_equal(end, PCMK__PARSE_INT_DEFAULT);
}

static void
no_range_end(void **state)
{
    long long start, end;

    assert_int_equal(pcmk__parse_ll_range("2000-", &start, &end), pcmk_rc_ok);
    assert_int_equal(start, 2000);
    assert_int_equal(end, PCMK__PARSE_INT_DEFAULT);
}

static void
no_range_start(void **state)
{
    long long start, end;

    assert_int_equal(pcmk__parse_ll_range("-2020", &start, &end), pcmk_rc_ok);
    assert_int_equal(start, PCMK__PARSE_INT_DEFAULT);
    assert_int_equal(end, 2020);
}

static void
range_start_and_end(void **state)
{
    long long start, end;

    assert_int_equal(pcmk__parse_ll_range("2000-2020", &start, &end), pcmk_rc_ok);
    assert_int_equal(start, 2000);
    assert_int_equal(end, 2020);

    assert_int_equal(pcmk__parse_ll_range("2000-2020-2030", &start, &end), pcmk_rc_bad_input);
}

static void
garbage(void **state)
{
    long long start, end;

    assert_int_equal(pcmk__parse_ll_range("2000x-", &start, &end), pcmk_rc_bad_input);
    assert_int_equal(start, PCMK__PARSE_INT_DEFAULT);
    assert_int_equal(end, PCMK__PARSE_INT_DEFAULT);

    assert_int_equal(pcmk__parse_ll_range("-x2000", &start, &end), pcmk_rc_bad_input);
    assert_int_equal(start, PCMK__PARSE_INT_DEFAULT);
    assert_int_equal(end, PCMK__PARSE_INT_DEFAULT);
}

static void
strtoll_errors(void **state)
{
    long long start, end;

    assert_int_equal(pcmk__parse_ll_range("20000000000000000000-", &start, &end), EOVERFLOW);
    assert_int_equal(pcmk__parse_ll_range("100-20000000000000000000", &start, &end), EOVERFLOW);
}

PCMK__UNIT_TEST(NULL, NULL,
                cmocka_unit_test(empty_input_string),
                cmocka_unit_test(null_input_variables),
                cmocka_unit_test(missing_separator),
                cmocka_unit_test(only_separator),
                cmocka_unit_test(no_range_end),
                cmocka_unit_test(no_range_start),
                cmocka_unit_test(range_start_and_end),
                cmocka_unit_test(strtoll_errors),
                cmocka_unit_test(garbage))
