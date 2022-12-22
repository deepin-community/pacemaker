/*
 * Copyright 2004-2021 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU General Public License version 2
 * or later (GPLv2+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>

#include <stdbool.h>

#include <crm/pengine/rules.h>
#include <crm/msg_xml.h>
#include <crm/common/xml_internal.h>
#include <pacemaker-internal.h>
#include <crm/services.h>

// The controller removes the resource from the CIB, making this redundant
// #define DELETE_THEN_REFRESH 1

#define INFINITY_HACK   (INFINITY * -100)

#define VARIANT_NATIVE 1
#include <lib/pengine/variant.h>

extern bool pcmk__is_daemon;

static void Recurring(pe_resource_t *rsc, pe_action_t *start, pe_node_t *node,
                      pe_working_set_t *data_set);
static void RecurringOp(pe_resource_t *rsc, pe_action_t *start, pe_node_t *node,
                        xmlNode *operation, pe_working_set_t *data_set);
static void Recurring_Stopped(pe_resource_t *rsc, pe_action_t *start, pe_node_t *node,
                              pe_working_set_t *data_set);
static void RecurringOp_Stopped(pe_resource_t *rsc, pe_action_t *start, pe_node_t *node,
                                xmlNode *operation, pe_working_set_t *data_set);

void ReloadRsc(pe_resource_t * rsc, pe_node_t *node, pe_working_set_t * data_set);
gboolean DeleteRsc(pe_resource_t * rsc, pe_node_t * node, gboolean optional, pe_working_set_t * data_set);
gboolean StopRsc(pe_resource_t * rsc, pe_node_t * next, gboolean optional, pe_working_set_t * data_set);
gboolean StartRsc(pe_resource_t * rsc, pe_node_t * next, gboolean optional, pe_working_set_t * data_set);
gboolean DemoteRsc(pe_resource_t * rsc, pe_node_t * next, gboolean optional, pe_working_set_t * data_set);
gboolean PromoteRsc(pe_resource_t * rsc, pe_node_t * next, gboolean optional,
                    pe_working_set_t * data_set);
gboolean RoleError(pe_resource_t * rsc, pe_node_t * next, gboolean optional, pe_working_set_t * data_set);
gboolean NullOp(pe_resource_t * rsc, pe_node_t * next, gboolean optional, pe_working_set_t * data_set);

/* This array says what the *next* role should be when transitioning from one
 * role to another. For example going from Stopped to Promoted, the next role is
 * RSC_ROLE_UNPROMOTED, because the resource must be started before being promoted.
 * The current state then becomes Started, which is fed into this array again,
 * giving a next role of RSC_ROLE_PROMOTED.
 */
static enum rsc_role_e rsc_state_matrix[RSC_ROLE_MAX][RSC_ROLE_MAX] = {
/* Current state  Next state*/
/*                 Unknown           Stopped           Started           Unpromoted           Promoted */
/* Unknown */    { RSC_ROLE_UNKNOWN, RSC_ROLE_STOPPED, RSC_ROLE_STOPPED, RSC_ROLE_STOPPED,    RSC_ROLE_STOPPED },
/* Stopped */    { RSC_ROLE_STOPPED, RSC_ROLE_STOPPED, RSC_ROLE_STARTED, RSC_ROLE_UNPROMOTED, RSC_ROLE_UNPROMOTED },
/* Started */    { RSC_ROLE_STOPPED, RSC_ROLE_STOPPED, RSC_ROLE_STARTED, RSC_ROLE_UNPROMOTED, RSC_ROLE_PROMOTED },
/* Unpromoted */ { RSC_ROLE_STOPPED, RSC_ROLE_STOPPED, RSC_ROLE_STOPPED, RSC_ROLE_UNPROMOTED, RSC_ROLE_PROMOTED },
/* Promoted  */  { RSC_ROLE_STOPPED, RSC_ROLE_UNPROMOTED, RSC_ROLE_UNPROMOTED, RSC_ROLE_UNPROMOTED, RSC_ROLE_PROMOTED },
};

typedef gboolean (*rsc_transition_fn)(pe_resource_t *rsc, pe_node_t *next,
                                      gboolean optional,
                                      pe_working_set_t *data_set);

// This array picks the function needed to transition from one role to another
static rsc_transition_fn rsc_action_matrix[RSC_ROLE_MAX][RSC_ROLE_MAX] = {
/* Current state   Next state                                            */
/*                 Unknown    Stopped    Started    Unpromoted Promoted  */
/* Unknown */    { RoleError, StopRsc,   RoleError, RoleError, RoleError,    },
/* Stopped */    { RoleError, NullOp,    StartRsc,  StartRsc,  RoleError,    },
/* Started */    { RoleError, StopRsc,   NullOp,    NullOp,    PromoteRsc,   },
/* Unpromoted */ { RoleError, StopRsc,   StopRsc,   NullOp,    PromoteRsc,   },
/* Promoted  */  { RoleError, DemoteRsc, DemoteRsc, DemoteRsc, NullOp,       },
};

#define clear_node_weights_flags(nw_flags, nw_rsc, flags_to_clear) do {     \
        flags = pcmk__clear_flags_as(__func__, __LINE__, LOG_TRACE,         \
                                     "Node weight", (nw_rsc)->id, (flags),  \
                                     (flags_to_clear), #flags_to_clear);    \
    } while (0)

static gboolean
native_choose_node(pe_resource_t * rsc, pe_node_t * prefer, pe_working_set_t * data_set)
{
    GList *nodes = NULL;
    pe_node_t *chosen = NULL;
    pe_node_t *best = NULL;
    int multiple = 1;
    int length = 0;
    gboolean result = FALSE;

    process_utilization(rsc, &prefer, data_set);

    if (!pcmk_is_set(rsc->flags, pe_rsc_provisional)) {
        return rsc->allocated_to ? TRUE : FALSE;
    }

    // Sort allowed nodes by weight
    if (rsc->allowed_nodes) {
        length = g_hash_table_size(rsc->allowed_nodes);
    }
    if (length > 0) {
        nodes = g_hash_table_get_values(rsc->allowed_nodes);
        nodes = sort_nodes_by_weight(nodes, pe__current_node(rsc), data_set);

        // First node in sorted list has the best score
        best = g_list_nth_data(nodes, 0);
    }

    if (prefer && nodes) {
        chosen = g_hash_table_lookup(rsc->allowed_nodes, prefer->details->id);

        if (chosen == NULL) {
            pe_rsc_trace(rsc, "Preferred node %s for %s was unknown",
                         prefer->details->uname, rsc->id);

        /* Favor the preferred node as long as its weight is at least as good as
         * the best allowed node's.
         *
         * An alternative would be to favor the preferred node even if the best
         * node is better, when the best node's weight is less than INFINITY.
         */
        } else if ((chosen->weight < 0) || (chosen->weight < best->weight)) {
            pe_rsc_trace(rsc, "Preferred node %s for %s was unsuitable",
                         chosen->details->uname, rsc->id);
            chosen = NULL;

        } else if (!can_run_resources(chosen)) {
            pe_rsc_trace(rsc, "Preferred node %s for %s was unavailable",
                         chosen->details->uname, rsc->id);
            chosen = NULL;

        } else {
            pe_rsc_trace(rsc,
                         "Chose preferred node %s for %s (ignoring %d candidates)",
                         chosen->details->uname, rsc->id, length);
        }
    }

    if ((chosen == NULL) && nodes) {
        /* Either there is no preferred node, or the preferred node is not
         * available, but there are other nodes allowed to run the resource.
         */

        chosen = best;
        pe_rsc_trace(rsc, "Chose node %s for %s from %d candidates",
                     chosen ? chosen->details->uname : "<none>", rsc->id, length);

        if (!pe_rsc_is_unique_clone(rsc->parent)
            && chosen && (chosen->weight > 0) && can_run_resources(chosen)) {
            /* If the resource is already running on a node, prefer that node if
             * it is just as good as the chosen node.
             *
             * We don't do this for unique clone instances, because
             * distribute_children() has already assigned instances to their
             * running nodes when appropriate, and if we get here, we don't want
             * remaining unallocated instances to prefer a node that's already
             * running another instance.
             */
            pe_node_t *running = pe__current_node(rsc);

            if (running && (can_run_resources(running) == FALSE)) {
                pe_rsc_trace(rsc, "Current node for %s (%s) can't run resources",
                             rsc->id, running->details->uname);
            } else if (running) {
                for (GList *iter = nodes->next; iter; iter = iter->next) {
                    pe_node_t *tmp = (pe_node_t *) iter->data;

                    if (tmp->weight != chosen->weight) {
                        // The nodes are sorted by weight, so no more are equal
                        break;
                    }
                    if (tmp->details == running->details) {
                        // Scores are equal, so prefer the current node
                        chosen = tmp;
                    }
                    multiple++;
                }
            }
        }
    }

    if (multiple > 1) {
        static char score[33];
        int log_level = (chosen->weight >= INFINITY)? LOG_WARNING : LOG_INFO;

        score2char_stack(chosen->weight, score, sizeof(score));
        do_crm_log(log_level,
                   "Chose node %s for %s from %d nodes with score %s",
                   chosen->details->uname, rsc->id, multiple, score);
    }

    result = native_assign_node(rsc, chosen, FALSE);
    g_list_free(nodes);
    return result;
}

/*!
 * \internal
 * \brief Find score of highest-scored node that matches colocation attribute
 *
 * \param[in] rsc    Resource whose allowed nodes should be searched
 * \param[in] attr   Colocation attribute name (must not be NULL)
 * \param[in] value  Colocation attribute value to require
 */
static int
best_node_score_matching_attr(const pe_resource_t *rsc, const char *attr,
                              const char *value)
{
    GHashTableIter iter;
    pe_node_t *node = NULL;
    int best_score = -INFINITY;
    const char *best_node = NULL;

    // Find best allowed node with matching attribute
    g_hash_table_iter_init(&iter, rsc->allowed_nodes);
    while (g_hash_table_iter_next(&iter, NULL, (void **) &node)) {

        if ((node->weight > best_score) && can_run_resources(node)
            && pcmk__str_eq(value, pe_node_attribute_raw(node, attr), pcmk__str_casei)) {

            best_score = node->weight;
            best_node = node->details->uname;
        }
    }

    if (!pcmk__str_eq(attr, CRM_ATTR_UNAME, pcmk__str_casei)) {
        if (best_node == NULL) {
            crm_info("No allowed node for %s matches node attribute %s=%s",
                     rsc->id, attr, value);
        } else {
            crm_info("Allowed node %s for %s had best score (%d) "
                     "of those matching node attribute %s=%s",
                     best_node, rsc->id, best_score, attr, value);
        }
    }
    return best_score;
}

/*!
 * \internal
 * \brief Add resource's colocation matches to current node allocation scores
 *
 * For each node in a given table, if any of a given resource's allowed nodes
 * have a matching value for the colocation attribute, add the highest of those
 * nodes' scores to the node's score.
 *
 * \param[in,out] nodes  Hash table of nodes with allocation scores so far
 * \param[in]     rsc    Resource whose allowed nodes should be compared
 * \param[in]     attr   Colocation attribute that must match (NULL for default)
 * \param[in]     factor Factor by which to multiply scores being added
 * \param[in]     only_positive  Whether to add only positive scores
 */
static void
add_node_scores_matching_attr(GHashTable *nodes, const pe_resource_t *rsc,
                              const char *attr, float factor,
                              bool only_positive)
{
    GHashTableIter iter;
    pe_node_t *node = NULL;

    if (attr == NULL) {
        attr = CRM_ATTR_UNAME;
    }

    // Iterate through each node
    g_hash_table_iter_init(&iter, nodes);
    while (g_hash_table_iter_next(&iter, NULL, (void **)&node)) {
        float weight_f = 0;
        int weight = 0;
        int score = 0;
        int new_score = 0;

        score = best_node_score_matching_attr(rsc, attr,
                                              pe_node_attribute_raw(node, attr));

        if ((factor < 0) && (score < 0)) {
            /* Negative preference for a node with a negative score
             * should not become a positive preference.
             *
             * @TODO Consider filtering only if weight is -INFINITY
             */
            crm_trace("%s: Filtering %d + %f * %d (double negative disallowed)",
                      node->details->uname, node->weight, factor, score);
            continue;
        }

        if (node->weight == INFINITY_HACK) {
            crm_trace("%s: Filtering %d + %f * %d (node was marked unusable)",
                      node->details->uname, node->weight, factor, score);
            continue;
        }

        weight_f = factor * score;

        // Round the number; see http://c-faq.com/fp/round.html
        weight = (int) ((weight_f < 0)? (weight_f - 0.5) : (weight_f + 0.5));

        /* Small factors can obliterate the small scores that are often actually
         * used in configurations. If the score and factor are nonzero, ensure
         * that the result is nonzero as well.
         */
        if ((weight == 0) && (score != 0)) {
            if (factor > 0.0) {
                weight = 1;
            } else if (factor < 0.0) {
                weight = -1;
            }
        }

        new_score = pe__add_scores(weight, node->weight);

        if (only_positive && (new_score < 0) && (node->weight > 0)) {
            crm_trace("%s: Filtering %d + %f * %d = %d "
                      "(negative disallowed, marking node unusable)",
                      node->details->uname, node->weight, factor, score,
                      new_score);
            node->weight = INFINITY_HACK;
            continue;
        }

        if (only_positive && (new_score < 0) && (node->weight == 0)) {
            crm_trace("%s: Filtering %d + %f * %d = %d (negative disallowed)",
                      node->details->uname, node->weight, factor, score,
                      new_score);
            continue;
        }

        crm_trace("%s: %d + %f * %d = %d", node->details->uname,
                  node->weight, factor, score, new_score);
        node->weight = new_score;
    }
}

static inline bool
is_nonempty_group(pe_resource_t *rsc)
{
    return rsc && (rsc->variant == pe_group) && (rsc->children != NULL);
}

/*!
 * \internal
 * \brief Incorporate colocation constraint scores into node weights
 *
 * \param[in,out] rsc    Resource being placed
 * \param[in]     rhs    ID of 'with' resource
 * \param[in,out] nodes  Nodes, with scores as of this point
 * \param[in]     attr   Colocation attribute (ID by default)
 * \param[in]     factor Incorporate scores multiplied by this factor
 * \param[in]     flags  Bitmask of enum pe_weights values
 *
 * \return Nodes, with scores modified by this constraint
 * \note This function assumes ownership of the nodes argument. The caller
 *       should free the returned copy rather than the original.
 */
