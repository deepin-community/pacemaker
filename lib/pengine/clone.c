/*
 * Copyright 2004-2021 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU Lesser General Public License
 * version 2.1 or later (LGPLv2.1+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>

#include <crm/pengine/rules.h>
#include <crm/pengine/status.h>
#include <crm/pengine/internal.h>
#include <pe_status_private.h>
#include <crm/msg_xml.h>
#include <crm/common/output.h>
#include <crm/common/xml_internal.h>

#define VARIANT_CLONE 1
#include "./variant.h"

#ifdef PCMK__COMPAT_2_0
#define PROMOTED_INSTANCES   RSC_ROLE_PROMOTED_LEGACY_S "s"
#define UNPROMOTED_INSTANCES RSC_ROLE_UNPROMOTED_LEGACY_S "s"
#else
#define PROMOTED_INSTANCES   RSC_ROLE_PROMOTED_S
#define UNPROMOTED_INSTANCES RSC_ROLE_UNPROMOTED_S
#endif

static void
clone_header(pcmk__output_t *out, int *rc, pe_resource_t *rsc, clone_variant_data_t *clone_data)
{
    char *attrs = NULL;
    size_t len = 0;

    if (pcmk_is_set(rsc->flags, pe_rsc_promotable)) {
        pcmk__add_separated_word(&attrs, &len, "promotable", ", ");
    }

    if (pcmk_is_set(rsc->flags, pe_rsc_unique)) {
        pcmk__add_separated_word(&attrs, &len, "unique", ", ");
    }

    if (!pcmk_is_set(rsc->flags, pe_rsc_managed)) {
        pcmk__add_separated_word(&attrs, &len, "unmanaged", ", ");
    }

    if (pe__resource_is_disabled(rsc)) {
        pcmk__add_separated_word(&attrs, &len, "disabled", ", ");
    }

    if (attrs) {
        PCMK__OUTPUT_LIST_HEADER(out, FALSE, *rc, "Clone Set: %s [%s] (%s)",
                             rsc->id, ID(clone_data->xml_obj_child),
                                 attrs);
        free(attrs);
    } else {
        PCMK__OUTPUT_LIST_HEADER(out, FALSE, *rc, "Clone Set: %s [%s]",
                                 rsc->id, ID(clone_data->xml_obj_child))
    }
}

void
pe__force_anon(const char *standard, pe_resource_t *rsc, const char *rid,
               pe_working_set_t *data_set)
{
    if (pe_rsc_is_clone(rsc)) {
        clone_variant_data_t *clone_data = NULL;

        get_clone_variant_data(clone_data, rsc);

        pe_warn("Ignoring " XML_RSC_ATTR_UNIQUE " for %s because %s resources "
                "such as %s can be used only as anonymous clones",
                rsc->id, standard, rid);

        clone_data->clone_node_max = 1;
        clone_data->clone_max = QB_MIN(clone_data->clone_max,
                                       g_list_length(data_set->nodes));
    }
}

pe_resource_t *
find_clone_instance(pe_resource_t * rsc, const char *sub_id, pe_working_set_t * data_set)
{
    char *child_id = NULL;
    pe_resource_t *child = NULL;
    const char *child_base = NULL;
    clone_variant_data_t *clone_data = NULL;

    get_clone_variant_data(clone_data, rsc);

    child_base = ID(clone_data->xml_obj_child);
    child_id = crm_strdup_printf("%s:%s", child_base, sub_id);
    child = pe_find_resource(rsc->children, child_id);

    free(child_id);
    return child;
}

pe_resource_t *
pe__create_clone_child(pe_resource_t *rsc, pe_working_set_t *data_set)
{
    gboolean as_orphan = FALSE;
    char *inc_num = NULL;
    char *inc_max = NULL;
    pe_resource_t *child_rsc = NULL;
    xmlNode *child_copy = NULL;
    clone_variant_data_t *clone_data = NULL;

    get_clone_variant_data(clone_data, rsc);

    CRM_CHECK(clone_data->xml_obj_child != NULL, return FALSE);

    if (clone_data->total_clones >= clone_data->clone_max) {
        // If we've already used all available instances, this is an orphan
        as_orphan = TRUE;
    }

    // Allocate instance numbers in numerical order (starting at 0)
    inc_num = pcmk__itoa(clone_data->total_clones);
    inc_max = pcmk__itoa(clone_data->clone_max);

    child_copy = copy_xml(clone_data->xml_obj_child);

    crm_xml_add(child_copy, XML_RSC_ATTR_INCARNATION, inc_num);

    if (common_unpack(child_copy, &child_rsc, rsc, data_set) == FALSE) {
        pe_err("Failed unpacking resource %s", crm_element_value(child_copy, XML_ATTR_ID));
        child_rsc = NULL;
        goto bail;
    }
/*  child_rsc->globally_unique = rsc->globally_unique; */

    CRM_ASSERT(child_rsc);
    clone_data->total_clones += 1;
    pe_rsc_trace(child_rsc, "Setting clone attributes for: %s", child_rsc->id);
    rsc->children = g_list_append(rsc->children, child_rsc);
    if (as_orphan) {
        pe__set_resource_flags_recursive(child_rsc, pe_rsc_orphan);
    }

    add_hash_param(child_rsc->meta, XML_RSC_ATTR_INCARNATION_MAX, inc_max);
    pe_rsc_trace(rsc, "Added %s instance %s", rsc->id, child_rsc->id);

  bail:
    free(inc_num);
    free(inc_max);

    return child_rsc;
}

