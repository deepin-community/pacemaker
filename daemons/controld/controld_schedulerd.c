/*
 * Copyright 2004-2020 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU General Public License version 2
 * or later (GPLv2+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>

#include <unistd.h>  /* pid_t, sleep, ssize_t */

#include <crm/cib.h>
#include <crm/cluster.h>
#include <crm/common/xml.h>
#include <crm/crm.h>
#include <crm/msg_xml.h>
#include <crm/common/xml_internal.h>

#include <pacemaker-controld.h>

static mainloop_io_t *pe_subsystem = NULL;

/*!
 * \internal
 * \brief Close any scheduler connection and free associated memory
 */
void
pe_subsystem_free(void)
{
    controld_clear_fsa_input_flags(R_PE_REQUIRED);
    if (pe_subsystem) {
        controld_expect_sched_reply(NULL);
        mainloop_del_ipc_client(pe_subsystem);
        pe_subsystem = NULL;
        controld_clear_fsa_input_flags(R_PE_CONNECTED);
    }
}

/*!
 * \internal
 * \brief Save CIB query result to file, raising FSA error
 *
 * \param[in] msg        Ignored
 * \param[in] call_id    Call ID of CIB query
 * \param[in] rc         Return code of CIB query
 * \param[in] output     Result of CIB query
 * \param[in] user_data  Unique identifier for filename (will be freed)
 *
 * \note This is intended to be called after a scheduler connection fails.
 */
static void
save_cib_contents(xmlNode *msg, int call_id, int rc, xmlNode *output,
                  void *user_data)
{
    char *id = user_data;

    register_fsa_error_adv(C_FSA_INTERNAL, I_ERROR, NULL, NULL, __func__);
    CRM_CHECK(id != NULL, return);

    if (rc == pcmk_ok) {
        char *filename = crm_strdup_printf(PE_STATE_DIR "/pe-core-%s.bz2", id);

        if (write_xml_file(output, filename, TRUE) < 0) {
            crm_err("Could not save Cluster Information Base to %s after scheduler crash",
                    filename);
        } else {
            crm_notice("Saved Cluster Information Base to %s after scheduler crash",
                       filename);
        }
        free(filename);
    }
}

/*!
 * \internal
 * \brief Respond to scheduler connection failure
 *
 * \param[in] user_data  Ignored
 */
static void
pe_ipc_destroy(gpointer user_data)
{
    // If we aren't connected to the scheduler, we can't expect a reply
    controld_expect_sched_reply(NULL);

    if (pcmk_is_set(fsa_input_register, R_PE_REQUIRED)) {
        int rc = pcmk_ok;
        char *uuid_str = crm_generate_uuid();

        crm_crit("Connection to the scheduler failed "
                 CRM_XS " uuid=%s", uuid_str);

        /*
         * The scheduler died...
         *
         * Save the current CIB so that we have a chance of
         * figuring out what killed it.
         *
         * Delay raising the I_ERROR until the query below completes or
         * 5s is up, whichever comes first.
         *
         */
        rc = fsa_cib_conn->cmds->query(fsa_cib_conn, NULL, NULL, cib_scope_local);
        fsa_register_cib_callback(rc, FALSE, uuid_str, save_cib_contents);

    } else {
        crm_info("Connection to the scheduler released");
    }

    controld_clear_fsa_input_flags(R_PE_CONNECTED);
    pe_subsystem = NULL;
    mainloop_set_trigger(fsa_source);
    return;
}

/*!
 * \internal
 * \brief Handle message from scheduler connection
 *
 * \param[in] buffer    XML message (will be freed)
 * \param[in] length    Ignored
 * \param[in] userdata  Ignored
 *
 * \return 0
 */
static int
pe_ipc_dispatch(const char *buffer, ssize_t length, gpointer userdata)
{
    xmlNode *msg = string2xml(buffer);

    if (msg) {
        route_message(C_IPC_MESSAGE, msg);
    }
    free_xml(msg);
    return 0;
}

/*!
 * \internal
 * \brief Make new connection to scheduler
 *
 * \return TRUE on success, FALSE otherwise
 */
