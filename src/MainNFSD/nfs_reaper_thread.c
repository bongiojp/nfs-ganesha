/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
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
 * @file nfs_reaper_thread.c
 * @brief check for expired clients and whack them.
 */

#include "config.h"
#include <pthread.h>
#include <unistd.h>
#include <malloc.h>
#include "log.h"
#include "nfs4.h"
#include "sal_functions.h"
#include "nfs_proto_functions.h"
#include "nfs_core.h"
#include "log.h"
#include "fridgethr.h"

#define REAPER_DELAY 10
#define TRIM_DELAY 6

unsigned int reaper_delay = REAPER_DELAY;
unsigned int trim_delay = TRIM_DELAY;

static struct fridgethr *reaper_fridge;

static int reap_expired_open_owners(hash_table_t *ht_reap)
{
	struct rbt_head *head_rbt;
	struct hash_data *addr = NULL;
	uint32_t i;
	int rc;
	struct rbt_node *pn;
	int count = 0;
	char str[LOG_BUFF_LEN];
	struct display_buffer dspbuf = {
			sizeof(str), str, str};
	state_owner_t *powner;
	time_t tnow, tclose, texpire;
	struct gsh_buffdesc buffkey;
	struct gsh_buffdesc old_value;
	struct gsh_buffdesc old_key;

	/* For each bucket of the requested hashtable */
	for (i = 0; i < ht_reap->parameter.index_size; i++) {
		head_rbt = &ht_reap->partitions[i].rbt;

 restart:
		/* Use the time at the start or restart of a bucket to
		 * check for validity (don't call time() too many times).
		 */
		tnow = time(NULL);

		/* acquire mutex */
		PTHREAD_RWLOCK_wrlock(&ht_reap->partitions[i].lock);

		/* go through all entries in the red-black-tree */
		RBT_LOOP(head_rbt, pn) {
			addr = RBT_OPAQ(pn);


			powner = addr->val.addr;
			count++;

			if (powner->so_type != STATE_OPEN_OWNER_NFSV4) {
				RBT_INCREMENT(pn);
				continue;
			}

			display_owner(&dspbuf, powner);

			/* Cleanup the open owner only if its refcount is zero
			 * and its last_close_time exceeds the lease_lifetime
			 */
			tclose = atomic_fetch_time_t(&powner->so_owner.
						     so_nfs4_owner.
						     last_close_time);
			texpire = tclose + nfs_param.nfsv4_param.lease_lifetime;

			if ((tclose == 0) || (texpire > tnow)) {
				if (tclose != 0 &&
					isFullDebug(COMPONENT_STATE)) {
					LogFullDebug(COMPONENT_STATE,
							"Did not release CLOSE_PENDING %s, %d seconds left",
							str,
							(int) (texpire - tnow));
				}
				RBT_INCREMENT(pn);
				continue;
			}
			LogFullDebug(COMPONENT_STATE, "Free {%s}", str);
			buffkey.addr = powner;
			buffkey.len = sizeof(*powner);

			atomic_inc_int32_t(&powner->so_refcount);
			PTHREAD_RWLOCK_unlock(&ht_reap->partitions[i].
					      lock);
			rc = HashTable_Del(ht_reap, &buffkey,
					   &old_key, &old_value);

			if (rc != HASHTABLE_SUCCESS) {
				LogCrit(COMPONENT_CLIENTID,
					"Could not remove expired owner %s"
					" error=%s", str,
					hash_table_err_to_str(rc));
			}

			atomic_dec_int32_t(&powner->so_refcount);
			free_state_owner(powner);
			goto restart;

		}
		PTHREAD_RWLOCK_unlock(&ht_reap->partitions[i].lock);
	}

	return count;
}

