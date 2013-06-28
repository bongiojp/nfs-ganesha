/*
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright CEA/DAM/DIF  (2008)
 * contributeur : Philippe DENIEL   philippe.deniel@cea.fr
 *                Thomas LEIBOVICI  thomas.leibovici@cea.fr
 *
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 3 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
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
 * @file    nfs4_pseudo.c
 * @brief   Routines used for managing the NFS4 pseudo file system.
 *
 * Routines used for managing the NFS4 pseudo file system.
 */
#include "config.h"
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/file.h>           /* for having FNDELAY */
#include "HashTable.h"
#include "log.h"
#include "ganesha_rpc.h"
#include "nfs4.h"
#include "nfs_core.h"
#include "nfs_proto_functions.h"
#include "nfs_tools.h"
#include "nfs_proto_tools.h"
#include "nfs_exports.h"
#include "nfs_file_handle.h"
#include "cache_inode.h"
#include "cache_inode_lru.h"
#include "export_mgr.h"
#include "city.h"
#include "cache_inode_avl.h"
#include "avltree.h"

#define NB_TOK_ARG 10
#define NB_OPT_TOK 10
#define NB_TOK_PATH 20

#define min(a,b)                        \
  ({ typeof (a) _a = (a);               \
    typeof (b) _b = (b);                \
    _a < _b ? _a : _b; })

static pseudofs_t gPseudoFs;
hash_table_t *ht_nfs4_pseudo;
struct alloc_file_handle_v4 notused; /* Used for sizeof()*/

/* Print a nfsv4 handle to a string buffer. */
void v4handle2str(char *buffer, void *fsopaque, int len)
{
  int i;
  for(i = 0; i < len; i++)
    sprintf(buffer+2*i, "%02x", ((unsigned char *)fsopaque)[i]);
}

void free_pseudo_handle_key(hash_buffer_t key)
{
  gsh_free(key.pdata);
}

char *package_pseudo_handle(char *pseudopath, ushort len, uint64 hashkey)
{
  char *buff = NULL;
  int opaque_bytes_used=0,pathlen=0;

  /* This is the size of the v4 file handle opaque area used for pseudofs or
   * FSAL file handles. */
  buff = gsh_malloc(sizeof(notused.pad)); 
  if (buff == NULL) {
    LogCrit(COMPONENT_NFS_V4_PSEUDO,
            "Failed to malloc space for pseudofs handle.");
    return NULL;
  }

  memcpy(buff, &hashkey, sizeof(hashkey));
  opaque_bytes_used += sizeof(hashkey);

  /* include length of the path in the handle */
  /* MAXPATHLEN=4096 ... max path length can be contained in a short int. */
  memcpy(buff + opaque_bytes_used, &len, sizeof(ushort));
  opaque_bytes_used += sizeof(ushort);

  /* Either the nfsv4 fh opaque size or the length of the pseudopath. Ideally
   * we can include entire pseudofs pathname for guaranteed uniqueness of
   * pseudofs handles. */
  pathlen = min(sizeof(notused.pad) - opaque_bytes_used, len);
  memcpy(buff + opaque_bytes_used, pseudopath, pathlen);
  opaque_bytes_used += pathlen;

  /* If there is more space in the opaque handle due to a short pseudofs path
   * ... zero it. */
  if (opaque_bytes_used < sizeof(notused.pad)) {
    memset(buff + opaque_bytes_used, 0,
           sizeof(notused.pad) - opaque_bytes_used);
  }

  return buff;
}

hash_buffer_t create_pseudo_handle_key(char *pseudopath, int len)
{
  hash_buffer_t key;
  ushort smalllen = len;
  uint64 hashkey;


  hashkey = CityHash64(pseudopath, len);
  key.pdata = package_pseudo_handle(pseudopath, smalllen, hashkey);
  key.len = sizeof(notused.pad);

  /* key.pdata == NULL upon error */
  return key;
}

/* avltree nodes are compared by name and name length */
static inline int avl_pseudo_name_cmp(const struct avltree_node *lhs,
                                      const struct avltree_node *rhs)
{
  pseudofs_entry_t *lk, *rk;
  int llen,rlen,minlen,res;
  lk = avltree_container_of(lhs, pseudofs_entry_t, nameavlnode);
  rk = avltree_container_of(rhs, pseudofs_entry_t, nameavlnode);

  llen = strlen(lk->name);
  rlen = strlen(rk->name);
  minlen = min(llen, rlen);

  /* Compare strings*/
  res = strncmp(lk->name, rk->name, minlen);
  if (res != 0)
    return res;

  /* Same characters so far, now compare length of string. */
  if (llen < rlen)
    return (-1);
  if (llen > rlen)
    return (1);

  /* Exact same name */
  return 0;
}

/* Compare pseudofs ids. There is a chance of a collision. We will
 * not have the node name to avoid the collision */
static inline int avl_pseudo_id_cmp(const struct avltree_node *lhs,
                                    const struct avltree_node *rhs)
{
  pseudofs_entry_t *lk, *rk;
  lk = avltree_container_of(lhs, pseudofs_entry_t, idavlnode);
  rk = avltree_container_of(rhs, pseudofs_entry_t, idavlnode);

  if (lk->pseudo_id < rk->pseudo_id)
    return (-1);
  if (lk->pseudo_id > rk->pseudo_id)
    return (1);
  return 0;
}

void fullpath(char *fullpseudopath, char **PathTok, int tok, int maxlen)
{
  int currtok, currlen=0;

  fullpseudopath[currlen++] = '/';
  for(currtok=0; currtok<=tok&&currlen<maxlen; currtok++) {
    if (currlen + strlen(PathTok[currtok]) > maxlen) {
      LogWarn(COMPONENT_NFS_V4_PSEUDO,"Pseudopath length is too long, can't "
      "create pseudofs node.");
      break;
    }
    strncpy(fullpseudopath+currlen, PathTok[currtok], strlen(PathTok[currtok]));
    currlen += strlen(PathTok[currtok]);
    if (currtok<tok)
      fullpseudopath[currlen++] = '/';
  }
  fullpseudopath[currlen] = '\0';
}

/**
 * @brief Convert a file handle to the id of an object in the pseudofs
 *
 * This routine merely extracts a field from the file handle which is
 * not seen as opaque in this case. Because file handle are opaque
 * structure, it is prefered to have a dedicated function for this and
 * so hiding the file handle internal structure.
 *
 * @param[in] fh4p file handle to process
 *
 * @return The pseudo ID.
 *
 * @see nfs_GetPseudoFs
 */

uint64_t nfs4_PseudoToId(nfs_fh4 * fh4p)
{
  file_handle_v4_t *pfhandle4;
  uint64_t out = 0LL;

  pfhandle4 = (file_handle_v4_t *) (fh4p->nfs_fh4_val);

  out = (uint64_t) (pfhandle4->pseudofs_id);
  return out;
}

/**
 * @brief Get the root of the pseudo file system
 *
 * Gets the root of the pseudo file system. This is only a wrapper to
 * static variable gPseudoFs.
 *
 * @return The pseudo fs root
 */

pseudofs_t *nfs4_GetPseudoFs(void)
{
  return &gPseudoFs;
}

/**
 * @brief Build a pseudo fs from an exportlist
 *
 * This export list itself is obtained by reading the configuration
 * file.
 *
 * @parma[in] pexportlist The export list
 *
 * @return the pseudo fs root.
 */

