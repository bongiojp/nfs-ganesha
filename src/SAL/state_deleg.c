/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright IBM  (2014)
 * contributeur : Jeremy Bongio   jbongio@us.ibm.com
 *
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 * ---------------------------------------
 */

/**
 * @defgroup SAL State abstraction layer
 * @{
 */

/**
 * @file state_deleg.c
 * @brief Delegation management
 */

#include "config.h"
#include <unistd.h>
#include <sys/types.h>
#include <sys/param.h>
#include <time.h>
#include <pthread.h>
#include <string.h>
#include <assert.h>

#include "fsal.h"
#include "nfs_core.h"
#include "nfs4.h"
#include "sal_functions.h"
#include "nlm_util.h"
#include "cache_inode_lru.h"

/* The caller must hold the state lock exclusively. */
void free_deleg_locked(state_lock_entry_t *deleg_lock, cache_entry_t *entry,
                       struct fsal_export *export,
                       struct req_op_context *fake_req_ctx) {
  nfs_client_id_t *clientid =
    deleg_lock->sle_owner->so_owner.so_nfs4_owner.so_clientrec;

  /* Decrement state lock entry reference, which will eventually remove it. */
  state_unlock(entry, export->exp_entry,
               fake_req_ctx,
               deleg_lock->sle_owner,
               deleg_lock->sle_state,
               &deleg_lock->sle_lock,
               deleg_lock->sle_type);

  /* Remove state entry */
  state_del_locked(deleg_lock->sle_state, entry);

  deleg_heuristics_recall(entry, clientid, deleg_lock);
}

void init_clientfile_deleg(clientfile_deleg_heuristics_t *clfile_entry) {

}

void init_new_deleg_state(state_data_t *deleg_state, state_t *open_state,
                          open_delegation_type4 sd_type,
                          nfs_client_id_t *clientid) {
  clientfile_deleg_heuristics_t *clfile_entry =
    &deleg_state->deleg.clfile_stats;
  // deleg_state->deleg.sd_stateid is created uniquely with state_add_impl()
  deleg_state->deleg.sd_open_state = open_state;
  deleg_state->deleg.sd_type = sd_type;
  deleg_state->deleg.grant_time = time(NULL);

  clfile_entry->clientid = clientid;
  clfile_entry->dh_last_del = 0;
  clfile_entry->num_recalls = 0;
  clfile_entry->num_recall_badhandles = 0;
  clfile_entry->num_recall_races = 0;
  clfile_entry->num_recall_timeouts = 0;
  clfile_entry->num_recall_aborts = 0;
}

bool update_delegation_stats(cache_entry_t *entry, state_t *state) {
  clientfile_deleg_heuristics_t *clfile_entry = &state->state_data.deleg.clfile_stats;

  /* Update delegation stats for file. */
  file_deleg_heuristics_t *statistics = &entry->object.file.deleg_heuristics;
  statistics->curr_delegations++;
  statistics->dh_disabled = false;
  statistics->dh_del_count++;
  statistics->dh_last_del = time(NULL);

  /* Update delegation stats for client. */
  clfile_entry->clientid->deleg_heuristics.deleg_grants++;

  /* Update delegation stats for client-file. */
  clfile_entry->dh_last_del = statistics->dh_last_del;

  return true;
}

static int advance_avg(time_t prev_avg, time_t new_time,
		       uint32_t prev_tot, uint32_t curr_tot) {
  return ((prev_tot * prev_avg) + new_time) / curr_tot;
}

bool deleg_heuristics_recall(cache_entry_t *entry, nfs_client_id_t *clientid,
                             state_lock_entry_t *deleg_lock) {
  //  clientfile_deleg_heuristics_t *clfile_entry;

  /* Update delegation stats for file. */
  file_deleg_heuristics_t *statistics = &entry->object.file.deleg_heuristics;
  statistics->curr_delegations--;
  statistics->dh_disabled = false;
  statistics->dh_rec_count++;

  /* Update delegation stats for client. */
  clientid->deleg_heuristics.deleg_grants++;

  /* Update delegation stats for client-file. */
  statistics->dh_avg_hold = advance_avg(statistics->dh_avg_hold,
                                        time(NULL) - statistics->dh_last_del,
                                        statistics->dh_rec_count - 1,
                                        statistics->dh_rec_count);
  return true;
}

void init_deleg_heuristics(cache_entry_t *entry) {
  file_deleg_heuristics_t *statistics; 
  if (entry->type == REGULAR_FILE ||
      entry->type == CHARACTER_FILE ||
      entry->type == BLOCK_FILE ||
      entry->type == SOCKET_FILE ||
      entry->type == FIFO_FILE) {
    statistics = &entry->object.file.deleg_heuristics;

    statistics->curr_delegations = 0;
    statistics->deleg_type = OPEN_DELEGATE_NONE;
    statistics->dh_disabled = false;
    statistics->dh_del_count = 0;
    statistics->dh_rec_count = 0;
    statistics->dh_last_del = 0;
    statistics->dh_last_rec = 0;
    statistics->dh_avg_hold = 0;
    statistics->num_opens = 0;
    statistics->first_open = 0;
  }
}

