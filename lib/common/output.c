/*
 * Copyright 2019-2021 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU Lesser General Public License
 * version 2.1 or later (LGPLv2.1+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>

#include <crm/common/util.h>
#include <crm/common/xml.h>
#include <libxml/tree.h>

static GHashTable *formatters = NULL;

void
pcmk__output_free(pcmk__output_t *out) {
    out->free_priv(out);

    if (out->messages != NULL) {
        g_hash_table_destroy(out->messages);
    }

    g_free(out->request);
    free(out);
}

int
pcmk__output_new(pcmk__output_t **out, const char *fmt_name, const char *filename,
                 char **argv) {
    pcmk__output_factory_t create = NULL;;

    if (formatters == NULL) {
        return EINVAL;
    }

    /* If no name was given, just try "text".  It's up to each tool to register
     * what it supports so this also may not be valid.
     */
    if (fmt_name == NULL) {
        create = g_hash_table_lookup(formatters, "text");
    } else {
        create = g_hash_table_lookup(formatters, fmt_name);
    }

    if (create == NULL) {
        return pcmk_rc_unknown_format;
    }

    *out = create(argv);
    if (*out == NULL) {
        return ENOMEM;
    }

    if (pcmk__str_eq(filename, "-", pcmk__str_null_matches)) {
        (*out)->dest = stdout;
    } else {
        (*out)->dest = fopen(filename, "w");
        if ((*out)->dest == NULL) {
            return errno;
        }
    }

    (*out)->quiet = false;
    (*out)->messages = pcmk__strkey_table(free, NULL);

    if ((*out)->init(*out) == false) {
        pcmk__output_free(*out);
        return ENOMEM;
    }

    setenv("OCF_OUTPUT_FORMAT", (*out)->fmt_name, 1);

    return pcmk_rc_ok;
}

int
pcmk__register_format(GOptionGroup *group, const char *name,
                      pcmk__output_factory_t create, GOptionEntry *options) {
    if (create == NULL) {
        return -EINVAL;
    }

    if (formatters == NULL) {
        formatters = pcmk__strkey_table(free, NULL);
    }

    if (options != NULL && group != NULL) {
        g_option_group_add_entries(group, options);
    }

    g_hash_table_insert(formatters, strdup(name), create);
    return 0;
}

void
pcmk__register_formats(GOptionGroup *group, pcmk__supported_format_t *formats) {
    pcmk__supported_format_t *entry = NULL;

    if (formats == NULL) {
        return;
    }

    for (entry = formats; entry->name != NULL; entry++) {
        pcmk__register_format(group, entry->name, entry->create, entry->options);
    }
}

void
pcmk__unregister_formats() {
    if (formatters != NULL) {
        g_hash_table_destroy(formatters);
    }
}

int
pcmk__call_message(pcmk__output_t *out, const char *message_id, ...) {
    va_list args;
    int rc = pcmk_rc_ok;
    pcmk__message_fn_t fn;

    fn = g_hash_table_lookup(out->messages, message_id);
    if (fn == NULL) {
        crm_debug("Called unknown output message '%s' for format '%s'",
                  message_id, out->fmt_name);
        return EINVAL;
    }

    va_start(args, message_id);
    rc = fn(out, args);
    va_end(args);

    return rc;
}

void
pcmk__register_message(pcmk__output_t *out, const char *message_id,
                       pcmk__message_fn_t fn) {
    g_hash_table_replace(out->messages, strdup(message_id), fn);
}

void
pcmk__register_messages(pcmk__output_t *out, pcmk__message_entry_t *table) {
    pcmk__message_entry_t *entry;

    for (entry = table; entry->message_id != NULL; entry++) {
        if (pcmk__strcase_any_of(entry->fmt_name, "default", out->fmt_name, NULL)) {
            pcmk__register_message(out, entry->message_id, entry->fn);
        }
    }
}

void
pcmk__output_and_clear_error(GError *error, pcmk__output_t *out)
{
    if (error == NULL) {
        return;
    }

    if (out != NULL) {
        out->err(out, "%s: %s", g_get_prgname(), error->message);
    } else {
        fprintf(stderr, "%s: %s\n", g_get_prgname(), error->message);
    }

    g_clear_error(&error);
}
