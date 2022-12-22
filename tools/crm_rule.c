/*
 * Copyright 2019-2021 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU General Public License version 2
 * or later (GPLv2+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>

#include <crm/cib.h>
#include <crm/common/cmdline_internal.h>
#include <crm/common/output_internal.h>
#include <crm/common/iso8601.h>
#include <crm/msg_xml.h>
#include <crm/pengine/rules_internal.h>
#include <crm/pengine/status.h>
#include <pacemaker-internal.h>

#include <sys/stat.h>

#define SUMMARY "evaluate rules from the Pacemaker configuration"

enum crm_rule_mode {
    crm_rule_mode_none,
    crm_rule_mode_check
};

struct {
    char *date;
    char *input_xml;
    enum crm_rule_mode mode;
    char *rule;
} options = {
    .mode = crm_rule_mode_none
};

static int crm_rule_check(pe_working_set_t *data_set, const char *rule_id, crm_time_t *effective_date);

static gboolean mode_cb(const gchar *option_name, const gchar *optarg, gpointer data, GError **error);

static GOptionEntry mode_entries[] = {
    { "check", 'c', G_OPTION_FLAG_NO_ARG, G_OPTION_ARG_CALLBACK, mode_cb,
      "Check whether a rule is in effect",
      NULL },

    { NULL }
};

static GOptionEntry data_entries[] = {
    { "xml-text", 'X', 0, G_OPTION_ARG_STRING, &options.input_xml,
      "Use argument for XML (or stdin if '-')",
      NULL },

    { NULL }
};

static GOptionEntry addl_entries[] = {
    { "date", 'd', 0, G_OPTION_ARG_STRING, &options.date,
      "Whether the rule is in effect on a given date",
      NULL },
    { "rule", 'r', 0, G_OPTION_ARG_STRING, &options.rule,
      "The ID of the rule to check",
      NULL },

    { NULL }
};

static gboolean
mode_cb(const gchar *option_name, const gchar *optarg, gpointer data, GError **error) {
    if (strcmp(option_name, "c")) {
        options.mode = crm_rule_mode_check;
    }

    return TRUE;
}

/*!
 * \internal
 * \brief Evaluate a date expression for a specific time
 *
 * \param[in]  time_expr    date_expression XML
 * \param[in]  now          Time for which to evaluate expression
 * \param[out] next_change  If not NULL, set to when evaluation will change
 *
 * \return Standard Pacemaker return code
 */
static int
eval_date_expression(xmlNode *expr, crm_time_t *now, crm_time_t *next_change)
{
    pe_rule_eval_data_t rule_data = {
        .node_hash = NULL,
        .role = RSC_ROLE_UNKNOWN,
        .now = now,
        .match_data = NULL,
        .rsc_data = NULL,
        .op_data = NULL
    };

    return pe__eval_date_expr(expr, &rule_data, next_change);
}

