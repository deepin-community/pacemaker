/*
 * Copyright 2021 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU General Public License version 2
 * or later (GPLv2+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>

#include <errno.h>
#include <glib.h>
#include <libxml/tree.h>

#include <crm/common/mainloop.h>
#include <crm/common/results.h>
#include <crm/common/output_internal.h>
#include <crm/pengine/internal.h>

#include <pacemaker.h>
#include <pacemaker-internal.h>

// Search path for resource operation history (takes node name and resource ID)
#define XPATH_OP_HISTORY "//" XML_CIB_TAG_STATUS                            \
                         "/" XML_CIB_TAG_STATE "[@" XML_ATTR_UNAME "='%s']" \
                         "/" XML_CIB_TAG_LRM "/" XML_LRM_TAG_RESOURCES      \
                         "/" XML_LRM_TAG_RESOURCE "[@" XML_ATTR_ID "='%s']"

static xmlNode *
best_op(pe_resource_t *rsc, pe_node_t *node, pe_working_set_t *data_set)
{
    char *xpath = NULL;
    xmlNode *history = NULL;
    xmlNode *best = NULL;

    // Find node's resource history
    xpath = crm_strdup_printf(XPATH_OP_HISTORY, node->details->uname, rsc->id);
    history = get_xpath_object(xpath, data_set->input, LOG_NEVER);
    free(xpath);

    // Examine each history entry
    for (xmlNode *lrm_rsc_op = first_named_child(history, XML_LRM_TAG_RSC_OP);
         lrm_rsc_op != NULL; lrm_rsc_op = crm_next_same_xml(lrm_rsc_op)) {

        const char *digest = crm_element_value(lrm_rsc_op,
                                               XML_LRM_ATTR_RESTART_DIGEST);
        guint interval_ms = 0;

        crm_element_value_ms(lrm_rsc_op, XML_LRM_ATTR_INTERVAL, &interval_ms);

        if (pcmk__ends_with(ID(lrm_rsc_op), "_last_failure_0")
            || (interval_ms != 0)) {

            // Only use last failure or recurring op if nothing else available
            if (best == NULL) {
                best = lrm_rsc_op;
            }
            continue;
        }

        best = lrm_rsc_op;
        if (digest != NULL) {
            // Any non-recurring action with a restart digest is sufficient
            break;
        }
    }
    return best;
}

/*!
 * \internal
 * \brief Calculate and output resource operation digests
 *
 * \param[in]  out        Output object
 * \param[in]  rsc        Resource to calculate digests for
 * \param[in]  node       Node whose operation history should be used
 * \param[in]  overrides  Hash table of configuration parameters to override
 * \param[in]  data_set   Cluster working set (with status)
 *
 * \return Standard Pacemaker return code
 */
int
pcmk__resource_digests(pcmk__output_t *out, pe_resource_t *rsc,
                       pe_node_t *node, GHashTable *overrides,
                       pe_working_set_t *data_set)
{
    const char *task = NULL;
    xmlNode *xml_op = NULL;
    op_digest_cache_t *digests = NULL;
    guint interval_ms = 0;
    int rc = pcmk_rc_ok;

    if ((out == NULL) || (rsc == NULL) || (node == NULL) || (data_set == NULL)) {
        return EINVAL;
    }
    if (rsc->variant != pe_native) {
        // Only primitives get operation digests
        return EOPNOTSUPP;
    }

    // Find XML of operation history to use
    xml_op = best_op(rsc, node, data_set);

    // Generate an operation key
    if (xml_op != NULL) {
        task = crm_element_value(xml_op, XML_LRM_ATTR_TASK);
        crm_element_value_ms(xml_op, XML_LRM_ATTR_INTERVAL_MS, &interval_ms);
    }
    if (task == NULL) { // Assume start if no history is available
        task = RSC_START;
        interval_ms = 0;
    }

    // Calculate and show digests
    digests = pe__calculate_digests(rsc, task, &interval_ms, node, xml_op,
                                    overrides, true, data_set);
    rc = out->message(out, "digests", rsc, node, task, interval_ms, digests);

    pe__free_digests(digests);
    return rc;
}

int
pcmk_resource_digests(xmlNodePtr *xml, pe_resource_t *rsc,
                      pe_node_t *node, GHashTable *overrides,
                      pe_working_set_t *data_set)
{
    pcmk__output_t *out = NULL;
    int rc = pcmk_rc_ok;

    rc = pcmk__out_prologue(&out, xml);
    if (rc != pcmk_rc_ok) {
        return rc;
    }
    pcmk__register_lib_messages(out);
    rc = pcmk__resource_digests(out, rsc, node, overrides, data_set);
    pcmk__out_epilogue(out, xml, rc);
    return rc;
}
