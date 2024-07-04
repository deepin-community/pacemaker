/*
 * Copyright 2004-2022 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU Lesser General Public License
 * version 2.1 or later (LGPLv2.1+) WITHOUT ANY WARRANTY.
 */

#ifndef PCMK__CRM_STONITH_NG__H
#  define PCMK__CRM_STONITH_NG__H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \file
 * \brief Fencing aka. STONITH
 * \ingroup fencing
 */

/* IMPORTANT: DLM source code includes this file directly, without having access
 * to other Pacemaker headers on its include path, so this file should *not*
 * include any other Pacemaker headers. (DLM might be updated to avoid the
 * issue, but we should still follow this guideline for a long time after.)
 */

#  include <dlfcn.h>
#  include <errno.h>
#  include <stdbool.h>  // bool
#  include <stdint.h>   // uint32_t
#  include <time.h>     // time_t

#  define T_STONITH_NOTIFY_DISCONNECT     "st_notify_disconnect"
#  define T_STONITH_NOTIFY_FENCE          "st_notify_fence"
#  define T_STONITH_NOTIFY_HISTORY        "st_notify_history"
#  define T_STONITH_NOTIFY_HISTORY_SYNCED "st_notify_history_synced"

/* *INDENT-OFF* */
enum stonith_state {
    stonith_connected_command,
    stonith_connected_query,
    stonith_disconnected,
};

enum stonith_call_options {
    st_opt_none            = 0x00000000,
    st_opt_verbose         = 0x00000001,
    st_opt_allow_suicide   = 0x00000002,

    st_opt_manual_ack      = 0x00000008,
    st_opt_discard_reply   = 0x00000010,
/*    st_opt_all_replies     = 0x00000020, */
    st_opt_topology        = 0x00000040,
    st_opt_scope_local     = 0x00000100,
    st_opt_cs_nodeid       = 0x00000200,
    st_opt_sync_call       = 0x00001000,
    /*! Allow the timeout period for a callback to be adjusted
     *  based on the time the server reports the operation will take. */
    st_opt_timeout_updates = 0x00002000,
    /*! Only report back if operation is a success in callback */
    st_opt_report_only_success = 0x00004000,
    /* used where ever apropriate - e.g. cleanup of history */
    st_opt_cleanup         = 0x000080000,
    /* used where ever apropriate - e.g. send out a history query to all nodes */
    st_opt_broadcast       = 0x000100000,
};

/*! Order matters here, do not change values */
enum op_state
{
    st_query,
    st_exec,
    st_done,
    st_duplicate,
    st_failed,
};

// Supported fence agent interface standards
enum stonith_namespace {
    st_namespace_invalid,
    st_namespace_any,
    st_namespace_internal,  // Implemented internally by Pacemaker

    /* Neither of these projects are active any longer, but the fence agent
     * interfaces they created are still in use and supported by Pacemaker.
     */
    st_namespace_rhcs,      // Red Hat Cluster Suite compatible
    st_namespace_lha,       // Linux-HA compatible
};

enum stonith_namespace stonith_text2namespace(const char *namespace_s);
const char *stonith_namespace2text(enum stonith_namespace st_namespace);
enum stonith_namespace stonith_get_namespace(const char *agent,
                                             const char *namespace_s);

typedef struct stonith_key_value_s {
    char *key;
    char *value;
        struct stonith_key_value_s *next;
} stonith_key_value_t;

typedef struct stonith_history_s {
    char *target;
    char *action;
    char *origin;
    char *delegate;
    char *client;
    int state;
    time_t completed;
    struct stonith_history_s *next;
    long completed_nsec;
    char *exit_reason;
} stonith_history_t;

typedef struct stonith_s stonith_t;

typedef struct stonith_event_s
{
    char *id;
    char *type;     //!< \deprecated Will be removed in future release
    char *message;  //!< \deprecated Will be removed in future release
    char *operation;

    int result;
    char *origin;
    char *target;
    char *action;
    char *executioner;

    char *device;

    /*! The name of the client that initiated the action. */
    char *client_origin;

    //! \internal This field should be treated as internal to Pacemaker
    void *opaque;
} stonith_event_t;