gboolean
clone_unpack(pe_resource_t * rsc, pe_working_set_t * data_set)
{
    int lpc = 0;
    xmlNode *a_child = NULL;
    xmlNode *xml_obj = rsc->xml;
    clone_variant_data_t *clone_data = NULL;

    const char *ordered = g_hash_table_lookup(rsc->meta, XML_RSC_ATTR_ORDERED);
    const char *max_clones = g_hash_table_lookup(rsc->meta, XML_RSC_ATTR_INCARNATION_MAX);
    const char *max_clones_node = g_hash_table_lookup(rsc->meta, XML_RSC_ATTR_INCARNATION_NODEMAX);

    pe_rsc_trace(rsc, "Processing resource %s...", rsc->id);

    clone_data = calloc(1, sizeof(clone_variant_data_t));
    rsc->variant_opaque = clone_data;

    if (pcmk_is_set(rsc->flags, pe_rsc_promotable)) {
        const char *promoted_max = NULL;
        const char *promoted_node_max = NULL;

        promoted_max = g_hash_table_lookup(rsc->meta,
                                           XML_RSC_ATTR_PROMOTED_MAX);
        if (promoted_max == NULL) {
            // @COMPAT deprecated since 2.0.0
            promoted_max = g_hash_table_lookup(rsc->meta,
                                               PCMK_XE_PROMOTED_MAX_LEGACY);
        }

        promoted_node_max = g_hash_table_lookup(rsc->meta,
                                                XML_RSC_ATTR_PROMOTED_NODEMAX);
        if (promoted_node_max == NULL) {
            // @COMPAT deprecated since 2.0.0
            promoted_node_max = g_hash_table_lookup(rsc->meta,
                                                    PCMK_XE_PROMOTED_NODE_MAX_LEGACY);
        }

        // Use 1 as default but 0 for minimum and invalid
        if (promoted_max == NULL) {
            clone_data->promoted_max = 1;
        } else {
            pcmk__scan_min_int(promoted_max, &(clone_data->promoted_max), 0);
        }

        // Use 1 as default but 0 for minimum and invalid
        if (promoted_node_max == NULL) {
            clone_data->promoted_node_max = 1;
        } else {
            pcmk__scan_min_int(promoted_node_max,
                               &(clone_data->promoted_node_max), 0);
        }
    }

    // Implied by calloc()
    /* clone_data->xml_obj_child = NULL; */

    // Use 1 as default but 0 for minimum and invalid
    if (max_clones_node == NULL) {
        clone_data->clone_node_max = 1;
    } else {
        pcmk__scan_min_int(max_clones_node, &(clone_data->clone_node_max), 0);
    }

    /* Use number of nodes (but always at least 1, which is handy for crm_verify
     * for a CIB without nodes) as default, but 0 for minimum and invalid
     */
    if (max_clones == NULL) {
        clone_data->clone_max = QB_MAX(1, g_list_length(data_set->nodes));
    } else {
        pcmk__scan_min_int(max_clones, &(clone_data->clone_max), 0);
    }

    clone_data->ordered = crm_is_true(ordered);

    if ((rsc->flags & pe_rsc_unique) == 0 && clone_data->clone_node_max > 1) {
        pcmk__config_err("Ignoring " XML_RSC_ATTR_PROMOTED_MAX " for %s "
                         "because anonymous clones support only one instance "
                         "per node", rsc->id);
        clone_data->clone_node_max = 1;
    }

    pe_rsc_trace(rsc, "Options for %s", rsc->id);
    pe_rsc_trace(rsc, "\tClone max: %d", clone_data->clone_max);
    pe_rsc_trace(rsc, "\tClone node max: %d", clone_data->clone_node_max);
    pe_rsc_trace(rsc, "\tClone is unique: %s",
                 pe__rsc_bool_str(rsc, pe_rsc_unique));
    pe_rsc_trace(rsc, "\tClone is promotable: %s",
                 pe__rsc_bool_str(rsc, pe_rsc_promotable));

    // Clones may contain a single group or primitive
    for (a_child = pcmk__xe_first_child(xml_obj); a_child != NULL;
         a_child = pcmk__xe_next(a_child)) {

        if (pcmk__str_any_of((const char *)a_child->name, XML_CIB_TAG_RESOURCE, XML_CIB_TAG_GROUP, NULL)) {
            clone_data->xml_obj_child = a_child;
            break;
        }
    }

    if (clone_data->xml_obj_child == NULL) {
        pcmk__config_err("%s has nothing to clone", rsc->id);
        return FALSE;
    }

    /*
     * Make clones ever so slightly sticky by default
     *
     * This helps ensure clone instances are not shuffled around the cluster
     * for no benefit in situations when pre-allocation is not appropriate
     */
    if (g_hash_table_lookup(rsc->meta, XML_RSC_ATTR_STICKINESS) == NULL) {
        add_hash_param(rsc->meta, XML_RSC_ATTR_STICKINESS, "1");
    }

    /* This ensures that the globally-unique value always exists for children to
     * inherit when being unpacked, as well as in resource agents' environment.
     */
    add_hash_param(rsc->meta, XML_RSC_ATTR_UNIQUE,
                   pe__rsc_bool_str(rsc, pe_rsc_unique));

    if (clone_data->clone_max <= 0) {
        /* Create one child instance so that unpack_find_resource() will hook up
         * any orphans up to the parent correctly.
         */
        if (pe__create_clone_child(rsc, data_set) == NULL) {
            return FALSE;
        }

    } else {
        // Create a child instance for each available instance number
        for (lpc = 0; lpc < clone_data->clone_max; lpc++) {
            if (pe__create_clone_child(rsc, data_set) == NULL) {
                return FALSE;
            }
        }
    }

    pe_rsc_trace(rsc, "Added %d children to resource %s...", clone_data->clone_max, rsc->id);
    return TRUE;
}

