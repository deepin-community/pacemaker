/*
 * Copyright 2004-2020 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU General Public License version 2
 * or later (GPLv2+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>
#include <crm/crm.h>
#include <crm/msg_xml.h>
#include <crm/common/xml.h>

#include <pacemaker-controld.h>

gboolean
stop_te_timer(crm_action_timer_t * timer)
{
    if (timer == NULL) {
        return FALSE;
    }
    if (timer->source_id != 0) {
        crm_trace("Stopping action timer");
        g_source_remove(timer->source_id);
        timer->source_id = 0;
    } else {
        crm_trace("Action timer was already stopped");
        return FALSE;
    }
    return TRUE;
}

gboolean
te_graph_trigger(gpointer user_data)
{
    enum transition_status graph_rc = -1;

    if (transition_graph == NULL) {
        crm_debug("Nothing to do");
        return TRUE;
    }

    crm_trace("Invoking graph %d in state %s", transition_graph->id, fsa_state2string(fsa_state));

    switch (fsa_state) {
        case S_STARTING:
        case S_PENDING:
        case S_NOT_DC:
        case S_HALT:
        case S_ILLEGAL:
        case S_STOPPING:
        case S_TERMINATE:
            return TRUE;
        default:
            break;
    }

    if (transition_graph->complete == FALSE) {
        int limit = transition_graph->batch_limit;

        transition_graph->batch_limit = throttle_get_total_job_limit(limit);
        graph_rc = run_graph(transition_graph);
        transition_graph->batch_limit = limit; /* Restore the configured value */

        /* significant overhead... */
        /* print_graph(LOG_TRACE, transition_graph); */

        if (graph_rc == transition_active) {
            crm_trace("Transition not yet complete");
            return TRUE;

        } else if (graph_rc == transition_pending) {
            crm_trace("Transition not yet complete - no actions fired");
            return TRUE;
        }

        if (graph_rc != transition_complete) {
            crm_warn("Transition failed: %s", transition_status(graph_rc));
            print_graph(LOG_NOTICE, transition_graph);
        }
    }

    crm_debug("Transition %d is now complete", transition_graph->id);
    transition_graph->complete = TRUE;
    notify_crmd(transition_graph);

    return TRUE;
}

void
trigger_graph_processing(const char *fn, int line)
{
    crm_trace("%s:%d - Triggered graph processing", fn, line);
    mainloop_set_trigger(transition_trigger);
}

static struct abort_timer_s {
    bool aborted;
    guint id;
    int priority;
    enum transition_action action;
    const char *text;
} abort_timer = { 0, };

static gboolean
abort_timer_popped(gpointer data)
{
    if (AM_I_DC && (abort_timer.aborted == FALSE)) {
        abort_transition(abort_timer.priority, abort_timer.action,
                         abort_timer.text, NULL);
    }
    abort_timer.id = 0;
    return FALSE; // do not immediately reschedule timer
}

/*!
 * \internal
 * \brief Abort transition after delay, if not already aborted in that time
 *
 * \param[in] abort_text  Must be literal string
 */
void
abort_after_delay(int abort_priority, enum transition_action abort_action,
                  const char *abort_text, guint delay_ms)
{
    if (abort_timer.id) {
        // Timer already in progress, stop and reschedule
        g_source_remove(abort_timer.id);
    }
    abort_timer.aborted = FALSE;
    abort_timer.priority = abort_priority;
    abort_timer.action = abort_action;
    abort_timer.text = abort_text;
    abort_timer.id = g_timeout_add(delay_ms, abort_timer_popped, NULL);
}