static bool
pe_subsystem_new(void)
{
    struct ipc_client_callbacks pe_callbacks = {
        .dispatch = pe_ipc_dispatch,
        .destroy = pe_ipc_destroy
    };
    static bool retry_one = TRUE;

    controld_set_fsa_input_flags(R_PE_REQUIRED);
retry:
    pe_subsystem = mainloop_add_ipc_client(CRM_SYSTEM_PENGINE,
                                           G_PRIORITY_DEFAULT,
                                           5 * 1024 * 1024 /* 5MB */,
                                           NULL, &pe_callbacks);
    if (pe_subsystem == NULL) {
        crm_debug("Could not connect to scheduler : %s(%d)", pcmk_rc_str(errno), errno);
        if (errno == EAGAIN && retry_one) {
            /* In rare cases, a SIGTERM may be received and the connection may fail when the cluster shuts down. */
            /* At this time, the connection will be retried only once. */
            crm_debug("Scheduler connection attempt.");
            retry_one = FALSE;
            goto retry;
        }
        return FALSE;
    }
    controld_set_fsa_input_flags(R_PE_CONNECTED);
    return TRUE;
}

/*!
 * \internal
 * \brief Send an XML message to the scheduler
 *
 * \param[in] cmd  XML message to send
 *
 * \return pcmk_ok on success, -errno otherwise
 */
static int
pe_subsystem_send(xmlNode *cmd)
{
    if (pe_subsystem) {
        int sent = crm_ipc_send(mainloop_get_ipc_client(pe_subsystem), cmd,
                                0, 0, NULL);

        if (sent == 0) {
            sent = -ENODATA;
        } else if (sent > 0) {
            sent = pcmk_ok;
        }
        return sent;
    }
    return -ENOTCONN;
}

static void do_pe_invoke_callback(xmlNode *msg, int call_id, int rc,
                                  xmlNode *output, void *user_data);

/*	 A_PE_START, A_PE_STOP, O_PE_RESTART	*/
void
do_pe_control(long long action,
              enum crmd_fsa_cause cause,
              enum crmd_fsa_state cur_state,
              enum crmd_fsa_input current_input, fsa_data_t * msg_data)
{
    if (action & A_PE_STOP) {
        pe_subsystem_free();
    }
    if ((action & A_PE_START)
        && !pcmk_is_set(fsa_input_register, R_PE_CONNECTED)) {

        if (cur_state == S_STOPPING) {
            crm_info("Ignoring request to connect to scheduler while shutting down");

        } else if (!pe_subsystem_new()) {
            crm_warn("Could not connect to scheduler");
            register_fsa_error(C_FSA_INTERNAL, I_FAIL, NULL);
        }
    }
}

int fsa_pe_query = 0;
char *fsa_pe_ref = NULL;
static mainloop_timer_t *controld_sched_timer = NULL;

// @TODO Make this a configurable cluster option if there's demand for it
#define SCHED_TIMEOUT_MS (120000)

/*!
 * \internal
 * \brief Handle a timeout waiting for scheduler reply
 *
 * \param[in] user_data  Ignored
 *
 * \return FALSE (indicating that timer should not be restarted)
 */
static gboolean
controld_sched_timeout(gpointer user_data)
{
    if (AM_I_DC) {
        /* If this node is the DC but can't communicate with the scheduler, just
         * exit (and likely get fenced) so this node doesn't interfere with any
         * further DC elections.
         *
         * @TODO We could try something less drastic first, like disconnecting
         * and reconnecting to the scheduler, but something is likely going
         * seriously wrong, so perhaps it's better to just fail as quickly as
         * possible.
         */
        crmd_exit(CRM_EX_FATAL);
    }
    return FALSE;
}

void
controld_stop_sched_timer(void)
{
    if (controld_sched_timer && fsa_pe_ref) {
        crm_trace("Stopping timer for scheduler reply %s", fsa_pe_ref);
    }
    mainloop_timer_stop(controld_sched_timer);
}

/*!
 * \internal
 * \brief Set the scheduler request currently being waited on
 *
 * \param[in] msg  Request to expect reply to (or NULL for none)
 */
