/**
 *
 * \file    fsal_creds.c
 * \author  $Author: leibovic $
 * \date    $Date: 2006/01/24 13:45:36 $
 * \version $Revision: 1.15 $
 * \brief   FSAL credentials handling functions.
 *
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "fsal.h"
#include "fsal_internal.h"
#include "fsal_convert.h"
#include "cache_inode.h"
#include "nfs_proto_tools.h"
#include <pwd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <pthread.h>
#include <mntent.h>             /* for handling mntent */
#include <libgen.h>             /* for dirname */
#include <sys/vfs.h>            /* for fsid */

/**
 * @defgroup FSALCredFunctions Credential handling functions.
 *
 * Those functions handle security contexts (credentials).
 * 
 * @{
 */

static pthread_t inode_update_thread = 0;

void *inode_update(void *argp)
{
  int rc = 0;
  int rc2 = 0;
  struct stat64 buf;
  struct flock fl;
  struct callback_arg callback;
  cache_inode_fsal_data_t pfsal_data;
  gpfsfsal_handle_t *phandle = (gpfsfsal_handle_t *) &pfsal_data.handle;
  int reason = 0;
  unsigned int *fhP;

  gpfsfsal_export_context_t *p_export_context = (gpfsfsal_export_context_t *)argp;

  SetNameFunction("inode_update_thread");

  LogInfo(COMPONENT_FSAL,
               "inode_update: pid %p: start",
               (caddr_t) pthread_self());

  pfsal_data.cookie = 0;
  phandle->data.handle.handle_size = OPENHANDLE_HANDLE_LEN;
  phandle->data.handle.handle_key_size = 0;
  callback.mountdirfd = p_export_context->mount_root_fd;
  callback.handle = &phandle->data.handle;
  callback.reason = &reason;
  callback.buf = &buf;
  callback.fl = (struct glock *) &fl;

  while(rc == 0)
    {
      rc = gpfs_ganesha(OPENHANDLE_INODE_UPDATE, &callback);
      LogDebug(COMPONENT_FSAL,
               "inode update: pid %p: rc %d reason %d update ino %ld",
                (caddr_t) pthread_self(), rc, reason,
                callback.buf->st_ino);
      LogDebug(COMPONENT_FSAL,
               "inode update: handle size = %u key_size = %u",
                callback.handle->handle_size,
                callback.handle->handle_key_size);
      fhP = (int *)&(callback.handle->f_handle[0]);
      LogDebug(COMPONENT_FSAL,
               "inode update: handle %08x %08x %08x %08x %08x %08x %08x\n",
                fhP[0],fhP[1],fhP[2],fhP[3],fhP[4],fhP[5],fhP[6]);

      if (reason == INODE_LOCK_GRANTED)
      {
        LogDebug(COMPONENT_FSAL,
               "inode update: pid %p: lock pid %d type %d start %lld len %lld",
                (caddr_t) pthread_self(), fl.l_pid, fl.l_type, (long long) fl.l_start,
                (long long) fl.l_len);
        continue;
      }
      rc2 =  cache_inode_invalidate (0, &pfsal_data);
      if ( rc2 ) {
        LogDebug(COMPONENT_FSAL,
                "Inode update: invalidate cache failed with %d", rc2);
      }
    }
    LogInfo(COMPONENT_FSAL,
                 "inode_update: pid %p: error %d exit",
                 (caddr_t) pthread_self(), rc);
  return(0);
}

/**
 * build the export entry
 */