GHashTable *
pcmk__native_merge_weights(pe_resource_t *rsc, const char *rhs,
                           GHashTable *nodes, const char *attr, float factor,
                           uint32_t flags)
{
    GHashTable *work = NULL;

    // Avoid infinite recursion
    if (pcmk_is_set(rsc->flags, pe_rsc_merging)) {
        pe_rsc_info(rsc, "%s: Breaking dependency loop at %s", rhs, rsc->id);
        return nodes;
    }
    pe__set_resource_flags(rsc, pe_rsc_merging);

    if (pcmk_is_set(flags, pe_weights_init)) {
        if (is_nonempty_group(rsc)) {
            GList *last = g_list_last(rsc->children);
            pe_resource_t *last_rsc = last->data;

            pe_rsc_trace(rsc, "%s: Merging scores from group %s "
                         "using last member %s (at %.6f)",
                         rhs, rsc->id, last_rsc->id, factor);
            work = pcmk__native_merge_weights(last_rsc, rhs, NULL, attr, factor,
                                              flags);
        } else {
            work = pcmk__copy_node_table(rsc->allowed_nodes);
        }
        clear_node_weights_flags(flags, rsc, pe_weights_init);

    } else if (is_nonempty_group(rsc)) {
        /* The first member of the group will recursively incorporate any
         * constraints involving other members (including the group internal
         * colocation).
         *
         * @TODO The indirect colocations from the dependent group's other
         *       members will be incorporated at full strength rather than by
         *       factor, so the group's combined stickiness will be treated as
         *       (factor + (#members - 1)) * stickiness. It is questionable what
         *       the right approach should be.
         */
        pe_rsc_trace(rsc, "%s: Merging scores from first member of group %s "
                     "(at %.6f)", rhs, rsc->id, factor);
        work = pcmk__copy_node_table(nodes);
        work = pcmk__native_merge_weights(rsc->children->data, rhs, work, attr,
                                          factor, flags);

    } else {
        pe_rsc_trace(rsc, "%s: Merging scores from %s (at %.6f)",
                     rhs, rsc->id, factor);
        work = pcmk__copy_node_table(nodes);
        add_node_scores_matching_attr(work, rsc, attr, factor,
                                      pcmk_is_set(flags, pe_weights_positive));
    }

    if (can_run_any(work)) {
        GList *gIter = NULL;
        int multiplier = (factor < 0)? -1 : 1;

        if (pcmk_is_set(flags, pe_weights_forward)) {
            gIter = rsc->rsc_cons;
            pe_rsc_trace(rsc,
                         "Checking additional %d optional '%s with' constraints",
                         g_list_length(gIter), rsc->id);

        } else if (is_nonempty_group(rsc)) {
            pe_resource_t *last_rsc = g_list_last(rsc->children)->data;

            gIter = last_rsc->rsc_cons_lhs;
            pe_rsc_trace(rsc, "Checking additional %d optional 'with group %s' "
                         "constraints using last member %s",
                         g_list_length(gIter), rsc->id, last_rsc->id);

        } else {
            gIter = rsc->rsc_cons_lhs;
            pe_rsc_trace(rsc,
                         "Checking additional %d optional 'with %s' constraints",
                         g_list_length(gIter), rsc->id);
        }

        for (; gIter != NULL; gIter = gIter->next) {
            pe_resource_t *other = NULL;
            pcmk__colocation_t *constraint = (pcmk__colocation_t *) gIter->data;

            if (pcmk_is_set(flags, pe_weights_forward)) {
                other = constraint->rsc_rh;
            } else if (!pcmk__colocation_has_influence(constraint, NULL)) {
                continue;
            } else {
                other = constraint->rsc_lh;
            }

            pe_rsc_trace(rsc, "Optionally merging score of '%s' constraint (%s with %s)",
                         constraint->id, constraint->rsc_lh->id,
                         constraint->rsc_rh->id);
            work = pcmk__native_merge_weights(other, rhs, work,
                                              constraint->node_attribute,
                                              multiplier * constraint->score / (float) INFINITY,
                                              flags|pe_weights_rollback);
            pe__show_node_weights(true, NULL, rhs, work, rsc->cluster);
        }

    } else if (pcmk_is_set(flags, pe_weights_rollback)) {
        pe_rsc_info(rsc, "%s: Rolling back optional scores from %s",
                    rhs, rsc->id);
        g_hash_table_destroy(work);
        pe__clear_resource_flags(rsc, pe_rsc_merging);
        return nodes;
    }


    if (pcmk_is_set(flags, pe_weights_positive)) {
        pe_node_t *node = NULL;
        GHashTableIter iter;

        g_hash_table_iter_init(&iter, work);
        while (g_hash_table_iter_next(&iter, NULL, (void **)&node)) {
            if (node->weight == INFINITY_HACK) {
                node->weight = 1;
            }
        }
    }

    if (nodes) {
        g_hash_table_destroy(nodes);
    }

    pe__clear_resource_flags(rsc, pe_rsc_merging);
    return work;
}

static inline bool
node_has_been_unfenced(pe_node_t *node)
{
    const char *unfenced = pe_node_attribute_raw(node, CRM_ATTR_UNFENCED);

    return !pcmk__str_eq(unfenced, "0", pcmk__str_null_matches);
}

static inline bool
is_unfence_device(pe_resource_t *rsc, pe_working_set_t *data_set)
{
    return pcmk_is_set(rsc->flags, pe_rsc_fence_device)
           && pcmk_is_set(data_set->flags, pe_flag_enable_unfencing);
}

pe_node_t *
pcmk__native_allocate(pe_resource_t *rsc, pe_node_t *prefer,
                      pe_working_set_t *data_set)
{
    GList *gIter = NULL;

    if (rsc->parent && !pcmk_is_set(rsc->parent->flags, pe_rsc_allocating)) {
        /* never allocate children on their own */
        pe_rsc_debug(rsc, "Escalating allocation of %s to its parent: %s", rsc->id,
                     rsc->parent->id);
        rsc->parent->cmds->allocate(rsc->parent, prefer, data_set);
    }

    if (!pcmk_is_set(rsc->flags, pe_rsc_provisional)) {
        return rsc->allocated_to;
    }

    if (pcmk_is_set(rsc->flags, pe_rsc_allocating)) {
        pe_rsc_debug(rsc, "Dependency loop detected involving %s", rsc->id);
        return NULL;
    }

    pe__set_resource_flags(rsc, pe_rsc_allocating);
    pe__show_node_weights(true, rsc, "Pre-alloc", rsc->allowed_nodes, data_set);

    for (gIter = rsc->rsc_cons; gIter != NULL; gIter = gIter->next) {
        pcmk__colocation_t *constraint = (pcmk__colocation_t *) gIter->data;

        GHashTable *archive = NULL;
        pe_resource_t *rsc_rh = constraint->rsc_rh;

        if ((constraint->role_lh >= RSC_ROLE_PROMOTED)
            || (constraint->score < 0 && constraint->score > -INFINITY)) {
            archive = pcmk__copy_node_table(rsc->allowed_nodes);
        }

        pe_rsc_trace(rsc,
                     "%s: Allocating %s first (constraint=%s score=%d role=%s)",
                     rsc->id, rsc_rh->id, constraint->id,
                     constraint->score, role2text(constraint->role_lh));
        rsc_rh->cmds->allocate(rsc_rh, NULL, data_set);
        rsc->cmds->rsc_colocation_lh(rsc, rsc_rh, constraint, data_set);
        if (archive && can_run_any(rsc->allowed_nodes) == FALSE) {
            pe_rsc_info(rsc, "%s: Rolling back scores from %s", rsc->id, rsc_rh->id);
            g_hash_table_destroy(rsc->allowed_nodes);
            rsc->allowed_nodes = archive;
            archive = NULL;
        }
        if (archive) {
            g_hash_table_destroy(archive);
        }
    }

    pe__show_node_weights(true, rsc, "Post-coloc", rsc->allowed_nodes, data_set);

    for (gIter = rsc->rsc_cons_lhs; gIter != NULL; gIter = gIter->next) {
        pcmk__colocation_t *constraint = (pcmk__colocation_t *) gIter->data;

        if (!pcmk__colocation_has_influence(constraint, NULL)) {
            continue;
        }
        pe_rsc_trace(rsc, "Merging score of '%s' constraint (%s with %s)",
                     constraint->id, constraint->rsc_lh->id,
                     constraint->rsc_rh->id);
        rsc->allowed_nodes =
            constraint->rsc_lh->cmds->merge_weights(constraint->rsc_lh, rsc->id, rsc->allowed_nodes,
                                                    constraint->node_attribute,
                                                    (float)constraint->score / INFINITY,
                                                    pe_weights_rollback);
    }

    if (rsc->next_role == RSC_ROLE_STOPPED) {
        pe_rsc_trace(rsc, "Making sure %s doesn't get allocated", rsc->id);
        /* make sure it doesn't come up again */
        resource_location(rsc, NULL, -INFINITY, XML_RSC_ATTR_TARGET_ROLE, data_set);

    } else if(rsc->next_role > rsc->role
              && !pcmk_is_set(data_set->flags, pe_flag_have_quorum)
              && data_set->no_quorum_policy == no_quorum_freeze) {
        crm_notice("Resource %s cannot be elevated from %s to %s: no-quorum-policy=freeze",
                   rsc->id, role2text(rsc->role), role2text(rsc->next_role));
        pe__set_next_role(rsc, rsc->role, "no-quorum-policy=freeze");
    }

    pe__show_node_weights(!pcmk_is_set(data_set->flags, pe_flag_show_scores),
                          rsc, __func__, rsc->allowed_nodes, data_set);
    if (pcmk_is_set(data_set->flags, pe_flag_stonith_enabled)
        && !pcmk_is_set(data_set->flags, pe_flag_have_stonith_resource)) {
        pe__clear_resource_flags(rsc, pe_rsc_managed);
    }

    if (!pcmk_is_set(rsc->flags, pe_rsc_managed)) {
        const char *reason = NULL;
        pe_node_t *assign_to = NULL;

        pe__set_next_role(rsc, rsc->role, "unmanaged");
        assign_to = pe__current_node(rsc);
        if (assign_to == NULL) {
            reason = "inactive";
        } else if (rsc->role == RSC_ROLE_PROMOTED) {
            reason = "promoted";
        } else if (pcmk_is_set(rsc->flags, pe_rsc_failed)) {
            reason = "failed";
        } else {
            reason = "active";
        }
        pe_rsc_info(rsc, "Unmanaged resource %s allocated to %s: %s", rsc->id,
                    (assign_to? assign_to->details->uname : "no node"), reason);
        native_assign_node(rsc, assign_to, TRUE);

    } else if (pcmk_is_set(data_set->flags, pe_flag_stop_everything)) {
        pe_rsc_debug(rsc, "Forcing %s to stop", rsc->id);
        native_assign_node(rsc, NULL, TRUE);

    } else if (pcmk_is_set(rsc->flags, pe_rsc_provisional)
               && native_choose_node(rsc, prefer, data_set)) {
        pe_rsc_trace(rsc, "Allocated resource %s to %s", rsc->id,
                     rsc->allocated_to->details->uname);

    } else if (rsc->allocated_to == NULL) {
        if (!pcmk_is_set(rsc->flags, pe_rsc_orphan)) {
            pe_rsc_info(rsc, "Resource %s cannot run anywhere", rsc->id);
        } else if (rsc->running_on != NULL) {
            pe_rsc_info(rsc, "Stopping orphan resource %s", rsc->id);
        }

    } else {
        pe_rsc_debug(rsc, "Pre-Allocated resource %s to %s", rsc->id,
                     rsc->allocated_to->details->uname);
    }

    pe__clear_resource_flags(rsc, pe_rsc_allocating);

    if (rsc->is_remote_node) {
        pe_node_t *remote_node = pe_find_node(data_set->nodes, rsc->id);

        CRM_ASSERT(remote_node != NULL);
        if (rsc->allocated_to && rsc->next_role != RSC_ROLE_STOPPED) {
            crm_trace("Setting Pacemaker Remote node %s to ONLINE",
                      remote_node->details->id);
            remote_node->details->online = TRUE;
            /* We shouldn't consider an unseen remote-node unclean if we are going
             * to try and connect to it. Otherwise we get an unnecessary fence */
            if (remote_node->details->unseen == TRUE) {
                remote_node->details->unclean = FALSE;
            }

        } else {
            crm_trace("Setting Pacemaker Remote node %s to SHUTDOWN (next role %s, %sallocated)",
                      remote_node->details->id, role2text(rsc->next_role),
                      (rsc->allocated_to? "" : "un"));
            remote_node->details->shutdown = TRUE;
        }
    }

    return rsc->allocated_to;
}

static gboolean
is_op_dup(pe_resource_t *rsc, const char *name, guint interval_ms)
{
    gboolean dup = FALSE;
    const char *id = NULL;
    const char *value = NULL;
    xmlNode *operation = NULL;
    guint interval2_ms = 0;

    CRM_ASSERT(rsc);
    for (operation = pcmk__xe_first_child(rsc->ops_xml); operation != NULL;
         operation = pcmk__xe_next(operation)) {

        if (pcmk__str_eq((const char *)operation->name, "op", pcmk__str_none)) {
            value = crm_element_value(operation, "name");
            if (!pcmk__str_eq(value, name, pcmk__str_casei)) {
                continue;
            }

            value = crm_element_value(operation, XML_LRM_ATTR_INTERVAL);
            interval2_ms = crm_parse_interval_spec(value);
            if (interval_ms != interval2_ms) {
                continue;
            }

            if (id == NULL) {
                id = ID(operation);

            } else {
                pcmk__config_err("Operation %s is duplicate of %s (do not use "
                                 "same name and interval combination more "
                                 "than once per resource)", ID(operation), id);
                dup = TRUE;
            }
        }
    }

    return dup;
}

static bool
op_cannot_recur(const char *name)
{
    return pcmk__strcase_any_of(name, RSC_STOP, RSC_START, RSC_DEMOTE, RSC_PROMOTE, NULL);
}