int nfs4_ExportToPseudoFS(struct glist_head *pexportlist)
{
  hash_buffer_t key, value;
  exportlist_t *entry;
  struct glist_head * glist;
  int j=0, NbTokPath;
  char tmp_pseudopath[MAXPATHLEN+2],
    fullpseudopath[MAXPATHLEN+2],
    *PathTok[NB_TOK_PATH];
  pseudofs_t *PseudoFs = NULL;
  pseudofs_entry_t *PseudoFsRoot = NULL;
  pseudofs_entry_t *PseudoFsCurrent = NULL;
  pseudofs_entry_t *newPseudoFsEntry = NULL;
  pseudofs_entry_t *iterPseudoFs = NULL;
  pseudofs_entry_t *PseudoFsCurrent=NULL,*newPseudoFsEntry=NULL;
  hash_error_t hrc = 0;
  struct hash_latch latch;
  struct avltree_node *node;

  PseudoFs = &gPseudoFs;

  /* Init Root of the Pseudo FS tree */
  strcpy(PseudoFs->root.name, "/");
  PseudoFs->root.pseudo_id = 0;
  PseudoFs->root.junction_export = NULL;
  /* root is its own parent */
  PseudoFs->root.parent = &PseudoFs->root;
  avltree_init(&PseudoFs->root.child_tree_byname, avl_pseudo_name_cmp, 0);
  avltree_init(&PseudoFs->root.child_tree_byid, avl_pseudo_id_cmp, 0);

  key = create_pseudo_handle_key(PseudoFs->root.name, strlen(PseudoFs->root.name));
  if(isFullDebug(COMPONENT_NFS_V4_PSEUDO)) {
    char handle2str[256];
    memset(handle2str, 0, sizeof(handle2str));
    v4handle2str(handle2str, key.pdata, key.len);
    LogFullDebug(COMPONENT_NFS_V4_PSEUDO,"created key for path:%s handle:%s",
                 PseudoFs->root.name,handle2str);
  }

  PseudoFs->root.fsopaque = (uint8_t *)key.pdata;
  value.pdata = &PseudoFs->root;
  value.len = sizeof(PseudoFs->root);
  hrc = HashTable_Test_And_Set(ht_nfs4_pseudo, &key, &value,
                               HASHTABLE_SET_HOW_SET_NO_OVERWRITE);
  if(hrc != HASHTABLE_SUCCESS) {
    LogCrit(COMPONENT_NFS_V4_PSEUDO,
            "Failed to add ROOT pseudofs path %s due to hashtable error: %s",
            PseudoFs->root.name, hash_table_err_to_str(hrc));
    return -1;
  }
 
  LogDebug(COMPONENT_NFS_V4_PSEUDO, "Added root pseudofs node to hashtable");

  glist_for_each(glist, pexportlist)
    {
      entry = glist_entry(glist, exportlist_t, exp_list);

      /* skip exports that aren't for NFS v4 */
      if((entry->export_perms.options & EXPORT_OPTION_NFSV4) == 0 ||
	 entry->pseudopath == NULL)
        {  continue;   }

      if(entry->export_perms.options & EXPORT_OPTION_PSEUDO)
        {
	  char *tmp_pseudopath;

	  tmp_pseudopath = alloca(strlen(entry->pseudopath) + 2);
          LogDebug(COMPONENT_NFS_V4_PSEUDO,
                   "BUILDING PSEUDOFS: Export_Id %d Path %s Pseudo Path %s",
                   entry->id, entry->fullpath, entry->pseudopath);
          /* there must be a leading '/' in the pseudo path */
          if(entry->pseudopath[0] != '/')
            {
              /* Path is badly formed */
              LogCrit(COMPONENT_NFS_V4_PSEUDO,
                      "Pseudo Path '%s' is badly formed",
                      entry->pseudopath);
              continue;
            }

	  if(strlen(entry->pseudopath) > MAXPATHLEN)
            {
              /* Path is badly formed */
              LogCrit(COMPONENT_NFS_V4_PSEUDO,
                      "Bad Pseudo=\"%s\", path too long",
                      entry->pseudopath);
              continue;
            }
          /* Parsing the path
	   * Make a copy of the pseudopath since it will be modified,
           * also, skip the leading '/'.
           */
          strcpy(tmp_pseudopath, entry->pseudopath + 1);

          NbTokPath = nfs_ParseConfLine(PathTok,
                                        NB_TOK_PATH,
                                        tmp_pseudopath,
                                        '/');
          if(NbTokPath < 0)
            {
              /* Path is badly formed */
              LogCrit(COMPONENT_NFS_V4_PSEUDO,
                      "Bad Pseudo=\"%s\", path has too many components",
                      entry->pseudopath);
              continue;
            }

          /* Start at the pseudo root. */
          PseudoFsCurrent = &(PseudoFs->root);

          /* Loop on each token. */
          for(j = 0; j < NbTokPath; j++)
            {
              found = 0;
	      LogFullDebug(COMPONENT_NFS_V4_PSEUDO, "token %s", PathTok[j]);
                            /* Pseudofs path */
              fullpath(fullpseudopath, PathTok, j, MAXPATHLEN);
              key = create_pseudo_handle_key(fullpseudopath, strlen(fullpseudopath));
              
              if(isFullDebug(COMPONENT_NFS_V4_PSEUDO))
                {
                  char handle2str[256];
                  memset(handle2str, 0, sizeof(handle2str));
                  v4handle2str(handle2str, key.pdata, key.len);
                  LogFullDebug(COMPONENT_NFS_V4_PSEUDO,"created key for "
                               "path:%s handle:%s", fullpseudopath,handle2str);
                }
              /* Now we create the pseudo entry */
              newPseudoFsEntry = gsh_malloc(sizeof(pseudofs_entry_t));
              if(newPseudoFsEntry == NULL)
                {
                  LogMajor(COMPONENT_NFS_V4_PSEUDO,
                           "Insufficient memory to create pseudo fs node");
                  return ENOMEM;
                }
              
              /* Creating the new pseudofs entry */
              strncpy(newPseudoFsEntry->name, PathTok[j], strlen(PathTok[j]));
              newPseudoFsEntry->name[strlen(PathTok[j])] = '\0';
              newPseudoFsEntry->fsopaque = (uint8_t *)key.pdata;
              memcpy(&newPseudoFsEntry->pseudo_id, key.pdata,
                     sizeof(newPseudoFsEntry->pseudo_id));
              newPseudoFsEntry->junction_export = NULL;
              newPseudoFsEntry->parent = PseudoFsCurrent;
              avltree_init(&newPseudoFsEntry->child_tree_byname,avl_pseudo_name_cmp, 0);
              avltree_init(&newPseudoFsEntry->child_tree_byid,avl_pseudo_id_cmp, 0);
              
              LogMidDebug(COMPONENT_NFS_V4_PSEUDO,
                          "Creating pseudo fs entry for %s, pseudo_id %"PRIu64,
                          newPseudoFsEntry->name, newPseudoFsEntry->pseudo_id);
              
              value.pdata = newPseudoFsEntry;
              value.len = sizeof(newPseudoFsEntry);
              
              /* Looking for a matching entry and creating if nonexistent */
              hrc = HashTable_Test_And_Set(ht_nfs4_pseudo, &key, &value,
                                           HASHTABLE_SET_HOW_SET_NO_OVERWRITE);
              if(hrc != HASHTABLE_SUCCESS && hrc != HASHTABLE_ERROR_KEY_ALREADY_EXISTS) {
                LogCrit(COMPONENT_NFS_V4_PSEUDO,
                        "Failed to add pseudofs path %s due to hashtable error: %s",
                        newPseudoFsEntry->name, hash_table_err_to_str(hrc));
                return -1;
              }
              if (hrc == HASHTABLE_ERROR_KEY_ALREADY_EXISTS) {
                LogDebug(COMPONENT_NFS_V4_PSEUDO,
                         "Failed to add pseudofs path, path already exists: %s",
                         newPseudoFsEntry->name);
                
                /* Now set PseudoFsCurrent to existing entry. */
                hrc = HashTable_GetLatch(ht_nfs4_pseudo, &key, &value,
                                         FALSE, &latch);
                if ((hrc != HASHTABLE_SUCCESS))
                  {
                    /* This should not happened */
                    LogCrit(COMPONENT_NFS_V4_PSEUDO, "Can't add/get key for %s"
                            " hashtable error: %s",
                            newPseudoFsEntry->name, hash_table_err_to_str(hrc));
                    return -1;
                  } else
                  {
                    /* Now we have the cached pseudofs entry*/
                    PseudoFsCurrent = value.pdata;
                    
                    /* Release the lock ... we should be calling this funciton
                     * in a serial fashion before Ganesha is operational. No
                     * chance of contention. */
                    HashTable_ReleaseLatched(ht_nfs4_pseudo, &latch);
                  }
                /* Free the key and value that we weren't able to add. */
                free_pseudo_handle_key(key);
                gsh_free(newPseudoFsEntry);
                continue;
              }
              /* Insert new pseudofs entry into tree */
              node = avltree_insert(&newPseudoFsEntry->nameavlnode,
                                    &PseudoFsCurrent->child_tree_byname);
              assert(!node);
              node = avltree_insert(&newPseudoFsEntry->idavlnode,
                                    &PseudoFsCurrent->child_tree_byid);
              assert(!node);
              
              PseudoFsCurrent = newPseudoFsEntry;
            }                   /* for j */
          
          /* Now that all entries are added to pseudofs tree,
	   * add the junction to the pseudofs */
          PseudoFsCurrent->junction_export = entry;

          /* And fill in our part of the export root data */
          entry->exp_mounted_on_file_id = PseudoFsCurrent->pseudo_id;

        }
    }                           /* glist_for_each */

  if(isMidDebug(COMPONENT_NFS_V4_PSEUDO))
    {
      /*int i;

      for(i = 0; i <= PseudoFs->last_pseudo_id; i++)
        {
          if(PseudoFs->reverse_tab[i]->junction_export != NULL)
            LogMidDebug(COMPONENT_NFS_V4_PSEUDO,
                        "pseudo_id %d is %s junction_export %p"
			"Export_id %d Path %s mounted_on_fileid %"PRIu64,
                        i, PseudoFs->reverse_tab[i]->name,
                        PseudoFs->reverse_tab[i]->junction_export,
                        PseudoFs->reverse_tab[i]->junction_export->id,
                        PseudoFs->reverse_tab[i]->junction_export->fullpath,
                        (uint64_t) PseudoFs->reverse_tab[i]->
			           junction_export->exp_mounted_on_file_id);
          else
            LogMidDebug(COMPONENT_NFS_V4_PSEUDO,
                        "pseudo_id %d is %s (not a junction)",
                        i, PseudoFs->reverse_tab[i]->name);
        }
      */
    }

  return (0);
}

