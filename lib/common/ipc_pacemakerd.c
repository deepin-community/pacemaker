/*
 * Copyright 2020-2022 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU Lesser General Public License
 * version 2.1 or later (LGPLv2.1+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>

#include <stdlib.h>
#include <time.h>

#include <crm/crm.h>
#include <crm/msg_xml.h>
#include <crm/common/xml.h>
#include <crm/common/ipc.h>
#include <crm/common/ipc_internal.h>
#include <crm/common/ipc_pacemakerd.h>
#include "crmcommon_private.h"

typedef struct pacemakerd_api_private_s {
    enum pcmk_pacemakerd_state state;
    char *client_uuid;
} pacemakerd_api_private_t;

static const char *pacemakerd_state_str[] = {
    XML_PING_ATTR_PACEMAKERDSTATE_INIT,
    XML_PING_ATTR_PACEMAKERDSTATE_STARTINGDAEMONS,
    XML_PING_ATTR_PACEMAKERDSTATE_WAITPING,
    XML_PING_ATTR_PACEMAKERDSTATE_RUNNING,
    XML_PING_ATTR_PACEMAKERDSTATE_SHUTTINGDOWN,
    XML_PING_ATTR_PACEMAKERDSTATE_SHUTDOWNCOMPLETE,
    XML_PING_ATTR_PACEMAKERDSTATE_REMOTE,
};

enum pcmk_pacemakerd_state
pcmk_pacemakerd_api_daemon_state_text2enum(const char *state)
{
    int i;

    if (state == NULL) {
        return pcmk_pacemakerd_state_invalid;
    }
    for (i=pcmk_pacemakerd_state_init; i <= pcmk_pacemakerd_state_max;
         i++) {
        if (pcmk__str_eq(state, pacemakerd_state_str[i], pcmk__str_none)) {
            return i;
        }
    }
    return pcmk_pacemakerd_state_invalid;
}

const char *
pcmk_pacemakerd_api_daemon_state_enum2text(
    enum pcmk_pacemakerd_state state)
{
    if ((state >= pcmk_pacemakerd_state_init) &&
        (state <= pcmk_pacemakerd_state_max)) {
        return pacemakerd_state_str[state];
    }
    return "invalid";
}

/*!
 * \internal
 * \brief Return a friendly string representation of a \p pacemakerd state
 *
 * \param[in] state  \p pacemakerd state
 *
 * \return A user-friendly string representation of \p state, or
 *         <tt>"Invalid pacemakerd state"</tt>
 */
const char *
pcmk__pcmkd_state_enum2friendly(enum pcmk_pacemakerd_state state)
{
    switch (state) {
        case pcmk_pacemakerd_state_init:
            return "Initializing pacemaker";
        case pcmk_pacemakerd_state_starting_daemons:
            return "Pacemaker daemons are starting";
        case pcmk_pacemakerd_state_wait_for_ping:
            return "Waiting for startup trigger from SBD";
        case pcmk_pacemakerd_state_running:
            return "Pacemaker is running";
        case pcmk_pacemakerd_state_shutting_down:
            return "Pacemaker daemons are shutting down";
        case pcmk_pacemakerd_state_shutdown_complete:
            /* Assuming pacemakerd won't process messages while in
             * shutdown_complete state unless reporting to SBD
             */
            return "Pacemaker daemons are shut down (reporting to SBD)";
        case pcmk_pacemakerd_state_remote:
            return "pacemaker-remoted is running (on a Pacemaker Remote node)";
        default:
            return "Invalid pacemakerd state";
    }
}

/*!
 * \internal
 * \brief Get a string representation of a \p pacemakerd API reply type
 *
 * \param[in] reply  \p pacemakerd API reply type
 *
 * \return String representation of a \p pacemakerd API reply type
 */
const char *
pcmk__pcmkd_api_reply2str(enum pcmk_pacemakerd_api_reply reply)
{
    switch (reply) {
        case pcmk_pacemakerd_reply_ping:
            return "ping";
        case pcmk_pacemakerd_reply_shutdown:
            return "shutdown";
        default:
            return "unknown";
    }
}

// \return Standard Pacemaker return code
static int
new_data(pcmk_ipc_api_t *api)
{
    struct pacemakerd_api_private_s *private = NULL;

    api->api_data = calloc(1, sizeof(struct pacemakerd_api_private_s));

    if (api->api_data == NULL) {
        return errno;
    }

    private = api->api_data;
    private->state = pcmk_pacemakerd_state_invalid;
    /* other as with cib, controld, ... we are addressing pacemakerd just
       from the local node -> pid is unique and thus sufficient as an ID
     */
    private->client_uuid = pcmk__getpid_s();

    return pcmk_rc_ok;
}

static void
free_data(void *data)
{
    free(((struct pacemakerd_api_private_s *) data)->client_uuid);
    free(data);
}

// \return Standard Pacemaker return code
static int
post_connect(pcmk_ipc_api_t *api)
{
    struct pacemakerd_api_private_s *private = NULL;

    if (api->api_data == NULL) {
        return EINVAL;
    }
    private = api->api_data;
    private->state = pcmk_pacemakerd_state_invalid;

    return pcmk_rc_ok;
}

static void
post_disconnect(pcmk_ipc_api_t *api)
{
    struct pacemakerd_api_private_s *private = NULL;

    if (api->api_data == NULL) {
        return;
    }
    private = api->api_data;
    private->state = pcmk_pacemakerd_state_invalid;

    return;
}