static void
RecurringOp(pe_resource_t * rsc, pe_action_t * start, pe_node_t * node,
            xmlNode * operation, pe_working_set_t * data_set)
{
    char *key = NULL;
    const char *name = NULL;
    const char *role = NULL;
    const char *interval_spec = NULL;
    const char *node_uname = node? node->details->uname : "n/a";

    guint interval_ms = 0;
    pe_action_t *mon = NULL;
    gboolean is_optional = TRUE;
    GList *possible_matches = NULL;

    CRM_ASSERT(rsc);

    /* Only process for the operations without role="Stopped" */
    role = crm_element_value(operation, "role");
    if (role && text2role(role) == RSC_ROLE_STOPPED) {
        return;
    }

    interval_spec = crm_element_value(operation, XML_LRM_ATTR_INTERVAL);
    interval_ms = crm_parse_interval_spec(interval_spec);
    if (interval_ms == 0) {
        return;
    }

    name = crm_element_value(operation, "name");
    if (is_op_dup(rsc, name, interval_ms)) {
        crm_trace("Not creating duplicate recurring action %s for %dms %s",
                  ID(operation), interval_ms, name);
        return;
    }

    if (op_cannot_recur(name)) {
        pcmk__config_err("Ignoring %s because action '%s' cannot be recurring",
                         ID(operation), name);
        return;
    }

    key = pcmk__op_key(rsc->id, name, interval_ms);
    if (find_rsc_op_entry(rsc, key) == NULL) {
        crm_trace("Not creating recurring action %s for disabled resource %s",
                  ID(operation), rsc->id);
        free(key);
        return;
    }

    pe_rsc_trace(rsc, "Creating recurring action %s for %s in role %s on %s",
                 ID(operation), rsc->id, role2text(rsc->next_role), node_uname);

    if (start != NULL) {
        pe_rsc_trace(rsc, "Marking %s %s due to %s", key,
                     pcmk_is_set(start->flags, pe_action_optional)? "optional" : "mandatory",
                     start->uuid);
        is_optional = (rsc->cmds->action_flags(start, NULL) & pe_action_optional);
    } else {
        pe_rsc_trace(rsc, "Marking %s optional", key);
        is_optional = TRUE;
    }

    /* start a monitor for an already active resource */
    possible_matches = find_actions_exact(rsc->actions, key, node);
    if (possible_matches == NULL) {
        is_optional = FALSE;
        pe_rsc_trace(rsc, "Marking %s mandatory: not active", key);

    } else {
        GList *gIter = NULL;

        for (gIter = possible_matches; gIter != NULL; gIter = gIter->next) {
            pe_action_t *op = (pe_action_t *) gIter->data;

            if (pcmk_is_set(op->flags, pe_action_reschedule)) {
                is_optional = FALSE;
                break;
            }
        }
        g_list_free(possible_matches);
    }

    if (((rsc->next_role == RSC_ROLE_PROMOTED) && (role == NULL))
        || (role != NULL && text2role(role) != rsc->next_role)) {
        int log_level = LOG_TRACE;
        const char *result = "Ignoring";

        if (is_optional) {
            char *after_key = NULL;
            pe_action_t *cancel_op = NULL;

            // It's running, so cancel it
            log_level = LOG_INFO;
            result = "Cancelling";
            cancel_op = pe_cancel_op(rsc, name, interval_ms, node, data_set);

            switch (rsc->role) {
                case RSC_ROLE_UNPROMOTED:
                case RSC_ROLE_STARTED:
                    if (rsc->next_role == RSC_ROLE_PROMOTED) {
                        after_key = promote_key(rsc);

                    } else if (rsc->next_role == RSC_ROLE_STOPPED) {
                        after_key = stop_key(rsc);
                    }

                    break;
                case RSC_ROLE_PROMOTED:
                    after_key = demote_key(rsc);
                    break;
                default:
                    break;
            }

            if (after_key) {
                custom_action_order(rsc, NULL, cancel_op, rsc, after_key, NULL,
                                    pe_order_runnable_left, data_set);
            }
        }

        do_crm_log(log_level, "%s action %s (%s vs. %s)",
                   result, key, role ? role : role2text(RSC_ROLE_UNPROMOTED),
                   role2text(rsc->next_role));

        free(key);
        return;
    }

    mon = custom_action(rsc, key, name, node, is_optional, TRUE, data_set);
    key = mon->uuid;
    if (is_optional) {
        pe_rsc_trace(rsc, "%s\t   %s (optional)", node_uname, mon->uuid);
    }

    if ((start == NULL) || !pcmk_is_set(start->flags, pe_action_runnable)) {
        pe_rsc_debug(rsc, "%s\t   %s (cancelled : start un-runnable)",
                     node_uname, mon->uuid);
        pe__clear_action_flags(mon, pe_action_runnable);

    } else if (node == NULL || node->details->online == FALSE || node->details->unclean) {
        pe_rsc_debug(rsc, "%s\t   %s (cancelled : no node available)",
                     node_uname, mon->uuid);
        pe__clear_action_flags(mon, pe_action_runnable);

    } else if (!pcmk_is_set(mon->flags, pe_action_optional)) {
        pe_rsc_info(rsc, " Start recurring %s (%us) for %s on %s",
                    mon->task, interval_ms / 1000, rsc->id, node_uname);
    }

    if (rsc->next_role == RSC_ROLE_PROMOTED) {
        char *running_promoted = pcmk__itoa(PCMK_OCF_RUNNING_PROMOTED);

        add_hash_param(mon->meta, XML_ATTR_TE_TARGET_RC, running_promoted);
        free(running_promoted);
    }

    if ((node == NULL) || pcmk_is_set(rsc->flags, pe_rsc_managed)) {
        custom_action_order(rsc, start_key(rsc), NULL,
                            NULL, strdup(key), mon,
                            pe_order_implies_then | pe_order_runnable_left, data_set);

        custom_action_order(rsc, reload_key(rsc), NULL,
                            NULL, strdup(key), mon,
                            pe_order_implies_then | pe_order_runnable_left, data_set);

        if (rsc->next_role == RSC_ROLE_PROMOTED) {
            custom_action_order(rsc, promote_key(rsc), NULL,
                                rsc, NULL, mon,
                                pe_order_optional | pe_order_runnable_left, data_set);

        } else if (rsc->role == RSC_ROLE_PROMOTED) {
            custom_action_order(rsc, demote_key(rsc), NULL,
                                rsc, NULL, mon,
                                pe_order_optional | pe_order_runnable_left, data_set);
        }
    }
}

static void
Recurring(pe_resource_t * rsc, pe_action_t * start, pe_node_t * node, pe_working_set_t * data_set)
{
    if (!pcmk_is_set(rsc->flags, pe_rsc_maintenance) &&
        (node == NULL || node->details->maintenance == FALSE)) {
        xmlNode *operation = NULL;

        for (operation = pcmk__xe_first_child(rsc->ops_xml);
             operation != NULL;
             operation = pcmk__xe_next(operation)) {

            if (pcmk__str_eq((const char *)operation->name, "op", pcmk__str_none)) {
                RecurringOp(rsc, start, node, operation, data_set);
            }
        }
    }
}

static void
RecurringOp_Stopped(pe_resource_t * rsc, pe_action_t * start, pe_node_t * node,
                    xmlNode * operation, pe_working_set_t * data_set)
{
    char *key = NULL;
    const char *name = NULL;
    const char *role = NULL;
    const char *interval_spec = NULL;
    const char *node_uname = node? node->details->uname : "n/a";

    guint interval_ms = 0;
    GList *possible_matches = NULL;
    GList *gIter = NULL;

    /* Only process for the operations with role="Stopped" */
    role = crm_element_value(operation, "role");
    if (role == NULL || text2role(role) != RSC_ROLE_STOPPED) {
        return;
    }

    interval_spec = crm_element_value(operation, XML_LRM_ATTR_INTERVAL);
    interval_ms = crm_parse_interval_spec(interval_spec);
    if (interval_ms == 0) {
        return;
    }

    name = crm_element_value(operation, "name");
    if (is_op_dup(rsc, name, interval_ms)) {
        crm_trace("Not creating duplicate recurring action %s for %dms %s",
                  ID(operation), interval_ms, name);
        return;
    }

    if (op_cannot_recur(name)) {
        pcmk__config_err("Ignoring %s because action '%s' cannot be recurring",
                         ID(operation), name);
        return;
    }

    key = pcmk__op_key(rsc->id, name, interval_ms);
    if (find_rsc_op_entry(rsc, key) == NULL) {
        crm_trace("Not creating recurring action %s for disabled resource %s",
                  ID(operation), rsc->id);
        free(key);
        return;
    }

    // @TODO add support
    if (!pcmk_is_set(rsc->flags, pe_rsc_unique)) {
        crm_notice("Ignoring %s (recurring monitors for Stopped role are "
                   "not supported for anonymous clones)",
                   ID(operation));
        return;
    }

    pe_rsc_trace(rsc,
                 "Creating recurring action %s for %s in role %s on nodes where it should not be running",
                 ID(operation), rsc->id, role2text(rsc->next_role));

    /* if the monitor exists on the node where the resource will be running, cancel it */
    if (node != NULL) {
        possible_matches = find_actions_exact(rsc->actions, key, node);
        if (possible_matches) {
            pe_action_t *cancel_op = NULL;

            g_list_free(possible_matches);

            cancel_op = pe_cancel_op(rsc, name, interval_ms, node, data_set);

            if ((rsc->next_role == RSC_ROLE_STARTED)
                || (rsc->next_role == RSC_ROLE_UNPROMOTED)) {
                /* rsc->role == RSC_ROLE_STOPPED: cancel the monitor before start */
                /* rsc->role == RSC_ROLE_STARTED: for a migration, cancel the monitor on the target node before start */
                custom_action_order(rsc, NULL, cancel_op, rsc, start_key(rsc), NULL,
                                    pe_order_runnable_left, data_set);
            }

            pe_rsc_info(rsc, "Cancel action %s (%s vs. %s) on %s",
                        key, role, role2text(rsc->next_role), node_uname);
        }
    }

    for (gIter = data_set->nodes; gIter != NULL; gIter = gIter->next) {
        pe_node_t *stop_node = (pe_node_t *) gIter->data;
        const char *stop_node_uname = stop_node->details->uname;
        gboolean is_optional = TRUE;
        gboolean probe_is_optional = TRUE;
        gboolean stop_is_optional = TRUE;
        pe_action_t *stopped_mon = NULL;
        char *rc_inactive = NULL;
        GList *probe_complete_ops = NULL;
        GList *stop_ops = NULL;
        GList *local_gIter = NULL;

        if (node && pcmk__str_eq(stop_node_uname, node_uname, pcmk__str_casei)) {
            continue;
        }

        pe_rsc_trace(rsc, "Creating recurring action %s for %s on %s",
                     ID(operation), rsc->id, crm_str(stop_node_uname));

        /* start a monitor for an already stopped resource */
        possible_matches = find_actions_exact(rsc->actions, key, stop_node);
        if (possible_matches == NULL) {
            pe_rsc_trace(rsc, "Marking %s mandatory on %s: not active", key,
                         crm_str(stop_node_uname));
            is_optional = FALSE;
        } else {
            pe_rsc_trace(rsc, "Marking %s optional on %s: already active", key,
                         crm_str(stop_node_uname));
            is_optional = TRUE;
            g_list_free(possible_matches);
        }

        stopped_mon = custom_action(rsc, strdup(key), name, stop_node, is_optional, TRUE, data_set);

        rc_inactive = pcmk__itoa(PCMK_OCF_NOT_RUNNING);
        add_hash_param(stopped_mon->meta, XML_ATTR_TE_TARGET_RC, rc_inactive);
        free(rc_inactive);

        if (pcmk_is_set(rsc->flags, pe_rsc_managed)) {
            GList *probes = pe__resource_actions(rsc, stop_node, RSC_STATUS,
                                                 FALSE);
            GList *pIter = NULL;

            for (pIter = probes; pIter != NULL; pIter = pIter->next) {
                pe_action_t *probe = (pe_action_t *) pIter->data;

                order_actions(probe, stopped_mon, pe_order_runnable_left);
                crm_trace("%s then %s on %s", probe->uuid, stopped_mon->uuid, stop_node->details->uname);
            }

            g_list_free(probes);
        }

        if (probe_complete_ops) {
            g_list_free(probe_complete_ops);
        }

        stop_ops = pe__resource_actions(rsc, stop_node, RSC_STOP, TRUE);

        for (local_gIter = stop_ops; local_gIter != NULL; local_gIter = local_gIter->next) {
            pe_action_t *stop = (pe_action_t *) local_gIter->data;

            if (!pcmk_is_set(stop->flags, pe_action_optional)) {
                stop_is_optional = FALSE;
            }

            if (!pcmk_is_set(stop->flags, pe_action_runnable)) {
                crm_debug("%s\t   %s (cancelled : stop un-runnable)",
                          crm_str(stop_node_uname), stopped_mon->uuid);
                pe__clear_action_flags(stopped_mon, pe_action_runnable);
            }

            if (pcmk_is_set(rsc->flags, pe_rsc_managed)) {
                custom_action_order(rsc, stop_key(rsc), stop,
                                    NULL, strdup(key), stopped_mon,
                                    pe_order_implies_then | pe_order_runnable_left, data_set);
            }

        }

        if (stop_ops) {
            g_list_free(stop_ops);
        }

        if (is_optional == FALSE && probe_is_optional && stop_is_optional
            && !pcmk_is_set(rsc->flags, pe_rsc_managed)) {
            pe_rsc_trace(rsc, "Marking %s optional on %s due to unmanaged",
                         key, crm_str(stop_node_uname));
            pe__set_action_flags(stopped_mon, pe_action_optional);
        }

        if (pcmk_is_set(stopped_mon->flags, pe_action_optional)) {
            pe_rsc_trace(rsc, "%s\t   %s (optional)", crm_str(stop_node_uname), stopped_mon->uuid);
        }

        if (stop_node->details->online == FALSE || stop_node->details->unclean) {
            pe_rsc_debug(rsc, "%s\t   %s (cancelled : no node available)",
                         crm_str(stop_node_uname), stopped_mon->uuid);
            pe__clear_action_flags(stopped_mon, pe_action_runnable);
        }

        if (pcmk_is_set(stopped_mon->flags, pe_action_runnable)
            && !pcmk_is_set(stopped_mon->flags, pe_action_optional)) {
            crm_notice(" Start recurring %s (%us) for %s on %s", stopped_mon->task,
                       interval_ms / 1000, rsc->id, crm_str(stop_node_uname));
        }
    }

    free(key);
}

static void
Recurring_Stopped(pe_resource_t * rsc, pe_action_t * start, pe_node_t * node, pe_working_set_t * data_set)
{
    if (!pcmk_is_set(rsc->flags, pe_rsc_maintenance) &&
        (node == NULL || node->details->maintenance == FALSE)) {
        xmlNode *operation = NULL;

        for (operation = pcmk__xe_first_child(rsc->ops_xml);
             operation != NULL;
             operation = pcmk__xe_next(operation)) {

            if (pcmk__str_eq((const char *)operation->name, "op", pcmk__str_none)) {
                RecurringOp_Stopped(rsc, start, node, operation, data_set);
            }
        }
    }
}

static void
handle_migration_actions(pe_resource_t * rsc, pe_node_t *current, pe_node_t *chosen, pe_working_set_t * data_set)
{
    pe_action_t *migrate_to = NULL;
    pe_action_t *migrate_from = NULL;
    pe_action_t *start = NULL;
    pe_action_t *stop = NULL;
    gboolean partial = rsc->partial_migration_target ? TRUE : FALSE;

    pe_rsc_trace(rsc, "Processing migration actions %s moving from %s to %s . partial migration = %s",
    rsc->id, current->details->id, chosen->details->id, partial ? "TRUE" : "FALSE");
    start = start_action(rsc, chosen, TRUE);
    stop = stop_action(rsc, current, TRUE);

    if (partial == FALSE) {
        migrate_to = custom_action(rsc, pcmk__op_key(rsc->id, RSC_MIGRATE, 0),
                                   RSC_MIGRATE, current, TRUE, TRUE, data_set);
    }

    migrate_from = custom_action(rsc, pcmk__op_key(rsc->id, RSC_MIGRATED, 0),
                                 RSC_MIGRATED, chosen, TRUE, TRUE, data_set);

    if ((migrate_to && migrate_from) || (migrate_from && partial)) {

        pe__set_action_flags(start, pe_action_migrate_runnable);
        pe__set_action_flags(stop, pe_action_migrate_runnable);

        // This is easier than trying to delete it from the graph
        pe__set_action_flags(start, pe_action_pseudo);

        /* order probes before migrations */
        if (partial) {
            pe__set_action_flags(migrate_from, pe_action_migrate_runnable);
            migrate_from->needs = start->needs;

            custom_action_order(rsc, pcmk__op_key(rsc->id, RSC_STATUS, 0), NULL,
                                rsc, pcmk__op_key(rsc->id, RSC_MIGRATED, 0),
                                NULL, pe_order_optional, data_set);

        } else {
            pe__set_action_flags(migrate_from, pe_action_migrate_runnable);
            pe__set_action_flags(migrate_to, pe_action_migrate_runnable);
            migrate_to->needs = start->needs;

            custom_action_order(rsc, pcmk__op_key(rsc->id, RSC_STATUS, 0), NULL,
                                rsc, pcmk__op_key(rsc->id, RSC_MIGRATE, 0),
                                NULL, pe_order_optional, data_set);
            custom_action_order(rsc, pcmk__op_key(rsc->id, RSC_MIGRATE, 0),
                                NULL, rsc,
                                pcmk__op_key(rsc->id, RSC_MIGRATED, 0), NULL,
                                pe_order_optional|pe_order_implies_first_migratable,
                                data_set);
        }

        custom_action_order(rsc, pcmk__op_key(rsc->id, RSC_MIGRATED, 0), NULL,
                            rsc, pcmk__op_key(rsc->id, RSC_STOP, 0), NULL,
                            pe_order_optional|pe_order_implies_first_migratable,
                            data_set);
        custom_action_order(rsc, pcmk__op_key(rsc->id, RSC_MIGRATED, 0), NULL,
                            rsc, pcmk__op_key(rsc->id, RSC_START, 0), NULL,
                            pe_order_optional|pe_order_implies_first_migratable|pe_order_pseudo_left,
                            data_set);

    }

    if (migrate_to) {
        add_hash_param(migrate_to->meta, XML_LRM_ATTR_MIGRATE_SOURCE, current->details->uname);
        add_hash_param(migrate_to->meta, XML_LRM_ATTR_MIGRATE_TARGET, chosen->details->uname);

        /* Pacemaker Remote connections don't require pending to be recorded in
         * the CIB. We can reduce CIB writes by not setting PENDING for them.
         */
        if (rsc->is_remote_node == FALSE) {
            /* migrate_to takes place on the source node, but can 
             * have an effect on the target node depending on how
             * the agent is written. Because of this, we have to maintain
             * a record that the migrate_to occurred, in case the source node
             * loses membership while the migrate_to action is still in-flight.
             */
            add_hash_param(migrate_to->meta, XML_OP_ATTR_PENDING, "true");
        }
    }

    if (migrate_from) {
        add_hash_param(migrate_from->meta, XML_LRM_ATTR_MIGRATE_SOURCE, current->details->uname);
        add_hash_param(migrate_from->meta, XML_LRM_ATTR_MIGRATE_TARGET, chosen->details->uname);
    }
}