typedef struct stonith_callback_data_s {
    int rc;
    int call_id;
    void *userdata;

    //! \internal This field should be treated as internal to Pacemaker
    void *opaque;
} stonith_callback_data_t;

typedef struct stonith_api_operations_s
{
    /*!
     * \brief Destroy a fencer connection
     *
     * \param[in,out] st  Fencer connection to destroy
     */
    int (*free) (stonith_t *st);

    /*!
     * \brief Connect to the local fencer
     *
     * \param[in,out] st          Fencer connection to connect
     * \param[in]     name        Client name to use
     * \param[out]    stonith_fd  If NULL, use a main loop, otherwise
     *                            store IPC file descriptor here
     *
     * \return Legacy Pacemaker return code
     */
    int (*connect) (stonith_t *st, const char *name, int *stonith_fd);

    /*!
     * \brief Disconnect from the local stonith daemon.
     *
     * \param[in,out] st  Fencer connection to disconnect
     *
     * \return Legacy Pacemaker return code
     */
    int (*disconnect)(stonith_t *st);

    /*!
     * \brief Unregister a fence device with the local fencer
     *
     * \param[in,out] st       Fencer connection to disconnect
     * \param[in]     options  Group of enum stonith_call_options
     * \param[in]     name     ID of fence device to unregister
     *
     * \return pcmk_ok (if synchronous) or positive call ID (if asynchronous)
     *         on success, otherwise a negative legacy Pacemaker return code
     */
    int (*remove_device)(stonith_t *st, int options, const char *name);

    /*!
     * \brief Register a fence device with the local fencer
     *
     * \param[in,out] st         Fencer connection to use
     * \param[in]     options    Group of enum stonith_call_options
     * \param[in]     id         ID of fence device to register
     * \param[in]     namespace  Type of fence agent to search for ("redhat"
     *                           or "stonith-ng" for RHCS-style, "internal" for
     *                           Pacemaker-internal devices, "heartbeat" for
     *                           LHA-style, or "any" or NULL for any)
     * \param[in]     agent      Name of fence agent for device
     * \param[in]     params     Fence agent parameters for device
     *
     * \return pcmk_ok (if synchronous) or positive call ID (if asynchronous)
     *         on success, otherwise a negative legacy Pacemaker return code
     */
    int (*register_device)(stonith_t *st, int options, const char *id,
                           const char *namespace, const char *agent,
                           const stonith_key_value_t *params);

    /*!
     * \brief Unregister a fencing level for specified node with local fencer
     *
     * \param[in,out] st       Fencer connection to use
     * \param[in]     options  Group of enum stonith_call_options
     * \param[in]     node     Target node to unregister level for
     * \param[in]     level    Topology level number to unregister
     *
     * \return pcmk_ok (if synchronous) or positive call ID (if asynchronous)
     *         on success, otherwise a negative legacy Pacemaker return code
     */
    int (*remove_level)(stonith_t *st, int options, const char *node,
                        int level);

    /*!
     * \brief Register a fencing level for specified node with local fencer
     *
     * \param[in,out] st           Fencer connection to use
     * \param[in]     options      Group of enum stonith_call_options
     * \param[in]     node         Target node to register level for
     * \param[in]     level        Topology level number to register
     * \param[in]     device_list  Devices to register in level
     *
     * \return pcmk_ok (if synchronous) or positive call ID (if asynchronous)
     *         on success, otherwise a negative legacy Pacemaker return code
     */
    int (*register_level)(stonith_t *st, int options, const char *node,
                          int level, const stonith_key_value_t *device_list);

    /*!
     * \brief Retrieve a fence agent's metadata
     *
     * \param[in,out] stonith       Fencer connection
     * \param[in]     call_options  Group of enum stonith_call_options
     *                              (currently ignored)
     * \param[in]     agent         Fence agent to query
     * \param[in]     namespace     Type of fence agent to search for ("redhat"
     *                              or "stonith-ng" for RHCS-style, "internal"
     *                              for Pacemaker-internal devices, "heartbeat"
     *                              for LHA-style, or "any" or NULL for any)
     * \param[out]    output        Where to store metadata
     * \param[in]     timeout_sec   Error if not complete within this time
     *
     * \return Legacy Pacemaker return code
     * \note The caller is responsible for freeing *output using free().
     */
    int (*metadata)(stonith_t *stonith, int call_options, const char *agent,
                    const char *namespace, char **output, int timeout_sec);

    /*!
     * \brief Retrieve a list of installed fence agents
     *
     * \param[in,out] stonith       Fencer connection to use
     * \param[in]     call_options  Group of enum stonith_call_options
     *                              (currently ignored)
     * \param[in]     namespace     Type of fence agents to list ("redhat"
     *                              or "stonith-ng" for RHCS-style, "internal" for
     *                              Pacemaker-internal devices, "heartbeat" for
     *                              LHA-style, or "any" or NULL for all)
     * \param[out]    devices       Where to store agent list
     * \param[in]     timeout       Error if unable to complete within this
     *                              (currently ignored)
     *
     * \return Number of items in list on success, or negative errno otherwise
     * \note The caller is responsible for freeing the returned list with
     *       stonith_key_value_freeall().
     */
    int (*list_agents)(stonith_t *stonith, int call_options,
                       const char *namespace, stonith_key_value_t **devices,
                       int timeout);

    /*!
     * \brief Get the output of a fence device's list action
     *
     * \param[in,out] stonith       Fencer connection to use
     * \param[in]     call_options  Group of enum stonith_call_options
     * \param[in]     id            Fence device ID to run list for
     * \param[out]    list_info     Where to store list output
     * \param[in]     timeout       Error if unable to complete within this
     *
     * \return pcmk_ok (if synchronous) or positive call ID (if asynchronous)
     *         on success, otherwise a negative legacy Pacemaker return code
     */
    int (*list)(stonith_t *stonith, int call_options, const char *id,
                char **list_info, int timeout);

    /*!
     * \brief Check whether a fence device is reachable by monitor action
     *
     * \param[in,out] stonith       Fencer connection to use
     * \param[in]     call_options  Group of enum stonith_call_options
     * \param[in]     id            Fence device ID to run monitor for
     * \param[in]     timeout       Error if unable to complete within this
     *
     * \return pcmk_ok (if synchronous) or positive call ID (if asynchronous)
     *         on success, otherwise a negative legacy Pacemaker return code
     */
    int (*monitor)(stonith_t *stonith, int call_options, const char *id,
                   int timeout);

    /*!
     * \brief Check whether a fence device target is reachable by status action
     *
     * \param[in,out] stonith       Fencer connection to use
     * \param[in]     call_options  Group of enum stonith_call_options
     * \param[in]     id            Fence device ID to run status for
     * \param[in]     port          Fence target to run status for
     * \param[in]     timeout       Error if unable to complete within this
     *
     * \return pcmk_ok (if synchronous) or positive call ID (if asynchronous)
     *         on success, otherwise a negative legacy Pacemaker return code
     */
    int (*status)(stonith_t *stonith, int call_options, const char *id,
                  const char *port, int timeout);

    /*!
     * \brief List registered fence devices
     *
     * \param[in,out] stonith       Fencer connection to use
     * \param[in]     call_options  Group of enum stonith_call_options
     * \param[in]     target        Fence target to run status for
     * \param[out]    devices       Where to store list of fence devices
     * \param[in]     timeout       Error if unable to complete within this
     *
     * \note If node is provided, only devices that can fence the node id
     *       will be returned.
     *
     * \return Number of items in list on success, or negative errno otherwise
     */
    int (*query)(stonith_t *stonith, int call_options, const char *target,
                 stonith_key_value_t **devices, int timeout);

    /*!
     * \brief Request that a target get fenced
     *
     * \param[in,out] stonith       Fencer connection to use
     * \param[in]     call_options  Group of enum stonith_call_options
     * \param[in]     node          Fence target
     * \param[in]     action        "on", "off", or "reboot"
     * \param[in]     timeout       Default per-device timeout to use with
     *                              each executed device
     * \param[in]     tolerance     Accept result of identical fence action
     *                              completed within this time
     *
     * \return pcmk_ok (if synchronous) or positive call ID (if asynchronous)
     *         on success, otherwise a negative legacy Pacemaker return code
     */
    int (*fence)(stonith_t *stonith, int call_options, const char *node,
                 const char *action, int timeout, int tolerance);

    /*!
     * \brief Manually confirm that a node has been fenced
     *
     * \param[in,out] stonith       Fencer connection to use
     * \param[in]     call_options  Group of enum stonith_call_options
     * \param[in]     target        Fence target
     *
     * \return pcmk_ok (if synchronous) or positive call ID (if asynchronous)
     *         on success, otherwise a negative legacy Pacemaker return code
     */
    int (*confirm)(stonith_t *stonith, int call_options, const char *target);

    /*!
     * \brief List fencing actions that have occurred for a target
     *
     * \param[in,out] stonith       Fencer connection to use
     * \param[in]     call_options  Group of enum stonith_call_options
     * \param[in]     node          Fence target
     * \param[out]    history       Where to store list of fencing actions
     * \param[in]     timeout       Error if unable to complete within this
     *
     * \return Legacy Pacemaker return code
     */
    int (*history)(stonith_t *stonith, int call_options, const char *node,
                   stonith_history_t **history, int timeout);

    /*!
     * \brief Register a callback for fence notifications
     *
     * \param[in,out] stonith       Fencer connection to use
     * \param[in]     event         Event to register for
     * \param[in]     callback      Callback to register
     *
     * \return Legacy Pacemaker return code
     */
    int (*register_notification)(stonith_t *stonith, const char *event,
                                 void (*callback)(stonith_t *st,
                                                  stonith_event_t *e));

    /*!
     * \brief Unregister callbacks for fence notifications
     *
     * \param[in,out] stonith  Fencer connection to use
     * \param[in]     event    Event to unregister callbacks for (NULL for all)
     *
     * \return Legacy Pacemaker return code
     */
    int (*remove_notification)(stonith_t *stonith, const char *event);

    /*!
     * \brief Register a callback for an asynchronous fencing result
     *
     * \param[in,out] stonith        Fencer connection to use
     * \param[in]     call_id        Call ID to register callback for
     * \param[in]     timeout        Error if result not received in this time
     * \param[in]     options        Group of enum stonith_call_options
     *                               (respects \c st_opt_timeout_updates and
     *                               \c st_opt_report_only_success)
     * \param[in,out] user_data      Pointer to pass to callback
     * \param[in]     callback_name  Unique identifier for callback
     * \param[in]     callback       Callback to register (may be called
     *                               immediately if \p call_id indicates error)
     *
     * \return \c TRUE on success, \c FALSE if call_id indicates error,
     *         or -EINVAL if \p stonith is not valid
     */
    int (*register_callback)(stonith_t *stonith, int call_id, int timeout,
                             int options, void *user_data,
                             const char *callback_name,
                             void (*callback)(stonith_t *st,
                                              stonith_callback_data_t *data));

    /*!
     * \brief Unregister callbacks for asynchronous fencing results
     *
     * \param[in,out] stonith        Fencer connection to use
     * \param[in]     call_id        If \p all_callbacks is false, call ID
     *                               to unregister callback for
     * \param[in]     all_callbacks  If true, unregister all callbacks
     *
     * \return pcmk_ok
     */
    int (*remove_callback)(stonith_t *stonith, int call_id, bool all_callbacks);

    /*!
     * \brief Unregister fencing level for specified node, pattern or attribute
     *
     * \param[in,out] st       Fencer connection to use
     * \param[in]     options  Group of enum stonith_call_options
     * \param[in]     node     If not NULL, unregister level targeting this node
     * \param[in]     pattern  If not NULL, unregister level targeting nodes
     *                         whose names match this regular expression
     * \param[in]     attr     If this and \p value are not NULL, unregister
     *                         level targeting nodes with this node attribute
     *                         set to \p value
     * \param[in]     value    If this and \p attr are not NULL, unregister
     *                         level targeting nodes with node attribute \p attr
     *                         set to this
     * \param[in]     level    Topology level number to remove
     *
     * \return pcmk_ok (if synchronous) or positive call ID (if asynchronous)
     *         on success, otherwise a negative legacy Pacemaker return code
     * \note The caller should set only one of \p node, \p pattern, or \p attr
     *       and \p value.
     */
    int (*remove_level_full)(stonith_t *st, int options,
                             const char *node, const char *pattern,
                             const char *attr, const char *value, int level);

    /*!
     * \brief Register fencing level for specified node, pattern or attribute
     *
     * \param[in,out] st           Fencer connection to use
     * \param[in]     options      Group of enum stonith_call_options
     * \param[in]     node         If not NULL, register level targeting this
     *                             node by name
     * \param[in]     pattern      If not NULL, register level targeting nodes
     *                             whose names match this regular expression
     * \param[in]     attr         If this and \p value are not NULL, register
     *                             level targeting nodes with this node
     *                             attribute set to \p value
     * \param[in]     value        If this and \p attr are not NULL, register
     *                             level targeting nodes with node attribute
     *                             \p attr set to this
     * \param[in]     level        Topology level number to remove
     * \param[in]     device_list  Devices to use in level
     *
     * \return pcmk_ok (if synchronous) or positive call ID (if asynchronous)
     *         on success, otherwise a negative legacy Pacemaker return code
     *
     * \note The caller should set only one of node, pattern or attr/value.
     */
    int (*register_level_full)(stonith_t *st, int options,
                               const char *node, const char *pattern,
                               const char *attr, const char *value, int level,
                               const stonith_key_value_t *device_list);

    /*!
     * \brief Validate an arbitrary stonith device configuration
     *
     * \param[in,out] st            Fencer connection to use
     * \param[in]     call_options  Group of enum stonith_call_options
     * \param[in]     rsc_id        ID used to replace CIB secrets in \p params
     * \param[in]     namespace_s   Type of fence agent to validate ("redhat"
     *                              or "stonith-ng" for RHCS-style, "internal"
     *                              for Pacemaker-internal devices, "heartbeat"
     *                              for LHA-style, or "any" or NULL for any)
     * \param[in]     agent         Fence agent to validate
     * \param[in]     params        Configuration parameters to pass to agent
     * \param[in]     timeout       Fail if no response within this many seconds
     * \param[out]    output        If non-NULL, where to store any agent output
     * \param[out]    error_output  If non-NULL, where to store agent error output
     *
     * \return pcmk_ok if validation succeeds, -errno otherwise
     * \note If pcmk_ok is returned, the caller is responsible for freeing
     *       the output (if requested) with free().
     */
    int (*validate)(stonith_t *st, int call_options, const char *rsc_id,
                    const char *namespace_s, const char *agent,
                    const stonith_key_value_t *params, int timeout,
                    char **output, char **error_output);

    /*!
     * \brief Request delayed fencing of a target
     *
     * \param[in,out] stonith       Fencer connection to use
     * \param[in]     call_options  Group of enum stonith_call_options
     * \param[in]     node          Fence target
     * \param[in]     action        "on", "off", or "reboot"
     * \param[in]     timeout       Default per-device timeout to use with
     *                              each executed device
     * \param[in]     tolerance     Accept result of identical fence action
     *                              completed within this time
     * \param[in]     delay         Execute fencing after this delay (-1
     *                              disables any delay from pcmk_delay_base
     *                              and pcmk_delay_max)
     *
     * \return pcmk_ok (if synchronous) or positive call ID (if asynchronous)
     *         on success, otherwise a negative legacy Pacemaker return code
     */
    int (*fence_with_delay)(stonith_t *stonith, int call_options,
                            const char *node, const char *action, int timeout,
                            int tolerance, int delay);

} stonith_api_operations_t;

