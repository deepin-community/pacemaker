/*
 * Copyright 2020-2021 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU General Public License version 2
 * or later (GPLv2+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>

#include <crm/common/unittest_internal.h>
#include <crm/common/acl.h>

#include "../../crmcommon_private.h"

static void
is_xml_acl_enabled_without_node(void **state)
{
    xmlNode *test_xml = create_xml_node(NULL, "test_xml");
    assert_false(xml_acl_enabled(test_xml));

    test_xml->doc->_private = NULL;
    assert_false(xml_acl_enabled(test_xml));

    test_xml->doc = NULL;
    assert_false(xml_acl_enabled(test_xml));

    test_xml = NULL;
    assert_false(xml_acl_enabled(test_xml));
}

static void
is_xml_acl_enabled_with_node(void **state)
{
    xml_doc_private_t *docpriv;
    
    xmlNode *test_xml = create_xml_node(NULL, "test_xml");

    // allocate memory for _private, which is NULL by default
    test_xml->doc->_private = calloc(1, sizeof(xml_doc_private_t));

    assert_false(xml_acl_enabled(test_xml));

    // cast _private from void* to xml_doc_private_t*
    docpriv = test_xml->doc->_private;

    // enable an irrelevant flag
    docpriv->flags |= pcmk__xf_acl_denied;

    assert_false(xml_acl_enabled(test_xml));

    // enable pcmk__xf_acl_enabled
    docpriv->flags |= pcmk__xf_acl_enabled;

    assert_true(xml_acl_enabled(test_xml));
}

PCMK__UNIT_TEST(NULL, NULL,
                cmocka_unit_test(is_xml_acl_enabled_without_node),
                cmocka_unit_test(is_xml_acl_enabled_with_node))