/**
 * @brief Get the attributes for an entry in the pseudofs
 *
 * This function gets the attributes for an entry in the
 * pseudofs. Because pseudo fs structure is very simple (it is
 * read-only and contains only directory that belongs to root), a set
 * of standardized values is returned
 *
 * @param[out] psfsp  Pseudo fs entry on which attributes are queried
 * @param[in]  Fattr  Buffer that will contain the queried attributes
 * @param[in]  data   Compound request data
 * @param[in]  objFH  NFSv4 filehandle, in case they request it
 * @param[in]  Bitmap Bitmap describing the attributes to be returned
 *
 * @retval 0 if successfull.
 * @retval -1 on error, in this case, too many attributes were requested.
 */

int nfs4_PseudoToFattr(pseudofs_entry_t *psfsp,
                       fattr4 *Fattr,
                       compound_data_t *data,
		       nfs_fh4 *objFH,
		       struct bitmap4 *Bitmap)
{
	struct attrlist attrs;

	/* cobble up an inode (attributes) for this pseudo */
	memset(&attrs, 0, sizeof(attrs));
	attrs.type = DIRECTORY;
	attrs.filesize = DEV_BSIZE;
	if(psfsp->junction_export == NULL) {
		attrs.fsid.major = 152LL;
		attrs.fsid.minor = 152LL;
	} else {
		attrs.fsid.major = 153LL;  /* @todo BUGAZOMEU : tres cradem mais utile */
		attrs.fsid.minor = nfs_htonl64(153LL);
	}
	attrs.fileid = psfsp->pseudo_id;
	attrs.mode = unix2fsal_mode(0555);
	attrs.numlinks = 2; /* not 1.  for '.' and '../me' */
	attrs.owner = 0; /* root */
	attrs.group = 2; /* daemon? */
	attrs.atime = ServerBootTime;
	attrs.ctime = ServerBootTime;
	attrs.chgtime = ServerBootTime;
	attrs.change = ServerBootTime.tv_sec;
	attrs.spaceused = DEV_BSIZE;
	attrs.mounted_on_fileid = psfsp->pseudo_id;
	return nfs4_FSALattr_To_Fattr(&attrs, Fattr, data, objFH, Bitmap);
}

/**
 * @brief Convert an NFSv4 file handle to a pseudofs ID
 *
 * This function converts an NFSv4 file handle to a pseudofs id and
 * checks if the fh is related to a pseudofs entry.
 *
 * @param[in]  fh4p      NFSv4 filehandle
 * @param[in]  psfstree  Root of the pseudofs
 * @param[out] psfsentry Pseudofs entry
 *
 * @retval True if successfull
 * @retval FALSE if the fh4 was not related a pseudofs entry
 */

static bool nfs4_FhandleToPseudo(nfs_fh4 * fh4p, pseudofs_t * psfstree,
				 pseudofs_entry_t **psfsentry)
{
  file_handle_v4_t *pfhandle4;
  hash_buffer_t key, value;
  hash_error_t hrc = 0;
  struct hash_latch latch;

  /* Map the filehandle to the correct structure */
  pfhandle4 = (file_handle_v4_t *) (fh4p->nfs_fh4_val);

  /* The function must be called with a fh pointed to a pseudofs entry */
  if( pfhandle4->exportid != 0 )
    return false;

  /* Find pseudofs in hashtable */
  /* key generated from pathname and cityhash64 of pathname */
  key.pdata = pfhandle4->fsopaque;
  key.len = pfhandle4->fs_len;

  if(isFullDebug(COMPONENT_NFS_V4_PSEUDO)) {
    char handle2str[256];
    memset(handle2str, 0, sizeof(handle2str));
    v4handle2str(handle2str, key.pdata, key.len);
    LogFullDebug(COMPONENT_NFS_V4_PSEUDO,"looking up pseudofs node for handle:%s",
                 handle2str);
  }

  hrc = HashTable_GetLatch(ht_nfs4_pseudo, &key, &value, FALSE, &latch);
  if ((hrc != HASHTABLE_SUCCESS)) {
    /* This should not happened */
    LogDebug(COMPONENT_NFS_V4_PSEUDO, "Can't get key for FHToPseudo conversion"
             ", hashtable error %s", hash_table_err_to_str(hrc));
    *psfsentry = NULL;
  } else {
    *psfsentry = value.pdata;

    /* Release the lock ... This is a read-only hashtable and entry.
     * It's possible we reload exports ... but in that case we 
     * catch the worker threads at a safe location where we aren't
     * using any export entries. */
    HashTable_ReleaseLatched(ht_nfs4_pseudo, &latch);
  }

  return true;
}                               /* nfs4_FhandleToPseudo */

/**
 * nfs4_PseudoToFhandle: converts an id in the pseudo fs to a NFSv4 file handle
 * 
 * Converts an id in the pseudo fs to a NFSv4 file handle. 
 *
 * @param fh4p      [OUT] pointer to nfsv4 filehandle
 * @param psfsentry [IN]  pointer to pseudofs entry
 * 
 * @return NFS4_OK if successfull, other values show an error. 
 * 
 */

