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
element_nodes(void **state)
{
    const char *new_value_root;
    const char *new_value_child;    
    const char *new_value_grandchild;

    xmlNode *test_xml_root = create_xml_node(NULL, "test_xml_root");
    xmlNode *test_xml_child = create_xml_node(test_xml_root, "test_xml_child");
    xmlNode *test_xml_grandchild = create_xml_node(test_xml_child, "test_xml_grandchild");
    xmlNode *test_xml_text = pcmk_create_xml_text_node(test_xml_root, "text_xml_text", "content");
    xmlNode *test_xml_comment = string2xml("<!-- a comment -->");

    crm_xml_add(test_xml_root, "X", "5");    
    crm_xml_add(test_xml_child, "X", "X++");
    crm_xml_add(test_xml_grandchild, "X", "X+=2");
    crm_xml_add(test_xml_text, "X", "X++");

    fix_plus_plus_recursive(test_xml_root);
    fix_plus_plus_recursive(test_xml_comment);

    new_value_root = crm_element_value(test_xml_root, "X");
    new_value_child = crm_element_value(test_xml_child, "X");
    new_value_grandchild = crm_element_value(test_xml_grandchild, "X");

    assert_string_equal(new_value_root, "5");
    assert_string_equal(new_value_child, "1");
    assert_string_equal(new_value_grandchild, "2");
}

PCMK__UNIT_TEST(NULL, NULL,
                cmocka_unit_test(element_nodes))
