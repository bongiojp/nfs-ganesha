/*
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright CEA/DAM/DIF  (2011)
 * contributeur : Philippe DENIEL   philippe.deniel@cea.fr
 *                Thomas LEIBOVICI  thomas.leibovici@cea.fr
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * ---------------------------------------
 */

/**
 * \file    nfs_msk_proto_tools.c
 * \brief   NFS_MSK version
 *
 * nfs_msk_proto_tools.c : _NFS_MSK_interpretor, protocol's service functions
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef _SOLARIS
#include "solaris_port.h"
#endif

#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include "nfs_core.h"
#include "log.h"
#include "nfs_msk.h"
#include "config_parsing.h"

int nfs_msk_read_conf( config_file_t   in_config,
                   nfs_msk_parameter_t *pparam )
{
  int var_max;
  int var_index;
  int err;
  char *key_name;
  char *key_value;
  config_item_t block;

  int DebugLevel = -1;
  char *LogFile = NULL;

  /* Is the config tree initialized ? */
  if(in_config == NULL || pparam == NULL)
    return -1 ;

  /* Get the config BLOCK */
  if((block = config_FindItemByName(in_config, CONF_LABEL_NFS_MSK )) == NULL)
    return -2 ; /* no NFS_MSK config, use default */
  else if(config_ItemType(block) != CONFIG_ITEM_BLOCK)
    return -1 ;

  var_max = config_GetNbItems(block);

  for(var_index = 0; var_index < var_max; var_index++)
    {
      config_item_t item;

      item = config_GetItemByIndex(block, var_index);

      /* Get key's name */
      if((err = config_GetKeyValue(item, &key_name, &key_value)) != 0)
        {
          fprintf(stderr,
                  "Error reading key[%d] from section \"%s\" of configuration file.\n",
                  var_index, CONF_LABEL_NFS_MSK );
          return -1 ;
        }

      if(!strcasecmp(key_name, "NFS_MSK_Port"))     
        {
          pparam->nfs_msk_port = atoi( key_value ) ;
        }
      else if(!strcasecmp(key_name, "DebugLevel"))
        {
          DebugLevel = ReturnLevelAscii(key_value);

          if(DebugLevel == -1)
            {
              fprintf(stderr,
                      "NFS_MSK: ERROR: Invalid debug level name: \"%s\".",
                      key_value);
              return -1 ;
            }
        }
      else if(!strcasecmp(key_name, "LogFile"))
        {
          LogFile = key_value;
        }
      else
        {
          fprintf(stderr,
                  "Unknown or unsettable key: %s (item %s)\n",
                  key_name, CONF_LABEL_NFS_MSK);
          return -1 ;
        }
    } /* for */

  /* init logging */
  if(LogFile)
    SetComponentLogFile(COMPONENT_NFS_MSK, LogFile);

  if(DebugLevel > -1)
    SetComponentLogLevel(COMPONENT_NFS_MSK, DebugLevel);


  return 0 ;
} /* nfs_msk_read_conf */
