/*
 * Copyright 2010-2021 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU Lesser General Public License
 * version 2.1 or later (LGPLv2.1+) WITHOUT ANY WARRANTY.
 */

#ifndef SERVICES_LSB__H
#  define SERVICES_LSB__H

G_GNUC_INTERNAL int services__get_lsb_metadata(const char *type, char **output);
G_GNUC_INTERNAL GList *services__list_lsb_agents(void);
G_GNUC_INTERNAL char *services__lsb_agent_path(const char *agent);
G_GNUC_INTERNAL bool services__lsb_agent_exists(const char *agent);

#endif