void
native_create_actions(pe_resource_t * rsc, pe_working_set_t * data_set)
{
    pe_action_t *start = NULL;
    pe_node_t *chosen = NULL;
    pe_node_t *current = NULL;
    gboolean need_stop = FALSE;
    bool need_promote = FALSE;
    gboolean is_moving = FALSE;
    gboolean allow_migrate = pcmk_is_set(rsc->flags, pe_rsc_allow_migrate)? TRUE : FALSE;

    GList *gIter = NULL;
    unsigned int num_all_active = 0;
    unsigned int num_clean_active = 0;
    bool multiply_active = FALSE;
    enum rsc_role_e role = RSC_ROLE_UNKNOWN;
    enum rsc_role_e next_role = RSC_ROLE_UNKNOWN;

    CRM_ASSERT(rsc);
    chosen = rsc->allocated_to;
    next_role = rsc->next_role;
    if (next_role == RSC_ROLE_UNKNOWN) {
        pe__set_next_role(rsc,
                          (chosen == NULL)? RSC_ROLE_STOPPED : RSC_ROLE_STARTED,
                          "allocation");
    }
    pe_rsc_trace(rsc, "Creating all actions for %s transition from %s to %s (%s) on %s",
                 rsc->id, role2text(rsc->role), role2text(rsc->next_role),
                 ((next_role == RSC_ROLE_UNKNOWN)? "implicit" : "explicit"),
                 ((chosen == NULL)? "no node" : chosen->details->uname));

    current = pe__find_active_on(rsc, &num_all_active, &num_clean_active);

    for (gIter = rsc->dangling_migrations; gIter != NULL; gIter = gIter->next) {
        pe_node_t *dangling_source = (pe_node_t *) gIter->data;

        pe_action_t *stop = NULL;

        pe_rsc_trace(rsc, "Creating stop action %sfor %s on %s due to dangling migration",
                     pcmk_is_set(data_set->flags, pe_flag_remove_after_stop)? "and cleanup " : "",
                     rsc->id, dangling_source->details->uname);
        stop = stop_action(rsc, dangling_source, FALSE);
        pe__set_action_flags(stop, pe_action_dangle);
        if (pcmk_is_set(data_set->flags, pe_flag_remove_after_stop)) {
            DeleteRsc(rsc, dangling_source, FALSE, data_set);
        }
    }

    if ((num_all_active == 2) && (num_clean_active == 2) && chosen
        && rsc->partial_migration_source && rsc->partial_migration_target
        && (current->details == rsc->partial_migration_source->details)
        && (chosen->details == rsc->partial_migration_target->details)) {

        /* The chosen node is still the migration target from a partial
         * migration. Attempt to continue the migration instead of recovering
         * by stopping the resource everywhere and starting it on a single node.
         */
        pe_rsc_trace(rsc, "Will attempt to continue with partial migration "
                     "to target %s from %s",
                     rsc->partial_migration_target->details->id,
                     rsc->partial_migration_source->details->id);

    } else if (!pcmk_is_set(rsc->flags, pe_rsc_needs_fencing)) {
        /* If a resource has "requires" set to nothing or quorum, don't consider
         * it active on unclean nodes (similar to how all resources behave when
         * stonith-enabled is false). We can start such resources elsewhere
         * before fencing completes, and if we considered the resource active on
         * the failed node, we would attempt recovery for being active on
         * multiple nodes.
         */
        multiply_active = (num_clean_active > 1);
    } else {
        multiply_active = (num_all_active > 1);
    }

    if (multiply_active) {
        if (rsc->partial_migration_target && rsc->partial_migration_source) {
            // Migration was in progress, but we've chosen a different target
            crm_notice("Resource %s can no longer migrate from %s to %s "
                       "(will stop on both nodes)",
                       rsc->id, rsc->partial_migration_source->details->uname,
                       rsc->partial_migration_target->details->uname);

        } else {
            const char *class = crm_element_value(rsc->xml, XML_AGENT_ATTR_CLASS);

            // Resource was incorrectly multiply active
            pe_proc_err("%s resource %s is active on %u nodes (%s)",
                        crm_str(class), rsc->id, num_all_active,
                        recovery2text(rsc->recovery_type));
            crm_notice("See https://wiki.clusterlabs.org/wiki/FAQ#Resource_is_Too_Active for more information");
        }

        if (rsc->recovery_type == recovery_stop_start) {
            need_stop = TRUE;
        }

        /* If by chance a partial migration is in process, but the migration
         * target is not chosen still, clear all partial migration data.
         */
        rsc->partial_migration_source = rsc->partial_migration_target = NULL;
        allow_migrate = FALSE;
    }

    if (pcmk_is_set(rsc->flags, pe_rsc_start_pending)) {
        pe_rsc_trace(rsc, "Creating start action for %s to represent already pending start",
                     rsc->id);
        start = start_action(rsc, chosen, TRUE);
        pe__set_action_flags(start, pe_action_print_always);
    }

    if (current && chosen && current->details != chosen->details) {
        pe_rsc_trace(rsc, "Moving %s from %s to %s",
                     rsc->id, crm_str(current->details->uname),
                     crm_str(chosen->details->uname));
        is_moving = TRUE;
        need_stop = TRUE;

    } else if (pcmk_is_set(rsc->flags, pe_rsc_failed)) {
        if (pcmk_is_set(rsc->flags, pe_rsc_stop)) {
            need_stop = TRUE;
            pe_rsc_trace(rsc, "Recovering %s", rsc->id);
        } else {
            pe_rsc_trace(rsc, "Recovering %s by demotion", rsc->id);
            if (rsc->next_role == RSC_ROLE_PROMOTED) {
                need_promote = TRUE;
            }
        }

    } else if (pcmk_is_set(rsc->flags, pe_rsc_block)) {
        pe_rsc_trace(rsc, "Blocking further actions on %s", rsc->id);
        need_stop = TRUE;

    } else if (rsc->role > RSC_ROLE_STARTED && current != NULL && chosen != NULL) {
        pe_rsc_trace(rsc, "Creating start action for promoted resource %s",
                     rsc->id);
        start = start_action(rsc, chosen, TRUE);
        if (!pcmk_is_set(start->flags, pe_action_optional)) {
            // Recovery of a promoted resource
            pe_rsc_trace(rsc, "%s restart is required for recovery", rsc->id);
            need_stop = TRUE;
        }
    }

    /* Create any additional actions required when bringing resource down and
     * back up to same level.
     */
    role = rsc->role;
    while (role != RSC_ROLE_STOPPED) {
        next_role = rsc_state_matrix[role][RSC_ROLE_STOPPED];
        pe_rsc_trace(rsc, "Creating %s action to take %s down from %s to %s",
                     (need_stop? "required" : "optional"), rsc->id,
                     role2text(role), role2text(next_role));
        if (rsc_action_matrix[role][next_role] (rsc, current, !need_stop, data_set) == FALSE) {
            break;
        }
        role = next_role;
    }


    while ((rsc->role <= rsc->next_role) && (role != rsc->role)
           && !pcmk_is_set(rsc->flags, pe_rsc_block)) {
        bool required = need_stop;

        next_role = rsc_state_matrix[role][rsc->role];
        if ((next_role == RSC_ROLE_PROMOTED) && need_promote) {
            required = true;
        }
        pe_rsc_trace(rsc, "Creating %s action to take %s up from %s to %s",
                     (required? "required" : "optional"), rsc->id,
                     role2text(role), role2text(next_role));
        if (rsc_action_matrix[role][next_role](rsc, chosen, !required,
                                               data_set) == FALSE) {
            break;
        }
        role = next_role;
    }
    role = rsc->role;

    /* Required steps from this role to the next */
    while (role != rsc->next_role) {
        next_role = rsc_state_matrix[role][rsc->next_role];
        pe_rsc_trace(rsc, "Creating action to take %s from %s to %s (ending at %s)",
                     rsc->id, role2text(role), role2text(next_role),
                     role2text(rsc->next_role));
        if (rsc_action_matrix[role][next_role] (rsc, chosen, FALSE, data_set) == FALSE) {
            break;
        }
        role = next_role;
    }

    if (pcmk_is_set(rsc->flags, pe_rsc_block)) {
        pe_rsc_trace(rsc, "Not creating recurring monitors for blocked resource %s",
                     rsc->id);

    } else if ((rsc->next_role != RSC_ROLE_STOPPED)
               || !pcmk_is_set(rsc->flags, pe_rsc_managed)) {
        pe_rsc_trace(rsc, "Creating recurring monitors for %s resource %s",
                     ((rsc->next_role == RSC_ROLE_STOPPED)? "unmanaged" : "active"),
                     rsc->id);
        start = start_action(rsc, chosen, TRUE);
        Recurring(rsc, start, chosen, data_set);
        Recurring_Stopped(rsc, start, chosen, data_set);

    } else {
        pe_rsc_trace(rsc, "Creating recurring monitors for inactive resource %s",
                     rsc->id);
        Recurring_Stopped(rsc, NULL, NULL, data_set);
    }

    /* if we are stuck in a partial migration, where the target
     * of the partial migration no longer matches the chosen target.
     * A full stop/start is required */
    if (rsc->partial_migration_target && (chosen == NULL || rsc->partial_migration_target->details != chosen->details)) {
        pe_rsc_trace(rsc, "Not allowing partial migration of %s to continue",
                     rsc->id);
        allow_migrate = FALSE;

    } else if (!is_moving || !pcmk_is_set(rsc->flags, pe_rsc_managed)
               || pcmk_any_flags_set(rsc->flags,
                                     pe_rsc_failed|pe_rsc_start_pending)
               || (current && current->details->unclean)
               || rsc->next_role < RSC_ROLE_STARTED) {

        allow_migrate = FALSE;
    }

    if (allow_migrate) {
        handle_migration_actions(rsc, current, chosen, data_set);
    }
}

static void
rsc_avoids_remote_nodes(pe_resource_t *rsc)
{
    GHashTableIter iter;
    pe_node_t *node = NULL;
    g_hash_table_iter_init(&iter, rsc->allowed_nodes);
    while (g_hash_table_iter_next(&iter, NULL, (void **)&node)) {
        if (node->details->remote_rsc) {
            node->weight = -INFINITY;
        }
    }
}

/*!
 * \internal
 * \brief Return allowed nodes as (possibly sorted) list
 *
 * Convert a resource's hash table of allowed nodes to a list. If printing to
 * stdout, sort the list, to keep action ID numbers consistent for regression
 * test output (while avoiding the performance hit on a live cluster).
 *
 * \param[in] rsc       Resource to check for allowed nodes
 * \param[in] data_set  Cluster working set
 *
 * \return List of resource's allowed nodes
 * \note Callers should take care not to rely on the list being sorted.
 */
static GList *
allowed_nodes_as_list(pe_resource_t *rsc, pe_working_set_t *data_set)
{
    GList *allowed_nodes = NULL;

    if (rsc->allowed_nodes) {
        allowed_nodes = g_hash_table_get_values(rsc->allowed_nodes);
    }

    if (!pcmk__is_daemon) {
        allowed_nodes = g_list_sort(allowed_nodes, sort_node_uname);
    }

    return allowed_nodes;
}

