/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright CEA/DAM/DIF  (2008)
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 *
 * -------------
 */

/**
 * \file    fsal_lookup.c
 * \date    $Date: 2006/01/24 13:45:37 $
 * \brief   Lookup operations.
 *
 */
#include "config.h"

#include <string.h>
#include "fsal.h"
#include "fsal_internal.h"
#include "fsal_convert.h"
#include "gpfs_methods.h"

/**
 * FSAL_lookup :
 * Looks up for an object into a directory.
 *
 * Note : if parent handle and filename are NULL,
 *        this retrieves root's handle.
 *
 * \param parent_directory_handle (input)
 *        Handle of the parent directory to search the object in.
 * \param filename (input)
 *        The name of the object to find.
 * \param p_context (input)
 *        Authentication context for the operation (user,...).
 * \param object_handle (output)
 *        The handle of the object corresponding to filename.
 * \param object_attributes (optional input/output)
 *        Pointer to the attributes of the object we found.
 *        As input, it defines the attributes that the caller
 *        wants to retrieve (by positioning flags into this structure)
 *        and the output is built considering this input
 *        (it fills the structure according to the flags it contains).
 *        It can be NULL (increases performances).
 *
 * \return - ERR_FSAL_NO_ERROR, if no error.
 *         - Another error code else.
 *
 */
fsal_status_t GPFSFSAL_lookup(const struct req_op_context *p_context,
			      struct fsal_obj_handle *parent,
			      const char *p_filename,
			      struct attrlist *p_object_attr,
			      struct gpfs_file_handle *fh)
{
	fsal_status_t status;
	int parent_fd;
	int mnt_fd;
	struct gpfs_fsal_obj_handle *parent_hdl;

	if (!parent || !p_filename)
		return fsalstat(ERR_FSAL_FAULT, 0);

	mnt_fd = gpfs_get_root_fd(p_context->fsal_export);
	parent_hdl =
	    container_of(parent, struct gpfs_fsal_obj_handle, obj_handle);

	status =
	    fsal_internal_handle2fd_at(mnt_fd, parent_hdl->handle, &parent_fd,
				       O_RDONLY, 0);
	if (FSAL_IS_ERROR(status))
		return status;

	/* Be careful about junction crossing, symlinks, hardlinks,... */
	switch (parent->type) {
	case DIRECTORY:
		/* OK */
		break;

	case FS_JUNCTION:
		/* This is a junction */
		close(parent_fd);
		return fsalstat(ERR_FSAL_XDEV, 0);

	case REGULAR_FILE:
	case SYMBOLIC_LINK:
		/* not a directory */
		close(parent_fd);
		return fsalstat(ERR_FSAL_NOTDIR, 0);

	default:
		close(parent_fd);
		return fsalstat(ERR_FSAL_SERVERFAULT, 0);
	}

	status = fsal_internal_get_handle_at(parent_fd, p_filename, fh);
	if (FSAL_IS_ERROR(status)) {
		close(parent_fd);
		return status;
	}
	/* get object attributes */
	if (p_object_attr) {
		p_object_attr->mask =
		    p_context->fsal_export->ops->
		    fs_supported_attrs(p_context->fsal_export);
		status = GPFSFSAL_getattrs(p_context->fsal_export,
					   p_context, fh, p_object_attr);
		if (FSAL_IS_ERROR(status)) {
			FSAL_CLEAR_MASK(p_object_attr->mask);
			FSAL_SET_MASK(p_object_attr->mask, ATTR_RDATTR_ERR);
		}
	}
	close(parent_fd);

	/* lookup complete ! */
	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}