void
controld_expect_sched_reply(xmlNode *msg)
{
    char *ref = NULL;

    if (msg) {
        ref = crm_element_value_copy(msg, XML_ATTR_REFERENCE);
        CRM_ASSERT(ref != NULL);

        if (controld_sched_timer == NULL) {
            controld_sched_timer = mainloop_timer_add("scheduler_reply_timer",
                                                      SCHED_TIMEOUT_MS, FALSE,
                                                      controld_sched_timeout,
                                                      NULL);
        }
        mainloop_timer_start(controld_sched_timer);
    } else {
        controld_stop_sched_timer();
    }
    free(fsa_pe_ref);
    fsa_pe_ref = ref;
}

/*!
 * \internal
 * \brief Free the scheduler reply timer
 */
void
controld_free_sched_timer(void)
{
    if (controld_sched_timer != NULL) {
        mainloop_timer_del(controld_sched_timer);
        controld_sched_timer = NULL;
    }
}

/*	 A_PE_INVOKE	*/
void
do_pe_invoke(long long action,
             enum crmd_fsa_cause cause,
             enum crmd_fsa_state cur_state,
             enum crmd_fsa_input current_input, fsa_data_t * msg_data)
{
    if (AM_I_DC == FALSE) {
        crm_err("Not invoking scheduler because not DC: %s",
                fsa_action2string(action));
        return;
    }

    if (!pcmk_is_set(fsa_input_register, R_PE_CONNECTED)) {
        if (pcmk_is_set(fsa_input_register, R_SHUTDOWN)) {
            crm_err("Cannot shut down gracefully without the scheduler");
            register_fsa_input_before(C_FSA_INTERNAL, I_TERMINATE, NULL);

        } else {
            crm_info("Waiting for the scheduler to connect");
            crmd_fsa_stall(FALSE);
            controld_set_fsa_action_flags(A_PE_START);
            trigger_fsa();
        }
        return;
    }

    if (cur_state != S_POLICY_ENGINE) {
        crm_notice("Not invoking scheduler because in state %s",
                   fsa_state2string(cur_state));
        return;
    }
    if (!pcmk_is_set(fsa_input_register, R_HAVE_CIB)) {
        crm_err("Attempted to invoke scheduler without consistent Cluster Information Base!");

        /* start the join from scratch */
        register_fsa_input_before(C_FSA_INTERNAL, I_ELECTION, NULL);
        return;
    }

    fsa_pe_query = fsa_cib_conn->cmds->query(fsa_cib_conn, NULL, NULL, cib_scope_local);

    crm_debug("Query %d: Requesting the current CIB: %s", fsa_pe_query,
              fsa_state2string(fsa_state));

    controld_expect_sched_reply(NULL);
    fsa_register_cib_callback(fsa_pe_query, FALSE, NULL, do_pe_invoke_callback);
}

static void
force_local_option(xmlNode *xml, const char *attr_name, const char *attr_value)
{
    int max = 0;
    int lpc = 0;
    char *xpath_string = NULL;
    xmlXPathObjectPtr xpathObj = NULL;

    xpath_string = crm_strdup_printf("%.128s//%s//nvpair[@name='%.128s']",
                                     get_object_path(XML_CIB_TAG_CRMCONFIG),
                                     XML_CIB_TAG_PROPSET, attr_name);
    xpathObj = xpath_search(xml, xpath_string);
    max = numXpathResults(xpathObj);
    free(xpath_string);

    for (lpc = 0; lpc < max; lpc++) {
        xmlNode *match = getXpathResult(xpathObj, lpc);
        crm_trace("Forcing %s/%s = %s", ID(match), attr_name, attr_value);
        crm_xml_add(match, XML_NVPAIR_ATTR_VALUE, attr_value);
    }

    if(max == 0) {
        xmlNode *configuration = NULL;
        xmlNode *crm_config = NULL;
        xmlNode *cluster_property_set = NULL;

        crm_trace("Creating %s-%s for %s=%s",
                  CIB_OPTIONS_FIRST, attr_name, attr_name, attr_value);

        configuration = pcmk__xe_match(xml, XML_CIB_TAG_CONFIGURATION, NULL,
                                       NULL);
        if (configuration == NULL) {
            configuration = create_xml_node(xml, XML_CIB_TAG_CONFIGURATION);
        }

        crm_config = pcmk__xe_match(configuration, XML_CIB_TAG_CRMCONFIG, NULL,
                                    NULL);
        if (crm_config == NULL) {
            crm_config = create_xml_node(configuration, XML_CIB_TAG_CRMCONFIG);
        }

        cluster_property_set = pcmk__xe_match(crm_config, XML_CIB_TAG_PROPSET,
                                              NULL, NULL);
        if (cluster_property_set == NULL) {
            cluster_property_set = create_xml_node(crm_config, XML_CIB_TAG_PROPSET);
            crm_xml_add(cluster_property_set, XML_ATTR_ID, CIB_OPTIONS_FIRST);
        }

        xml = create_xml_node(cluster_property_set, XML_CIB_TAG_NVPAIR);

        crm_xml_set_id(xml, "%s-%s", CIB_OPTIONS_FIRST, attr_name);
        crm_xml_add(xml, XML_NVPAIR_ATTR_NAME, attr_name);
        crm_xml_add(xml, XML_NVPAIR_ATTR_VALUE, attr_value);
    }
    freeXpathObject(xpathObj);
}