void
native_internal_constraints(pe_resource_t * rsc, pe_working_set_t * data_set)
{
    /* This function is on the critical path and worth optimizing as much as possible */

    pe_resource_t *top = NULL;
    GList *allowed_nodes = NULL;
    bool check_unfencing = FALSE;
    bool check_utilization = FALSE;

    if (!pcmk_is_set(rsc->flags, pe_rsc_managed)) {
        pe_rsc_trace(rsc,
                     "Skipping native constraints for unmanaged resource: %s",
                     rsc->id);
        return;
    }

    top = uber_parent(rsc);

    // Whether resource requires unfencing
    check_unfencing = !pcmk_is_set(rsc->flags, pe_rsc_fence_device)
                      && pcmk_is_set(data_set->flags, pe_flag_enable_unfencing)
                      && pcmk_is_set(rsc->flags, pe_rsc_needs_unfencing);

    // Whether a non-default placement strategy is used
    check_utilization = (g_hash_table_size(rsc->utilization) > 0)
                        && !pcmk__str_eq(data_set->placement_strategy,
                                         "default", pcmk__str_casei);

    // Order stops before starts (i.e. restart)
    custom_action_order(rsc, pcmk__op_key(rsc->id, RSC_STOP, 0), NULL,
                        rsc, pcmk__op_key(rsc->id, RSC_START, 0), NULL,
                        pe_order_optional|pe_order_implies_then|pe_order_restart,
                        data_set);

    // Promotable ordering: demote before stop, start before promote
    if (pcmk_is_set(top->flags, pe_rsc_promotable)
        || (rsc->role > RSC_ROLE_UNPROMOTED)) {

        custom_action_order(rsc, pcmk__op_key(rsc->id, RSC_DEMOTE, 0), NULL,
                            rsc, pcmk__op_key(rsc->id, RSC_STOP, 0), NULL,
                            pe_order_promoted_implies_first, data_set);

        custom_action_order(rsc, pcmk__op_key(rsc->id, RSC_START, 0), NULL,
                            rsc, pcmk__op_key(rsc->id, RSC_PROMOTE, 0), NULL,
                            pe_order_runnable_left, data_set);
    }

    // Don't clear resource history if probing on same node
    custom_action_order(rsc, pcmk__op_key(rsc->id, CRM_OP_LRM_DELETE, 0),
                        NULL, rsc, pcmk__op_key(rsc->id, RSC_STATUS, 0),
                        NULL, pe_order_same_node|pe_order_then_cancels_first,
                        data_set);

    // Certain checks need allowed nodes
    if (check_unfencing || check_utilization || rsc->container) {
        allowed_nodes = allowed_nodes_as_list(rsc, data_set);
    }

    if (check_unfencing) {
        /* Check if the node needs to be unfenced first */

        for (GList *item = allowed_nodes; item; item = item->next) {
            pe_node_t *node = item->data;
            pe_action_t *unfence = pe_fence_op(node, "on", TRUE, NULL, FALSE, data_set);

            crm_debug("Ordering any stops of %s before %s, and any starts after",
                      rsc->id, unfence->uuid);

            /*
             * It would be more efficient to order clone resources once,
             * rather than order each instance, but ordering the instance
             * allows us to avoid unnecessary dependencies that might conflict
             * with user constraints.
             *
             * @TODO: This constraint can still produce a transition loop if the
             * resource has a stop scheduled on the node being unfenced, and
             * there is a user ordering constraint to start some other resource
             * (which will be ordered after the unfence) before stopping this
             * resource. An example is "start some slow-starting cloned service
             * before stopping an associated virtual IP that may be moving to
             * it":
             *       stop this -> unfencing -> start that -> stop this
             */
            custom_action_order(rsc, stop_key(rsc), NULL,
                                NULL, strdup(unfence->uuid), unfence,
                                pe_order_optional|pe_order_same_node, data_set);

            custom_action_order(NULL, strdup(unfence->uuid), unfence,
                                rsc, start_key(rsc), NULL,
                                pe_order_implies_then_on_node|pe_order_same_node,
                                data_set);
        }
    }

    if (check_utilization) {
        GList *gIter = NULL;

        pe_rsc_trace(rsc, "Creating utilization constraints for %s - strategy: %s",
                     rsc->id, data_set->placement_strategy);

        for (gIter = rsc->running_on; gIter != NULL; gIter = gIter->next) {
            pe_node_t *current = (pe_node_t *) gIter->data;

            char *load_stopped_task = crm_strdup_printf(LOAD_STOPPED "_%s",
                                                        current->details->uname);
            pe_action_t *load_stopped = get_pseudo_op(load_stopped_task, data_set);

            if (load_stopped->node == NULL) {
                load_stopped->node = pe__copy_node(current);
                pe__clear_action_flags(load_stopped, pe_action_optional);
            }

            custom_action_order(rsc, stop_key(rsc), NULL,
                                NULL, load_stopped_task, load_stopped, pe_order_load, data_set);
        }

        for (GList *item = allowed_nodes; item; item = item->next) {
            pe_node_t *next = item->data;
            char *load_stopped_task = crm_strdup_printf(LOAD_STOPPED "_%s",
                                                        next->details->uname);
            pe_action_t *load_stopped = get_pseudo_op(load_stopped_task, data_set);

            if (load_stopped->node == NULL) {
                load_stopped->node = pe__copy_node(next);
                pe__clear_action_flags(load_stopped, pe_action_optional);
            }

            custom_action_order(NULL, strdup(load_stopped_task), load_stopped,
                                rsc, start_key(rsc), NULL, pe_order_load, data_set);

            custom_action_order(NULL, strdup(load_stopped_task), load_stopped,
                                rsc, pcmk__op_key(rsc->id, RSC_MIGRATE, 0),
                                NULL, pe_order_load, data_set);

            free(load_stopped_task);
        }
    }

    if (rsc->container) {
        pe_resource_t *remote_rsc = NULL;

        if (rsc->is_remote_node) {
            // rsc is the implicit remote connection for a guest or bundle node

            /* Do not allow a guest resource to live on a Pacemaker Remote node,
             * to avoid nesting remotes. However, allow bundles to run on remote
             * nodes.
             */
            if (!pcmk_is_set(rsc->flags, pe_rsc_allow_remote_remotes)) {
                rsc_avoids_remote_nodes(rsc->container);
            }

            /* If someone cleans up a guest or bundle node's container, we will
             * likely schedule a (re-)probe of the container and recovery of the
             * connection. Order the connection stop after the container probe,
             * so that if we detect the container running, we will trigger a new
             * transition and avoid the unnecessary recovery.
             */
            new_rsc_order(rsc->container, RSC_STATUS, rsc, RSC_STOP,
                          pe_order_optional, data_set);

        /* A user can specify that a resource must start on a Pacemaker Remote
         * node by explicitly configuring it with the container=NODENAME
         * meta-attribute. This is of questionable merit, since location
         * constraints can accomplish the same thing. But we support it, so here
         * we check whether a resource (that is not itself a remote connection)
         * has container set to a remote node or guest node resource.
         */
        } else if (rsc->container->is_remote_node) {
            remote_rsc = rsc->container;
        } else  {
            remote_rsc = pe__resource_contains_guest_node(data_set,
                                                          rsc->container);
        }

        if (remote_rsc) {
            /* Force the resource on the Pacemaker Remote node instead of
             * colocating the resource with the container resource.
             */
            for (GList *item = allowed_nodes; item; item = item->next) {
                pe_node_t *node = item->data;

                if (node->details->remote_rsc != remote_rsc) {
                    node->weight = -INFINITY;
                }
            }

        } else {
            /* This resource is either a filler for a container that does NOT
             * represent a Pacemaker Remote node, or a Pacemaker Remote
             * connection resource for a guest node or bundle.
             */
            int score;

            crm_trace("Order and colocate %s relative to its container %s",
                      rsc->id, rsc->container->id);

            custom_action_order(rsc->container,
                                pcmk__op_key(rsc->container->id, RSC_START, 0),
                                NULL, rsc, pcmk__op_key(rsc->id, RSC_START, 0),
                                NULL,
                                pe_order_implies_then|pe_order_runnable_left,
                                data_set);

            custom_action_order(rsc, pcmk__op_key(rsc->id, RSC_STOP, 0), NULL,
                                rsc->container,
                                pcmk__op_key(rsc->container->id, RSC_STOP, 0),
                                NULL, pe_order_implies_first, data_set);

            if (pcmk_is_set(rsc->flags, pe_rsc_allow_remote_remotes)) {
                score = 10000;    /* Highly preferred but not essential */
            } else {
                score = INFINITY; /* Force them to run on the same host */
            }
            pcmk__new_colocation("resource-with-container", NULL, score, rsc,
                                 rsc->container, NULL, NULL, true, data_set);
        }
    }

    if (rsc->is_remote_node || pcmk_is_set(rsc->flags, pe_rsc_fence_device)) {
        /* don't allow remote nodes to run stonith devices
         * or remote connection resources.*/
        rsc_avoids_remote_nodes(rsc);
    }
    g_list_free(allowed_nodes);
}

void
native_rsc_colocation_lh(pe_resource_t *rsc_lh, pe_resource_t *rsc_rh,
                         pcmk__colocation_t *constraint,
                         pe_working_set_t *data_set)
{
    if (rsc_lh == NULL) {
        pe_err("rsc_lh was NULL for %s", constraint->id);
        return;

    } else if (constraint->rsc_rh == NULL) {
        pe_err("rsc_rh was NULL for %s", constraint->id);
        return;
    }

    pe_rsc_trace(rsc_lh, "Processing colocation constraint between %s and %s", rsc_lh->id,
                 rsc_rh->id);

    rsc_rh->cmds->rsc_colocation_rh(rsc_lh, rsc_rh, constraint, data_set);
}

enum filter_colocation_res
filter_colocation_constraint(pe_resource_t * rsc_lh, pe_resource_t * rsc_rh,
                             pcmk__colocation_t *constraint, gboolean preview)
{
    /* rh side must be allocated before we can process constraint */
    if (!preview && pcmk_is_set(rsc_rh->flags, pe_rsc_provisional)) {
        return influence_nothing;
    }

    if ((constraint->role_lh >= RSC_ROLE_UNPROMOTED) &&
        rsc_lh->parent && pcmk_is_set(rsc_lh->parent->flags, pe_rsc_promotable)
        && !pcmk_is_set(rsc_lh->flags, pe_rsc_provisional)) {

        /* LH and RH resources have already been allocated, place the correct
         * priority on LH rsc for the given promotable clone resource role */
        return influence_rsc_priority;
    }

    if (!preview && !pcmk_is_set(rsc_lh->flags, pe_rsc_provisional)) {
        // Log an error if we violated a mandatory colocation constraint
        const pe_node_t *rh_node = rsc_rh->allocated_to;

        if (rsc_lh->allocated_to == NULL) {
            // Dependent resource isn't allocated, so constraint doesn't matter
            return influence_nothing;
        }

        if (constraint->score >= INFINITY) {
            // Dependent resource must colocate with rh_node

            if ((rh_node == NULL)
                || (rh_node->details != rsc_lh->allocated_to->details)) {
                crm_err("%s must be colocated with %s but is not (%s vs. %s)",
                        rsc_lh->id, rsc_rh->id,
                        rsc_lh->allocated_to->details->uname,
                        (rh_node? rh_node->details->uname : "unallocated"));
            }

        } else if (constraint->score <= -INFINITY) {
            // Dependent resource must anti-colocate with rh_node

            if ((rh_node != NULL)
                && (rsc_lh->allocated_to->details == rh_node->details)) {
                crm_err("%s and %s must be anti-colocated but are allocated "
                        "to the same node (%s)",
                        rsc_lh->id, rsc_rh->id, rh_node->details->uname);
            }
        }
        return influence_nothing;
    }

    if (constraint->score > 0
        && constraint->role_lh != RSC_ROLE_UNKNOWN && constraint->role_lh != rsc_lh->next_role) {
        crm_trace("LH: Skipping constraint: \"%s\" state filter nextrole is %s",
                  role2text(constraint->role_lh), role2text(rsc_lh->next_role));
        return influence_nothing;
    }

    if (constraint->score > 0
        && constraint->role_rh != RSC_ROLE_UNKNOWN && constraint->role_rh != rsc_rh->next_role) {
        crm_trace("RH: Skipping constraint: \"%s\" state filter", role2text(constraint->role_rh));
        return influence_nothing;
    }

    if (constraint->score < 0
        && constraint->role_lh != RSC_ROLE_UNKNOWN && constraint->role_lh == rsc_lh->next_role) {
        crm_trace("LH: Skipping negative constraint: \"%s\" state filter",
                  role2text(constraint->role_lh));
        return influence_nothing;
    }

    if (constraint->score < 0
        && constraint->role_rh != RSC_ROLE_UNKNOWN && constraint->role_rh == rsc_rh->next_role) {
        crm_trace("RH: Skipping negative constraint: \"%s\" state filter",
                  role2text(constraint->role_rh));
        return influence_nothing;
    }

    return influence_rsc_location;
}

static void
influence_priority(pe_resource_t *rsc_lh, pe_resource_t *rsc_rh,
                   pcmk__colocation_t *constraint)
{
    const char *rh_value = NULL;
    const char *lh_value = NULL;
    const char *attribute = CRM_ATTR_ID;
    int score_multiplier = 1;

    if (!rsc_rh->allocated_to || !rsc_lh->allocated_to) {
        return;
    }

    if (constraint->node_attribute != NULL) {
        attribute = constraint->node_attribute;
    }

    lh_value = pe_node_attribute_raw(rsc_lh->allocated_to, attribute);
    rh_value = pe_node_attribute_raw(rsc_rh->allocated_to, attribute);

    if (!pcmk__str_eq(lh_value, rh_value, pcmk__str_casei)) {
        if ((constraint->score == INFINITY)
            && (constraint->role_lh == RSC_ROLE_PROMOTED)) {
            rsc_lh->priority = -INFINITY;
        }
        return;
    }

    if (constraint->role_rh && (constraint->role_rh != rsc_rh->next_role)) {
        return;
    }

    if (constraint->role_lh == RSC_ROLE_UNPROMOTED) {
        score_multiplier = -1;
    }

    rsc_lh->priority = pe__add_scores(score_multiplier * constraint->score,
                                      rsc_lh->priority);
}

static void
colocation_match(pe_resource_t *rsc_lh, pe_resource_t *rsc_rh,
                 pcmk__colocation_t *constraint)
{
    const char *attribute = CRM_ATTR_ID;
    const char *value = NULL;
    GHashTable *work = NULL;
    GHashTableIter iter;
    pe_node_t *node = NULL;

    if (constraint->node_attribute != NULL) {
        attribute = constraint->node_attribute;
    }

    if (rsc_rh->allocated_to) {
        value = pe_node_attribute_raw(rsc_rh->allocated_to, attribute);

    } else if (constraint->score < 0) {
        // Nothing to do (anti-colocation with something that is not running)
        return;
    }

    work = pcmk__copy_node_table(rsc_lh->allowed_nodes);

    g_hash_table_iter_init(&iter, work);
    while (g_hash_table_iter_next(&iter, NULL, (void **)&node)) {
        if (rsc_rh->allocated_to == NULL) {
            pe_rsc_trace(rsc_lh, "%s: %s@%s -= %d (%s inactive)",
                         constraint->id, rsc_lh->id, node->details->uname,
                         constraint->score, rsc_rh->id);
            node->weight = pe__add_scores(-constraint->score, node->weight);

        } else if (pcmk__str_eq(pe_node_attribute_raw(node, attribute), value, pcmk__str_casei)) {
            if (constraint->score < CRM_SCORE_INFINITY) {
                pe_rsc_trace(rsc_lh, "%s: %s@%s += %d",
                             constraint->id, rsc_lh->id,
                             node->details->uname, constraint->score);
                node->weight = pe__add_scores(constraint->score, node->weight);
            }

        } else if (constraint->score >= CRM_SCORE_INFINITY) {
            pe_rsc_trace(rsc_lh, "%s: %s@%s -= %d (%s mismatch)",
                         constraint->id, rsc_lh->id, node->details->uname,
                         constraint->score, attribute);
            node->weight = pe__add_scores(-constraint->score, node->weight);
        }
    }

    if (can_run_any(work)
        || constraint->score <= -INFINITY || constraint->score >= INFINITY) {
        g_hash_table_destroy(rsc_lh->allowed_nodes);
        rsc_lh->allowed_nodes = work;
        work = NULL;

    } else {
        pe_rsc_info(rsc_lh,
                    "%s: Rolling back scores from %s (no available nodes)",
                    rsc_lh->id, rsc_rh->id);
    }

    if (work) {
        g_hash_table_destroy(work);
    }
}

void
native_rsc_colocation_rh(pe_resource_t *rsc_lh, pe_resource_t *rsc_rh,
                         pcmk__colocation_t *constraint,
                         pe_working_set_t *data_set)
{
    enum filter_colocation_res filter_results;

    CRM_ASSERT(rsc_lh);
    CRM_ASSERT(rsc_rh);
    filter_results = filter_colocation_constraint(rsc_lh, rsc_rh, constraint, FALSE);
    pe_rsc_trace(rsc_lh, "%s %s with %s (%s, score=%d, filter=%d)",
                 ((constraint->score > 0)? "Colocating" : "Anti-colocating"),
                 rsc_lh->id, rsc_rh->id, constraint->id, constraint->score, filter_results);

    switch (filter_results) {
        case influence_rsc_priority:
            influence_priority(rsc_lh, rsc_rh, constraint);
            break;
        case influence_rsc_location:
            colocation_match(rsc_lh, rsc_rh, constraint);
            break;
        case influence_nothing:
        default:
            return;
    }
}

static gboolean
filter_rsc_ticket(pe_resource_t * rsc_lh, rsc_ticket_t * rsc_ticket)
{
    if (rsc_ticket->role_lh != RSC_ROLE_UNKNOWN && rsc_ticket->role_lh != rsc_lh->role) {
        pe_rsc_trace(rsc_lh, "LH: Skipping constraint: \"%s\" state filter",
                     role2text(rsc_ticket->role_lh));
        return FALSE;
    }

    return TRUE;
}

