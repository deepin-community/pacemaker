/*
 * Copyright 2004-2020 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU Lesser General Public License
 * version 2.1 or later (LGPLv2.1+) WITHOUT ANY WARRANTY.
 */

#ifndef PCMK__COMMON_ACL__H
#  define PCMK__COMMON_ACL__H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \file
 * \brief Low-level API for XML Access Control Lists (ACLs)
 * \ingroup core
 */

#  include <libxml/tree.h> // xmlNode

bool xml_acl_enabled(xmlNode *xml);
void xml_acl_disable(xmlNode *xml);
bool xml_acl_denied(xmlNode *xml);
bool xml_acl_filtered_copy(const char *user, xmlNode* acl_source, xmlNode *xml,
                           xmlNode **result);

bool pcmk_acl_required(const char *user);

#ifdef __cplusplus
}
#endif

#endif // PCMK__COMMON_ACL__H
