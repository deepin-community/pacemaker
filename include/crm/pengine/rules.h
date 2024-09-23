/*
 * Copyright 2004-2022 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU Lesser General Public License
 * version 2.1 or later (LGPLv2.1+) WITHOUT ANY WARRANTY.
 */

#ifndef PCMK__CRM_PENGINE_RULES__H
#  define PCMK__CRM_PENGINE_RULES__H

#  include <glib.h>
#  include <crm/crm.h>
#  include <crm/common/iso8601.h>
#  include <crm/pengine/common.h>

#ifdef __cplusplus
extern "C" {
#endif

enum expression_type {
    not_expr        = 0,
    nested_rule     = 1,
    attr_expr       = 2,
    loc_expr        = 3,
    role_expr       = 4,
    time_expr       = 5,
#if !defined(PCMK_ALLOW_DEPRECATED) || (PCMK_ALLOW_DEPRECATED == 1)
    //! \deprecated Do not use (will be removed in a future release)
    version_expr    = 6,
#endif
    rsc_expr        = 7,
    op_expr         = 8,
};

enum expression_type find_expression_type(xmlNode * expr);

gboolean pe_evaluate_rules(xmlNode *ruleset, GHashTable *node_hash,
                           crm_time_t *now, crm_time_t *next_change);

gboolean pe_test_rule(xmlNode *rule, GHashTable *node_hash,
                      enum rsc_role_e role, crm_time_t *now,
                      crm_time_t *next_change, pe_match_data_t *match_data);

gboolean pe_test_expression(xmlNode *expr, GHashTable *node_hash,
                            enum rsc_role_e role, crm_time_t *now,
                            crm_time_t *next_change,
                            pe_match_data_t *match_data);

void pe_eval_nvpairs(xmlNode *top, const xmlNode *xml_obj, const char *set_name,
                     const pe_rule_eval_data_t *rule_data, GHashTable *hash,
                     const char *always_first, gboolean overwrite,
                     crm_time_t *next_change);

void pe_unpack_nvpairs(xmlNode *top, const xmlNode *xml_obj,
                       const char *set_name, GHashTable *node_hash,
                       GHashTable *hash, const char *always_first,
                       gboolean overwrite, crm_time_t *now,
                       crm_time_t *next_change);

char *pe_expand_re_matches(const char *string,
                           const pe_re_match_data_t *match_data);

gboolean pe_eval_rules(xmlNode *ruleset, const pe_rule_eval_data_t *rule_data,
                       crm_time_t *next_change);
gboolean pe_eval_expr(xmlNode *rule, const pe_rule_eval_data_t *rule_data,
                      crm_time_t *next_change);
gboolean pe_eval_subexpr(xmlNode *expr, const pe_rule_eval_data_t *rule_data,
                         crm_time_t *next_change);

#if !defined(PCMK_ALLOW_DEPRECATED) || (PCMK_ALLOW_DEPRECATED == 1)
#include <crm/pengine/rules_compat.h>
#endif

#ifdef __cplusplus
}
#endif

#endif
