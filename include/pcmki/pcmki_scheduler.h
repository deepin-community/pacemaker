/*
 * Copyright 2014-2021 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU Lesser General Public License
 * version 2.1 or later (LGPLv2.1+) WITHOUT ANY WARRANTY.
 */

#ifndef PENGINE__H
#  define PENGINE__H

typedef struct rsc_ticket_s rsc_ticket_t;
typedef struct lrm_agent_s lrm_agent_t;

#  include <glib.h>
#  include <crm/crm.h>
#  include <crm/common/iso8601.h>
#  include <crm/pengine/rules.h>
#  include <crm/pengine/common.h>
#  include <crm/pengine/status.h>

#  include <crm/pengine/complex.h>

enum pe_stop_fail {
    pesf_block,
    pesf_stonith,
    pesf_ignore
};

enum pe_weights {
    pe_weights_none = 0x0,
    pe_weights_init = 0x1,
    pe_weights_forward = 0x4,
    pe_weights_positive = 0x8,
    pe_weights_rollback = 0x10,
};

typedef struct {
    const char *id;
    const char *node_attribute;
    pe_resource_t *rsc_lh;
    pe_resource_t *rsc_rh;

    int role_lh;
    int role_rh;

    int score;
    bool influence; // Whether rsc_lh should influence active rsc_rh placement
} pcmk__colocation_t;

enum loss_ticket_policy_e {
    loss_ticket_stop,
    loss_ticket_demote,
    loss_ticket_fence,
    loss_ticket_freeze
};

struct rsc_ticket_s {
    const char *id;
    pe_resource_t *rsc_lh;
    pe_ticket_t *ticket;
    enum loss_ticket_policy_e loss_policy;

    int role_lh;
};

extern gboolean stage0(pe_working_set_t * data_set);
extern gboolean probe_resources(pe_working_set_t * data_set);
extern gboolean stage2(pe_working_set_t * data_set);
extern gboolean stage3(pe_working_set_t * data_set);
extern gboolean stage4(pe_working_set_t * data_set);
extern gboolean stage5(pe_working_set_t * data_set);
extern gboolean stage6(pe_working_set_t * data_set);
extern gboolean stage7(pe_working_set_t * data_set);
extern gboolean stage8(pe_working_set_t * data_set);

extern gboolean summary(GList *resources);

extern gboolean unpack_constraints(xmlNode * xml_constraints, pe_working_set_t * data_set);

extern gboolean shutdown_constraints(pe_node_t * node, pe_action_t * shutdown_op,
                                     pe_working_set_t * data_set);

void pcmk__order_vs_fence(pe_action_t *stonith_op, pe_working_set_t *data_set);

extern int custom_action_order(pe_resource_t * lh_rsc, char *lh_task, pe_action_t * lh_action,
                               pe_resource_t * rh_rsc, char *rh_task, pe_action_t * rh_action,
                               enum pe_ordering type, pe_working_set_t * data_set);

extern int new_rsc_order(pe_resource_t * lh_rsc, const char *lh_task,
                         pe_resource_t * rh_rsc, const char *rh_task,
                         enum pe_ordering type, pe_working_set_t * data_set);

#  define order_start_start(rsc1,rsc2, type)				\
    new_rsc_order(rsc1, CRMD_ACTION_START, rsc2, CRMD_ACTION_START, type, data_set)
#  define order_stop_stop(rsc1, rsc2, type)				\
    new_rsc_order(rsc1, CRMD_ACTION_STOP, rsc2, CRMD_ACTION_STOP, type, data_set)

extern void graph_element_from_action(pe_action_t * action, pe_working_set_t * data_set);
extern void add_maintenance_update(pe_working_set_t *data_set);
xmlNode *pcmk__schedule_actions(pe_working_set_t *data_set, xmlNode *xml_input,
                                crm_time_t *now);
bool pcmk__ordering_is_invalid(pe_action_t *action, pe_action_wrapper_t *input);

extern const char *transition_idle_timeout;

/*!
 * \internal
 * \brief Check whether colocation's left-hand preferences should be considered
 *
 * \param[in] colocation  Colocation constraint
 * \param[in] rsc         Right-hand instance (normally this will be
 *                        colocation->rsc_rh, which NULL will be treated as,
 *                        but for clones or bundles with multiple instances
 *                        this can be a particular instance)
 *
 * \return true if colocation influence should be effective, otherwise false
 */
static inline bool
pcmk__colocation_has_influence(const pcmk__colocation_t *colocation,
                               const pe_resource_t *rsc)
{
    if (rsc == NULL) {
        rsc = colocation->rsc_rh;
    }

    /* The left hand of a colocation influences the right hand's location
     * if the influence option is true, or the right hand is not yet active.
     */
    return colocation->influence || (rsc->running_on == NULL);
}

#endif
