/*
 * Copyright 2004-2020 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU General Public License version 2
 * or later (GPLv2+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>
#include <stdio.h>
#include <crm/crm.h>
#include <crm/msg_xml.h>
#include <crm/common/xml.h>
#include <crm/common/ipc.h>
#include <crm/cib/internal.h>

static int message_timeout_ms = 30;
static int command_options = 0;
static int request_id = 0;
static int bump_log_num = 0;

static const char *host = NULL;
static const char *cib_user = NULL;
static const char *cib_action = NULL;
static const char *obj_type = NULL;

static cib_t *the_cib = NULL;
static GMainLoop *mainloop = NULL;
static gboolean force_flag = FALSE;
static crm_exit_t exit_code = CRM_EX_OK;

int do_init(void);
int do_work(xmlNode *input, int command_options, xmlNode **output);
void cibadmin_op_callback(xmlNode *msg, int call_id, int rc, xmlNode *output,
                          void *user_data);

static pcmk__cli_option_t long_options[] = {
    // long option, argument type, storage, short option, description, flags
    {
        "help", no_argument, NULL, '?',
        "\tThis text", pcmk__option_default
    },
    {
        "version", no_argument, NULL, '$',
        "\tVersion information", pcmk__option_default
    },
    {
        "verbose", no_argument, NULL, 'V',
        "\tIncrease debug output\n", pcmk__option_default
    },

    {
        "-spacer-", no_argument, NULL, '-',
        "Commands:", pcmk__option_default
    },
    {
        "upgrade", no_argument, NULL, 'u',
        "\tUpgrade the configuration to the latest syntax", pcmk__option_default
    },
    {
        "query", no_argument, NULL, 'Q',
        "\tQuery the contents of the CIB", pcmk__option_default
    },
    {
        "erase", no_argument, NULL, 'E',
        "\tErase the contents of the whole CIB", pcmk__option_default
    },
    {
        "bump", no_argument, NULL, 'B',
        "\tIncrease the CIB's epoch value by 1", pcmk__option_default
    },
    {
        "create", no_argument, NULL, 'C',
        "\tCreate an object in the CIB (will fail if object already exists)",
        pcmk__option_default
    },
    {
        "modify", no_argument, NULL, 'M',
        "\tFind object somewhere in CIB's XML tree and update it "
            "(fails if object does not exist unless -c is also specified)",
        pcmk__option_default
    },
    {
        "patch", no_argument, NULL, 'P',
        "\tSupply an update in the form of an XML diff (see crm_diff(8))",
        pcmk__option_default
    },
    {
        "replace", no_argument, NULL, 'R',
        "\tRecursively replace an object in the CIB", pcmk__option_default
    },
    {
        "delete", no_argument, NULL, 'D',
        "\tDelete first object matching supplied criteria "
            "(for example, <op id=\"rsc1_op1\" name=\"monitor\"/>)",
        pcmk__option_default
    },
    {
        "-spacer-", no_argument, NULL, '-',
        "\n\tThe XML element name and all attributes must match "
            "in order for the element to be deleted.\n",
        pcmk__option_default
    },
    {
        "delete-all", no_argument, NULL, 'd',
        "When used with --xpath, remove all matching objects in the "
            "configuration instead of just the first one",
        pcmk__option_default
    },
    {
        "empty", no_argument, NULL, 'a',
        "\tOutput an empty CIB", pcmk__option_default
    },
    {
        "md5-sum", no_argument, NULL, '5',
        "\tCalculate the on-disk CIB digest", pcmk__option_default
    },
    {
        "md5-sum-versioned", no_argument, NULL, '6',
        "Calculate an on-the-wire versioned CIB digest", pcmk__option_default
    },
    {
        "blank", no_argument, NULL, '-',
        NULL, pcmk__option_hidden
    },

    {
        "-spacer-", required_argument, NULL, '-',
        "\nAdditional options:", pcmk__option_default
    },
    {
        "force", no_argument, NULL, 'f',
        NULL, pcmk__option_default
    },
    {
        "timeout", required_argument, NULL, 't',
        "Time (in seconds) to wait before declaring the operation failed",
        pcmk__option_default
    },
    {
        "user", required_argument, NULL, 'U',
        "Run the command with permissions of the named user (valid only for "
            "the root and " CRM_DAEMON_USER " accounts)",
        pcmk__option_default
    },
    {
        "sync-call", no_argument, NULL, 's',
        "Wait for call to complete before returning", pcmk__option_default
    },
    {
        "local", no_argument, NULL, 'l',
        "\tCommand takes effect locally (should be used only for queries)",
        pcmk__option_default
    },
    {
        "allow-create", no_argument, NULL, 'c',
        "(Advanced) Allow target of --modify/-M to be created "
            "if it does not exist",
        pcmk__option_default
    },
    {
        "no-children", no_argument, NULL, 'n',
        "(Advanced) When querying an object, do not include its children "
            "in the result",
        pcmk__option_default
    },
    {
        "no-bcast", no_argument, NULL, 'b',
        NULL, pcmk__option_hidden
    },

    {
        "-spacer-", no_argument, NULL, '-',
        "\nData:", pcmk__option_default
    },
    {
        "xml-text", required_argument, NULL, 'X',
        "Retrieve XML from the supplied string", pcmk__option_default
    },
    {
        "xml-file", required_argument, NULL, 'x',
        "Retrieve XML from the named file", pcmk__option_default
    },
    {
        "xml-pipe", no_argument, NULL, 'p',
        "Retrieve XML from stdin\n", pcmk__option_default
    },

    {
        "scope", required_argument, NULL, 'o',
        "Limit scope of operation to specific section of CIB",
        pcmk__option_default
    },
    {
        "-spacer-", no_argument, NULL, '-',
        "\tValid values: configuration, nodes, resources, constraints, "
            "crm_config, rsc_defaults, op_defaults, acls, fencing-topology, "
            "tags, alerts",
        pcmk__option_default
    },

    {
        "xpath", required_argument, NULL, 'A',
        "A valid XPath to use instead of --scope/-o", pcmk__option_default
    },
    {
        "node-path", no_argument, NULL, 'e',
        "When performing XPath queries, return path of any matches found",
        pcmk__option_default
    },
    {
        "-spacer-", no_argument, NULL, '-',
        "\t(for example, \"/cib/configuration/resources/clone[@id='ms_RH1_SCS']"
            "/primitive[@id='prm_RH1_SCS']\")",
        pcmk__option_paragraph
    },
    {
        "node", required_argument, NULL, 'N',
        "(Advanced) Send command to the specified host", pcmk__option_default
    },
    {
        "-spacer-", no_argument, NULL, '!',
        NULL, pcmk__option_hidden
    },
    {
        "-spacer-", no_argument, NULL, '-',
        "\n\nExamples:\n", pcmk__option_default
    },
    {
        "-spacer-", no_argument, NULL, '-',
        "Query the configuration from the local node:", pcmk__option_paragraph
    },
    {
        "-spacer-", no_argument, NULL, '-',
        " cibadmin --query --local", pcmk__option_example
    },

    {
        "-spacer-", no_argument, NULL, '-',
        "Query just the cluster options configuration:", pcmk__option_paragraph
    },
    {
        "-spacer-", no_argument, NULL, '-',
        " cibadmin --query --scope crm_config", pcmk__option_example
    },

    {
        "-spacer-", no_argument, NULL, '-',
        "Query all 'target-role' settings:", pcmk__option_paragraph
    },
    {
        "-spacer-", no_argument, NULL, '-',
        " cibadmin --query --xpath \"//nvpair[@name='target-role']\"",
        pcmk__option_example
    },

    {
        "-spacer-", no_argument, NULL, '-',
        "Remove all 'is-managed' settings:", pcmk__option_paragraph
    },
    {
        "-spacer-", no_argument, NULL, '-',
        " cibadmin --delete-all --xpath \"//nvpair[@name='is-managed']\"",
        pcmk__option_example
    },

    {
        "-spacer-", no_argument, NULL, '-',
        "Remove the resource named 'old':", pcmk__option_paragraph
    },
    {
        "-spacer-", no_argument, NULL, '-',
        " cibadmin --delete --xml-text '<primitive id=\"old\"/>'",
        pcmk__option_example
    },
    {
        "-spacer-", no_argument, NULL, '-',
        "Remove all resources from the configuration:", pcmk__option_paragraph
    },
    {
        "-spacer-", no_argument, NULL, '-',
        " cibadmin --replace --scope resources --xml-text '<resources/>'",
        pcmk__option_example
    },
    {
        "-spacer-", no_argument, NULL, '-',
        "Replace complete configuration with contents of $HOME/pacemaker.xml:",
        pcmk__option_paragraph
    },
    {
        "-spacer-", no_argument, NULL, '-',
        " cibadmin --replace --xml-file $HOME/pacemaker.xml",
        pcmk__option_example
    },
    {
        "-spacer-", no_argument, NULL, '-',
        "Replace constraints section of configuration with contents of "
            "$HOME/constraints.xml:",
        pcmk__option_paragraph
    },
    {
        "-spacer-", no_argument, NULL, '-',
        " cibadmin --replace --scope constraints --xml-file "
            "$HOME/constraints.xml",
        pcmk__option_example
    },
    {
        "-spacer-", no_argument, NULL, '-',
        "Increase configuration version to prevent old configurations from "
            "being loaded accidentally:",
        pcmk__option_paragraph
    },
    {
        "-spacer-", no_argument, NULL, '-',
        " cibadmin --modify --xml-text '<cib admin_epoch=\"admin_epoch++\"/>'",
        pcmk__option_example
    },
    {
        "-spacer-", no_argument, NULL, '-',
        "Edit the configuration with your favorite $EDITOR:",
        pcmk__option_paragraph
    },
    {
        "-spacer-", no_argument, NULL, '-',
        " cibadmin --query > $HOME/local.xml", pcmk__option_example
    },
    {
        "-spacer-", no_argument, NULL, '-',
        " $EDITOR $HOME/local.xml", pcmk__option_example
    },
    {
        "-spacer-", no_argument, NULL, '-',
        " cibadmin --replace --xml-file $HOME/local.xml", pcmk__option_example
    },
    {
        "-spacer-", no_argument, NULL, '-',
        "SEE ALSO:", pcmk__option_default
    },
    {
        "-spacer-", no_argument, NULL, '-',
        " crm(8), pcs(8), crm_shadow(8), crm_diff(8)", pcmk__option_default
    },
    {
        "host", required_argument, NULL, 'h',
        "deprecated", pcmk__option_hidden
    },
    { 0, 0, 0, 0 }
};

static void
print_xml_output(xmlNode * xml)
{
    char *buffer;

    if (!xml) {
        return;
    } else if (xml->type != XML_ELEMENT_NODE) {
        return;
    }

    if (command_options & cib_xpath_address) {
        const char *id = crm_element_value(xml, XML_ATTR_ID);

        if (pcmk__str_eq((const char *)xml->name, "xpath-query", pcmk__str_casei)) {
            xmlNode *child = NULL;

            for (child = xml->children; child; child = child->next) {
                print_xml_output(child);
            }

        } else if (id) {
            printf("%s\n", id);
        }

    } else {
        buffer = dump_xml_formatted(xml);
        fprintf(stdout, "%s", crm_str(buffer));
        free(buffer);
    }
}

// Upgrade requested but already at latest schema
static void
report_schema_unchanged(void)
{
    const char *err = pcmk_strerror(pcmk_err_schema_unchanged);

    crm_info("Upgrade unnecessary: %s\n", err);
    printf("Upgrade unnecessary: %s\n", err);
    exit_code = CRM_EX_OK;
}

int
main(int argc, char **argv)
{
    int argerr = 0;
    int rc = pcmk_ok;
    int flag;
    const char *source = NULL;
    const char *admin_input_xml = NULL;
    const char *admin_input_file = NULL;
    gboolean dangerous_cmd = FALSE;
    gboolean admin_input_stdin = FALSE;
    xmlNode *output = NULL;
    xmlNode *input = NULL;

    int option_index = 0;

    pcmk__cli_init_logging("cibadmin", 0);
    set_crm_log_level(LOG_CRIT);
    pcmk__set_cli_options(NULL, "<command> [options]", long_options,
                          "query and edit the Pacemaker configuration");

    if (argc < 2) {
        pcmk__cli_help('?', CRM_EX_USAGE);
    }

    while (1) {
        flag = pcmk__next_cli_option(argc, argv, &option_index, NULL);
        if (flag == -1)
            break;

        switch (flag) {
            case 't':
                message_timeout_ms = atoi(optarg);
                if (message_timeout_ms < 1) {
                    message_timeout_ms = 30;
                }
                break;
            case 'A':
                obj_type = optarg;
                cib__set_call_options(command_options, crm_system_name,
                                      cib_xpath);
                break;
            case 'e':
                cib__set_call_options(command_options, crm_system_name,
                                      cib_xpath_address);
                break;
            case 'u':
                cib_action = CIB_OP_UPGRADE;
                dangerous_cmd = TRUE;
                break;
            case 'E':
                cib_action = CIB_OP_ERASE;
                dangerous_cmd = TRUE;
                break;
            case 'Q':
                cib_action = CIB_OP_QUERY;
                break;
            case 'P':
                cib_action = CIB_OP_APPLY_DIFF;
                break;
            case 'U':
                cib_user = optarg;
                break;
            case 'M':
                cib_action = CIB_OP_MODIFY;
                break;
            case 'R':
                cib_action = CIB_OP_REPLACE;
                break;
            case 'C':
                cib_action = CIB_OP_CREATE;
                break;
            case 'D':
                cib_action = CIB_OP_DELETE;
                break;
            case '5':
                cib_action = "md5-sum";
                break;
            case '6':
                cib_action = "md5-sum-versioned";
                break;
            case 'c':
                cib__set_call_options(command_options, crm_system_name,
                                      cib_can_create);
                break;
            case 'n':
                cib__set_call_options(command_options, crm_system_name,
                                      cib_no_children);
                break;
            case 'B':
                cib_action = CIB_OP_BUMP;
                crm_log_args(argc, argv);
                break;
            case 'V':
                cib__set_call_options(command_options, crm_system_name,
                                      cib_verbose);
                bump_log_num++;
                break;
            case '?':
            case '$':
            case '!':
                pcmk__cli_help(flag, CRM_EX_OK);
                break;
            case 'o':
                crm_trace("Option %c => %s", flag, optarg);
                obj_type = optarg;
                break;
            case 'X':
                crm_trace("Option %c => %s", flag, optarg);
                admin_input_xml = optarg;
                crm_log_args(argc, argv);
                break;
            case 'x':
                crm_trace("Option %c => %s", flag, optarg);
                admin_input_file = optarg;
                crm_log_args(argc, argv);
                break;
            case 'p':
                admin_input_stdin = TRUE;
                crm_log_args(argc, argv);
                break;
            case 'N':
            case 'h':
                host = strdup(optarg);
                break;
            case 'l':
                cib__set_call_options(command_options, crm_system_name,
                                      cib_scope_local);
                break;
            case 'd':
                cib_action = CIB_OP_DELETE;
                cib__set_call_options(command_options, crm_system_name,
                                      cib_multiple);
                dangerous_cmd = TRUE;
                break;
            case 'b':
                dangerous_cmd = TRUE;
                cib__set_call_options(command_options, crm_system_name,
                                      cib_inhibit_bcast|cib_scope_local);
                break;
            case 's':
                cib__set_call_options(command_options, crm_system_name,
                                      cib_sync_call);
                break;
            case 'f':
                force_flag = TRUE;
                cib__set_call_options(command_options, crm_system_name,
                                      cib_quorum_override);
                crm_log_args(argc, argv);
                break;
            case 'a':
                output = createEmptyCib(1);
                if (optind < argc) {
                    crm_xml_add(output, XML_ATTR_VALIDATION, argv[optind]);
                }
                admin_input_xml = dump_xml_formatted(output);
                fprintf(stdout, "%s\n", crm_str(admin_input_xml));
                crm_exit(CRM_EX_OK);
                break;
            default:
                printf("Argument code 0%o (%c)" " is not (?yet?) supported\n", flag, flag);
                ++argerr;
                break;
        }
    }

    while (bump_log_num > 0) {
        crm_bump_log_level(argc, argv);
        bump_log_num--;
    }

    if (optind < argc) {
        printf("non-option ARGV-elements: ");
        while (optind < argc)
            printf("%s ", argv[optind++]);
        printf("\n");
        pcmk__cli_help('?', CRM_EX_USAGE);
    }

    if (optind > argc || cib_action == NULL) {
        ++argerr;
    }

    if (argerr) {
        pcmk__cli_help('?', CRM_EX_USAGE);
    }

    if (dangerous_cmd && force_flag == FALSE) {
        fprintf(stderr, "The supplied command is considered dangerous."
                "  To prevent accidental destruction of the cluster,"
                " the --force flag is required in order to proceed.\n");
        fflush(stderr);
        crm_exit(CRM_EX_UNSAFE);
    }

    if (admin_input_file != NULL) {
        input = filename2xml(admin_input_file);
        source = admin_input_file;

    } else if (admin_input_xml != NULL) {
        source = "input string";
        input = string2xml(admin_input_xml);

    } else if (admin_input_stdin) {
        source = "STDIN";
        input = stdin2xml();
    }

    if (input != NULL) {
        crm_log_xml_debug(input, "[admin input]");

    } else if (source) {
        fprintf(stderr, "Couldn't parse input from %s.\n", source);
        crm_exit(CRM_EX_CONFIG);
    }

    if (pcmk__str_eq(cib_action, "md5-sum", pcmk__str_casei)) {
        char *digest = NULL;

        if (input == NULL) {
            fprintf(stderr, "Please supply XML to process with -X, -x or -p\n");
            crm_exit(CRM_EX_USAGE);
        }

        digest = calculate_on_disk_digest(input);
        fprintf(stderr, "Digest: ");
        fprintf(stdout, "%s\n", crm_str(digest));
        free(digest);
        free_xml(input);
        crm_exit(CRM_EX_OK);

    } else if (pcmk__str_eq(cib_action, "md5-sum-versioned", pcmk__str_casei)) {
        char *digest = NULL;
        const char *version = NULL;

        if (input == NULL) {
            fprintf(stderr, "Please supply XML to process with -X, -x or -p\n");
            crm_exit(CRM_EX_USAGE);
        }

        version = crm_element_value(input, XML_ATTR_CRM_VERSION);
        digest = calculate_xml_versioned_digest(input, FALSE, TRUE, version);
        fprintf(stderr, "Versioned (%s) digest: ", version);
        fprintf(stdout, "%s\n", crm_str(digest));
        free(digest);
        free_xml(input);
        crm_exit(CRM_EX_OK);
    }

    rc = do_init();
    if (rc != pcmk_ok) {
        crm_err("Init failed, could not perform requested operations");
        fprintf(stderr, "Init failed, could not perform requested operations\n");
        free_xml(input);
        crm_exit(crm_errno2exit(rc));
    }

    rc = do_work(input, command_options, &output);
    if (rc > 0) {
        /* wait for the reply by creating a mainloop and running it until
         * the callbacks are invoked...
         */
        request_id = rc;

        the_cib->cmds->register_callback(the_cib, request_id, message_timeout_ms, FALSE, NULL,
                                         "cibadmin_op_callback", cibadmin_op_callback);

        mainloop = g_main_loop_new(NULL, FALSE);

        crm_trace("%s waiting for reply from the local CIB", crm_system_name);

        crm_info("Starting mainloop");
        g_main_loop_run(mainloop);

    } else if ((rc == -pcmk_err_schema_unchanged)
               && pcmk__str_eq(cib_action, CIB_OP_UPGRADE, pcmk__str_none)) {
        report_schema_unchanged();

    } else if (rc < 0) {
        crm_err("Call failed: %s", pcmk_strerror(rc));
        fprintf(stderr, "Call failed: %s\n", pcmk_strerror(rc));

        if (rc == -pcmk_err_schema_validation) {
            if (pcmk__str_eq(cib_action, CIB_OP_UPGRADE, pcmk__str_none)) {
                xmlNode *obj = NULL;
                int version = 0, rc = 0;

                rc = the_cib->cmds->query(the_cib, NULL, &obj, command_options);
                if (rc == pcmk_ok) {
                    update_validation(&obj, &version, 0, TRUE, FALSE);
                }

            } else if (output) {
                validate_xml_verbose(output);
            }
        }
        exit_code = crm_errno2exit(rc);
    }

    if (output != NULL) {
        print_xml_output(output);
        free_xml(output);
    }

    crm_trace("%s exiting normally", crm_system_name);

    free_xml(input);
    rc = the_cib->cmds->signoff(the_cib);
    if (exit_code == CRM_EX_OK) {
        exit_code = crm_errno2exit(rc);
    }
    cib_delete(the_cib);

    crm_exit(exit_code);
}

