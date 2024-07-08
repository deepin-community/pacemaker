/*
 * Copyright 2004-2022 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU Lesser General Public License
 * version 2.1 or later (LGPLv2.1+) WITHOUT ANY WARRANTY.
 */

#ifndef CONTROLD_FENCING__H
#  define CONTROLD_FENCING__H

#include <stdbool.h>                // bool
#include <pacemaker-internal.h>     // pcmk__graph_t, pcmk__graph_action_t

void controld_configure_fencing(GHashTable *options);

// stonith fail counts
void st_fail_count_reset(const char * target);

// stonith API client
void controld_trigger_fencer_connect(void);
void controld_disconnect_fencer(bool destroy);
int controld_execute_fence_action(pcmk__graph_t *graph,
                                  pcmk__graph_action_t *action);
bool controld_verify_stonith_watchdog_timeout(const char *value);

// stonith cleanup list
void add_stonith_cleanup(const char *target);
void remove_stonith_cleanup(const char *target);
void purge_stonith_cleanup(void);
void execute_stonith_cleanup(void);

// stonith history synchronization
void te_trigger_stonith_history_sync(bool long_timeout);
void te_cleanup_stonith_history_sync(stonith_t *st, bool free_timers);

#endif