void
rsc_ticket_constraint(pe_resource_t * rsc_lh, rsc_ticket_t * rsc_ticket, pe_working_set_t * data_set)
{
    if (rsc_ticket == NULL) {
        pe_err("rsc_ticket was NULL");
        return;
    }

    if (rsc_lh == NULL) {
        pe_err("rsc_lh was NULL for %s", rsc_ticket->id);
        return;
    }

    if (rsc_ticket->ticket->granted && rsc_ticket->ticket->standby == FALSE) {
        return;
    }

    if (rsc_lh->children) {
        GList *gIter = rsc_lh->children;

        pe_rsc_trace(rsc_lh, "Processing ticket dependencies from %s", rsc_lh->id);

        for (; gIter != NULL; gIter = gIter->next) {
            pe_resource_t *child_rsc = (pe_resource_t *) gIter->data;

            rsc_ticket_constraint(child_rsc, rsc_ticket, data_set);
        }
        return;
    }

    pe_rsc_trace(rsc_lh, "%s: Processing ticket dependency on %s (%s, %s)",
                 rsc_lh->id, rsc_ticket->ticket->id, rsc_ticket->id,
                 role2text(rsc_ticket->role_lh));

    if ((rsc_ticket->ticket->granted == FALSE)
        && (rsc_lh->running_on != NULL)) {

        GList *gIter = NULL;

        switch (rsc_ticket->loss_policy) {
            case loss_ticket_stop:
                resource_location(rsc_lh, NULL, -INFINITY, "__loss_of_ticket__", data_set);
                break;

            case loss_ticket_demote:
                // Promotion score will be set to -INFINITY in promotion_order()
                if (rsc_ticket->role_lh != RSC_ROLE_PROMOTED) {
                    resource_location(rsc_lh, NULL, -INFINITY, "__loss_of_ticket__", data_set);
                }
                break;

            case loss_ticket_fence:
                if (filter_rsc_ticket(rsc_lh, rsc_ticket) == FALSE) {
                    return;
                }

                resource_location(rsc_lh, NULL, -INFINITY, "__loss_of_ticket__", data_set);

                for (gIter = rsc_lh->running_on; gIter != NULL; gIter = gIter->next) {
                    pe_node_t *node = (pe_node_t *) gIter->data;

                    pe_fence_node(data_set, node, "deadman ticket was lost", FALSE);
                }
                break;

            case loss_ticket_freeze:
                if (filter_rsc_ticket(rsc_lh, rsc_ticket) == FALSE) {
                    return;
                }
                if (rsc_lh->running_on != NULL) {
                    pe__clear_resource_flags(rsc_lh, pe_rsc_managed);
                    pe__set_resource_flags(rsc_lh, pe_rsc_block);
                }
                break;
        }

    } else if (rsc_ticket->ticket->granted == FALSE) {

        if ((rsc_ticket->role_lh != RSC_ROLE_PROMOTED)
            || (rsc_ticket->loss_policy == loss_ticket_stop)) {
            resource_location(rsc_lh, NULL, -INFINITY, "__no_ticket__", data_set);
        }

    } else if (rsc_ticket->ticket->standby) {

        if ((rsc_ticket->role_lh != RSC_ROLE_PROMOTED)
            || (rsc_ticket->loss_policy == loss_ticket_stop)) {
            resource_location(rsc_lh, NULL, -INFINITY, "__ticket_standby__", data_set);
        }
    }
}

enum pe_action_flags
native_action_flags(pe_action_t * action, pe_node_t * node)
{
    return action->flags;
}

static inline bool
is_primitive_action(pe_action_t *action)
{
    return action && action->rsc && (action->rsc->variant == pe_native);
}

/*!
 * \internal
 * \brief Set action bits appropriately when pe_restart_order is used
 *
 * \param[in] first   'First' action in an ordering with pe_restart_order
 * \param[in] then    'Then' action in an ordering with pe_restart_order
 * \param[in] filter  What ordering flags to care about
 *
 * \note pe_restart_order is set for "stop resource before starting it" and
 *       "stop later group member before stopping earlier group member"
 */
static void
handle_restart_ordering(pe_action_t *first, pe_action_t *then,
                        enum pe_action_flags filter)
{
    const char *reason = NULL;

    CRM_ASSERT(is_primitive_action(first));
    CRM_ASSERT(is_primitive_action(then));

    // We need to update the action in two cases:

    // ... if 'then' is required
    if (pcmk_is_set(filter, pe_action_optional)
        && !pcmk_is_set(then->flags, pe_action_optional)) {
        reason = "restart";
    }

    /* ... if 'then' is unrunnable start of managed resource (if a resource
     * should restart but can't start, we still want to stop)
     */
    if (pcmk_is_set(filter, pe_action_runnable)
        && !pcmk_is_set(then->flags, pe_action_runnable)
        && pcmk_is_set(then->rsc->flags, pe_rsc_managed)
        && pcmk__str_eq(then->task, RSC_START, pcmk__str_casei)) {
        reason = "stop";
    }

    if (reason == NULL) {
        return;
    }

    pe_rsc_trace(first->rsc, "Handling %s -> %s for %s",
                 first->uuid, then->uuid, reason);

    // Make 'first' required if it is runnable
    if (pcmk_is_set(first->flags, pe_action_runnable)) {
        pe_action_implies(first, then, pe_action_optional);
    }

    // Make 'first' required if 'then' is required
    if (!pcmk_is_set(then->flags, pe_action_optional)) {
        pe_action_implies(first, then, pe_action_optional);
    }

    // Make 'first' unmigratable if 'then' is unmigratable
    if (!pcmk_is_set(then->flags, pe_action_migrate_runnable)) {
        pe_action_implies(first, then, pe_action_migrate_runnable);
    }

    // Make 'then' unrunnable if 'first' is required but unrunnable
    if (!pcmk_is_set(first->flags, pe_action_optional)
        && !pcmk_is_set(first->flags, pe_action_runnable)) {
        pe_action_implies(then, first, pe_action_runnable);
    }
}

enum pe_graph_flags
native_update_actions(pe_action_t *first, pe_action_t *then, pe_node_t *node,
                      enum pe_action_flags flags, enum pe_action_flags filter,
                      enum pe_ordering type, pe_working_set_t *data_set)
{
    /* flags == get_action_flags(first, then_node) called from update_action() */
    enum pe_graph_flags changed = pe_graph_none;
    enum pe_action_flags then_flags = then->flags;
    enum pe_action_flags first_flags = first->flags;

    crm_trace(   "Testing %s on %s (0x%.6x) with %s 0x%.6x",
                 first->uuid, first->node ? first->node->details->uname : "[none]",
                 first->flags, then->uuid, then->flags);

    if (type & pe_order_asymmetrical) {
        pe_resource_t *then_rsc = then->rsc;
        enum rsc_role_e then_rsc_role = then_rsc ? then_rsc->fns->state(then_rsc, TRUE) : 0;

        if (!then_rsc) {
            /* ignore */
        } else if ((then_rsc_role == RSC_ROLE_STOPPED) && pcmk__str_eq(then->task, RSC_STOP, pcmk__str_casei)) {
            /* ignore... if 'then' is supposed to be stopped after 'first', but
             * then is already stopped, there is nothing to be done when non-symmetrical.  */
        } else if ((then_rsc_role >= RSC_ROLE_STARTED)
                   && pcmk__str_eq(then->task, RSC_START, pcmk__str_casei)
                   && pcmk_is_set(then->flags, pe_action_optional)
                   && then->node
                   && pcmk__list_of_1(then_rsc->running_on)
                   && then->node->details == ((pe_node_t *) then_rsc->running_on->data)->details) {
            /* Ignore. If 'then' is supposed to be started after 'first', but
             * 'then' is already started, there is nothing to be done when
             * asymmetrical -- unless the start is mandatory, which indicates
             * the resource is restarting, and the ordering is still needed.
             */
        } else if (!(first->flags & pe_action_runnable)) {
            /* prevent 'then' action from happening if 'first' is not runnable and
             * 'then' has not yet occurred. */
            pe_action_implies(then, first, pe_action_optional);
            pe_action_implies(then, first, pe_action_runnable);

            pe_rsc_trace(then->rsc, "Unset optional and runnable on %s", then->uuid);
        } else {
            /* ignore... then is allowed to start/stop if it wants to. */
        }
    }

    if (pcmk_is_set(type, pe_order_implies_first)
        && !pcmk_is_set(then_flags, pe_action_optional)) {
        // Then is required, and implies first should be, too

        if (pcmk_is_set(filter, pe_action_optional)
            && !pcmk_is_set(flags, pe_action_optional)
            && pcmk_is_set(first_flags, pe_action_optional)) {
            pe_rsc_trace(first->rsc,
                         "Unset optional on %s because %s implies first",
                         first->uuid, then->uuid);
            pe_action_implies(first, then, pe_action_optional);
        }

        if (pcmk_is_set(flags, pe_action_migrate_runnable) &&
            !pcmk_is_set(then->flags, pe_action_migrate_runnable)) {

            pe_rsc_trace(first->rsc, "Unset migrate runnable on %s because of %s",
                         first->uuid, then->uuid);
            pe_action_implies(first, then, pe_action_migrate_runnable);
        }
    }

    if (type & pe_order_promoted_implies_first) {
        if ((filter & pe_action_optional) &&
            ((then->flags & pe_action_optional) == FALSE) &&
            (then->rsc != NULL) && (then->rsc->role == RSC_ROLE_PROMOTED)) {
            pe_action_implies(first, then, pe_action_optional);

            if (pcmk_is_set(first->flags, pe_action_migrate_runnable) &&
                !pcmk_is_set(then->flags, pe_action_migrate_runnable)) {

                pe_rsc_trace(first->rsc, "Unset migrate runnable on %s because of %s", first->uuid, then->uuid);
                pe_action_implies(first, then, pe_action_migrate_runnable);
            }
            pe_rsc_trace(then->rsc,
                         "Unset optional on %s because %s (promoted) implies first",
                         first->uuid, then->uuid);
        }
    }

    if ((type & pe_order_implies_first_migratable)
        && pcmk_is_set(filter, pe_action_optional)) {

        if (((then->flags & pe_action_migrate_runnable) == FALSE) ||
            ((then->flags & pe_action_runnable) == FALSE)) {

            pe_rsc_trace(then->rsc, "Unset runnable on %s because %s is neither runnable or migratable", first->uuid, then->uuid);
            pe_action_implies(first, then, pe_action_runnable);
        }

        if ((then->flags & pe_action_optional) == 0) {
            pe_rsc_trace(then->rsc, "Unset optional on %s because %s is not optional", first->uuid, then->uuid);
            pe_action_implies(first, then, pe_action_optional);
        }
    }

    if ((type & pe_order_pseudo_left)
        && pcmk_is_set(filter, pe_action_optional)) {

        if ((first->flags & pe_action_runnable) == FALSE) {
            pe_action_implies(then, first, pe_action_migrate_runnable);
            pe__clear_action_flags(then, pe_action_pseudo);
            pe_rsc_trace(then->rsc, "Unset pseudo on %s because %s is not runnable", then->uuid, first->uuid);
        }

    }

    if (pcmk_is_set(type, pe_order_runnable_left)
        && pcmk_is_set(filter, pe_action_runnable)
        && pcmk_is_set(then->flags, pe_action_runnable)
        && !pcmk_is_set(flags, pe_action_runnable)) {
        pe_rsc_trace(then->rsc, "Unset runnable on %s because of %s", then->uuid, first->uuid);
        pe_action_implies(then, first, pe_action_runnable);
        pe_action_implies(then, first, pe_action_migrate_runnable);
    }

    if (pcmk_is_set(type, pe_order_implies_then)
        && pcmk_is_set(filter, pe_action_optional)
        && pcmk_is_set(then->flags, pe_action_optional)
        && !pcmk_is_set(flags, pe_action_optional)
        && !pcmk_is_set(first->flags, pe_action_migrate_runnable)) {

        pe_rsc_trace(then->rsc,
                     "Unset optional on %s because %s implies 'then'",
                     then->uuid, first->uuid);
        pe_action_implies(then, first, pe_action_optional);
    }

    if (pcmk_is_set(type, pe_order_restart)) {
        handle_restart_ordering(first, then, filter);
    }

    if (then_flags != then->flags) {
        pe__set_graph_flags(changed, first, pe_graph_updated_then);
        pe_rsc_trace(then->rsc,
                     "Then: Flags for %s on %s are now  0x%.6x (was 0x%.6x) because of %s 0x%.6x",
                     then->uuid, then->node ? then->node->details->uname : "[none]", then->flags,
                     then_flags, first->uuid, first->flags);

        if(then->rsc && then->rsc->parent) {
            /* "X_stop then X_start" doesn't get handled for cloned groups unless we do this */
            update_action(then, data_set);
        }
    }

    if (first_flags != first->flags) {
        pe__set_graph_flags(changed, first, pe_graph_updated_first);
        pe_rsc_trace(first->rsc,
                     "First: Flags for %s on %s are now  0x%.6x (was 0x%.6x) because of %s 0x%.6x",
                     first->uuid, first->node ? first->node->details->uname : "[none]",
                     first->flags, first_flags, then->uuid, then->flags);
    }

    return changed;
}

void
native_rsc_location(pe_resource_t *rsc, pe__location_t *constraint)
{
    GList *gIter = NULL;
    bool need_role = false;

    CRM_CHECK((constraint != NULL) && (rsc != NULL), return);

    // If a role was specified, ensure constraint is applicable
    need_role = (constraint->role_filter > RSC_ROLE_UNKNOWN);
    if (need_role && (constraint->role_filter != rsc->next_role)) {
        pe_rsc_trace(rsc,
                     "Not applying %s to %s because role will be %s not %s",
                     constraint->id, rsc->id, role2text(rsc->next_role),
                     role2text(constraint->role_filter));
        return;
    }

    if (constraint->node_list_rh == NULL) {
        pe_rsc_trace(rsc, "Not applying %s to %s because no nodes match",
                     constraint->id, rsc->id);
        return;
    }

    pe_rsc_trace(rsc, "Applying %s%s%s to %s", constraint->id,
                 (need_role? " for role " : ""),
                 (need_role? role2text(constraint->role_filter) : ""), rsc->id);

    for (gIter = constraint->node_list_rh; gIter != NULL; gIter = gIter->next) {
        pe_node_t *node = (pe_node_t *) gIter->data;
        pe_node_t *other_node = NULL;

        other_node = (pe_node_t *) pe_hash_table_lookup(rsc->allowed_nodes, node->details->id);

        if (other_node != NULL) {
            pe_rsc_trace(rsc, "* + %d on %s",
                         node->weight, node->details->uname);
            other_node->weight = pe__add_scores(other_node->weight,
                                                node->weight);

        } else {
            pe_rsc_trace(rsc, "* = %d on %s",
                         node->weight, node->details->uname);
            other_node = pe__copy_node(node);
            g_hash_table_insert(rsc->allowed_nodes, (gpointer) other_node->details->id, other_node);
        }

        if (other_node->rsc_discover_mode < constraint->discover_mode) {
            if (constraint->discover_mode == pe_discover_exclusive) {
                rsc->exclusive_discover = TRUE;
            }
            /* exclusive > never > always... always is default */
            other_node->rsc_discover_mode = constraint->discover_mode;
        }
    }
}