gboolean
clone_active(pe_resource_t * rsc, gboolean all)
{
    GList *gIter = rsc->children;

    for (; gIter != NULL; gIter = gIter->next) {
        pe_resource_t *child_rsc = (pe_resource_t *) gIter->data;
        gboolean child_active = child_rsc->fns->active(child_rsc, all);

        if (all == FALSE && child_active) {
            return TRUE;
        } else if (all && child_active == FALSE) {
            return FALSE;
        }
    }

    if (all) {
        return TRUE;
    } else {
        return FALSE;
    }
}

static void
short_print(char *list, const char *prefix, const char *type, const char *suffix, long options, void *print_data)
{
    if(suffix == NULL) {
        suffix = "";
    }

    if (list) {
        if (options & pe_print_html) {
            status_print("<li>");
        }
        status_print("%s%s: [ %s ]%s", prefix, type, list, suffix);

        if (options & pe_print_html) {
            status_print("</li>\n");

        } else if (options & pe_print_suppres_nl) {
            /* nothing */
        } else if ((options & pe_print_printf) || (options & pe_print_ncurses)) {
            status_print("\n");
        }

    }
}

static const char *
configured_role_str(pe_resource_t * rsc)
{
    const char *target_role = g_hash_table_lookup(rsc->meta,
                                                  XML_RSC_ATTR_TARGET_ROLE);

    if ((target_role == NULL) && rsc->children && rsc->children->data) {
        target_role = g_hash_table_lookup(((pe_resource_t*)rsc->children->data)->meta,
                                          XML_RSC_ATTR_TARGET_ROLE);
    }
    return target_role;
}

static enum rsc_role_e
configured_role(pe_resource_t * rsc)
{
    const char *target_role = configured_role_str(rsc);

    if (target_role) {
        return text2role(target_role);
    }
    return RSC_ROLE_UNKNOWN;
}