static bool
reply_expected(pcmk_ipc_api_t *api, xmlNode *request)
{
    const char *command = crm_element_value(request, F_CRM_TASK);

    if (command == NULL) {
        return false;
    }

    // We only need to handle commands that functions in this file can send
    return pcmk__str_any_of(command, CRM_OP_PING, CRM_OP_QUIT, NULL);
}

static bool
dispatch(pcmk_ipc_api_t *api, xmlNode *reply)
{
    crm_exit_t status = CRM_EX_OK;
    xmlNode *msg_data = NULL;
    pcmk_pacemakerd_api_reply_t reply_data = {
        pcmk_pacemakerd_reply_unknown
    };
    const char *value = NULL;
    long long value_ll = 0;

    if (pcmk__str_eq((const char *) reply->name, "ack", pcmk__str_none)) {
        long long int ack_status = 0;
        pcmk__scan_ll(crm_element_value(reply, "status"), &ack_status, CRM_EX_OK);
        return ack_status == CRM_EX_INDETERMINATE;
    }

    value = crm_element_value(reply, F_CRM_MSG_TYPE);
    if (pcmk__str_empty(value)
        || !pcmk__str_eq(value, XML_ATTR_RESPONSE, pcmk__str_none)) {
        crm_info("Unrecognizable message from pacemakerd: "
                 "message type '%s' not '" XML_ATTR_RESPONSE "'",
                 pcmk__s(value, ""));
        status = CRM_EX_PROTOCOL;
        goto done;
    }

    if (pcmk__str_empty(crm_element_value(reply, XML_ATTR_REFERENCE))) {
        crm_info("Unrecognizable message from pacemakerd: no reference");
        status = CRM_EX_PROTOCOL;
        goto done;
    }

    value = crm_element_value(reply, F_CRM_TASK);

    // Parse useful info from reply
    msg_data = get_message_xml(reply, F_CRM_DATA);
    crm_element_value_ll(msg_data, XML_ATTR_TSTAMP, &value_ll);

    if (pcmk__str_eq(value, CRM_OP_PING, pcmk__str_none)) {
        reply_data.reply_type = pcmk_pacemakerd_reply_ping;
        reply_data.data.ping.state =
            pcmk_pacemakerd_api_daemon_state_text2enum(
                crm_element_value(msg_data, XML_PING_ATTR_PACEMAKERDSTATE));
        reply_data.data.ping.status =
            pcmk__str_eq(crm_element_value(msg_data, XML_PING_ATTR_STATUS), "ok",
                         pcmk__str_casei)?pcmk_rc_ok:pcmk_rc_error;
        reply_data.data.ping.last_good = (value_ll < 0)? 0 : (time_t) value_ll;
        reply_data.data.ping.sys_from = crm_element_value(msg_data,
                                            XML_PING_ATTR_SYSFROM);
    } else if (pcmk__str_eq(value, CRM_OP_QUIT, pcmk__str_none)) {
        reply_data.reply_type = pcmk_pacemakerd_reply_shutdown;
        reply_data.data.shutdown.status = atoi(crm_element_value(msg_data, XML_LRM_ATTR_OPSTATUS));
    } else {
        crm_info("Unrecognizable message from pacemakerd: "
                 "unknown command '%s'", pcmk__s(value, ""));
        status = CRM_EX_PROTOCOL;
        goto done;
    }

done:
    pcmk__call_ipc_callback(api, pcmk_ipc_event_reply, status, &reply_data);
    return false;
}

pcmk__ipc_methods_t *
pcmk__pacemakerd_api_methods(void)
{
    pcmk__ipc_methods_t *cmds = calloc(1, sizeof(pcmk__ipc_methods_t));

    if (cmds != NULL) {
        cmds->new_data = new_data;
        cmds->free_data = free_data;
        cmds->post_connect = post_connect;
        cmds->reply_expected = reply_expected;
        cmds->dispatch = dispatch;
        cmds->post_disconnect = post_disconnect;
    }
    return cmds;
}

static int
do_pacemakerd_api_call(pcmk_ipc_api_t *api, const char *ipc_name, const char *task)
{
    pacemakerd_api_private_t *private;
    xmlNode *cmd;
    int rc;

    if (api == NULL) {
        return EINVAL;
    }

    private = api->api_data;
    CRM_ASSERT(private != NULL);

    cmd = create_request(task, NULL, NULL, CRM_SYSTEM_MCP,
                         pcmk__ipc_sys_name(ipc_name, "client"),
                         private->client_uuid);

    if (cmd) {
        rc = pcmk__send_ipc_request(api, cmd);
        if (rc != pcmk_rc_ok) {
            crm_debug("Couldn't send request to pacemakerd: %s rc=%d",
                      pcmk_rc_str(rc), rc);
        }
        free_xml(cmd);
    } else {
        rc = ENOMSG;
    }

    return rc;
}

int
pcmk_pacemakerd_api_ping(pcmk_ipc_api_t *api, const char *ipc_name)
{
    return do_pacemakerd_api_call(api, ipc_name, CRM_OP_PING);
}

int
pcmk_pacemakerd_api_shutdown(pcmk_ipc_api_t *api, const char *ipc_name)
{
    return do_pacemakerd_api_call(api, ipc_name, CRM_OP_QUIT);
}
