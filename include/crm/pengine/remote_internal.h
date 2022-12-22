/*
 * Copyright 2013-2019 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU Lesser General Public License
 * version 2.1 or later (LGPLv2.1+) WITHOUT ANY WARRANTY.
 */

#ifndef PE_REMOTE__H
#  define PE_REMOTE__H

#ifdef __cplusplus
extern "C" {
#endif

#include <glib.h>                   // gboolean
#include <libxml/tree.h>            // xmlNode
#include <crm/pengine/status.h>

bool xml_contains_remote_node(xmlNode *xml);
bool pe__is_remote_node(const pe_node_t *node);
bool pe__is_guest_node(const pe_node_t *node);
bool pe__is_guest_or_remote_node(const pe_node_t *node);
bool pe__is_bundle_node(const pe_node_t *node);
bool pe__resource_is_remote_conn(const pe_resource_t *rsc,
                                 const pe_working_set_t *data_set);
pe_resource_t *pe__resource_contains_guest_node(const pe_working_set_t *data_set,
                                                const pe_resource_t *rsc);
void pe_foreach_guest_node(const pe_working_set_t *data_set, const pe_node_t *host,
                           void (*helper)(const pe_node_t*, void*), void *user_data);
xmlNode *pe_create_remote_xml(xmlNode *parent, const char *uname,
                              const char *container_id, const char *migrateable,
                              const char *is_managed, const char *start_timeout,
                              const char *server, const char *port);

#ifdef __cplusplus
}
#endif

#endif