void
abort_transition_graph(int abort_priority, enum transition_action abort_action,
                       const char *abort_text, xmlNode * reason, const char *fn, int line)
{
    int add[] = { 0, 0, 0 };
    int del[] = { 0, 0, 0 };
    int level = LOG_INFO;
    xmlNode *diff = NULL;
    xmlNode *change = NULL;

    CRM_CHECK(transition_graph != NULL, return);

    switch (fsa_state) {
        case S_STARTING:
        case S_PENDING:
        case S_NOT_DC:
        case S_HALT:
        case S_ILLEGAL:
        case S_STOPPING:
        case S_TERMINATE:
            crm_info("Abort %s suppressed: state=%s (complete=%d)",
                     abort_text, fsa_state2string(fsa_state), transition_graph->complete);
            return;
        default:
            break;
    }

    abort_timer.aborted = TRUE;
    controld_expect_sched_reply(NULL);

    if (transition_graph->complete == FALSE) {
        if(update_abort_priority(transition_graph, abort_priority, abort_action, abort_text)) {
            level = LOG_NOTICE;
        }
    }

    if(reason) {
        xmlNode *search = NULL;

        for(search = reason; search; search = search->parent) {
            if (pcmk__str_eq(XML_TAG_DIFF, TYPE(search), pcmk__str_casei)) {
                diff = search;
                break;
            }
        }

        if(diff) {
            xml_patch_versions(diff, add, del);
            for(search = reason; search; search = search->parent) {
                if (pcmk__str_eq(XML_DIFF_CHANGE, TYPE(search), pcmk__str_casei)) {
                    change = search;
                    break;
                }
            }
        }
    }

    if(reason == NULL) {
        do_crm_log(level, "Transition %d aborted: %s "CRM_XS" source=%s:%d complete=%s",
                   transition_graph->id, abort_text, fn, line,
                   pcmk__btoa(transition_graph->complete));

    } else if(change == NULL) {
        char *local_path = xml_get_path(reason);

        do_crm_log(level, "Transition %d aborted by %s.%s: %s "
                   CRM_XS " cib=%d.%d.%d source=%s:%d path=%s complete=%s",
                   transition_graph->id, TYPE(reason), ID(reason), abort_text,
                   add[0], add[1], add[2], fn, line, local_path,
                   pcmk__btoa(transition_graph->complete));
        free(local_path);

    } else {
        const char *kind = NULL;
        const char *op = crm_element_value(change, XML_DIFF_OP);
        const char *path = crm_element_value(change, XML_DIFF_PATH);

        if(change == reason) {
            if(strcmp(op, "create") == 0) {
                reason = reason->children;

            } else if(strcmp(op, "modify") == 0) {
                reason = first_named_child(reason, XML_DIFF_RESULT);
                if(reason) {
                    reason = reason->children;
                }
            }
        }

        kind = TYPE(reason);
        if(strcmp(op, "delete") == 0) {
            const char *shortpath = strrchr(path, '/');

            do_crm_log(level, "Transition %d aborted by deletion of %s: %s "
                       CRM_XS " cib=%d.%d.%d source=%s:%d path=%s complete=%s",
                       transition_graph->id,
                       (shortpath? (shortpath + 1) : path), abort_text,
                       add[0], add[1], add[2], fn, line, path,
                       pcmk__btoa(transition_graph->complete));

        } else if (pcmk__str_eq(XML_CIB_TAG_NVPAIR, kind, pcmk__str_casei)) { 
            do_crm_log(level, "Transition %d aborted by %s doing %s %s=%s: %s "
                       CRM_XS " cib=%d.%d.%d source=%s:%d path=%s complete=%s",
                       transition_graph->id,
                       crm_element_value(reason, XML_ATTR_ID), op,
                       crm_element_value(reason, XML_NVPAIR_ATTR_NAME),
                       crm_element_value(reason, XML_NVPAIR_ATTR_VALUE),
                       abort_text, add[0], add[1], add[2], fn, line, path,
                       pcmk__btoa(transition_graph->complete));

        } else if (pcmk__str_eq(XML_LRM_TAG_RSC_OP, kind, pcmk__str_casei)) {
            const char *magic = crm_element_value(reason, XML_ATTR_TRANSITION_MAGIC);

            do_crm_log(level, "Transition %d aborted by operation %s '%s' on %s: %s "
                       CRM_XS " magic=%s cib=%d.%d.%d source=%s:%d complete=%s",
                       transition_graph->id,
                       crm_element_value(reason, XML_LRM_ATTR_TASK_KEY), op,
                       crm_element_value(reason, XML_LRM_ATTR_TARGET), abort_text,
                       magic, add[0], add[1], add[2], fn, line,
                       pcmk__btoa(transition_graph->complete));

        } else if (pcmk__strcase_any_of(kind, XML_CIB_TAG_STATE, XML_CIB_TAG_NODE, NULL)) {
            const char *uname = crm_peer_uname(ID(reason));

            do_crm_log(level, "Transition %d aborted by %s '%s' on %s: %s "
                       CRM_XS " cib=%d.%d.%d source=%s:%d complete=%s",
                       transition_graph->id,
                       kind, op, (uname? uname : ID(reason)), abort_text,
                       add[0], add[1], add[2], fn, line,
                       pcmk__btoa(transition_graph->complete));

        } else {
            const char *id = ID(reason);

            do_crm_log(level, "Transition %d aborted by %s.%s '%s': %s "
                       CRM_XS " cib=%d.%d.%d source=%s:%d path=%s complete=%s",
                       transition_graph->id,
                       TYPE(reason), (id? id : ""), (op? op : "change"),
                       abort_text, add[0], add[1], add[2], fn, line, path,
                       pcmk__btoa(transition_graph->complete));
        }
    }

    if (transition_graph->complete) {
        if (transition_timer->period_ms > 0) {
            controld_stop_timer(transition_timer);
            controld_start_timer(transition_timer);
        } else {
            register_fsa_input(C_FSA_INTERNAL, I_PE_CALC, NULL);
        }
        return;
    }

    mainloop_set_trigger(transition_trigger);
}