int nfs4_PseudoToFhandle(nfs_fh4 * fh4p, pseudofs_entry_t * psfsentry)
{
  file_handle_v4_t *fhandle4;

  assert(psfsentry != NULL);
  memset(fh4p->nfs_fh4_val, 0, sizeof(struct alloc_file_handle_v4)); /* clean whole thing */
  fhandle4 = (file_handle_v4_t *)fh4p->nfs_fh4_val;
  fhandle4->fhversion = GANESHA_FH_VERSION;

  fhandle4->exportid = 0;
  memcpy(fhandle4->fsopaque, psfsentry->fsopaque, sizeof(notused.pad));
  fhandle4->fs_len = sizeof(notused.pad);

  if(isFullDebug(COMPONENT_NFS_V4_PSEUDO)) {
    char handle2str[256];
    memset(handle2str, 0, sizeof(handle2str));
    v4handle2str(handle2str, psfsentry->fsopaque, sizeof(notused.pad));
    LogFullDebug(COMPONENT_NFS_V4_PSEUDO,"pseudoToFhandle for name:%s handle:%s",
                 psfsentry->name,handle2str);
  }
  fh4p->nfs_fh4_len = sizeof(file_handle_v4_t); /* no handle in opaque */

  return true;
}                               /* nfs4_PseudoToFhandle */

/**
 * nfs4_CreateROOTFH: Creates the file handle for the "/" of the pseudo file system
 *
 *  Creates the file handle for the "/" of the pseudo file syste.
 * 
 * @param fh4p [OUT]   pointer to the file handle to be allocated
 * @param data [INOUT] pointer to the compound request's data
 * 
 * @return NFS4_OK if successfull, NFS4ERR_BADHANDLE if an error occured when creating the file handle.
 *
 */

int nfs4_CreateROOTFH4(nfs_fh4 * fh4p, compound_data_t * data)
{
  pseudofs_entry_t psfsentry;
  int status = 0;

  psfsentry = *(data->pseudofs->reverse_tab[0]);

  LogFullDebug(COMPONENT_NFS_V4_PSEUDO,
               "CREATE ROOTFH (pseudo): root to pseudofs = #%s#",
               psfsentry.name);

  if((status = nfs4_AllocateFH(&(data->rootFH))) != NFS4_OK)
    return status;

  if(!nfs4_PseudoToFhandle(&(data->rootFH), &psfsentry))
    {
      LogFullDebug(COMPONENT_NFS_V4_PSEUDO,
                   "CREATE ROOTFH (pseudo): Creation of root fh is impossible");
      return NFS4ERR_BADHANDLE;
    }

  /* Test */
  if(isFullDebug(COMPONENT_NFS_V4))
    {
      char str[LEN_FH_STR];
      sprint_fhandle4(str, &data->rootFH);
      LogFullDebug(COMPONENT_NFS_V4, "CREATE ROOT FH: %s", str);
    }

  return NFS4_OK;
}                               /* nfs4_CreateROOTFH4 */

/**
 * nfs4_op_getattr_pseudo: Gets attributes for directory in pseudo fs
 * 
 * Gets attributes for directory in pseudo fs. These are hardcoded constants. 
 *
 * @param op    [IN]    pointer to nfs4_op arguments
 * @param data  [INOUT] Pointer to the compound request's data
 * @param resp  [IN]    Pointer to nfs4_op results
 * 
 * @return NFS4_OK 
 * 
 */

#define arg_GETATTR4 op->nfs_argop4_u.opgetattr
#define res_GETATTR4 resp->nfs_resop4_u.opgetattr

int nfs4_op_getattr_pseudo(struct nfs_argop4 *op,
                           compound_data_t * data, struct nfs_resop4 *resp)
{
  pseudofs_entry_t *psfsentry;
  resp->resop = NFS4_OP_GETATTR;

  /* Get the pseudo entry related to this fhandle */
  if(!nfs4_FhandleToPseudo(&(data->currentFH), data->pseudofs, &psfsentry))
    {
      res_GETATTR4.status = NFS4ERR_BADHANDLE;
      return res_GETATTR4.status;
    }

  /* All directories in pseudo fs have the same Fattr */
  if(nfs4_PseudoToFattr(psfsentry,
                        &(res_GETATTR4.GETATTR4res_u.resok4.obj_attributes),
                        data, &(data->currentFH), &(arg_GETATTR4.attr_request)) != 0)
    res_GETATTR4.status = NFS4ERR_SERVERFAULT;
  else
    res_GETATTR4.status = NFS4_OK;

  LogFullDebug(COMPONENT_NFS_V4,
               "Apres nfs4_PseudoToFattr: attrmask(bitmap4_len)=%d attrlist4_len=%d",
               res_GETATTR4.GETATTR4res_u.resok4.obj_attributes.attrmask.bitmap4_len,
               res_GETATTR4.GETATTR4res_u.resok4.obj_attributes.attr_vals.attrlist4_len);

  return res_GETATTR4.status;
}                               /* nfs4_op_getattr */

/**
 * nfs4_op_access_pseudo: Checks for object accessibility in pseudo fs. 
 * 
 * Checks for object accessibility in pseudo fs. All entries in pseudo fs return can't be accessed as 
 * ACCESS4_MODIFY|ACCESS4_EXTEND|ACCESS4_DELETE because pseudo fs is behaving as a read-only fs.
 *
 * @param op    [IN]    pointer to nfs4_op arguments
 * @param data  [INOUT] Pointer to the compound request's data
 * @param resp  [IN]    Pointer to nfs4_op results
 * 
 * @return NFS4_OK 
 * 
 */

/* Shorter notation to avoid typos */
#define res_ACCESS4 resp->nfs_resop4_u.opaccess
#define arg_ACCESS4 op->nfs_argop4_u.opaccess

int nfs4_op_access_pseudo(struct nfs_argop4 *op,
                          compound_data_t * data, struct nfs_resop4 *resp)
{
  resp->resop = NFS4_OP_ACCESS;

  /* All access types are supported */
  res_ACCESS4.ACCESS4res_u.resok4.supported = ACCESS4_READ | ACCESS4_LOOKUP;

  /* DELETE/MODIFY/EXTEND are not supported in the pseudo fs */
  res_ACCESS4.ACCESS4res_u.resok4.access =
      arg_ACCESS4.access & ~(ACCESS4_MODIFY | ACCESS4_EXTEND | ACCESS4_DELETE);

  return NFS4_OK;
}                               /* nfs4_op_access_pseudo */

/**
 * @brief set_compound_data_for_pseudo: fills in compound data for pseudo fs
 * 
 * Fills in:
 *
 * data->current_entry
 * data->current_filetype
 * data->pexport
 * data->export_perms.options
 *
 * @param data  [INOUT] Pointer to the compound request's data
 * 
 */
void set_compound_data_for_pseudo(compound_data_t * data)
{
  if (data->current_entry) {
     cache_inode_put(data->current_entry);
  }
  if (data->current_ds) {
      data->current_ds->ops->put(data->current_ds);
  }
  if(data->req_ctx->export != NULL) {
      put_gsh_export(data->req_ctx->export);
      data->req_ctx->export = NULL;
  }
  /* No cache inode entry for the directory within pseudo fs */
  data->current_ds           = NULL;
  data->current_entry        = NULL; /* No cache inode entry */
  data->current_filetype     = DIRECTORY; /* Always a directory */
  data->req_ctx->export      = NULL;
  data->pexport              = NULL; /* No exportlist is related to pseudo fs */
  data->export_perms.options = EXPORT_OPTION_ROOT |
                               EXPORT_OPTION_MD_READ_ACCESS |
                               EXPORT_OPTION_AUTH_TYPES |
                               EXPORT_OPTION_NFSV4 |
                               EXPORT_OPTION_TRANSPORTS;
}

/**
 * @brief Looks up into the pseudo fs.
 *
 * Looks up into the pseudo fs. If a junction traversal is detected,
 * does the necessary stuff for correcting traverse.
 *
 * @param[in]     op    nfs4_op arguments
 * @param[in,out] data  Compound request's data
 * @param[in]     resp  nfs4_op results
 *
 * @return NFS4_OK if successfull, other values show an error.
 */

