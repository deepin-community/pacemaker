/*
 * Copyright 2004-2021 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU Lesser General Public License
 * version 2.1 or later (LGPLv2.1+) WITHOUT ANY WARRANTY.
 */

#ifndef PCMK__MSG_XML_COMPAT__H
#  define PCMK__MSG_XML_COMPAT__H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \file
 * \brief Deprecated Pacemaker XML constants API
 * \ingroup core
 * \deprecated Do not include this header directly. The XML constants in this
 *             header, and the header itself, will be removed in a future
 *             release.
 */

//! \deprecated Use PCMK_STONITH_PROVIDES instead
#define XML_RSC_ATTR_PROVIDES       "provides"

//! \deprecated Use PCMK_XE_PROMOTED_LEGACY instead
#define XML_CIB_TAG_MASTER PCMK_XE_PROMOTED_LEGACY

//! \deprecated Use PCMK_XE_PROMOTED_LEGACY instead
#define XML_RSC_ATTR_MASTER_MAX PCMK_XE_PROMOTED_MAX_LEGACY

//! \deprecated Use PCMK_XE_PROMOTED_LEGACY instead
#define XML_RSC_ATTR_MASTER_NODEMAX PCMK_XE_PROMOTED_NODE_MAX_LEGACY

#ifdef __cplusplus
}
#endif

#endif // PCMK__MSG_XML_COMPAT__H
