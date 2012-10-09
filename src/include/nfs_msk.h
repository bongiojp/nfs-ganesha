/*
 * Copyright CEA/DAM/DIF  (2012)
 * contributeur : Dominique MARTINET <asmadeus@codewreck.org>
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

#ifndef _NFS_MSK_H
#define _NFS_MSK_H

typedef struct nfs_msk_param__
{
  unsigned short nfs_msk_port;
} nfs_msk_parameter_t ;


int nfs_msk_read_conf( config_file_t   in_config,
                   nfs_msk_parameter_t *pparam ) ;
int Init_nfs_msk();


#define CONF_LABEL_NFS_MSK "NFS_MSK"
#define NFS_MSK_PORT 20049


#endif // _NFS_MSK_H