/* Shorter notation to avoid typos */
#define arg_LOOKUP4 op->nfs_argop4_u.oplookup
#define res_LOOKUP4 resp->nfs_resop4_u.oplookup

int nfs4_op_lookup_pseudo(struct nfs_argop4 *op,
                          compound_data_t * data, struct nfs_resop4 *resp)
{
  char *name;
  struct pseudofs_entry *parent_fsentry=NULL, *thefsentry;
  int error = 0;
  cache_inode_status_t cache_status;
  cache_entry_t *entry = NULL;

  /* Used for child avltree lookup*/
  pseudofs_entry_t tempentry;
  struct avltree_node *keynode, *foundnode;

  resp->resop = NFS4_OP_LOOKUP;

  /* Validate and convert the UFT8 objname to a regular string */
  res_LOOKUP4.status = nfs4_utf8string2dynamic(&arg_LOOKUP4.objname,
					       UTF8_SCAN_ALL,
					       &name);
  if (res_LOOKUP4.status != NFS4_OK)
    {
      goto out;
    }

  /* Get the pseudo fs entry related to the file handle */
  if(!nfs4_FhandleToPseudo(&(data->currentFH), data->pseudofs,
                           &parent_fsentry))
    {
      res_LOOKUP4.status = NFS4ERR_BADHANDLE;
      goto out;
    }

  /* Search for name in pseudo fs directory. We use a temporary
   * avlnode and pseudofs_entry_t to perform a name lookup in
   * the child tree. If it's not here, it doesn't exist.*/
  keynode = &tempentry.nameavlnode;
  strcpy(tempentry.name, name);
  avltree_container_of(keynode, pseudofs_entry_t, nameavlnode);

  foundnode = avltree_lookup(keynode, &parent_fsentry->child_tree_byname);
  if(foundnode == NULL)
    {
      res_LOOKUP4.status = NFS4ERR_NOENT;
      goto out;
    }

  /* we found the requested pseudofs node. */
  thefsentry = avltree_container_of(foundnode, pseudofs_entry_t,
                                    nameavlnode);

  /* A matching entry was found */
  if(thefsentry->junction_export == NULL)
    {
      /* The entry is not a junction, we stay within the pseudo fs */
      nfs4_PseudoToFhandle(&(data->currentFH), thefsentry);
        {
          res_LOOKUP4.status = NFS4ERR_SERVERFAULT;
	  goto out;
        }
    }
  else
    {
      /* The entry is a junction */
      LogFullDebug(COMPONENT_NFS_V4_PSEUDO,
                   "A junction in pseudo fs is traversed: name = %s, id = %d",
                   thefsentry->name, thefsentry->junction_export->id);
      /**
       * @todo Danger Will Robinson!!
       * We do a get_gsh_export here to take a reference on the export
       * The whole pseudo fs tree is build directly from the exportlist
       * without gsh_export reference even though the export list itself
       * was created via get_gsh_export().
       * We need a reference here because we are transitioning the junction
       * and need to reference the export on the other side.  What really
       * needs to happen is that junction_export is of type struct gsh_export *
       * and we fill it with get_gsh_export.  A junction crossing would put
       * this (the pseudo's export) and get the one on the other side.  But
       * then, that would require the pseudo fs to be a FSAL...  Hence, we
       * hack it here until the next dev cycle.
       */

      data->req_ctx->export = get_gsh_export(iter->junction_export->id,
					     true);
      assert(data->req_ctx->export != NULL);
      data->pexport = &data->req_ctx->export->export;

      /* Build credentials */
      res_LOOKUP4.status = nfs4_MakeCred(data);
      if(res_LOOKUP4.status != NFS4_OK)
        {
          LogMajor(COMPONENT_NFS_V4_PSEUDO,
                   "PSEUDO FS JUNCTION TRAVERSAL: Failed to get FSAL credentials for %s, id=%d",
                   data->pexport->fullpath, data->pexport->id);
          res_LOOKUP4.status = NFS4ERR_WRONGSEC;
	  goto out;
        }

      cache_status = nfs_export_get_root_entry(data->pexport, &entry);
      if(cache_status != CACHE_INODE_SUCCESS)
        {
	  LogMajor(COMPONENT_NFS_V4_PSEUDO,
                   "PSEUDO FS JUNCTION TRAVERSAL: Failed to get root for %s, id=%d, status = %d",
                   data->pexport->fullpath, data->pexport->id, cache_status);
          res_LOOKUP4.status = NFS4ERR_SERVERFAULT;
	  goto out;
        }

      cache_inode_lru_ref(entry, LRU_REQ_INITIAL);
      if(data->currentFH.nfs_fh4_len == 0)
        {
          if((error = nfs4_AllocateFH(&(data->currentFH))) != NFS4_OK)
            {
              LogMajor(COMPONENT_NFS_V4_PSEUDO,
                       "PSEUDO FS JUNCTION TRAVERSAL: /!\\ | Failed to allocate the first file handle");
              res_LOOKUP4.status = NFS4ERR_SERVERFAULT;
	      goto out;
            }
        }

      /* Build the nfs4 handle */
      if(!nfs4_FSALToFhandle(&data->currentFH, entry->obj_handle))
        {
          LogMajor(COMPONENT_NFS_V4_PSEUDO,
                   "PSEUDO FS JUNCTION TRAVERSAL: /!\\ | Failed to build the first file handle");
          res_LOOKUP4.status = NFS4ERR_SERVERFAULT;
	  goto out;
        }


      /* Deref the old one, ref and keep the entry within the compound data */
      if (data->current_entry) {
	      cache_inode_lru_unref(data->current_entry, LRU_FLAG_NONE);
      }
      data->current_entry = entry;
      data->current_filetype = entry->type;
      entry = NULL;

    }                           /* else */


  res_LOOKUP4.status = NFS4_OK;

 out:

  if(entry)
    cache_inode_lru_unref(entry, LRU_FLAG_NONE);
  if (name)
    gsh_free(name);
  return NFS4_OK;
}

/**
 * nfs4_op_lookupp_pseudo: looks up into the pseudo fs for the parent directory
 * 
 * looks up into the pseudo fs for the parent directory of the current file handle. 
 *
 * @param op    [IN]    pointer to nfs4_op arguments
 * @param data  [INOUT] Pointer to the compound request's data
 * @param resp  [IN]    Pointer to nfs4_op results
 * 
 * @return NFS4_OK if successfull, other values show an error. 
 * 
 */

/* Shorter notation to avoid typos */
#define arg_LOOKUPP4 op->nfs_argop4_u.oplookupp
#define res_LOOKUPP4 resp->nfs_resop4_u.oplookupp

int nfs4_op_lookupp_pseudo(struct nfs_argop4 *op,
                           compound_data_t * data, struct nfs_resop4 *resp)
{
  pseudofs_entry_t *psfsentry;

  resp->resop = NFS4_OP_LOOKUPP;

  /* Get the pseudo fs entry related to the file handle */
  if(!nfs4_FhandleToPseudo(&(data->currentFH), data->pseudofs, &psfsentry))
    {
      res_LOOKUPP4.status = NFS4ERR_BADHANDLE;
      return res_LOOKUPP4.status;
    }

  /* lookupp on the root on the pseudofs should return NFS4ERR_NOENT (RFC3530, page 166) */
  if(psfsentry->pseudo_id == 0)
    {
      res_LOOKUPP4.status = NFS4ERR_NOENT;
      return res_LOOKUPP4.status;
    }

  /* A matching entry was found */
  if(!nfs4_PseudoToFhandle(&(data->currentFH), psfsentry->parent))
    {
      res_LOOKUPP4.status = NFS4ERR_SERVERFAULT;
      return res_LOOKUPP4.status;
    }

  /* Return the reference to the old current entry */
  if (data->current_entry) {
      cache_inode_put(data->current_entry);
  }
  data->current_entry = NULL;

  /* Fill in compound data */
  set_compound_data_for_pseudo(data);

  res_LOOKUPP4.status = NFS4_OK;
  return NFS4_OK;
}                               /* nfs4_op_lookupp_pseudo */