static void
clone_print_xml(pe_resource_t * rsc, const char *pre_text, long options, void *print_data)
{
    char *child_text = crm_strdup_printf("%s    ", pre_text);
    const char *target_role = configured_role_str(rsc);
    GList *gIter = rsc->children;

    status_print("%s<clone ", pre_text);
    status_print("id=\"%s\" ", rsc->id);
    status_print("multi_state=\"%s\" ",
                 pe__rsc_bool_str(rsc, pe_rsc_promotable));
    status_print("unique=\"%s\" ", pe__rsc_bool_str(rsc, pe_rsc_unique));
    status_print("managed=\"%s\" ", pe__rsc_bool_str(rsc, pe_rsc_managed));
    status_print("failed=\"%s\" ", pe__rsc_bool_str(rsc, pe_rsc_failed));
    status_print("failure_ignored=\"%s\" ",
                 pe__rsc_bool_str(rsc, pe_rsc_failure_ignored));
    if (target_role) {
        status_print("target_role=\"%s\" ", target_role);
    }
    status_print(">\n");

    for (; gIter != NULL; gIter = gIter->next) {
        pe_resource_t *child_rsc = (pe_resource_t *) gIter->data;

        child_rsc->fns->print(child_rsc, child_text, options, print_data);
    }

    status_print("%s</clone>\n", pre_text);
    free(child_text);
}

bool is_set_recursive(pe_resource_t * rsc, long long flag, bool any)
{
    GList *gIter;
    bool all = !any;

    if (pcmk_is_set(rsc->flags, flag)) {
        if(any) {
            return TRUE;
        }
    } else if(all) {
        return FALSE;
    }

    for (gIter = rsc->children; gIter != NULL; gIter = gIter->next) {
        if(is_set_recursive(gIter->data, flag, any)) {
            if(any) {
                return TRUE;
            }

        } else if(all) {
            return FALSE;
        }
    }

    if(all) {
        return TRUE;
    }
    return FALSE;
}