static int reap_hash_table(hash_table_t *ht_reap)
{
	struct rbt_head *head_rbt;
	struct hash_data *addr = NULL;
	uint32_t i;
	int rc;
	struct rbt_node *pn;
	nfs_client_id_t *pclientid;
	nfs_client_record_t *precord;
	int count = 0;

	/* For each bucket of the requested hashtable */
	for (i = 0; i < ht_reap->parameter.index_size; i++) {
		head_rbt = &ht_reap->partitions[i].rbt;

 restart:
		/* acquire mutex */
		PTHREAD_RWLOCK_wrlock(&ht_reap->partitions[i].lock);

		/* go through all entries in the red-black-tree */
		RBT_LOOP(head_rbt, pn) {
			addr = RBT_OPAQ(pn);

			pclientid = addr->val.addr;
			count++;

			PTHREAD_MUTEX_lock(&pclientid->cid_mutex);

			if (!valid_lease(pclientid)) {
				char str[LOG_BUFF_LEN];
				struct display_buffer dspbuf = {
					sizeof(str), str, str};
				bool str_valid = false;

				inc_client_id_ref(pclientid);

				/* Take a reference to the client record */
				precord = pclientid->cid_client_record;
				inc_client_record_ref(precord);

				PTHREAD_MUTEX_unlock(&pclientid->cid_mutex);

				PTHREAD_RWLOCK_unlock(&ht_reap->partitions[i].
						      lock);

				if (isDebug(COMPONENT_CLIENTID)) {
					display_client_id_rec(&dspbuf,
							      pclientid);

					LogFullDebug(COMPONENT_CLIENTID,
						     "Expire index %d %s", i,
						     str);
					str_valid = true;
				}

				/* Take cr_mutex and expire clientid */
				PTHREAD_MUTEX_lock(&precord->cr_mutex);

				rc = nfs_client_id_expire(pclientid, false);

				PTHREAD_MUTEX_unlock(&precord->cr_mutex);

				dec_client_id_ref(pclientid);
				dec_client_record_ref(precord);

				if (isFullDebug(COMPONENT_CLIENTID)) {
					if (!str_valid)
						display_printf(&dspbuf,
							       "clientid %p",
							       pclientid);
					LogFullDebug(COMPONENT_CLIENTID,
						     "Reaper done, expired {%s}",
						     str);
				}

				if (rc)
					goto restart;
			} else {
				PTHREAD_MUTEX_unlock(&pclientid->cid_mutex);
			}

			RBT_INCREMENT(pn);
		}

		PTHREAD_RWLOCK_unlock(&ht_reap->partitions[i].lock);
	}

	return count;
}

struct reaper_state {
	bool old_state_cleaned;
	size_t count;
	bool logged;
	bool in_grace;
};

static struct reaper_state reaper_state = {
	.old_state_cleaned = false,
	.count = 0,
	.logged = false,
	.in_grace = false
};

/*
 * getMemFromProc
 */
size_t getMemFromProc(const char *path, char *inputlabel)
{
  char line[240];
  char unit[10];
  char label[100];
  size_t number = 0;
  char *ptr = NULL;

  FILE *fp = fopen(path,"r");
  if(!fp) {
    LogWarn(COMPONENT_MEMLEAKS,
          "failed to open %s errno=%d", path, errno);
    return 0;
  }
  while (1) {
    ptr = fgets(line, sizeof(line), fp);
    if (ptr == NULL) {
      LogWarn(COMPONENT_MEMLEAKS,
          "failed to fget errno=%d", errno);
      break;
    }
    if (strncmp(inputlabel, line, strlen(inputlabel)) == 0) {
      int rc;
      rc = sscanf(line, "%s %lu %s", label, &number, unit);
      if (rc != 3) {
        LogWarn(COMPONENT_MEMLEAKS,
                 "sscanf failed rc=%d errno=%d", rc, errno);
        number = 0;
        break;
      }
      if (strcmp("kB", unit) != 0) {
        LogWarn(COMPONENT_MEMLEAKS,
                 "incorrect status format %s, expecting kB", line);
        number = 0;
        break;
      }
      break;
    }
  }
  fclose(fp);
  return number;
}