/**
 * nfs4_op_lookupp_pseudo_by_exp: looks up into the pseudo fs for the parent directory
 * 
 * looks up into the pseudo fs for the parent directory of the export. 
 *
 * @param op             [IN]    pointer to nfs4_op arguments
 * @param data           [INOUT] Pointer to the compound request's data
 * @paranm exp_root_data [IN]    Pointer to the export root data
 * @param resp           [IN]    Pointer to nfs4_op results
 * 
 * @return NFS4_OK if successfull, other values show an error. 
 * 
 */
int nfs4_op_lookupp_pseudo_by_exp(struct nfs_argop4  * op,
                                  compound_data_t    * data,
                                  struct nfs_resop4  * resp)
{
  pseudofs_entry_t * psfsentry;

  resp->resop = NFS4_OP_LOOKUPP;

  /* Get the parent pseudo fs entry related to the export */
  res_LOOKUP4.status = nfs4_CurrentFHToPseudo(data, &psfsentry);
  if(res_LOOKUP4.status != NFS4_OK)
    return res_LOOKUP4.status;

  LogDebug(COMPONENT_NFS_V4_PSEUDO,
           "LOOKUPP Traversing junction from Export_Id %d Pseudo %s back to pseudo fs id %"PRIu64,
           data->req_ctx->export->export.id,
           data->req_ctx->export->export.pseudopath,
           (uint64_t) data->req_ctx->export->export.exp_mounted_on_file_id);

  /* lookupp on the root on the pseudofs should return NFS4ERR_NOENT (RFC3530, page 166) */
  if(psfsentry->pseudo_id == 0)
    {
      LogDebug(COMPONENT_NFS_V4_PSEUDO,
               "Returning NFS4ERR_NOENT because pseudo_id == 0");
      res_LOOKUPP4.status = NFS4ERR_NOENT;
      return res_LOOKUPP4.status;
    }

  /* A matching entry was found */
  if(!nfs4_PseudoToFhandle(&(data->currentFH), psfsentry->parent))
    {
      LogEvent(COMPONENT_NFS_V4_PSEUDO,
               "LOOKUPP Traversing junction from Export_Id %d Pseudo %s back to pseudo fs id %"PRIu64" returning NFS4ERR_SERVERFAULT",
               data->req_ctx->export->export.id,
               data->req_ctx->export->export.pseudopath,
               (uint64_t) data->req_ctx->export->export.exp_mounted_on_file_id);
      res_LOOKUPP4.status = NFS4ERR_SERVERFAULT;
      return res_LOOKUPP4.status;
    }

  /* Return the reference to the old current entry */
  if (data->current_entry)
    {
      cache_inode_put(data->current_entry);
    }

  /* Fill in compound data */
  set_compound_data_for_pseudo(data);

  res_LOOKUPP4.status = NFS4_OK;
  return NFS4_OK;
}

/**
 * @brief Reads a directory in the pseudo fs
 *
 * Reads a directory in the pseudo fs.
 *
 * @param[in]     op    nfs4_op arguments
 * @param[in,out] data  Compound request's data
 * @param[out]    resp  nfs4_op results
 *
 * @return NFS4_OK if successfull, other values show an error.
 */

/* shorter notation to avoid typo */
#define arg_READDIR4 op->nfs_argop4_u.opreaddir
#define res_READDIR4 resp->nfs_resop4_u.opreaddir

static const struct bitmap4 RdAttrErrorBitmap = {
	.bitmap4_len = 1,
	.map = {[0] = (1<<FATTR4_RDATTR_ERROR),
		[1] = 0,
		[2] = 0}
};
static attrlist4 RdAttrErrorVals = { 0, NULL };      /* Nothing to be seen here */