void
native_expand(pe_resource_t * rsc, pe_working_set_t * data_set)
{
    GList *gIter = NULL;

    CRM_ASSERT(rsc);
    pe_rsc_trace(rsc, "Processing actions from %s", rsc->id);

    for (gIter = rsc->actions; gIter != NULL; gIter = gIter->next) {
        pe_action_t *action = (pe_action_t *) gIter->data;

        crm_trace("processing action %d for rsc=%s", action->id, rsc->id);
        graph_element_from_action(action, data_set);
    }

    for (gIter = rsc->children; gIter != NULL; gIter = gIter->next) {
        pe_resource_t *child_rsc = (pe_resource_t *) gIter->data;

        child_rsc->cmds->expand(child_rsc, data_set);
    }
}

#define STOP_SANITY_ASSERT(lineno) do {                                 \
        if(current && current->details->unclean) {                      \
            /* It will be a pseudo op */                                \
        } else if(stop == NULL) {                                       \
            crm_err("%s:%d: No stop action exists for %s",              \
                    __func__, lineno, rsc->id);                         \
            CRM_ASSERT(stop != NULL);                                   \
        } else if (pcmk_is_set(stop->flags, pe_action_optional)) {      \
            crm_err("%s:%d: Action %s is still optional",               \
                    __func__, lineno, stop->uuid);                      \
            CRM_ASSERT(!pcmk_is_set(stop->flags, pe_action_optional));  \
        }                                                               \
    } while(0)

void
LogActions(pe_resource_t * rsc, pe_working_set_t * data_set)
{
    pcmk__output_t *out = data_set->priv;

    pe_node_t *next = NULL;
    pe_node_t *current = NULL;

    gboolean moving = FALSE;

    if(rsc->variant == pe_container) {
        pcmk__bundle_log_actions(rsc, data_set);
        return;
    }

    if (rsc->children) {
        GList *gIter = NULL;

        for (gIter = rsc->children; gIter != NULL; gIter = gIter->next) {
            pe_resource_t *child_rsc = (pe_resource_t *) gIter->data;

            LogActions(child_rsc, data_set);
        }
        return;
    }

    next = rsc->allocated_to;
    if (rsc->running_on) {
        current = pe__current_node(rsc);
        if (rsc->role == RSC_ROLE_STOPPED) {
            /*
             * This can occur when resources are being recovered
             * We fiddle with the current role in native_create_actions()
             */
            rsc->role = RSC_ROLE_STARTED;
        }
    }

    if ((current == NULL) && pcmk_is_set(rsc->flags, pe_rsc_orphan)) {
        /* Don't log stopped orphans */
        return;
    }

    out->message(out, "rsc-action", rsc, current, next, moving);
}

gboolean
StopRsc(pe_resource_t * rsc, pe_node_t * next, gboolean optional, pe_working_set_t * data_set)
{
    GList *gIter = NULL;

    CRM_ASSERT(rsc);
    pe_rsc_trace(rsc, "%s", rsc->id);

    for (gIter = rsc->running_on; gIter != NULL; gIter = gIter->next) {
        pe_node_t *current = (pe_node_t *) gIter->data;
        pe_action_t *stop;

        if (rsc->partial_migration_target) {
            if (rsc->partial_migration_target->details == current->details) {
                pe_rsc_trace(rsc, "Filtered %s -> %s %s", current->details->uname,
                             next->details->uname, rsc->id);
                continue;
            } else {
                pe_rsc_trace(rsc, "Forced on %s %s", current->details->uname, rsc->id);
                optional = FALSE;
            }
        }

        pe_rsc_trace(rsc, "%s on %s", rsc->id, current->details->uname);
        stop = stop_action(rsc, current, optional);

        if(rsc->allocated_to == NULL) {
            pe_action_set_reason(stop, "node availability", TRUE);
        }

        if (!pcmk_is_set(rsc->flags, pe_rsc_managed)) {
            pe__clear_action_flags(stop, pe_action_runnable);
        }

        if (pcmk_is_set(data_set->flags, pe_flag_remove_after_stop)) {
            DeleteRsc(rsc, current, optional, data_set);
        }

        if (pcmk_is_set(rsc->flags, pe_rsc_needs_unfencing)) {
            pe_action_t *unfence = pe_fence_op(current, "on", TRUE, NULL, FALSE, data_set);

            order_actions(stop, unfence, pe_order_implies_first);
            if (!node_has_been_unfenced(current)) {
                pe_proc_err("Stopping %s until %s can be unfenced", rsc->id, current->details->uname);
            }
        }
    }

    return TRUE;
}

static void
order_after_unfencing(pe_resource_t *rsc, pe_node_t *node, pe_action_t *action,
                      enum pe_ordering order, pe_working_set_t *data_set)
{
    /* When unfencing is in use, we order unfence actions before any probe or
     * start of resources that require unfencing, and also of fence devices.
     *
     * This might seem to violate the principle that fence devices require
     * only quorum. However, fence agents that unfence often don't have enough
     * information to even probe or start unless the node is first unfenced.
     */
    if (is_unfence_device(rsc, data_set)
        || pcmk_is_set(rsc->flags, pe_rsc_needs_unfencing)) {

        /* Start with an optional ordering. Requiring unfencing would result in
         * the node being unfenced, and all its resources being stopped,
         * whenever a new resource is added -- which would be highly suboptimal.
         */
        pe_action_t *unfence = pe_fence_op(node, "on", TRUE, NULL, FALSE, data_set);

        order_actions(unfence, action, order);

        if (!node_has_been_unfenced(node)) {
            // But unfencing is required if it has never been done
            char *reason = crm_strdup_printf("required by %s %s",
                                             rsc->id, action->task);

            trigger_unfencing(NULL, node, reason, NULL, data_set);
            free(reason);
        }
    }
}

gboolean
StartRsc(pe_resource_t * rsc, pe_node_t * next, gboolean optional, pe_working_set_t * data_set)
{
    pe_action_t *start = NULL;

    CRM_ASSERT(rsc);
    pe_rsc_trace(rsc, "%s on %s %d %d", rsc->id, next ? next->details->uname : "N/A", optional, next ? next->weight : 0);
    start = start_action(rsc, next, TRUE);

    order_after_unfencing(rsc, next, start, pe_order_implies_then, data_set);

    if (pcmk_is_set(start->flags, pe_action_runnable) && !optional) {
        pe__clear_action_flags(start, pe_action_optional);
    }


    return TRUE;
}

gboolean
PromoteRsc(pe_resource_t * rsc, pe_node_t * next, gboolean optional, pe_working_set_t * data_set)
{
    GList *gIter = NULL;
    gboolean runnable = TRUE;
    GList *action_list = NULL;

    CRM_ASSERT(rsc);
    CRM_CHECK(next != NULL, return FALSE);
    pe_rsc_trace(rsc, "%s on %s", rsc->id, next->details->uname);

    action_list = pe__resource_actions(rsc, next, RSC_START, TRUE);

    for (gIter = action_list; gIter != NULL; gIter = gIter->next) {
        pe_action_t *start = (pe_action_t *) gIter->data;

        if (!pcmk_is_set(start->flags, pe_action_runnable)) {
            runnable = FALSE;
        }
    }
    g_list_free(action_list);

    if (runnable) {
        promote_action(rsc, next, optional);
        return TRUE;
    }

    pe_rsc_debug(rsc, "%s\tPromote %s (canceled)", next->details->uname, rsc->id);

    action_list = pe__resource_actions(rsc, next, RSC_PROMOTE, TRUE);

    for (gIter = action_list; gIter != NULL; gIter = gIter->next) {
        pe_action_t *promote = (pe_action_t *) gIter->data;

        pe__clear_action_flags(promote, pe_action_runnable);
    }

    g_list_free(action_list);
    return TRUE;
}

gboolean
DemoteRsc(pe_resource_t * rsc, pe_node_t * next, gboolean optional, pe_working_set_t * data_set)
{
    GList *gIter = NULL;

    CRM_ASSERT(rsc);
    pe_rsc_trace(rsc, "%s", rsc->id);

    /* CRM_CHECK(rsc->next_role == RSC_ROLE_UNPROMOTED, return FALSE); */
    for (gIter = rsc->running_on; gIter != NULL; gIter = gIter->next) {
        pe_node_t *current = (pe_node_t *) gIter->data;

        pe_rsc_trace(rsc, "%s on %s", rsc->id, next ? next->details->uname : "N/A");
        demote_action(rsc, current, optional);
    }
    return TRUE;
}

gboolean
RoleError(pe_resource_t * rsc, pe_node_t * next, gboolean optional, pe_working_set_t * data_set)
{
    CRM_ASSERT(rsc);
    crm_err("%s on %s", rsc->id, next ? next->details->uname : "N/A");
    CRM_CHECK(FALSE, return FALSE);
    return FALSE;
}

gboolean
NullOp(pe_resource_t * rsc, pe_node_t * next, gboolean optional, pe_working_set_t * data_set)
{
    CRM_ASSERT(rsc);
    pe_rsc_trace(rsc, "%s", rsc->id);
    return FALSE;
}

gboolean
DeleteRsc(pe_resource_t * rsc, pe_node_t * node, gboolean optional, pe_working_set_t * data_set)
{
    if (pcmk_is_set(rsc->flags, pe_rsc_failed)) {
        pe_rsc_trace(rsc, "Resource %s not deleted from %s: failed", rsc->id, node->details->uname);
        return FALSE;

    } else if (node == NULL) {
        pe_rsc_trace(rsc, "Resource %s not deleted: NULL node", rsc->id);
        return FALSE;

    } else if (node->details->unclean || node->details->online == FALSE) {
        pe_rsc_trace(rsc, "Resource %s not deleted from %s: unrunnable", rsc->id,
                     node->details->uname);
        return FALSE;
    }

    crm_notice("Removing %s from %s", rsc->id, node->details->uname);

    delete_action(rsc, node, optional);

    new_rsc_order(rsc, RSC_STOP, rsc, RSC_DELETE,
                  optional ? pe_order_implies_then : pe_order_optional, data_set);

    new_rsc_order(rsc, RSC_DELETE, rsc, RSC_START,
                  optional ? pe_order_implies_then : pe_order_optional, data_set);

    return TRUE;
}

gboolean
native_create_probe(pe_resource_t * rsc, pe_node_t * node, pe_action_t * complete,
                    gboolean force, pe_working_set_t * data_set)
{
    enum pe_ordering flags = pe_order_optional;
    char *key = NULL;
    pe_action_t *probe = NULL;
    pe_node_t *running = NULL;
    pe_node_t *allowed = NULL;
    pe_resource_t *top = uber_parent(rsc);

    static const char *rc_promoted = NULL;
    static const char *rc_inactive = NULL;

    if (rc_inactive == NULL) {
        rc_inactive = pcmk__itoa(PCMK_OCF_NOT_RUNNING);
        rc_promoted = pcmk__itoa(PCMK_OCF_RUNNING_PROMOTED);
    }

    CRM_CHECK(node != NULL, return FALSE);
    if (!force && !pcmk_is_set(data_set->flags, pe_flag_startup_probes)) {
        pe_rsc_trace(rsc, "Skipping active resource detection for %s", rsc->id);
        return FALSE;
    }

    if (pe__is_guest_or_remote_node(node)) {
        const char *class = crm_element_value(rsc->xml, XML_AGENT_ATTR_CLASS);

        if (pcmk__str_eq(class, PCMK_RESOURCE_CLASS_STONITH, pcmk__str_casei)) {
            pe_rsc_trace(rsc,
                         "Skipping probe for %s on %s because Pacemaker Remote nodes cannot run stonith agents",
                         rsc->id, node->details->id);
            return FALSE;
        } else if (pe__is_guest_node(node)
                   && pe__resource_contains_guest_node(data_set, rsc)) {
            pe_rsc_trace(rsc,
                         "Skipping probe for %s on %s because guest nodes cannot run resources containing guest nodes",
                         rsc->id, node->details->id);
            return FALSE;
        } else if (rsc->is_remote_node) {
            pe_rsc_trace(rsc,
                         "Skipping probe for %s on %s because Pacemaker Remote nodes cannot host remote connections",
                         rsc->id, node->details->id);
            return FALSE;
        }
    }

    if (rsc->children) {
        GList *gIter = NULL;
        gboolean any_created = FALSE;

        for (gIter = rsc->children; gIter != NULL; gIter = gIter->next) {
            pe_resource_t *child_rsc = (pe_resource_t *) gIter->data;

            any_created = child_rsc->cmds->create_probe(child_rsc, node, complete, force, data_set)
                || any_created;
        }

        return any_created;

    } else if ((rsc->container) && (!rsc->is_remote_node)) {
        pe_rsc_trace(rsc, "Skipping %s: it is within container %s", rsc->id, rsc->container->id);
        return FALSE;
    }

    if (pcmk_is_set(rsc->flags, pe_rsc_orphan)) {
        pe_rsc_trace(rsc, "Skipping orphan: %s", rsc->id);
        return FALSE;
    }

    // Check whether resource is already known on node
    if (!force && g_hash_table_lookup(rsc->known_on, node->details->id)) {
        pe_rsc_trace(rsc, "Skipping known: %s on %s", rsc->id, node->details->uname);
        return FALSE;
    }

    allowed = g_hash_table_lookup(rsc->allowed_nodes, node->details->id);

    if (rsc->exclusive_discover || top->exclusive_discover) {
        if (allowed == NULL) {
            /* exclusive discover is enabled and this node is not in the allowed list. */    
            pe_rsc_trace(rsc, "Skipping probe for %s on node %s, A", rsc->id, node->details->id);
            return FALSE;
        } else if (allowed->rsc_discover_mode != pe_discover_exclusive) {
            /* exclusive discover is enabled and this node is not marked
             * as a node this resource should be discovered on */ 
            pe_rsc_trace(rsc, "Skipping probe for %s on node %s, B", rsc->id, node->details->id);
            return FALSE;
        }
    }

    if(allowed == NULL && node->rsc_discover_mode == pe_discover_never) {
        /* If this node was allowed to host this resource it would
         * have been explicitly added to the 'allowed_nodes' list.
         * However it wasn't and the node has discovery disabled, so
         * no need to probe for this resource.
         */
        pe_rsc_trace(rsc, "Skipping probe for %s on node %s, C", rsc->id, node->details->id);
        return FALSE;
    }

    if (allowed && allowed->rsc_discover_mode == pe_discover_never) {
        /* this resource is marked as not needing to be discovered on this node */
        pe_rsc_trace(rsc, "Skipping probe for %s on node %s, discovery mode", rsc->id, node->details->id);
        return FALSE;
    }

    if (pe__is_guest_node(node)) {
        pe_resource_t *remote = node->details->remote_rsc->container;

        if(remote->role == RSC_ROLE_STOPPED) {
            /* If the container is stopped, then we know anything that
             * might have been inside it is also stopped and there is
             * no need to probe.
             *
             * If we don't know the container's state on the target
             * either:
             *
             * - the container is running, the transition will abort
             *   and we'll end up in a different case next time, or
             *
             * - the container is stopped
             *
             * Either way there is no need to probe.
             *
             */
            if(remote->allocated_to
               && g_hash_table_lookup(remote->known_on, remote->allocated_to->details->id) == NULL) {
                /* For safety, we order the 'rsc' start after 'remote'
                 * has been probed.
                 *
                 * Using 'top' helps for groups, but we may need to
                 * follow the start's ordering chain backwards.
                 */
                custom_action_order(remote,
                                    pcmk__op_key(remote->id, RSC_STATUS, 0),
                                    NULL, top,
                                    pcmk__op_key(top->id, RSC_START, 0), NULL,
                                    pe_order_optional, data_set);
            }
            pe_rsc_trace(rsc, "Skipping probe for %s on node %s, %s is stopped",
                         rsc->id, node->details->id, remote->id);
            return FALSE;

            /* Here we really we want to check if remote->stop is required,
             * but that information doesn't exist yet
             */
        } else if(node->details->remote_requires_reset
                  || node->details->unclean
                  || pcmk_is_set(remote->flags, pe_rsc_failed)
                  || remote->next_role == RSC_ROLE_STOPPED
                  || (remote->allocated_to
                      && pe_find_node(remote->running_on, remote->allocated_to->details->uname) == NULL)
            ) {
            /* The container is stopping or restarting, don't start
             * 'rsc' until 'remote' stops as this also implies that
             * 'rsc' is stopped - avoiding the need to probe
             */
            custom_action_order(remote, pcmk__op_key(remote->id, RSC_STOP, 0),
                                NULL, top, pcmk__op_key(top->id, RSC_START, 0),
                                NULL, pe_order_optional, data_set);
        pe_rsc_trace(rsc, "Skipping probe for %s on node %s, %s is stopping, restarting or moving",
                     rsc->id, node->details->id, remote->id);
            return FALSE;
/*      } else {
 *            The container is running so there is no problem probing it
 */
        }
    }

    key = pcmk__op_key(rsc->id, RSC_STATUS, 0);
    probe = custom_action(rsc, key, RSC_STATUS, node, FALSE, TRUE, data_set);
    pe__clear_action_flags(probe, pe_action_optional);

    order_after_unfencing(rsc, node, probe, pe_order_optional, data_set);

    /*
     * We need to know if it's running_on (not just known_on) this node
     * to correctly determine the target rc.
     */
    running = pe_find_node_id(rsc->running_on, node->details->id);
    if (running == NULL) {
        add_hash_param(probe->meta, XML_ATTR_TE_TARGET_RC, rc_inactive);

    } else if (rsc->role == RSC_ROLE_PROMOTED) {
        add_hash_param(probe->meta, XML_ATTR_TE_TARGET_RC, rc_promoted);
    }

    crm_debug("Probing %s on %s (%s) %d %p", rsc->id, node->details->uname, role2text(rsc->role),
              pcmk_is_set(probe->flags, pe_action_runnable), rsc->running_on);

    if (is_unfence_device(rsc, data_set) || !pe_rsc_is_clone(top)) {
        top = rsc;
    } else {
        crm_trace("Probing %s on %s (%s) as %s", rsc->id, node->details->uname, role2text(rsc->role), top->id);
    }

    if (!pcmk_is_set(probe->flags, pe_action_runnable)
        && (rsc->running_on == NULL)) {
        /* Prevent the start from occurring if rsc isn't active, but
         * don't cause it to stop if it was active already
         */
        pe__set_order_flags(flags, pe_order_runnable_left);
    }

    custom_action_order(rsc, NULL, probe,
                        top, pcmk__op_key(top->id, RSC_START, 0), NULL,
                        flags, data_set);

    // Order the probe before any agent reload
    custom_action_order(rsc, NULL, probe,
                        top, reload_key(rsc), NULL,
                        pe_order_optional, data_set);

#if 0
    // complete is always null currently
    if (!is_unfence_device(rsc, data_set)) {
        /* Normally rsc.start depends on probe complete which depends
         * on rsc.probe. But this can't be the case for fence devices
         * with unfencing, as it would create graph loops.
         *
         * So instead we explicitly order 'rsc.probe then rsc.start'
         */
        order_actions(probe, complete, pe_order_implies_then);
    }
#endif
    return TRUE;
}