fsal_status_t GPFSFSAL_BuildExportContext(fsal_export_context_t *export_context, /* OUT */
                                      fsal_path_t * p_export_path,      /* IN */
                                      char *fs_specific_options /* IN */
    )
{
  int rc, fd, mntexists;
  FILE *fp;
  struct mntent *p_mnt;
  struct statfs stat_buf;

  fsal_status_t status;
  fsal_op_context_t op_context;
  gpfsfsal_export_context_t *p_export_context = (gpfsfsal_export_context_t *)export_context;

  /* sanity check */
  if((p_export_context == NULL) || (p_export_path == NULL))
    {
      LogCrit(COMPONENT_FSAL,
              "NULL mandatory argument passed to %s()", __FUNCTION__);
      Return(ERR_FSAL_FAULT, 0, INDEX_FSAL_BuildExportContext);
    }

  /* open mnt file */
  fp = setmntent(MOUNTED, "r");

  if(fp == NULL)
    {
      rc = errno;
      LogCrit(COMPONENT_FSAL, "Error %d in setmntent(%s): %s", rc, MOUNTED,
                      strerror(rc));
      Return(posix2fsal_error(rc), rc, INDEX_FSAL_BuildExportContext);
    }

  /* Check if mount point is really a gpfs share. If not, we can't continue.*/
  mntexists = 0;
  while((p_mnt = getmntent(fp)) != NULL)
    if(p_mnt->mnt_dir != NULL  && p_mnt->mnt_type != NULL)
      /* There is probably a macro for "gpfs" type ... not sure where it is. */
      if (strncmp(p_mnt->mnt_type, "gpfs", 4) == 0)
        if (strncmp(p_mnt->mnt_dir, p_export_path->path, strlen(p_mnt->mnt_dir)) == 0)
          mntexists = 1;
  
  if (mntexists == 0)
    {
      LogMajor(COMPONENT_FSAL,
               "FSAL BUILD EXPORT CONTEXT: ERROR: Could not open GPFS mount point %s does not exist.",
               p_export_path->path);
      ReturnCode(ERR_FSAL_INVAL, 0);
    }

  /* save file descriptor to root of GPFS share */
  fd = open(p_export_path->path, O_RDONLY | O_DIRECTORY);
  if(fd < 0)
    {
      LogMajor(COMPONENT_FSAL,
               "FSAL BUILD EXPORT CONTEXT: ERROR: Could not open GPFS mount point %s: rc = %d",
               p_export_path->path, errno);
      ReturnCode(ERR_FSAL_INVAL, 0);
    }
  p_export_context->mount_root_fd = fd;

  /* save filesystem ID */
  rc = statfs(p_export_path->path, &stat_buf);
  if(rc)
    {
      LogMajor(COMPONENT_FSAL,
               "statfs call failed on file %s: %d", p_export_path->path, rc);
      ReturnCode(ERR_FSAL_INVAL, 0);
    }
  p_export_context->fsid[0] = stat_buf.f_fsid.__val[0];
  p_export_context->fsid[1] = stat_buf.f_fsid.__val[1];

  /* save file handle to root of GPFS share */
  op_context.export_context = p_export_context;
  // op_context.credential = ???
  status = fsal_internal_get_handle(&op_context,
                                    p_export_path,
                                    (fsal_handle_t *)(&(p_export_context->mount_root_handle)));
  if(FSAL_IS_ERROR(status))
    {
      close(p_export_context->mount_root_fd);
      LogMajor(COMPONENT_FSAL,
               "FSAL BUILD EXPORT CONTEXT: ERROR: Conversion from gpfs filesystem root path to handle failed : %d",
               status.minor);
      ReturnCode(ERR_FSAL_INVAL, 0);
    }

  if (inode_update_thread == 0)
    pthread_create(&inode_update_thread, NULL, inode_update, p_export_context);

  Return(ERR_FSAL_NO_ERROR, 0, INDEX_FSAL_BuildExportContext);
}

/**
 * FSAL_CleanUpExportContext :
 * this will clean up and state in an export that was created during
 * the BuildExportContext phase.  For many FSALs this may be a noop.
 *
 * \param p_export_context (in, gpfsfsal_export_context_t)
 */

fsal_status_t GPFSFSAL_CleanUpExportContext(fsal_export_context_t * p_export_context) 
{
  if(p_export_context == NULL) 
  {
    LogCrit(COMPONENT_FSAL,
            "NULL mandatory argument passed to %s()", __FUNCTION__);
    Return(ERR_FSAL_FAULT, 0, INDEX_FSAL_CleanUpExportContext);
  }
  
  close(((gpfsfsal_export_context_t *)p_export_context)->mount_root_fd);

  Return(ERR_FSAL_NO_ERROR, 0, INDEX_FSAL_CleanUpExportContext);
}

/* @} */