void
clone_print(pe_resource_t * rsc, const char *pre_text, long options, void *print_data)
{
    char *list_text = NULL;
    char *child_text = NULL;
    char *stopped_list = NULL;
    size_t list_text_len = 0;
    size_t stopped_list_len = 0;

    GList *promoted_list = NULL;
    GList *started_list = NULL;
    GList *gIter = rsc->children;

    clone_variant_data_t *clone_data = NULL;
    int active_instances = 0;

    if (pre_text == NULL) {
        pre_text = " ";
    }

    if (options & pe_print_xml) {
        clone_print_xml(rsc, pre_text, options, print_data);
        return;
    }

    get_clone_variant_data(clone_data, rsc);

    child_text = crm_strdup_printf("%s    ", pre_text);

    status_print("%sClone Set: %s [%s]%s%s%s",
                 pre_text ? pre_text : "", rsc->id, ID(clone_data->xml_obj_child),
                 pcmk_is_set(rsc->flags, pe_rsc_promotable)? " (promotable)" : "",
                 pcmk_is_set(rsc->flags, pe_rsc_unique)? " (unique)" : "",
                 pcmk_is_set(rsc->flags, pe_rsc_managed)? "" : " (unmanaged)");

    if (options & pe_print_html) {
        status_print("\n<ul>\n");

    } else if ((options & pe_print_log) == 0) {
        status_print("\n");
    }

    for (; gIter != NULL; gIter = gIter->next) {
        gboolean print_full = FALSE;
        pe_resource_t *child_rsc = (pe_resource_t *) gIter->data;
        gboolean partially_active = child_rsc->fns->active(child_rsc, FALSE);

        if (options & pe_print_clone_details) {
            print_full = TRUE;
        }

        if (pcmk_is_set(rsc->flags, pe_rsc_unique)) {
            // Print individual instance when unique (except stopped orphans)
            if (partially_active || !pcmk_is_set(rsc->flags, pe_rsc_orphan)) {
                print_full = TRUE;
            }

        // Everything else in this block is for anonymous clones

        } else if (pcmk_is_set(options, pe_print_pending)
                   && (child_rsc->pending_task != NULL)
                   && strcmp(child_rsc->pending_task, "probe")) {
            // Print individual instance when non-probe action is pending
            print_full = TRUE;

        } else if (partially_active == FALSE) {
            // List stopped instances when requested (except orphans)
            if (!pcmk_is_set(child_rsc->flags, pe_rsc_orphan)
                && !pcmk_is_set(options, pe_print_clone_active)) {
                pcmk__add_word(&stopped_list, &stopped_list_len, child_rsc->id);
            }

        } else if (is_set_recursive(child_rsc, pe_rsc_orphan, TRUE)
                   || is_set_recursive(child_rsc, pe_rsc_managed, FALSE) == FALSE
                   || is_set_recursive(child_rsc, pe_rsc_failed, TRUE)) {

            // Print individual instance when active orphaned/unmanaged/failed
            print_full = TRUE;

        } else if (child_rsc->fns->active(child_rsc, TRUE)) {
            // Instance of fully active anonymous clone

            pe_node_t *location = child_rsc->fns->location(child_rsc, NULL, TRUE);

            if (location) {
                // Instance is active on a single node

                enum rsc_role_e a_role = child_rsc->fns->state(child_rsc, TRUE);

                if (location->details->online == FALSE && location->details->unclean) {
                    print_full = TRUE;

                } else if (a_role > RSC_ROLE_UNPROMOTED) {
                    promoted_list = g_list_append(promoted_list, location);

                } else {
                    started_list = g_list_append(started_list, location);
                }

            } else {
                /* uncolocated group - bleh */
                print_full = TRUE;
            }

        } else {
            // Instance of partially active anonymous clone
            print_full = TRUE;
        }

        if (print_full) {
            if (options & pe_print_html) {
                status_print("<li>\n");
            }
            child_rsc->fns->print(child_rsc, child_text, options, print_data);
            if (options & pe_print_html) {
                status_print("</li>\n");
            }
        }
    }

    /* Promoted */
    promoted_list = g_list_sort(promoted_list, sort_node_uname);
    for (gIter = promoted_list; gIter; gIter = gIter->next) {
        pe_node_t *host = gIter->data;

        pcmk__add_word(&list_text, &list_text_len, host->details->uname);
	active_instances++;
    }

    short_print(list_text, child_text, PROMOTED_INSTANCES, NULL, options,
                print_data);
    g_list_free(promoted_list);
    free(list_text);
    list_text = NULL;
    list_text_len = 0;

    /* Started/Unpromoted */
    started_list = g_list_sort(started_list, sort_node_uname);
    for (gIter = started_list; gIter; gIter = gIter->next) {
        pe_node_t *host = gIter->data;

        pcmk__add_word(&list_text, &list_text_len, host->details->uname);
        active_instances++;
    }

    if (pcmk_is_set(rsc->flags, pe_rsc_promotable)) {
        enum rsc_role_e role = configured_role(rsc);

        if (role == RSC_ROLE_UNPROMOTED) {
            short_print(list_text, child_text,
                        UNPROMOTED_INSTANCES " (target-role)", NULL, options,
                        print_data);
        } else {
            short_print(list_text, child_text, UNPROMOTED_INSTANCES, NULL,
                        options, print_data);
        }

    } else {
        short_print(list_text, child_text, "Started", NULL, options, print_data);
    }

    g_list_free(started_list);
    free(list_text);
    list_text = NULL;
    list_text_len = 0;

    if (!pcmk_is_set(options, pe_print_clone_active)) {
        const char *state = "Stopped";
        enum rsc_role_e role = configured_role(rsc);

        if (role == RSC_ROLE_STOPPED) {
            state = "Stopped (disabled)";
        }

        if (!pcmk_is_set(rsc->flags, pe_rsc_unique)
            && (clone_data->clone_max > active_instances)) {

            GList *nIter;
            GList *list = g_hash_table_get_values(rsc->allowed_nodes);

            /* Custom stopped list for non-unique clones */
            free(stopped_list);
            stopped_list = NULL;
            stopped_list_len = 0;

            if (list == NULL) {
                /* Clusters with symmetrical=false haven't calculated allowed_nodes yet
                 * If we've not probed for them yet, the Stopped list will be empty
                 */
                list = g_hash_table_get_values(rsc->known_on);
            }

            list = g_list_sort(list, sort_node_uname);
            for (nIter = list; nIter != NULL; nIter = nIter->next) {
                pe_node_t *node = (pe_node_t *)nIter->data;

                if (pe_find_node(rsc->running_on, node->details->uname) == NULL) {
                    pcmk__add_word(&stopped_list, &stopped_list_len,
                                   node->details->uname);
                }
            }
            g_list_free(list);
        }

        short_print(stopped_list, child_text, state, NULL, options, print_data);
        free(stopped_list);
    }

    if (options & pe_print_html) {
        status_print("</ul>\n");
    }

    free(child_text);
}