int nfs4_op_readdir_pseudo(struct nfs_argop4 *op,
                           compound_data_t * data, struct nfs_resop4 *resp)
{
  unsigned long dircount = 0;
  unsigned long maxcount = 0;
  unsigned long estimated_num_entries = 0;
  unsigned long nodecount = 0;
  nfs_cookie4 cookie;
  verifier4 cookie_verifier;
  unsigned long space_used = 0;
  pseudofs_entry_t *psfsentry, *curr_psfsentry;
  entry4 *entry_nfs_array = NULL;
  exportlist_t *save_pexport;
  struct gsh_export *saved_gsh_export;
  nfs_fh4 entryFH;
  cache_inode_status_t cache_status;
  int error = 0;
  size_t namelen = 0;
  cache_entry_t *entry = NULL;
  seudofs_entry_t tempentry;
  struct avltree_node *keynode, *currnode;

  resp->resop = NFS4_OP_READDIR;
  res_READDIR4.status = NFS4_OK;

  entryFH.nfs_fh4_len = 0;

  LogFullDebug(COMPONENT_NFS_V4_PSEUDO, "Entering NFS4_OP_READDIR_PSEUDO");

  /* get the caracteristic value for readdir operation */
  dircount = arg_READDIR4.dircount;
  maxcount = arg_READDIR4.maxcount;
  cookie = arg_READDIR4.cookie;
  space_used = sizeof(entry4);

  /* dircount is considered meaningless by many nfsv4 client (like the CITI one). we use maxcount instead */
  estimated_num_entries = maxcount / sizeof(entry4);

  LogFullDebug(COMPONENT_NFS_V4_PSEUDO,
               "PSEUDOFS READDIR: dircount=%lu, maxcount=%lu, cookie=%"PRIu64", sizeof(entry4)=%lu num_entries=%lu",
               dircount, maxcount, (uint64_t)cookie, space_used, estimated_num_entries);

  /* If maxcount is too short, return NFS4ERR_TOOSMALL */
  if(maxcount < sizeof(entry4) || estimated_num_entries == 0)
    {
      res_READDIR4.status = NFS4ERR_TOOSMALL;
      return res_READDIR4.status;
    }

  /* Now resolve the file handle to pseudo fs */
  if(!nfs4_FhandleToPseudo(&(data->currentFH), data->pseudofs, &psfsentry))
    {
      res_READDIR4.status = NFS4ERR_BADHANDLE;
      return res_READDIR4.status;
    }
  LogFullDebug(COMPONENT_NFS_V4_PSEUDO,
               "PSEUDOFS READDIR in #%s#", psfsentry->name);

  /* If this a junction filehandle ? */
  if(psfsentry->junction_export != NULL)
    {
      /* This is a junction */
      LogFullDebug(COMPONENT_NFS_V4_PSEUDO,
                   "PSEUDOFS READDIR : DIR #%s# id=%u is a junction",
                   psfsentry->name, psfsentry->junction_export->id);

      /* Step up the compound data */
      /**
       * @todo Danger Will Robinson!!
       * This is the same hack as above in pseudo lookup
       */
      data->req_ctx->export = get_gsh_export(psfsentry->junction_export->id,
					     true);
      data->pexport = &data->req_ctx->export->export;

      /* Build the credentials */
      res_READDIR4.status = nfs4_MakeCred(data);
      if(res_READDIR4.status != NFS4_OK)
        {
          LogMajor(COMPONENT_NFS_V4_PSEUDO,
                   "PSEUDO FS JUNCTION TRAVERSAL: /!\\ | Failed to get FSAL credentials for %s, id=%d",
                   data->pexport->fullpath, data->pexport->id);
          return res_READDIR4.status;
        }

      cache_status = nfs_export_get_root_entry(data->pexport, &entry);
      if(cache_status != CACHE_INODE_SUCCESS)
        {
          LogMajor(COMPONENT_NFS_V4_PSEUDO,
                   "PSEUDO FS JUNCTION TRAVERSAL: Failed to get root for %s, id=%d, status = %d",
                   data->pexport->fullpath, data->pexport->id, cache_status);
          res_READDIR4.status = NFS4ERR_SERVERFAULT;
          return res_READDIR4.status;
        }

      if(data->currentFH.nfs_fh4_len == 0)
        {
          if((error = nfs4_AllocateFH(&(data->currentFH))) != NFS4_OK)
            {
              LogMajor(COMPONENT_NFS_V4_PSEUDO,
                       "PSEUDO FS JUNCTION TRAVERSAL: /!\\ | Failed to allocate the first file handle");
              res_READDIR4.status = NFS4ERR_SERVERFAULT;
              return res_READDIR4.status;
            }
        }

      /* Build the nfs4 handle */
      if(!nfs4_FSALToFhandle(&data->currentFH, entry->obj_handle))
        {
          LogMajor(COMPONENT_NFS_V4_PSEUDO,
                   "PSEUDO FS JUNCTION TRAVERSAL: /!\\ | Failed to build the first file handle");
          res_READDIR4.status = NFS4ERR_SERVERFAULT;
          return res_READDIR4.status;
        }


      /* Keep the entry within the compound data */

      cache_inode_lru_ref(entry, LRU_REQ_INITIAL);
      if (data->current_entry) {
          cache_inode_lru_unref(data->current_entry, LRU_FLAG_NONE);
      }
      data->current_entry = entry;
      data->current_filetype = entry->type;

      /* redo the call on the other side of the junction */
      return nfs4_op_readdir(op, data, resp);
    }

  /* Allocation of the entries array */
  if((entry_nfs_array =
      gsh_calloc(estimated_num_entries, sizeof(entry4))) == NULL)
    {
      LogError(COMPONENT_NFS_V4_PSEUDO, ERR_SYS, ERR_MALLOC, errno);
      res_READDIR4.status = NFS4ERR_SERVERFAULT;
      return res_READDIR4.status;
    }

  /* Cookie verifier has the value of the Server Boot Time for pseudo fs */
  memset(cookie_verifier, 0, NFS4_VERIFIER_SIZE);
#ifdef _WITH_COOKIE_VERIFIER
  /* BUGAZOMEU: management of the cookie verifier */
  if(NFS_SpecificConfig.UseCookieVerf == 1)
    {
      memcpy(cookie_verifier, &ServerBootTime.tv_sec, sizeof(cookie_verifier));
      if(cookie != 0)
        {
          if(memcmp(cookie_verifier, arg_READDIR4.cookieverf, NFS4_VERIFIER_SIZE) != 0)
            {
              res_READDIR4.status = NFS4ERR_BAD_COOKIE;
              gsh_free(entry_nfs_array);
              return res_READDIR4.status;
            }
        }
    }
#endif
  /* Cookie delivered by the server and used by the client SHOULD not ne 0, 1 or 2 (cf RFC3530, page192)
   * because theses value are reserved for special use.
   *      0 - cookie for first READDIR
   *      1 - reserved for . on client handside
   *      2 - reserved for .. on client handside
   * Entries '.' and '..' are not returned also
   * For these reason, there will be an offset of 3 between NFS4 cookie and HPSS cookie */

  /* make sure to start at the right position given by the cookie */
  if (cookie == 0)
    { currnode = avltree_first(&psfsentry->child_tree_byid); }
  else {
    /* Find entry with cookie (which was set to pseudo_id) */
    memset(&tempentry.idavlnode, 0, sizeof(tempentry.idavlnode));
    keynode = &tempentry.idavlnode;
    tempentry.pseudo_id = cookie;
    currnode = avltree_lookup(keynode, &psfsentry->child_tree_byid);
    if(currnode == NULL)
      {
        res_READDIR4.status = NFS4ERR_BAD_COOKIE;
        return res_READDIR4.status;
      }
  }


  /*
      namelen = strlen(iter->name);
      entry_nfs_array[i].name.utf8string_len = namelen;
      if ((entry_nfs_array[i].name.utf8string_val = gsh_malloc(namelen + 1)) == NULL) 
        {
            LogError(COMPONENT_NFS_V4_PSEUDO, ERR_SYS, ERR_MALLOC, errno);
            res_READDIR4.status = NFS4ERR_SERVERFAULT;
            return res_READDIR4.status;
        }
      memcpy(entry_nfs_array[i].name.utf8string_val, iter->name, namelen);
      entry_nfs_array[i].name.utf8string_val[namelen] = '\0';
      entry_nfs_array[i].cookie = iter->pseudo_id + 3;
  */
  for( ;
       currnode != NULL;
       currnode = avltree_next(currnode)) {
    curr_psfsentry = avltree_container_of(currnode, pseudofs_entry_t, idavlnode);
    LogMidDebug(COMPONENT_NFS_V4_PSEUDO,
                "PSEUDO FS: Found entry %s pseudo_id %"PRIu64,
                curr_psfsentry->name, curr_psfsentry->pseudo_id);

    /////////
      namelen = strlen(curr_psfsentry->name);
      entry_nfs_array[i].name.utf8string_len = namelen;
      if ((entry_nfs_array[i].name.utf8string_val = gsh_malloc(namelen + 1)) == NULL) 
        {
            LogError(COMPONENT_NFS_V4_PSEUDO, ERR_SYS, ERR_MALLOC, errno);
            res_READDIR4.status = NFS4ERR_SERVERFAULT;
            return res_READDIR4.status;
        }
      memcpy(entry_nfs_array[i].name.utf8string_val, curr_psfsentry->name, namelen);
      entry_nfs_array[i].name.utf8string_val[namelen] = '\0';
      entry_nfs_array[i].cookie = curr_psfsentry->pseudo_id;
    /////////

      /* This used to be in an if with a bogus check for FATTR4_FILEHANDLE. Such
       * a common case, elected to set up FH for call to xxxx_ToFattr
       * unconditionally.
       */ 
      if(entryFH.nfs_fh4_len == 0)
        {
          if(nfs4_AllocateFH(&entryFH) != NFS4_OK)
            {  return res_READDIR4.status; }
        }

      /* Do the case where we stay within the pseudo file system. */
      if(curr_psfsentry->junction_export == NULL)
        {
          nfs4_PseudoToFhandle(&entryFH, curr_psfsentry);
          
          if(nfs4_PseudoToFattr(curr_psfsentry,
                                &(entry_nfs_array[i].attrs),
                                data, &entryFH, &(arg_READDIR4.attr_request)) != 0)
            {
              LogFatal(COMPONENT_NFS_V4_PSEUDO,
                       "nfs4_PseudoToFattr failed to convert pseudo fs attr");
            }
        }
      else
        {
          /* This is a junction. Code used to not recognize this which resulted
           * in readdir giving different attributes ( including FH, FSid, etc... )
           * to clients from a lookup. AIX refused to list the directory because of
           * this. Now we go to the junction to get the attributes.
           */
          LogFullDebug(COMPONENT_NFS_V4_PSEUDO,
                       "PSEUDOFS READDIR : Offspring DIR #%s# id=%u is a junction full path %s ", 
                       curr_psfsentry->name,
                       curr_psfsentry->junction_export->id,
                       curr_psfsentry->junction_export->fullpath); 
          /* Save the compound data context */
          save_pexport = data->pexport;
          data->pexport = curr_psfsentry->junction_export;
	  saved_gsh_export = data->req_ctx->export;
	  data->req_ctx->export = get_gsh_export(curr_psfsentry->junction_export->id,
						 true);
          /* Build the credentials */
          /* XXX Is this really necessary for doing a lookup and 
           * getting attributes?
           * The logic is borrowed from the process invoked above in this code
           * when the target directory is a junction.
           */ 
          res_READDIR4.status = nfs4_MakeCred(data);

          if(res_READDIR4.status == NFS4ERR_ACCESS)
            {
              LogMajor(COMPONENT_NFS_V4_PSEUDO,
		       "PSEUDO FS JUNCTION TRAVERSAL: /!\\ |"
		       " Failed to get FSAL credentials for %s, id=%d",
		       curr_psfsentry->junction_export->fullpath,
		       curr_psfsentry->junction_export->id);
              return res_READDIR4.status;
            }
	  cache_status = nfs_export_get_root_entry( thefsentry->junction_export, &entry);
	  if(cache_status != CACHE_INODE_SUCCESS)
            {
              LogMajor(COMPONENT_NFS_V4_PSEUDO,
		       "PSEUDO FS JUNCTION TRAVERSAL: Failed to get root for %s ,"
		       " id=%d, status = %d",
		       thefsentry->junction_export->fullpath,
		       curr_psfsentry->junction_export->id, cache_status);
              res_READDIR4.status = NFS4ERR_SERVERFAULT;
              return res_READDIR4.status;
            }
          /* Build the nfs4 handle. Again, we do this unconditionally. */
          if(!nfs4_FSALToFhandle(&entryFH, entry->obj_handle))
            {
              LogMajor(COMPONENT_NFS_V4_PSEUDO,
                   "PSEUDO FS JUNCTION TRAVERSAL: /!\\ | Failed to build the first file handle");
              res_READDIR4.status = NFS4ERR_SERVERFAULT;
              return res_READDIR4.status;
            }
          if(cache_entry_To_Fattr(entry,
                                  &(entry_nfs_array[i].attrs),
                                  data, &entryFH, &(arg_READDIR4.attr_request)) != 0)
            {
              /* Return the fattr4_rdattr_error , cf RFC3530, page 192 */
              entry_nfs_array[i].attrs.attrmask = RdAttrErrorBitmap;
              entry_nfs_array[i].attrs.attr_vals = RdAttrErrorVals;
            }
	  put_gsh_export(data->req_ctx->export);
	  data->req_ctx->export = saved_gsh_export;
	  data->pexport = save_pexport;
      }
      /* Chain the entry together */
      entry_nfs_array[nodecount].nextentry = NULL;
      if(nodecount != 0)
        entry_nfs_array[nodecount - 1].nextentry =
          &(entry_nfs_array[nodecount]);

      /* Increment the counter */
      nodecount += 1;

      /* Did we reach the maximum number of entries */
      if(nodecount == estimated_num_entries)
        { break; }
  } /* avltree for loop */

  /* Resize entry_nfs_array */
  /* @todo : Is this reallocation actually needed ? */
#ifdef BUGAZOMEU
  if(nodecount < estimated_num_entries)
    if((entry_nfs_array = gsh_realloc(entry_nfs_array, nodecount *
                                      sizeof(entry4))) == NULL)
      {
        LogError(COMPONENT_NFS_V4_PSEUDO, ERR_SYS, ERR_MALLOC, errno);
        res_READDIR4.status = NFS4ERR_SERVERFAULT;
        gsh_free(entry_nfs_array);
        return res_READDIR4.status;
      }
#endif

  /* Build the reply */
  memcpy(res_READDIR4.READDIR4res_u.resok4.cookieverf, cookie_verifier,
         NFS4_VERIFIER_SIZE);
  if(nodecount == 0)
    res_READDIR4.READDIR4res_u.resok4.reply.entries = NULL;
  else
    res_READDIR4.READDIR4res_u.resok4.reply.entries = entry_nfs_array;

  /* did we reach the end ? */
  if(currnode == NULL) /* currnode from the for loop above. */
    {
      /* Yes, we did */
      res_READDIR4.READDIR4res_u.resok4.reply.eof = TRUE;
    }
  else
    {
      /* No, there are some more entries */
      res_READDIR4.READDIR4res_u.resok4.reply.eof = FALSE;
    }

  /* Exit properly */
  res_READDIR4.status = NFS4_OK;

  return NFS4_OK;
}                               /* nfs4_op_readdir_pseudo */