struct stonith_s
{
    enum stonith_state state;

    int call_id;
    int call_timeout;   //!< \deprecated Unused
    void *st_private;

    stonith_api_operations_t *cmds;
};
/* *INDENT-ON* */

/* Core functions */
stonith_t *stonith_api_new(void);
void stonith_api_delete(stonith_t * st);

void stonith_dump_pending_callbacks(stonith_t * st);

bool stonith_dispatch(stonith_t * st);

stonith_key_value_t *stonith_key_value_add(stonith_key_value_t * kvp, const char *key,
                                           const char *value);
void stonith_key_value_freeall(stonith_key_value_t * kvp, int keys, int values);

void stonith_history_free(stonith_history_t *history);

// Convenience functions
int stonith_api_connect_retry(stonith_t *st, const char *name,
                              int max_attempts);
const char *stonith_op_state_str(enum op_state state);

/* Basic helpers that allows nodes to be fenced and the history to be
 * queried without mainloop or the caller understanding the full API
 *
 * At least one of nodeid and uname are required
 */
int stonith_api_kick(uint32_t nodeid, const char *uname, int timeout, bool off);
time_t stonith_api_time(uint32_t nodeid, const char *uname, bool in_progress);

/*
 * Helpers for using the above functions without install-time dependencies
 *
 * Usage:
 *  #include <crm/stonith-ng.h>
 *
 * To turn a node off by corosync nodeid:
 *  stonith_api_kick_helper(nodeid, 120, 1);
 *
 * To check the last fence date/time (also by nodeid):
 *  last = stonith_api_time_helper(nodeid, 0);
 *
 * To check if fencing is in progress:
 *  if(stonith_api_time_helper(nodeid, 1) > 0) { ... }
 *
 * eg.

 #include <stdio.h>
 #include <time.h>
 #include <crm/stonith-ng.h>
 int
 main(int argc, char ** argv)
 {
     int rc = 0;
     int nodeid = 102;

     rc = stonith_api_time_helper(nodeid, 0);
     printf("%d last fenced at %s\n", nodeid, ctime(rc));

     rc = stonith_api_kick_helper(nodeid, 120, 1);
     printf("%d fence result: %d\n", nodeid, rc);

     rc = stonith_api_time_helper(nodeid, 0);
     printf("%d last fenced at %s\n", nodeid, ctime(rc));

     return 0;
 }

 */

