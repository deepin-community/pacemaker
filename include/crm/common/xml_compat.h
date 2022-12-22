/*
 * Copyright 2004-2021 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU Lesser General Public License
 * version 2.1 or later (LGPLv2.1+) WITHOUT ANY WARRANTY.
 */

#ifndef PCMK__COMMON_XML_COMPAT__H
#  define PCMK__COMMON_XML_COMPAT__H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \file
 * \brief Deprecated Pacemaker XML API
 * \ingroup core
 * \deprecated Do not include this header directly. The XML APIs in this
 *             header, and the header itself, will be removed in a future
 *             release.
 */

//! \deprecated This function will be removed in a future release
xmlNode *find_entity(xmlNode *parent, const char *node_name, const char *id);

//!  \deprecated Use xml_apply_patchset() instead
gboolean apply_xml_diff(xmlNode *old_xml, xmlNode *diff, xmlNode **new_xml);

#ifdef __cplusplus
}
#endif

#endif // PCMK__COMMON_XML_COMPAT__H