int
do_work(xmlNode * input, int call_options, xmlNode ** output)
{
    /* construct the request */
    the_cib->call_timeout = message_timeout_ms;
    if (strcasecmp(CIB_OP_REPLACE, cib_action) == 0
        && pcmk__str_eq(crm_element_name(input), XML_TAG_CIB, pcmk__str_casei)) {
        xmlNode *status = get_object_root(XML_CIB_TAG_STATUS, input);

        if (status == NULL) {
            create_xml_node(input, XML_CIB_TAG_STATUS);
        }
    }

    if (cib_action != NULL) {
        crm_trace("Passing \"%s\" to variant_op...", cib_action);
        return cib_internal_op(the_cib, cib_action, host, obj_type, input, output, call_options, cib_user);

    } else {
        crm_err("You must specify an operation");
    }
    return -EINVAL;
}

int
do_init(void)
{
    int rc = pcmk_ok;

    the_cib = cib_new();
    rc = the_cib->cmds->signon(the_cib, crm_system_name, cib_command);
    if (rc != pcmk_ok) {
        crm_err("Could not connect to the CIB: %s", pcmk_strerror(rc));
        fprintf(stderr, "Could not connect to the CIB: %s\n",
                pcmk_strerror(rc));
    }

    return rc;
}

void
cibadmin_op_callback(xmlNode * msg, int call_id, int rc, xmlNode * output, void *user_data)
{
    exit_code = crm_errno2exit(rc);

    if (rc == -pcmk_err_schema_unchanged) {
        report_schema_unchanged();

    } else if (rc != pcmk_ok) {
        crm_warn("Call %s failed (%d): %s", cib_action, rc, pcmk_strerror(rc));
        fprintf(stderr, "Call %s failed (%d): %s\n", cib_action, rc, pcmk_strerror(rc));
        print_xml_output(output);

    } else if (pcmk__str_eq(cib_action, CIB_OP_QUERY, pcmk__str_casei) && output == NULL) {
        crm_err("Query returned no output");
        crm_log_xml_err(msg, "no output");

    } else if (output == NULL) {
        crm_info("Call passed");

    } else {
        crm_info("Call passed");
        print_xml_output(output);
    }

    if (call_id == request_id) {
        g_main_loop_quit(mainloop);

    } else {
        crm_info("Message was not the response we were looking for (%d vs. %d)",
                 call_id, request_id);
    }
}