PCMK__OUTPUT_ARGS("clone", "unsigned int", "pe_resource_t *", "GList *", "GList *")
int
pe__clone_xml(pcmk__output_t *out, va_list args)
{
    unsigned int show_opts = va_arg(args, unsigned int);
    pe_resource_t *rsc = va_arg(args, pe_resource_t *);
    GList *only_node = va_arg(args, GList *);
    GList *only_rsc = va_arg(args, GList *);

    GList *gIter = rsc->children;
    GList *all = NULL;
    int rc = pcmk_rc_no_output;
    gboolean printed_header = FALSE;
    gboolean print_everything = TRUE;

    if (rsc->fns->is_filtered(rsc, only_rsc, TRUE)) {
        return rc;
    }

    print_everything = pcmk__str_in_list(only_rsc, rsc_printable_id(rsc), pcmk__str_none) ||
                       (strstr(rsc->id, ":") != NULL && pcmk__str_in_list(only_rsc, rsc->id, pcmk__str_none));

    all = g_list_prepend(all, (gpointer) "*");

    for (; gIter != NULL; gIter = gIter->next) {
        pe_resource_t *child_rsc = (pe_resource_t *) gIter->data;

        if (pcmk__rsc_filtered_by_node(child_rsc, only_node)) {
            continue;
        }

        if (child_rsc->fns->is_filtered(child_rsc, only_rsc, print_everything)) {
            continue;
        }

        if (!printed_header) {
            printed_header = TRUE;

            rc = pe__name_and_nvpairs_xml(out, true, "clone", 8,
                    "id", rsc->id,
                    "multi_state", pe__rsc_bool_str(rsc, pe_rsc_promotable),
                    "unique", pe__rsc_bool_str(rsc, pe_rsc_unique),
                    "managed", pe__rsc_bool_str(rsc, pe_rsc_managed),
                    "disabled", pcmk__btoa(pe__resource_is_disabled(rsc)),
                    "failed", pe__rsc_bool_str(rsc, pe_rsc_failed),
                    "failure_ignored", pe__rsc_bool_str(rsc, pe_rsc_failure_ignored),
                    "target_role", configured_role_str(rsc));
            CRM_ASSERT(rc == pcmk_rc_ok);
        }

        out->message(out, crm_map_element_name(child_rsc->xml), show_opts,
                     child_rsc, only_node, all);
    }

    if (printed_header) {
        pcmk__output_xml_pop_parent(out);
    }

    g_list_free(all);
    return rc;
}