static int
crm_rule_check(pe_working_set_t *data_set, const char *rule_id, crm_time_t *effective_date)
{
    xmlNode *cib_constraints = NULL;
    xmlNode *match = NULL;
    xmlXPathObjectPtr xpathObj = NULL;
    char *xpath = NULL;
    int rc = pcmk_rc_ok;
    int max = 0;

    /* Rules are under the constraints node in the XML, so first find that. */
    cib_constraints = get_object_root(XML_CIB_TAG_CONSTRAINTS, data_set->input);

    /* Get all rules matching the given ID which are also simple enough for us to check.
     * For the moment, these rules must only have a single date_expression child and:
     * - Do not have a date_spec operation, or
     * - Have a date_spec operation that contains years= but does not contain moon=.
     *
     * We do this in steps to provide better error messages.  First, check that there's
     * any rule with the given ID.
     */
    xpath = crm_strdup_printf("//rule[@id='%s']", rule_id);
    xpathObj = xpath_search(cib_constraints, xpath);
    max = numXpathResults(xpathObj);

    if (max == 0) {
        CMD_ERR("No rule found with ID=%s", rule_id);
        rc = ENXIO;
        goto done;
    } else if (max > 1) {
        CMD_ERR("More than one rule with ID=%s found", rule_id);
        rc = ENXIO;
        goto done;
    }

    free(xpath);
    freeXpathObject(xpathObj);

    /* Next, make sure it has exactly one date_expression. */
    xpath = crm_strdup_printf("//rule[@id='%s']//date_expression", rule_id);
    xpathObj = xpath_search(cib_constraints, xpath);
    max = numXpathResults(xpathObj);

    if (max != 1) {
        CMD_ERR("Can't check rule %s because it does not have exactly one date_expression", rule_id);
        rc = EOPNOTSUPP;
        goto done;
    }

    free(xpath);
    freeXpathObject(xpathObj);

    /* Then, check that it's something we actually support. */
    xpath = crm_strdup_printf("//rule[@id='%s']//date_expression[@operation!='date_spec']", rule_id);
    xpathObj = xpath_search(cib_constraints, xpath);
    max = numXpathResults(xpathObj);

    if (max == 0) {
        free(xpath);
        freeXpathObject(xpathObj);

        xpath = crm_strdup_printf("//rule[@id='%s']//date_expression[@operation='date_spec' and date_spec/@years and not(date_spec/@moon)]",
                                  rule_id);
        xpathObj = xpath_search(cib_constraints, xpath);
        max = numXpathResults(xpathObj);

        if (max == 0) {
            CMD_ERR("Rule either must not use date_spec, or use date_spec with years= but not moon=");
            rc = ENXIO;
            goto done;
        }
    }

    match = getXpathResult(xpathObj, 0);

    /* We should have ensured both of these pass with the xpath query above, but
     * double checking can't hurt.
     */
    CRM_ASSERT(match != NULL);
    CRM_ASSERT(find_expression_type(match) == time_expr);

    rc = eval_date_expression(match, effective_date, NULL);

    if (rc == pcmk_rc_within_range) {
        printf("Rule %s is still in effect\n", rule_id);
        rc = pcmk_rc_ok;
    } else if (rc == pcmk_rc_ok) {
        printf("Rule %s satisfies conditions\n", rule_id);
    } else if (rc == pcmk_rc_after_range) {
        printf("Rule %s is expired\n", rule_id);
    } else if (rc == pcmk_rc_before_range) {
        printf("Rule %s has not yet taken effect\n", rule_id);
    } else if (rc == pcmk_rc_op_unsatisfied) {
        printf("Rule %s does not satisfy conditions\n", rule_id);
    } else {
        printf("Could not determine whether rule %s is expired\n", rule_id);
    }

done:
    free(xpath);
    freeXpathObject(xpathObj);
    return rc;
}

static GOptionContext *
build_arg_context(pcmk__common_args_t *args) {
    GOptionContext *context = NULL;

    const char *description = "This tool is currently experimental.\n"
                              "The interface, behavior, and output may change with any version of pacemaker.";

    context = pcmk__build_arg_context(args, NULL, NULL, NULL);
    g_option_context_set_description(context, description);

    pcmk__add_arg_group(context, "modes", "Modes (mutually exclusive):",
                        "Show modes of operation", mode_entries);
    pcmk__add_arg_group(context, "data", "Data:",
                        "Show data options", data_entries);
    pcmk__add_arg_group(context, "additional", "Additional Options:",
                        "Show additional options", addl_entries);
    return context;
}