/*!
 * \internal
 * \brief Check whether a resource is known on a particular node
 *
 * \param[in] rsc   Resource to check
 * \param[in] node  Node to check
 *
 * \return TRUE if resource (or parent if an anonymous clone) is known
 */
static bool
rsc_is_known_on(pe_resource_t *rsc, const pe_node_t *node)
{
   if (pe_hash_table_lookup(rsc->known_on, node->details->id)) {
       return TRUE;

   } else if ((rsc->variant == pe_native)
              && pe_rsc_is_anon_clone(rsc->parent)
              && pe_hash_table_lookup(rsc->parent->known_on, node->details->id)) {
       /* We check only the parent, not the uber-parent, because we cannot
        * assume that the resource is known if it is in an anonymously cloned
        * group (which may be only partially known).
        */
       return TRUE;
   }
   return FALSE;
}

/*!
 * \internal
 * \brief Order a resource's start and promote actions relative to fencing
 *
 * \param[in] rsc         Resource to be ordered
 * \param[in] stonith_op  Fence action
 * \param[in] data_set    Cluster information
 */
static void
native_start_constraints(pe_resource_t * rsc, pe_action_t * stonith_op, pe_working_set_t * data_set)
{
    pe_node_t *target;
    GList *gIter = NULL;

    CRM_CHECK(stonith_op && stonith_op->node, return);
    target = stonith_op->node;

    for (gIter = rsc->actions; gIter != NULL; gIter = gIter->next) {
        pe_action_t *action = (pe_action_t *) gIter->data;

        switch (action->needs) {
            case rsc_req_nothing:
                // Anything other than start or promote requires nothing
                break;

            case rsc_req_stonith:
                order_actions(stonith_op, action, pe_order_optional);
                break;

            case rsc_req_quorum:
                if (pcmk__str_eq(action->task, RSC_START, pcmk__str_casei)
                    && pe_hash_table_lookup(rsc->allowed_nodes, target->details->id)
                    && !rsc_is_known_on(rsc, target)) {

                    /* If we don't know the status of the resource on the node
                     * we're about to shoot, we have to assume it may be active
                     * there. Order the resource start after the fencing. This
                     * is analogous to waiting for all the probes for a resource
                     * to complete before starting it.
                     *
                     * The most likely explanation is that the DC died and took
                     * its status with it.
                     */
                    pe_rsc_debug(rsc, "Ordering %s after %s recovery", action->uuid,
                                 target->details->uname);
                    order_actions(stonith_op, action,
                                  pe_order_optional | pe_order_runnable_left);
                }
                break;
        }
    }
}

static void
native_stop_constraints(pe_resource_t * rsc, pe_action_t * stonith_op, pe_working_set_t * data_set)
{
    GList *gIter = NULL;
    GList *action_list = NULL;
    bool order_implicit = false;

    pe_resource_t *top = uber_parent(rsc);
    pe_action_t *parent_stop = NULL;
    pe_node_t *target;

    CRM_CHECK(stonith_op && stonith_op->node, return);
    target = stonith_op->node;

    /* Get a list of stop actions potentially implied by the fencing */
    action_list = pe__resource_actions(rsc, target, RSC_STOP, FALSE);

    /* If resource requires fencing, implicit actions must occur after fencing.
     *
     * Implied stops and demotes of resources running on guest nodes are always
     * ordered after fencing, even if the resource does not require fencing,
     * because guest node "fencing" is actually just a resource stop.
     */
    if (pcmk_is_set(rsc->flags, pe_rsc_needs_fencing)
        || pe__is_guest_node(target)) {

        order_implicit = true;
    }

    if (action_list && order_implicit) {
        parent_stop = find_first_action(top->actions, NULL, RSC_STOP, NULL);
    }

    for (gIter = action_list; gIter != NULL; gIter = gIter->next) {
        pe_action_t *action = (pe_action_t *) gIter->data;

        // The stop would never complete, so convert it into a pseudo-action.
        pe__set_action_flags(action, pe_action_pseudo|pe_action_runnable);

        if (order_implicit) {
            pe__set_action_flags(action, pe_action_implied_by_stonith);

            /* Order the stonith before the parent stop (if any).
             *
             * Also order the stonith before the resource stop, unless the
             * resource is inside a bundle -- that would cause a graph loop.
             * We can rely on the parent stop's ordering instead.
             *
             * User constraints must not order a resource in a guest node
             * relative to the guest node container resource. The
             * pe_order_preserve flag marks constraints as generated by the
             * cluster and thus immune to that check (and is irrelevant if
             * target is not a guest).
             */
            if (!pe_rsc_is_bundled(rsc)) {
                order_actions(stonith_op, action, pe_order_preserve);
            }
            order_actions(stonith_op, parent_stop, pe_order_preserve);
        }

        if (pcmk_is_set(rsc->flags, pe_rsc_failed)) {
            crm_notice("Stop of failed resource %s is implicit %s %s is fenced",
                       rsc->id, (order_implicit? "after" : "because"),
                       target->details->uname);
        } else {
            crm_info("%s is implicit %s %s is fenced",
                     action->uuid, (order_implicit? "after" : "because"),
                     target->details->uname);
        }

        if (pcmk_is_set(rsc->flags, pe_rsc_notify)) {
            /* Create a second notification that will be delivered
             *   immediately after the node is fenced
             *
             * Basic problem:
             * - C is a clone active on the node to be shot and stopping on another
             * - R is a resource that depends on C
             *
             * + C.stop depends on R.stop
             * + C.stopped depends on STONITH
             * + C.notify depends on C.stopped
             * + C.healthy depends on C.notify
             * + R.stop depends on C.healthy
             *
             * The extra notification here changes
             *  + C.healthy depends on C.notify
             * into:
             *  + C.healthy depends on C.notify'
             *  + C.notify' depends on STONITH'
             * thus breaking the loop
             */
            create_secondary_notification(action, rsc, stonith_op, data_set);
        }

/* From Bug #1601, successful fencing must be an input to a failed resources stop action.

   However given group(rA, rB) running on nodeX and B.stop has failed,
   A := stop healthy resource (rA.stop)
   B := stop failed resource (pseudo operation B.stop)
   C := stonith nodeX
   A requires B, B requires C, C requires A
   This loop would prevent the cluster from making progress.

   This block creates the "C requires A" dependency and therefore must (at least
   for now) be disabled.

   Instead, run the block above and treat all resources on nodeX as B would be
   (marked as a pseudo op depending on the STONITH).

   TODO: Break the "A requires B" dependency in update_action() and re-enable this block

   } else if(is_stonith == FALSE) {
   crm_info("Moving healthy resource %s"
   " off %s before fencing",
   rsc->id, node->details->uname);

   * stop healthy resources before the
   * stonith op
   *
   custom_action_order(
   rsc, stop_key(rsc), NULL,
   NULL,strdup(CRM_OP_FENCE),stonith_op,
   pe_order_optional, data_set);
*/
    }

    g_list_free(action_list);

    /* Get a list of demote actions potentially implied by the fencing */
    action_list = pe__resource_actions(rsc, target, RSC_DEMOTE, FALSE);

    for (gIter = action_list; gIter != NULL; gIter = gIter->next) {
        pe_action_t *action = (pe_action_t *) gIter->data;

        if (action->node->details->online == FALSE || action->node->details->unclean == TRUE
            || pcmk_is_set(rsc->flags, pe_rsc_failed)) {

            if (pcmk_is_set(rsc->flags, pe_rsc_failed)) {
                pe_rsc_info(rsc,
                            "Demote of failed resource %s is implicit after %s is fenced",
                            rsc->id, target->details->uname);
            } else {
                pe_rsc_info(rsc, "%s is implicit after %s is fenced",
                            action->uuid, target->details->uname);
            }

            /* The demote would never complete and is now implied by the
             * fencing, so convert it into a pseudo-action.
             */
            pe__set_action_flags(action, pe_action_pseudo|pe_action_runnable);

            if (pe_rsc_is_bundled(rsc)) {
                /* Do nothing, let the recovery be ordered after the parent's implied stop */

            } else if (order_implicit) {
                order_actions(stonith_op, action, pe_order_preserve|pe_order_optional);
            }
        }
    }

    g_list_free(action_list);
}

void
rsc_stonith_ordering(pe_resource_t * rsc, pe_action_t * stonith_op, pe_working_set_t * data_set)
{
    if (rsc->children) {
        GList *gIter = NULL;

        for (gIter = rsc->children; gIter != NULL; gIter = gIter->next) {
            pe_resource_t *child_rsc = (pe_resource_t *) gIter->data;

            rsc_stonith_ordering(child_rsc, stonith_op, data_set);
        }

    } else if (!pcmk_is_set(rsc->flags, pe_rsc_managed)) {
        pe_rsc_trace(rsc, "Skipping fencing constraints for unmanaged resource: %s", rsc->id);

    } else {
        native_start_constraints(rsc, stonith_op, data_set);
        native_stop_constraints(rsc, stonith_op, data_set);
    }
}

void
ReloadRsc(pe_resource_t * rsc, pe_node_t *node, pe_working_set_t * data_set)
{
    GList *gIter = NULL;
    pe_action_t *reload = NULL;

    if (rsc->children) {
        for (gIter = rsc->children; gIter != NULL; gIter = gIter->next) {
            pe_resource_t *child_rsc = (pe_resource_t *) gIter->data;

            ReloadRsc(child_rsc, node, data_set);
        }
        return;

    } else if (rsc->variant > pe_native) {
        /* Complex resource with no children */
        return;

    } else if (!pcmk_is_set(rsc->flags, pe_rsc_managed)) {
        pe_rsc_trace(rsc, "%s: unmanaged", rsc->id);
        return;

    } else if (pcmk_is_set(rsc->flags, pe_rsc_failed)) {
        /* We don't need to specify any particular actions here, normal failure
         * recovery will apply.
         */
        pe_rsc_trace(rsc, "%s: preventing agent reload because failed",
                     rsc->id);
        return;

    } else if (pcmk_is_set(rsc->flags, pe_rsc_start_pending)) {
        /* If a resource's configuration changed while a start was pending,
         * force a full restart.
         */
        pe_rsc_trace(rsc, "%s: preventing agent reload because start pending",
                     rsc->id);
        stop_action(rsc, node, FALSE);
        return;

    } else if (node == NULL) {
        pe_rsc_trace(rsc, "%s: not active", rsc->id);
        return;
    }

    pe_rsc_trace(rsc, "Processing %s", rsc->id);
    pe__set_resource_flags(rsc, pe_rsc_reload);

    reload = custom_action(rsc, reload_key(rsc), CRMD_ACTION_RELOAD_AGENT, node,
                           FALSE, TRUE, data_set);
    pe_action_set_reason(reload, "resource definition change", FALSE);

    custom_action_order(NULL, NULL, reload, rsc, stop_key(rsc), NULL,
                        pe_order_optional|pe_order_then_cancels_first,
                        data_set);
    custom_action_order(NULL, NULL, reload, rsc, demote_key(rsc), NULL,
                        pe_order_optional|pe_order_then_cancels_first,
                        data_set);
}

void
native_append_meta(pe_resource_t * rsc, xmlNode * xml)
{
    char *value = g_hash_table_lookup(rsc->meta, XML_RSC_ATTR_INCARNATION);
    pe_resource_t *parent;

    if (value) {
        char *name = NULL;

        name = crm_meta_name(XML_RSC_ATTR_INCARNATION);
        crm_xml_add(xml, name, value);
        free(name);
    }

    value = g_hash_table_lookup(rsc->meta, XML_RSC_ATTR_REMOTE_NODE);
    if (value) {
        char *name = NULL;

        name = crm_meta_name(XML_RSC_ATTR_REMOTE_NODE);
        crm_xml_add(xml, name, value);
        free(name);
    }

    for (parent = rsc; parent != NULL; parent = parent->parent) {
        if (parent->container) {
            crm_xml_add(xml, CRM_META"_"XML_RSC_ATTR_CONTAINER, parent->container->id);
        }
    }
}