static void
do_pe_invoke_callback(xmlNode * msg, int call_id, int rc, xmlNode * output, void *user_data)
{
    xmlNode *cmd = NULL;
    pid_t watchdog = pcmk__locate_sbd();

    if (rc != pcmk_ok) {
        crm_err("Could not retrieve the Cluster Information Base: %s "
                CRM_XS " rc=%d call=%d", pcmk_strerror(rc), rc, call_id);
        register_fsa_error_adv(C_FSA_INTERNAL, I_ERROR, NULL, NULL, __func__);
        return;

    } else if (call_id != fsa_pe_query) {
        crm_trace("Skipping superseded CIB query: %d (current=%d)", call_id, fsa_pe_query);
        return;

    } else if (!AM_I_DC || !pcmk_is_set(fsa_input_register, R_PE_CONNECTED)) {
        crm_debug("No need to invoke the scheduler anymore");
        return;

    } else if (fsa_state != S_POLICY_ENGINE) {
        crm_debug("Discarding scheduler request in state: %s",
                  fsa_state2string(fsa_state));
        return;

    /* this callback counts as 1 */
    } else if (num_cib_op_callbacks() > 1) {
        crm_debug("Re-asking for the CIB: %d other peer updates still pending",
                  (num_cib_op_callbacks() - 1));
        sleep(1);
        controld_set_fsa_action_flags(A_PE_INVOKE);
        trigger_fsa();
        return;
    }

    CRM_LOG_ASSERT(output != NULL);

    /* Refresh the remote node cache and the known node cache when the
     * scheduler is invoked */
    pcmk__refresh_node_caches_from_cib(output);

    crm_xml_add(output, XML_ATTR_DC_UUID, fsa_our_uuid);
    crm_xml_add_int(output, XML_ATTR_HAVE_QUORUM, fsa_has_quorum);

    force_local_option(output, XML_ATTR_HAVE_WATCHDOG, pcmk__btoa(watchdog));

    if (ever_had_quorum && crm_have_quorum == FALSE) {
        crm_xml_add_int(output, XML_ATTR_QUORUM_PANIC, 1);
    }

    cmd = create_request(CRM_OP_PECALC, output, NULL, CRM_SYSTEM_PENGINE, CRM_SYSTEM_DC, NULL);

    rc = pe_subsystem_send(cmd);
    if (rc < 0) {
        crm_err("Could not contact the scheduler: %s " CRM_XS " rc=%d",
                pcmk_strerror(rc), rc);
        register_fsa_error_adv(C_FSA_INTERNAL, I_ERROR, NULL, NULL, __func__);
    } else {
        controld_expect_sched_reply(cmd);
        crm_debug("Invoking the scheduler: query=%d, ref=%s, seq=%llu, quorate=%d",
                  fsa_pe_query, fsa_pe_ref, crm_peer_seq, fsa_has_quorum);
    }
    free_xml(cmd);
}