/* Whether the export supports delegations should be checked before calling
 * this function. */
bool should_we_grant_deleg(cache_entry_t *entry, nfs_client_id_t *client,
                           state_t *open_state) {
  /* specific file, all clients, stats */
  file_deleg_heuristics_t *file_stats = &entry->object.file.deleg_heuristics;
  /* specific client, all files stats */
  client_deleg_heuristics_t *cl_stats = &client->deleg_heuristics;
  /* specific client, specific file stats */
  //  clientfile_deleg_heuristics_t *clfl_stats;
  float ACCEPTABLE_FAILS = 0.1; // 10%
  float ACCEPTABLE_OPEN_FREQUENCY = .01; // per second
  time_t spread;

  if (open_state->state_type != STATE_TYPE_SHARE) {
    LogDebug(COMPONENT_STATE, "should_we_grant_deleg() expects a SHARE open "
             "state and no other.");
    return false;
  }

  if (file_stats->deleg_type == OPEN_DELEGATE_NONE) {
    LogDebug(COMPONENT_STATE, "OPEN_DELEGATE_NONE requests, returning false.");
    return false;
  }

  /* Check if this file is opened too frequently to delegate. */
  spread = time(NULL) - file_stats->first_open;
  if ((file_stats->num_opens / spread) > ACCEPTABLE_OPEN_FREQUENCY) {
    LogDebug(COMPONENT_STATE, "This file is opened too frequently to delegate.");
    return false;
  }

  /* Check if open state and requested delegation agree. */
  if (file_stats->curr_delegations > 0) {
    if (file_stats->deleg_type == OPEN_DELEGATE_READ &&
        open_state->state_data.share.share_access & OPEN4_SHARE_ACCESS_WRITE) {
      LogMidDebug(COMPONENT_STATE, "READ delegate requested, but file is opened"
               " for WRITE.");
      return false;
    }
    if (file_stats->deleg_type == OPEN_DELEGATE_WRITE &&
        !(open_state->state_data.share.share_access & OPEN4_SHARE_ACCESS_WRITE)) {
            LogMidDebug(COMPONENT_STATE, "WRITE delegate requested, but file is not"
                     " opened for WRITE.");
    }
  }

  /* TODO: Check if other delegations have already been given or if file is already
   * opened for write by someone and we're requesting READ delegation. */

  /* TODO: Check if this file has too many OPENs for effective delegations */

  /* Check if this is a misbehaving or unreliable client */
  if (cl_stats->tot_recalls > 0 &&
      ((1.0 - (cl_stats->failed_recalls / cl_stats->tot_recalls))
       > ACCEPTABLE_FAILS)) {
    LogDebug(COMPONENT_STATE, "Client is %.0f unreliable during recalls."
             "Allowed failure rate is %.0f. Denying delegation.",
             1.0 - (cl_stats->failed_recalls / cl_stats->tot_recalls),
             ACCEPTABLE_FAILS);
    return false;
  }
  //cl_stats->deleg_grants;

  /* Check if the historical access of this file makes a delegation
   * intelligent. */

  // minimum average milliseconds that delegations should be held on a file.
  // if less, then this is not a good file for delegations.
#define MIN_AVG_HOLD 15000
  if (file_stats->dh_avg_hold < MIN_AVG_HOLD) {
    LogDebug(COMPONENT_STATE, "Average length of delegation (%lld) is less than "
             "minimum avg (%lld). Denying delegation.", (long long) file_stats->dh_avg_hold,
	     (long long) MIN_AVG_HOLD);
    return false;
  }

  /* Check this client's behavior with this file. */
  //  glist_for_each(glist, &entry->object.file.deleg_heuristic_list) {
  //    found_entry = glist_entry(glist, clientfile_deleg_heuristics_t, glist);

    /* Look for our client */
  //    if (found_entry->clientid->cid_clientid != client->cid_clientid)
  //      continue;

  //    LogFullDebug(COMPONENT_STATE, "Found a delegation heuristic for this "
  //                 "file-client pair!");
    //found_entry->clientfile_deleg_heuristics_t,dh_last_del;
    //found_entry->dh_last_rec;     // most recent recall
  //  }
  LogDebug(COMPONENT_STATE, "Let's delegate!!");
  return true;
}

void get_deleg_perm(cache_entry_t *entry, nfsace4 *permissions,
                    open_delegation_type4 type) {
  /* We need to create an access_mask that shows who
   * can OPEN this file. */
  if (type == OPEN_DELEGATE_WRITE) {
  } else if (type == OPEN_DELEGATE_READ) {
  }
  permissions->type = ACE4_ACCESS_ALLOWED_ACE_TYPE;
  permissions->flag = 0;                           
  permissions->access_mask = 0;                    
  permissions->who.utf8string_len = 0;             
  permissions->who.utf8string_val = NULL;          
}