void trim_memory(void)
{
  const char* proc_status = "/proc/self/status";
  const char* meminfo = "/proc/meminfo";

  size_t rss = 0;
  size_t totalmem = 0;
  size_t freemem = 0;

  /* 
   * extract the specific information from:
   * 1.  VmRss from /proc/self/status
   * 2.  MemFree from /proc/meminfo
   */
  rss = getMemFromProc(proc_status, "VmRSS:");
  totalmem = getMemFromProc(meminfo, "MemTotal:");
  freemem = getMemFromProc(meminfo, "MemFree:");

  /* trim if rss size is greater than 40% of total mem 
   * or freemem is less than 10% of total
   */
  if ((rss >= ((totalmem << 1) / 5)) ||
      (freemem <= (totalmem / 10)))
  {
     LogEvent(COMPONENT_MEMLEAKS,
                 "trim ganesha memory. rss=%lu total=%lu free=%lu",
                  rss, totalmem, freemem);
     malloc_trim(0);
  }
}

static void reaper_run(struct fridgethr_context *ctx)
{
	struct reaper_state *rst = ctx->arg;

	SetNameFunction("reaper");
	rst->in_grace = nfs_in_grace();

	if (!rst->old_state_cleaned) {
		/* if not in grace period, clean up the old state */
		if (!rst->in_grace) {
			nfs4_clean_old_recov_dir(v4_old_dir);
			rst->old_state_cleaned = true;
		}
	}

	if (isDebug(COMPONENT_CLIENTID) && ((rst->count > 0) || !rst->logged)) {
		LogDebug(COMPONENT_CLIENTID,
			 "Now checking NFS4 clients for expiration");

		rst->logged = (rst->count == 0);

#ifdef DEBUG_SAL
		if (rst->count == 0) {
			dump_all_states();
			dump_all_owners();
		}
#endif
	}

	rst->count =
	    (reap_hash_table(ht_confirmed_client_id) +
	     reap_hash_table(ht_unconfirmed_client_id));

      rst->count += reap_expired_open_owners(ht_nfs4_owner);

      /* trim every 6 iterations of REAPER_DELAY (10s) */
      if (--trim_delay > 0)
	      return;
      trim_delay = TRIM_DELAY;

      trim_memory();
}

int reaper_init(void)
{
	struct fridgethr_params frp;
	int rc = 0;

	if (nfs_param.nfsv4_param.lease_lifetime < (2 * REAPER_DELAY))
		reaper_delay = nfs_param.nfsv4_param.lease_lifetime / 2;

	memset(&frp, 0, sizeof(struct fridgethr_params));
	frp.thr_max = 1;
	frp.thr_min = 1;
	frp.thread_delay = reaper_delay;
	frp.flavor = fridgethr_flavor_looper;

	rc = fridgethr_init(&reaper_fridge, "reaper", &frp);
	if (rc != 0) {
		LogMajor(COMPONENT_CLIENTID,
			 "Unable to initialize reaper fridge, error code %d.",
			 rc);
		return rc;
	}

	rc = fridgethr_submit(reaper_fridge, reaper_run, &reaper_state);
	if (rc != 0) {
		LogMajor(COMPONENT_CLIENTID,
			 "Unable to start reaper thread, error code %d.", rc);
		return rc;
	}

	return 0;
}

int reaper_shutdown(void)
{
	int rc = fridgethr_sync_command(reaper_fridge,
					fridgethr_comm_stop,
					120);

	if (rc == ETIMEDOUT) {
		LogMajor(COMPONENT_CLIENTID,
			 "Shutdown timed out, cancelling threads.");
		fridgethr_cancel(reaper_fridge);
	} else if (rc != 0) {
		LogMajor(COMPONENT_CLIENTID,
			 "Failed shutting down reaper thread: %d", rc);
	}
	return rc;
}