PCMK__OUTPUT_ARGS("clone", "unsigned int", "pe_resource_t *", "GList *", "GList *")
int
pe__clone_default(pcmk__output_t *out, va_list args)
{
    unsigned int show_opts = va_arg(args, unsigned int);
    pe_resource_t *rsc = va_arg(args, pe_resource_t *);
    GList *only_node = va_arg(args, GList *);
    GList *only_rsc = va_arg(args, GList *);

    char *list_text = NULL;
    char *stopped_list = NULL;
    size_t list_text_len = 0;
    size_t stopped_list_len = 0;

    GList *promoted_list = NULL;
    GList *started_list = NULL;
    GList *gIter = rsc->children;

    clone_variant_data_t *clone_data = NULL;
    int active_instances = 0;
    int rc = pcmk_rc_no_output;
    gboolean print_everything = TRUE;

    get_clone_variant_data(clone_data, rsc);

    if (rsc->fns->is_filtered(rsc, only_rsc, TRUE)) {
        return rc;
    }

    print_everything = pcmk__str_in_list(only_rsc, rsc_printable_id(rsc), pcmk__str_none) ||
                       (strstr(rsc->id, ":") != NULL && pcmk__str_in_list(only_rsc, rsc->id, pcmk__str_none));

    for (; gIter != NULL; gIter = gIter->next) {
        gboolean print_full = FALSE;
        pe_resource_t *child_rsc = (pe_resource_t *) gIter->data;
        gboolean partially_active = child_rsc->fns->active(child_rsc, FALSE);

        if (pcmk__rsc_filtered_by_node(child_rsc, only_node)) {
            continue;
        }

        if (child_rsc->fns->is_filtered(child_rsc, only_rsc, print_everything)) {
            continue;
        }

        if (pcmk_is_set(show_opts, pcmk_show_clone_detail)) {
            print_full = TRUE;
        }

        if (pcmk_is_set(rsc->flags, pe_rsc_unique)) {
            // Print individual instance when unique (except stopped orphans)
            if (partially_active || !pcmk_is_set(rsc->flags, pe_rsc_orphan)) {
                print_full = TRUE;
            }

        // Everything else in this block is for anonymous clones

        } else if (pcmk_is_set(show_opts, pcmk_show_pending)
                   && (child_rsc->pending_task != NULL)
                   && strcmp(child_rsc->pending_task, "probe")) {
            // Print individual instance when non-probe action is pending
            print_full = TRUE;

        } else if (partially_active == FALSE) {
            // List stopped instances when requested (except orphans)
            if (!pcmk_is_set(child_rsc->flags, pe_rsc_orphan)
                && pcmk_is_set(show_opts, pcmk_show_inactive_rscs)) {
                pcmk__add_word(&stopped_list, &stopped_list_len, child_rsc->id);
            }

        } else if (is_set_recursive(child_rsc, pe_rsc_orphan, TRUE)
                   || is_set_recursive(child_rsc, pe_rsc_managed, FALSE) == FALSE
                   || is_set_recursive(child_rsc, pe_rsc_failed, TRUE)) {

            // Print individual instance when active orphaned/unmanaged/failed
            print_full = TRUE;

        } else if (child_rsc->fns->active(child_rsc, TRUE)) {
            // Instance of fully active anonymous clone

            pe_node_t *location = child_rsc->fns->location(child_rsc, NULL, TRUE);

            if (location) {
                // Instance is active on a single node

                enum rsc_role_e a_role = child_rsc->fns->state(child_rsc, TRUE);

                if (location->details->online == FALSE && location->details->unclean) {
                    print_full = TRUE;

                } else if (a_role > RSC_ROLE_UNPROMOTED) {
                    promoted_list = g_list_append(promoted_list, location);

                } else {
                    started_list = g_list_append(started_list, location);
                }

            } else {
                /* uncolocated group - bleh */
                print_full = TRUE;
            }

        } else {
            // Instance of partially active anonymous clone
            print_full = TRUE;
        }

        if (print_full) {
            GList *all = NULL;

            clone_header(out, &rc, rsc, clone_data);

            /* Print every resource that's a child of this clone. */
            all = g_list_prepend(all, (gpointer) "*");
            out->message(out, crm_map_element_name(child_rsc->xml), show_opts,
                         child_rsc, only_node, all);
            g_list_free(all);
        }
    }

    if (pcmk_is_set(show_opts, pcmk_show_clone_detail)) {
        free(stopped_list);
        PCMK__OUTPUT_LIST_FOOTER(out, rc);
        return pcmk_rc_ok;
    }

    /* Promoted */
    promoted_list = g_list_sort(promoted_list, sort_node_uname);
    for (gIter = promoted_list; gIter; gIter = gIter->next) {
        pe_node_t *host = gIter->data;

        if (!pcmk__str_in_list(only_node, host->details->uname, pcmk__str_casei)) {
            continue;
        }

        pcmk__add_word(&list_text, &list_text_len, host->details->uname);
        active_instances++;
    }
    g_list_free(promoted_list);

    if (list_text != NULL) {
        clone_header(out, &rc, rsc, clone_data);

        out->list_item(out, NULL, PROMOTED_INSTANCES ": [ %s ]", list_text);
        free(list_text);
        list_text = NULL;
        list_text_len = 0;
    }

    /* Started/Unpromoted */
    started_list = g_list_sort(started_list, sort_node_uname);
    for (gIter = started_list; gIter; gIter = gIter->next) {
        pe_node_t *host = gIter->data;

        if (!pcmk__str_in_list(only_node, host->details->uname, pcmk__str_casei)) {
            continue;
        }

        pcmk__add_word(&list_text, &list_text_len, host->details->uname);
        active_instances++;
    }
    g_list_free(started_list);

    if (list_text != NULL) {
        clone_header(out, &rc, rsc, clone_data);

        if (pcmk_is_set(rsc->flags, pe_rsc_promotable)) {
            enum rsc_role_e role = configured_role(rsc);

            if (role == RSC_ROLE_UNPROMOTED) {
                out->list_item(out, NULL,
                               UNPROMOTED_INSTANCES " (target-role): [ %s ]",
                               list_text);
            } else {
                out->list_item(out, NULL, UNPROMOTED_INSTANCES ": [ %s ]",
                               list_text);
            }

        } else {
            out->list_item(out, NULL, "Started: [ %s ]", list_text);
        }
        free(list_text);
        list_text = NULL;
        list_text_len = 0;
    }

    if (pcmk_is_set(show_opts, pcmk_show_inactive_rscs)) {
        const char *state = "Stopped";
        enum rsc_role_e role = configured_role(rsc);

        if (role == RSC_ROLE_STOPPED) {
            state = "Stopped (disabled)";
        }

        if (!pcmk_is_set(rsc->flags, pe_rsc_unique)
            && (clone_data->clone_max > active_instances)) {

            GList *nIter;
            GList *list = g_hash_table_get_values(rsc->allowed_nodes);

            /* Custom stopped list for non-unique clones */
            free(stopped_list);
            stopped_list = NULL;
            stopped_list_len = 0;

            if (list == NULL) {
                /* Clusters with symmetrical=false haven't calculated allowed_nodes yet
                 * If we've not probed for them yet, the Stopped list will be empty
                 */
                list = g_hash_table_get_values(rsc->known_on);
            }

            list = g_list_sort(list, sort_node_uname);
            for (nIter = list; nIter != NULL; nIter = nIter->next) {
                pe_node_t *node = (pe_node_t *)nIter->data;

                if (pe_find_node(rsc->running_on, node->details->uname) == NULL &&
                    pcmk__str_in_list(only_node, node->details->uname, pcmk__str_casei)) {
                    pcmk__add_word(&stopped_list, &stopped_list_len,
                                   node->details->uname);
                }
            }
            g_list_free(list);
        }

        if (stopped_list != NULL) {
            clone_header(out, &rc, rsc, clone_data);

            out->list_item(out, NULL, "%s: [ %s ]", state, stopped_list);
            free(stopped_list);
            stopped_list_len = 0;

        /* If there are no instances of this clone (perhaps because there are no
         * nodes configured), simply output the clone header by itself.  This can
         * come up in PCS testing.
         */
        } else if (active_instances == 0) {
            clone_header(out, &rc, rsc, clone_data);
            PCMK__OUTPUT_LIST_FOOTER(out, rc);
            return rc;
        }
    }

    PCMK__OUTPUT_LIST_FOOTER(out, rc);
    return rc;
}

