/*
 * Original copyright 2009 International Business Machines, IBM, Mark Hamzy
 * Later changes copyright 2009-2021 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU General Public License version 2
 * or later (GPLv2+) WITHOUT ANY WARRANTY.
 */

/* gcc -o notifyServicelogEvent `pkg-config --cflags servicelog-1` `pkg-config --libs servicelog-1` notifyServicelogEvent.c
*/

#include <crm_internal.h>

#include <string.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>
#include <servicelog.h>
#include <syslog.h>
#include <unistd.h>
#include <inttypes.h>  /* U64T ~ PRIu64, U64TS ~ SCNu64 */

#ifndef PCMK__CONFIG_H
#  define PCMK__CONFIG_H
#  include <config.h>
#endif

#include <crm/common/xml.h>
#include <crm/common/util.h>
#include <crm/common/attrd_internal.h>

typedef enum { STATUS_GREEN = 1, STATUS_YELLOW, STATUS_RED } STATUS;

const char *status2char(STATUS status);
STATUS event2status(struct sl_event *event);

const char *
status2char(STATUS status)
{
    switch (status) {
        default:
        case STATUS_GREEN:
            return "green";
        case STATUS_YELLOW:
            return "yellow";
        case STATUS_RED:
            return "red";
    }
}

STATUS
event2status(struct sl_event * event)
{
    STATUS status = STATUS_GREEN;

    crm_debug("Severity = %d, Disposition = %d", event->severity, event->disposition);

    /* @TBD */
    if (event->severity == SL_SEV_WARNING) {
        status = STATUS_YELLOW;
    }

    if (event->disposition == SL_DISP_UNRECOVERABLE) {
        status = STATUS_RED;
    }

    return status;
}

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
        "-spacer-", no_argument, NULL, '-',
        "\nUsage: notifyServicelogEvent event_id", pcmk__option_paragraph
    },
    {
        "-spacer-", no_argument, NULL, '-',
        "Where event_id is a unique unsigned event identifier which is "
            "then passed into servicelog",
        pcmk__option_paragraph
    },
    { 0, 0, 0, 0 }
};

int
main(int argc, char *argv[])
{
    int argerr = 0;
    int flag;
    int index = 0;
    int rc = 0;
    servicelog *slog = NULL;
    struct sl_event *event = NULL;
    uint64_t event_id = 0;

    pcmk__cli_init_logging("notifyServicelogEvent", 0);
    pcmk__set_cli_options(NULL, "<event_id>", long_options,
                          "handle events written to servicelog database");

    if (argc < 2) {
        argerr++;
    }

    while (1) {
        flag = pcmk__next_cli_option(argc, argv, &index, NULL);
        if (flag == -1)
            break;

        switch (flag) {
            case '?':
            case '$':
                pcmk__cli_help(flag, CRM_EX_OK);
                break;
            default:
                ++argerr;
                break;
        }
    }

    if (argc - optind != 1) {
        ++argerr;
    }

    if (argerr) {
        pcmk__cli_help('?', CRM_EX_USAGE);
    }

    openlog("notifyServicelogEvent", LOG_NDELAY, LOG_USER);

    if (sscanf(argv[optind], "%" U64TS, &event_id) != 1) {
        crm_err("Error: could not read event_id from args!");

        rc = 1;
        goto done;
    }

    if (event_id == 0) {
        crm_err("Error: event_id is 0!");

        rc = 1;
        goto done;
    }

    rc = servicelog_open(&slog, 0);     /* flags is one of SL_FLAG_xxx */

    if (!slog) {
        crm_err("Error: servicelog_open failed, rc = %d", rc);

        rc = 1;
        goto done;
    }

    if (slog) {
        rc = servicelog_event_get(slog, event_id, &event);
    }

    if (rc == 0) {
        STATUS status = STATUS_GREEN;
        const char *health_component = "#health-ipmi";
        const char *health_status = NULL;

        crm_debug("Event id = %" U64T ", Log timestamp = %s, Event timestamp = %s",
                  event_id, ctime(&(event->time_logged)), ctime(&(event->time_event)));

        status = event2status(event);

        health_status = status2char(status);

        if (health_status) {
            int attrd_rc;

            // @TODO pass pcmk__node_attr_remote when appropriate
            attrd_rc = pcmk__node_attr_request(NULL, 'v', NULL,
                                               health_component, health_status,
                                               NULL, NULL, NULL, NULL,
                                               pcmk__node_attr_none);
            crm_debug("Updating attribute ('%s', '%s') = %d",
                      health_component, health_status, attrd_rc);
        } else {
            crm_err("Error: status2char failed, status = %d", status);
            rc = 1;
        }
    } else {
        crm_err("Error: servicelog_event_get failed, rc = %d", rc);
    }

  done:
    if (event) {
        servicelog_event_free(event);
    }

    if (slog) {
        servicelog_close(slog);
    }

    closelog();

    return rc;
}
