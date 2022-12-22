/*
 * Copyright 2021 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU Lesser General Public License
 * version 2.1 or later (LGPLv2.1+) WITHOUT ANY WARRANTY.
 */
#ifndef PCMKI_RESOURCE__H
#define PCMKI_RESOURCE__H

#include <glib.h>

#include <crm/common/output_internal.h>
#include <crm/pengine/pe_types.h>

int pcmk__resource_digests(pcmk__output_t *out, pe_resource_t *rsc,
                           pe_node_t *node, GHashTable *overrides,
                           pe_working_set_t *data_set);

#endif /* PCMK_RESOURCE__H */