void
clone_free(pe_resource_t * rsc)
{
    clone_variant_data_t *clone_data = NULL;

    get_clone_variant_data(clone_data, rsc);

    pe_rsc_trace(rsc, "Freeing %s", rsc->id);

    for (GList *gIter = rsc->children; gIter != NULL; gIter = gIter->next) {
        pe_resource_t *child_rsc = (pe_resource_t *) gIter->data;

        CRM_ASSERT(child_rsc);
        pe_rsc_trace(child_rsc, "Freeing child %s", child_rsc->id);
        free_xml(child_rsc->xml);
        child_rsc->xml = NULL;
        /* There could be a saved unexpanded xml */
        free_xml(child_rsc->orig_xml);
        child_rsc->orig_xml = NULL;
        child_rsc->fns->free(child_rsc);
    }

    g_list_free(rsc->children);

    if (clone_data) {
        CRM_ASSERT(clone_data->demote_notify == NULL);
        CRM_ASSERT(clone_data->stop_notify == NULL);
        CRM_ASSERT(clone_data->start_notify == NULL);
        CRM_ASSERT(clone_data->promote_notify == NULL);
    }

    common_free(rsc);
}

enum rsc_role_e
clone_resource_state(const pe_resource_t * rsc, gboolean current)
{
    enum rsc_role_e clone_role = RSC_ROLE_UNKNOWN;
    GList *gIter = rsc->children;

    for (; gIter != NULL; gIter = gIter->next) {
        pe_resource_t *child_rsc = (pe_resource_t *) gIter->data;
        enum rsc_role_e a_role = child_rsc->fns->state(child_rsc, current);

        if (a_role > clone_role) {
            clone_role = a_role;
        }
    }

    pe_rsc_trace(rsc, "%s role: %s", rsc->id, role2text(clone_role));
    return clone_role;
}

/*!
 * \internal
 * \brief Check whether a clone has an instance for every node
 *
 * \param[in] rsc       Clone to check
 * \param[in] data_set  Cluster state
 */
bool
pe__is_universal_clone(pe_resource_t *rsc,
                       pe_working_set_t *data_set)
{
    if (pe_rsc_is_clone(rsc)) {
        clone_variant_data_t *clone_data = NULL;

        get_clone_variant_data(clone_data, rsc);
        if (clone_data->clone_max == g_list_length(data_set->nodes)) {
            return TRUE;
        }
    }
    return FALSE;
}

gboolean
pe__clone_is_filtered(pe_resource_t *rsc, GList *only_rsc, gboolean check_parent)
{
    gboolean passes = FALSE;
    clone_variant_data_t *clone_data = NULL;

    if (pcmk__str_in_list(only_rsc, rsc_printable_id(rsc), pcmk__str_none)) {
        passes = TRUE;
    } else {
        get_clone_variant_data(clone_data, rsc);
        passes = pcmk__str_in_list(only_rsc, ID(clone_data->xml_obj_child), pcmk__str_none);

        if (!passes) {
            for (GList *gIter = rsc->children; gIter != NULL; gIter = gIter->next) {
                pe_resource_t *child_rsc = (pe_resource_t *) gIter->data;

                if (!child_rsc->fns->is_filtered(child_rsc, only_rsc, FALSE)) {
                    passes = TRUE;
                    break;
                }
            }
        }
    }

    return !passes;
}