int
main(int argc, char **argv)
{
    cib_t *cib_conn = NULL;
    pe_working_set_t *data_set = NULL;

    crm_time_t *rule_date = NULL;
    xmlNode *input = NULL;

    int rc = pcmk_ok;
    crm_exit_t exit_code = CRM_EX_OK;
    GError *error = NULL;

    pcmk__common_args_t *args = pcmk__new_common_args(SUMMARY);
    GOptionContext *context = build_arg_context(args);
    gchar **processed_args = pcmk__cmdline_preproc(argv, "drX");

    if (!g_option_context_parse_strv(context, &processed_args, &error)) {
        exit_code = CRM_EX_USAGE;
        goto done;
    }

    pcmk__cli_init_logging("crm_rule", args->verbosity);

    if (args->version) {
        g_strfreev(processed_args);
        pcmk__free_arg_context(context);
        /* FIXME:  When crm_rule is converted to use formatted output, this can go. */
        pcmk__cli_help('v', CRM_EX_OK);
    }

    /* Check command line arguments before opening a connection to
     * the CIB manager or doing anything else important.
     */
    switch(options.mode) {
        case crm_rule_mode_check:
            if (options.rule == NULL) {
                CMD_ERR("--check requires use of --rule=");
                exit_code = CRM_EX_USAGE;
                goto done;
            }

            break;

        default:
            CMD_ERR("No mode operation given");
            exit_code = CRM_EX_USAGE;
            goto done;
            break;
    }

    /* Set up some defaults. */
    rule_date = crm_time_new(options.date);
    if (rule_date == NULL) {
        CMD_ERR("No --date given and can't determine current date");
        exit_code = CRM_EX_DATAERR;
        goto done;
    }

    /* Where does the XML come from?  If one of various command line options were
     * given, use those.  Otherwise, connect to the CIB and use that.
     */
    if (pcmk__str_eq(options.input_xml, "-", pcmk__str_casei)) {
        input = stdin2xml();

        if (input == NULL) {
            CMD_ERR("Couldn't parse input from STDIN\n");
            exit_code = CRM_EX_DATAERR;
            goto done;
        }
    } else if (options.input_xml != NULL) {
        input = string2xml(options.input_xml);

        if (input == NULL) {
            CMD_ERR("Couldn't parse input string: %s\n", options.input_xml);

            exit_code = CRM_EX_DATAERR;
            goto done;
        }
    } else {
        // Establish a connection to the CIB
        cib_conn = cib_new();
        rc = cib_conn->cmds->signon(cib_conn, crm_system_name, cib_command);
        if (rc != pcmk_ok) {
            CMD_ERR("Could not connect to CIB: %s", pcmk_strerror(rc));
            exit_code = crm_errno2exit(rc);
            goto done;
        }
    }

    /* Populate working set from CIB query */
    if (input == NULL) {
        rc = cib_conn->cmds->query(cib_conn, NULL, &input, cib_scope_local | cib_sync_call);
        if (rc != pcmk_ok) {
            exit_code = crm_errno2exit(rc);
            goto done;
        }
    }

    /* Populate the working set instance */
    data_set = pe_new_working_set();
    if (data_set == NULL) {
        exit_code = crm_errno2exit(ENOMEM);
        goto done;
    }
    pe__set_working_set_flags(data_set, pe_flag_no_counts|pe_flag_no_compat);

    data_set->input = input;
    data_set->now = rule_date;

    /* Unpack everything. */
    cluster_status(data_set);

    /* Now do whichever operation mode was asked for.  There's only one at the
     * moment so this looks a little silly, but I expect there will be more
     * modes in the future.
     */
    switch(options.mode) {
        case crm_rule_mode_check:
            rc = crm_rule_check(data_set, options.rule, rule_date);

            if (rc > 0) {
                CMD_ERR("Error checking rule: %s", pcmk_rc_str(rc));
            }

            exit_code = pcmk_rc2exitc(rc);
            break;

        default:
            break;
    }

done:
    if (cib_conn != NULL) {
        cib_conn->cmds->signoff(cib_conn);
        cib_delete(cib_conn);
    }

    g_strfreev(processed_args);
    pcmk__free_arg_context(context);
    pe_free_working_set(data_set);

    pcmk__output_and_clear_error(error, NULL);
    crm_exit(exit_code);
}
