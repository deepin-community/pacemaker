/*
 * Copyright 2020-2023 the Pacemaker project contributors
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
basic(void **state)
{
    char *rsc = NULL;
    char *ty = NULL;
    guint ms = 0;

    assert_true(parse_op_key("Fencing_monitor_60000", &rsc, &ty, &ms));
    assert_string_equal(rsc, "Fencing");
    assert_string_equal(ty, "monitor");
    assert_int_equal(ms, 60000);
    free(rsc);
    free(ty);

    // Single-character resource name
    assert_true(parse_op_key("R_monitor_100000", &rsc, &ty, &ms));
    assert_string_equal(rsc, "R");
    assert_string_equal(ty, "monitor");
    assert_int_equal(ms, 100000);
    free(rsc);
    free(ty);

    // Single-character action name
    assert_true(parse_op_key("R_A_0", &rsc, &ty, &ms));
    assert_string_equal(rsc, "R");
    assert_string_equal(ty, "A");
    assert_int_equal(ms, 0);
    free(rsc);
    free(ty);
}

static void
rsc_just_underbars(void **state)
{
    char *rsc = NULL;
    char *ty = NULL;
    guint ms = 0;

    assert_true(parse_op_key("__monitor_1000", &rsc, &ty, &ms));
    assert_string_equal(rsc, "_");
    assert_string_equal(ty, "monitor");
    assert_int_equal(ms, 1000);
    free(rsc);
    free(ty);

    assert_true(parse_op_key("___migrate_from_0", &rsc, &ty, &ms));
    assert_string_equal(rsc, "__");
    assert_string_equal(ty, "migrate_from");
    assert_int_equal(ms, 0);
    free(rsc);
    free(ty);

    assert_true(parse_op_key("____pre_notify_stop_0", &rsc, &ty, &ms));
    assert_string_equal(rsc, "___");
    assert_string_equal(ty, "pre_notify_stop");
    assert_int_equal(ms, 0);
    free(rsc);
    free(ty);
}

static void
colon_in_rsc(void **state)
{
    char *rsc = NULL;
    char *ty = NULL;
    guint ms = 0;

    assert_true(parse_op_key("ClusterIP:0_start_0", &rsc, &ty, &ms));
    assert_string_equal(rsc, "ClusterIP:0");
    assert_string_equal(ty, "start");
    assert_int_equal(ms, 0);
    free(rsc);
    free(ty);

    assert_true(parse_op_key("imagestoreclone:1_post_notify_stop_0", &rsc, &ty, &ms));
    assert_string_equal(rsc, "imagestoreclone:1");
    assert_string_equal(ty, "post_notify_stop");
    assert_int_equal(ms, 0);
    free(rsc);
    free(ty);
}

static void
dashes_in_rsc(void **state)
{
    char *rsc = NULL;
    char *ty = NULL;
    guint ms = 0;

    assert_true(parse_op_key("httpd-bundle-0_monitor_30000", &rsc, &ty, &ms));
    assert_string_equal(rsc, "httpd-bundle-0");
    assert_string_equal(ty, "monitor");
    assert_int_equal(ms, 30000);
    free(rsc);
    free(ty);

    assert_true(parse_op_key("httpd-bundle-ip-192.168.122.132_start_0", &rsc, &ty, &ms));
    assert_string_equal(rsc, "httpd-bundle-ip-192.168.122.132");
    assert_string_equal(ty, "start");
    assert_int_equal(ms, 0);
    free(rsc);
    free(ty);
}

static void
migrate_to_from(void **state)
{
    char *rsc = NULL;
    char *ty = NULL;
    guint ms = 0;

    assert_true(parse_op_key("vm_migrate_from_0", &rsc, &ty, &ms));
    assert_string_equal(rsc, "vm");
    assert_string_equal(ty, "migrate_from");
    assert_int_equal(ms, 0);
    free(rsc);
    free(ty);

    assert_true(parse_op_key("vm_migrate_to_0", &rsc, &ty, &ms));
    assert_string_equal(rsc, "vm");
    assert_string_equal(ty, "migrate_to");
    assert_int_equal(ms, 0);
    free(rsc);
    free(ty);

    assert_true(parse_op_key("vm_idcc_devel_migrate_to_0", &rsc, &ty, &ms));
    assert_string_equal(rsc, "vm_idcc_devel");
    assert_string_equal(ty, "migrate_to");
    assert_int_equal(ms, 0);
    free(rsc);
    free(ty);
}

static void
pre_post(void **state)
{
    char *rsc = NULL;
    char *ty = NULL;
    guint ms = 0;

    assert_true(parse_op_key("rsc_drbd_7788:1_post_notify_start_0", &rsc, &ty, &ms));
    assert_string_equal(rsc, "rsc_drbd_7788:1");
    assert_string_equal(ty, "post_notify_start");
    assert_int_equal(ms, 0);
    free(rsc);
    free(ty);

    assert_true(parse_op_key("rabbitmq-bundle-clone_pre_notify_stop_0", &rsc, &ty, &ms));
    assert_string_equal(rsc, "rabbitmq-bundle-clone");
    assert_string_equal(ty, "pre_notify_stop");
    assert_int_equal(ms, 0);
    free(rsc);
    free(ty);

    assert_true(parse_op_key("post_notify_start_0", &rsc, &ty, &ms));
    assert_string_equal(rsc, "post_notify");
    assert_string_equal(ty, "start");
    assert_int_equal(ms, 0);
    free(rsc);
    free(ty);

    assert_true(parse_op_key("r_confirmed-post_notify_start_0",
                             &rsc, &ty, &ms));
    assert_string_equal(rsc, "r");
    assert_string_equal(ty, "confirmed-post_notify_start");
    assert_int_equal(ms, 0);
    free(rsc);
    free(ty);
}

static void
skip_rsc(void **state)
{
    char *ty = NULL;
    guint ms = 0;

    assert_true(parse_op_key("Fencing_monitor_60000", NULL, &ty, &ms));
    assert_string_equal(ty, "monitor");
    assert_int_equal(ms, 60000);
    free(ty);
}

static void
skip_ty(void **state)
{
    char *rsc = NULL;
    guint ms = 0;

    assert_true(parse_op_key("Fencing_monitor_60000", &rsc, NULL, &ms));
    assert_string_equal(rsc, "Fencing");
    assert_int_equal(ms, 60000);
    free(rsc);
}

static void
skip_ms(void **state)
{
    char *rsc = NULL;
    char *ty = NULL;

    assert_true(parse_op_key("Fencing_monitor_60000", &rsc, &ty, NULL));
    assert_string_equal(rsc, "Fencing");
    assert_string_equal(ty, "monitor");
    free(rsc);
    free(ty);
}

static void
empty_input(void **state)
{
    char *rsc = NULL;
    char *ty = NULL;
    guint ms = 0;

    assert_false(parse_op_key("", &rsc, &ty, &ms));
    assert_null(rsc);
    assert_null(ty);
    assert_int_equal(ms, 0);

    assert_false(parse_op_key(NULL, &rsc, &ty, &ms));
    assert_null(rsc);
    assert_null(ty);
    assert_int_equal(ms, 0);
}

static void
malformed_input(void **state)
{
    char *rsc = NULL;
    char *ty = NULL;
    guint ms = 0;

    assert_false(parse_op_key("httpd-bundle-0", &rsc, &ty, &ms));
    assert_null(rsc);
    assert_null(ty);
    assert_int_equal(ms, 0);

    assert_false(parse_op_key("httpd-bundle-0_monitor", &rsc, &ty, &ms));
    assert_null(rsc);
    assert_null(ty);
    assert_int_equal(ms, 0);

    assert_false(parse_op_key("httpd-bundle-0_30000", &rsc, &ty, &ms));
    assert_null(rsc);
    assert_null(ty);
    assert_int_equal(ms, 0);
}

PCMK__UNIT_TEST(NULL, NULL,
                cmocka_unit_test(basic),
                cmocka_unit_test(rsc_just_underbars),
                cmocka_unit_test(colon_in_rsc),
                cmocka_unit_test(dashes_in_rsc),
                cmocka_unit_test(migrate_to_from),
                cmocka_unit_test(pre_post),
                cmocka_unit_test(skip_rsc),
                cmocka_unit_test(skip_ty),
                cmocka_unit_test(skip_ms),
                cmocka_unit_test(empty_input),
                cmocka_unit_test(malformed_input))