/**                                                                           
 *                                                                            
 * @brief Compares two pseudofs ids used in pseudofs hashtable                
 *                                                                            
 * Compare two keys used in pseudofs cache. These keys are made from          
 * the pseudofs pathname and a hash of that pathname.                         
 *                                                                            
 * @param[in] buff1 First key                                                 
 * @param[in] buff2 Second key                                                
 * @return 0 if keys are the same, 1 otherwise                                
 *                                                                            
 *                                                                            
 */
int compare_nfs4_pseudo_key(hash_buffer_t *buff1,
                            hash_buffer_t *buff2)
{
  /* This compares cityhash64, path length, and fullpath */
  if (buff1->len != buff2->len ||
      (memcmp(buff1->pdata, buff2->pdata, buff1->len != 0)) )
    return 1;
  return 0;
}

uint32_t nfs4_pseudo_value_hash_func(hash_parameter_t * p_hparam,
                                     hash_buffer_t    * buffclef)
{
  uint32_t res = 0;
  uint64_t ch64_hash = 0;

  memcpy(&ch64_hash, buffclef->pdata, sizeof(ch64_hash));
  /* 64 bit modulo 32 bit ... is that ok? */
  res = ch64_hash % (uint32_t) p_hparam->index_size;
  if(isDebug(COMPONENT_HASHTABLE))
    LogFullDebug(p_hparam->ht_log_component, "%s: value = %"PRIu32,
                 p_hparam->ht_name, res);
  return res;
}                               /* nfs4_owner_value_hash_func */

uint64_t nfs4_pseudo_rbt_hash_func(hash_parameter_t * p_hparam,
                                   hash_buffer_t    * buffclef)
{
  uint64_t res = 0;
  memcpy(&res, buffclef->pdata, sizeof(res));
  if(isDebug(COMPONENT_HASHTABLE))
    LogFullDebug(p_hparam->ht_log_component, "rbt = %"PRIu64, res);
  return res;
}                               /* state_id_rbt_hash_func */

int display_pseudo_val(struct display_buffer * dspbuf, hash_buffer_t * pbuff)
{
  pseudofs_entry_t *psfsentry = (pseudofs_entry_t *)pbuff->pdata;

  return display_printf(dspbuf, "nodename=%s nodeid=%"PRIu64, psfsentry->name,
                        psfsentry->pseudo_id);
}

int display_pseudo_key(struct display_buffer * dspbuf, hash_buffer_t * pbuff)
{
  uint64_t ch64_hash = 0;
  int len;
  char pseudopath[MAXPATHLEN+2];

  ch64_hash = *(uint64_t *)pbuff->pdata;
  len = *(ushort *)(pbuff->pdata+sizeof(ch64_hash));
  strncpy(pseudopath, pbuff->pdata + sizeof(ch64_hash) + sizeof(ushort),
          len);
  return display_printf(dspbuf, "len=%d path=%s", len, pseudopath);
}

/**                                                                                   
 *                                                                                    
 * Init_nfs4_pseudo: Init the hashtable for NFS Pseudofs nodeid cache.                
 *                                                                                    
 * Perform all the required initialization for hashtable Pseudofs nodeid cache        
 *                                                                                    
 * @param param [IN] parameter used to init the pseudofs nodeid cache                 
 *                                                                                    
 * @return 0 if successful, -1 otherwise                                              
 *                                                                                    
 */
int Init_nfs4_pseudo(nfs4_pseudo_parameter_t param)
{
  if((ht_nfs4_pseudo = HashTable_Init(&param.hash_param)) == NULL)
    {
      LogCrit(param.hash_param.ht_log_component,
              "Cannot init %s cache", param.hash_param.ht_name);
      return -1;
    }

  return 0;
}                               /* Init_nfs4_pseudo */