#  define STONITH_LIBRARY "libstonithd.so.26"

typedef int (*st_api_kick_fn) (int nodeid, const char *uname, int timeout, bool off);
typedef time_t (*st_api_time_fn) (int nodeid, const char *uname, bool in_progress);

static inline int
stonith_api_kick_helper(uint32_t nodeid, int timeout, bool off)
{
    static void *st_library = NULL;
    static st_api_kick_fn st_kick_fn;

    if (st_library == NULL) {
        st_library = dlopen(STONITH_LIBRARY, RTLD_LAZY);
    }
    if (st_library && st_kick_fn == NULL) {
        st_kick_fn = (st_api_kick_fn) dlsym(st_library, "stonith_api_kick");
    }
    if (st_kick_fn == NULL) {
#ifdef ELIBACC
        return -ELIBACC;
#else
        return -ENOSYS;
#endif
    }

    return (*st_kick_fn) (nodeid, NULL, timeout, off);
}

static inline time_t
stonith_api_time_helper(uint32_t nodeid, bool in_progress)
{
    static void *st_library = NULL;
    static st_api_time_fn st_time_fn;

    if (st_library == NULL) {
        st_library = dlopen(STONITH_LIBRARY, RTLD_LAZY);
    }
    if (st_library && st_time_fn == NULL) {
        st_time_fn = (st_api_time_fn) dlsym(st_library, "stonith_api_time");
    }
    if (st_time_fn == NULL) {
        return 0;
    }

    return (*st_time_fn) (nodeid, NULL, in_progress);
}

/**
 * Does the given agent describe a stonith resource that can exist?
 *
 * \param[in] agent     What is the name of the agent?
 * \param[in] timeout   Timeout to use when querying.  If 0 is given,
 *                      use a default of 120.
 *
 * \return A boolean
 */
bool stonith_agent_exists(const char *agent, int timeout);

/*!
 * \brief Turn fence action into a more readable string
 *
 * \param[in] action  Fence action
 */
const char *stonith_action_str(const char *action);

#if !defined(PCMK_ALLOW_DEPRECATED) || (PCMK_ALLOW_DEPRECATED == 1)
/* Normally we'd put this section in a separate file (crm/fencing/compat.h), but
 * we can't do that for the reason noted at the top of this file. That does mean
 * we have to duplicate these declarations where they're implemented.
 */

//! \deprecated Use stonith_get_namespace() instead
const char *get_stonith_provider(const char *agent, const char *provider);

#endif

#ifdef __cplusplus
}
#endif

#endif
