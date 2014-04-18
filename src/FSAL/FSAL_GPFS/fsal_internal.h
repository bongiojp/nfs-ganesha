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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 * ---------------------------------------
 */

/**
 *
 * @file    fsal_internal.h
 * @brief   Extern definitions for variables that are
 *          defined in fsal_internal.c.
 */

#define _USE_NFS4_ACL

#include <sys/stat.h>
#include "fsal.h"
#include "ganesha_list.h"
#include "fsal_types.h"
#include "fcntl.h"
#include "include/gpfs_nfs.h"
#include "fsal_up.h"

void gpfs_handle_ops_init(struct fsal_obj_ops *ops);

bool fsal_error_is_event(fsal_status_t status);
/*
 * Tests whether an error code should be raised as an info debug.
 */
bool fsal_error_is_info(fsal_status_t status);

void set_gpfs_verifier(verifier4 *verifier);

struct gpfs_fsal_up_ctx {
	/* There is one GPFS FSAL UP Context per GPFS file system */
	struct glist_head gf_list;	/* List of GPFS FSAL UP Contexts */
	struct glist_head gf_exports;	/* List of GPFS Export Contexts on
					   this FSAL UP context */
	struct fsal_export *gf_export;
	int gf_fd;		/* GPFS File System Directory fd */
	unsigned int gf_fsid[2];
	pthread_t gf_thread;
	struct fsal_module *gf_fsal; /* FSAL module */
};
/**
 * The full, 'private' DS (data server) handle
 */

struct gpfs_ds {
	struct gpfs_file_handle wire;	/*< Wire data */
	struct fsal_ds_handle ds;	/*< Public DS handle */
	bool connected;		/*< True if the handle has been connected */
};


/* defined the set of attributes supported with POSIX */
#define GPFS_SUPPORTED_ATTRIBUTES (                              \
		ATTR_TYPE     | ATTR_SIZE     |                  \
		ATTR_FSID     | ATTR_FILEID   |                  \
		ATTR_MODE     | ATTR_NUMLINKS | ATTR_OWNER     | \
		ATTR_GROUP    | ATTR_ATIME    | ATTR_RAWDEV    | \
		ATTR_CTIME    | ATTR_MTIME    | ATTR_SPACEUSED | \
		ATTR_CHGTIME | ATTR_ACL| ATTR4_SPACE_RESERVED)

/* the following variables must not be defined in fsal_internal.c */
#ifndef FSAL_INTERNAL_C

/* export_context_t is not given to every function, but
 * most functions need to use the open-by-handle funcionality.
 */

#endif

/* Define the buffer size for GPFS NFS4 ACL. */
#define GPFS_ACL_BUF_SIZE 0x1000

/* A set of buffers to retrieve multiple attributes at the same time. */
typedef struct fsal_xstat__ {
	int attr_valid;
	struct stat buffstat;
	char buffacl[GPFS_ACL_BUF_SIZE];
} gpfsfsal_xstat_t;

static inline size_t gpfs_sizeof_handle(const struct gpfs_file_handle *hdl)
{
	return offsetof(struct gpfs_file_handle, f_handle)+hdl->handle_size;
}

void export_ops_init(struct export_ops *ops);
void handle_ops_init(struct fsal_obj_ops *ops);
void ds_ops_init(struct fsal_ds_ops *ops);
void export_ops_pnfs(struct export_ops *ops);
void handle_ops_pnfs(struct fsal_obj_ops *ops);

fsal_status_t fsal_internal_close(int fd, void *owner, int cflags);

int fsal_internal_version();

fsal_status_t fsal_internal_get_handle(const char *p_fsalpath,	/* IN */
				struct gpfs_file_handle *p_handle);  /* OUT */

fsal_status_t fsal_internal_get_handle_at(int dfd,
				const char *p_fsalname,  /* IN */
				struct gpfs_file_handle *p_handle); /* OUT */

/**
 * Gets a fd from a handle
 */
fsal_status_t fsal_internal_handle2fd(int dirfd,
				      struct gpfs_file_handle *phandle,
				      int *pfd, int oflags, bool reopen);

fsal_status_t fsal_internal_handle2fd_at(int dirfd,
					 struct gpfs_file_handle *phandle,
					 int *pfd, int oflags, bool reopen);
/**
 * Gets a file handle from a parent handle and name
 */
fsal_status_t fsal_internal_get_fh(int dirfd,	/* IN */
				   struct gpfs_file_handle *p_dir_handle,
				   const char *p_fsalname,             /* IN */
				   struct gpfs_file_handle *p_handle); /* OUT */
/**
 * Access a link by a file handle.
 */
fsal_status_t fsal_readlink_by_handle(int dirfd,
				      struct gpfs_file_handle *p_handle,
				      char *__buf, size_t *maxlen);

/**
 * Get the handle for a path (posix or fid path)
 */
fsal_status_t fsal_internal_fd2handle(int fd,	/* IN */
				struct gpfs_file_handle *p_handle); /* OUT */

fsal_status_t fsal_internal_link_at(int srcfd, int dfd, char *name);

fsal_status_t fsal_internal_link_fh(int dirfd,
				    struct gpfs_file_handle *p_target_handle,
				    struct gpfs_file_handle *p_dir_handle,
				    const char *p_link_name);

fsal_status_t fsal_internal_stat_name(int dirfd,
				      struct gpfs_file_handle *p_dir_handle,
				      const char *p_stat_name,
				      struct stat *buf);

fsal_status_t fsal_internal_unlink(int dirfd,
				   struct gpfs_file_handle *p_dir_handle,
				   const char *p_stat_name, struct stat *buf);

fsal_status_t fsal_internal_create(int dirfd,
				   struct gpfs_file_handle *p_dir_handle,
				   const char *p_stat_name, mode_t mode,
				   dev_t dev,
				   struct gpfs_file_handle *p_new_handle,
				   struct stat *buf);

fsal_status_t fsal_internal_rename_fh(int dirfd,
				      struct gpfs_file_handle *p_old_handle,
				      struct gpfs_file_handle *p_new_handle,
				      const char *p_old_name,
				      const char *p_new_name);

/**
 *  test the access to a file from its POSIX attributes (struct stat)
 *  OR its FSAL attributes (fsal_attrib_list_t).
 *
 */

fsal_status_t fsal_internal_testAccess(const struct req_op_context *p_context,
				       fsal_accessflags_t access_type,
				       struct attrlist *p_object_attributes);

fsal_status_t fsal_stat_by_handle(int dirfd, struct gpfs_file_handle *p_handle,
				  struct stat *buf);

fsal_status_t fsal_get_xstat_by_handle(int dirfd,
				       struct gpfs_file_handle *p_handle,
				       gpfsfsal_xstat_t *p_buffxstat,
				       uint32_t *expire_time_attr,
				       bool expire);

fsal_status_t fsal_set_xstat_by_handle(int dirfd,
				       const struct req_op_context *p_context,
				       struct gpfs_file_handle *p_handle,
				       int attr_valid, int attr_changed,
				       gpfsfsal_xstat_t *p_buffxstat);

fsal_status_t fsal_check_access_by_mode(const struct req_op_context *p_context,
					fsal_accessflags_t access_type,	/* IN */
					struct stat *p_buffstat);	/* IN */

fsal_status_t fsal_trucate_by_handle(int dirfd,
				     const struct req_op_context *p_context,
				     struct gpfs_file_handle *p_handle,
				     u_int64_t size);

/* All the call to FSAL to be wrapped */
fsal_status_t GPFSFSAL_access(struct gpfs_file_handle *p_object_handle,	/* IN */
			      int dirfd,	/* IN */
			      fsal_accessflags_t access_type,	/* IN */
			      struct attrlist *p_object_attributes); /* IO */

fsal_status_t GPFSFSAL_getattrs(struct fsal_export *export,	/* IN */
				const struct req_op_context *p_context,	/* IN */
				struct gpfs_file_handle *p_filehandle,	/* IN */
				struct attrlist *p_object_attributes); /* IO */

fsal_status_t GPFSFSAL_getattrs_descriptor(int *p_file_descriptor,	/* IN */
				struct gpfs_file_handle *p_filehandle,	/* IN */
				int dirfd,	/* IN */
				struct attrlist *p_object_attributes);  /* IO */

fsal_status_t GPFSFSAL_statfs(int fd,				/* IN */
			      struct fsal_obj_handle *obj_hdl,	/* IN */
			      struct statfs *buf);		/* OUT */

fsal_status_t GPFSFSAL_setattrs(struct fsal_obj_handle *dir_hdl,	/* IN */
				const struct req_op_context *p_context,	/* IN */
				struct attrlist *p_object_attributes);	/* IN */

fsal_status_t GPFSFSAL_create(struct fsal_obj_handle *dir_hdl,	/* IN */
			      const char *p_filename,	/* IN */
			      const struct req_op_context *p_context,	/* IN */
			      uint32_t accessmode,	/* IN */
			      struct gpfs_file_handle *p_object_handle,/* OUT */
			      struct attrlist *p_object_attributes);    /* IO */

fsal_status_t GPFSFSAL_mkdir(struct fsal_obj_handle *dir_hdl,	/* IN */
			     const char *p_dirname,	/* IN */
			     const struct req_op_context *p_context, /* IN */
			     uint32_t accessmode,	/* IN */
			     struct gpfs_file_handle *p_object_handle, /* OUT */
			     struct attrlist *p_object_attributes);   /* IO */

fsal_status_t GPFSFSAL_link(struct fsal_obj_handle *dir_hdl,	/* IN */
			    struct gpfs_file_handle *p_target_handle,	/* IN */
			    const char *p_link_name,	/* IN */
			    const struct req_op_context *p_context,	/* IN */
			    struct attrlist *p_attributes);	/* IN/OUT */

fsal_status_t GPFSFSAL_mknode(struct fsal_obj_handle *dir_hdl,	/* IN */
			      const char *p_node_name,	/* IN */
			      const struct req_op_context *p_context,	/* IN */
			      uint32_t accessmode,	/* IN */
			      mode_t nodetype,	/* IN */
			      fsal_dev_t *dev,	/* IN */
			      struct gpfs_file_handle *p_object_handle,/* OUT */
			      struct attrlist *node_attributes);	/* IO */

fsal_status_t GPFSFSAL_opendir(struct gpfs_file_handle *p_dir_handle,	/* IN */
			       int dirfd,	/* IN */
			       int *p_dir_descriptor,	/* OUT */
			       struct attrlist *p_dir_attributes);  /* IN/OUT */

fsal_status_t GPFSFSAL_closedir(int *p_dir_descriptor);

fsal_status_t GPFSFSAL_open_by_name(struct gpfs_file_handle *dirhandle,	/* IN */
				    const char *filename,	/* IN */
				    int dirfd,	/* IN */
				    fsal_openflags_t openflags,	/* IN */
				    int *file_descriptor,	/* OUT */
				    struct attrlist *file_attributes);/* IO */

fsal_status_t GPFSFSAL_open(struct fsal_obj_handle *obj_hdl,	/* IN */
			    const struct req_op_context *p_context, /* IN */
			    fsal_openflags_t openflags,	/* IN */
			    int *p_file_descriptor,	/* IN/OUT */
			    struct attrlist *p_file_attributes, /* IN/OUT */
			    bool reopen); /* IN */

fsal_status_t GPFSFSAL_read(int fd,	/* IN */
			    uint64_t offset,	/* IN */
			    size_t buffer_size,	/* IN */
			    caddr_t buffer,	/* OUT */
			    size_t *p_read_amount, /* OUT */
			    bool *p_end_of_file); /* OUT */

fsal_status_t GPFSFSAL_write(int fd,	/* IN */
			     uint64_t offset,	/* IN */
			     size_t buffer_size,	/* IN */
			     caddr_t buffer,	/* IN */
			     size_t *p_write_amount,	/* OUT */
			     bool *fsal_stable,	/* IN/OUT */
			     const struct req_op_context *p_context);

fsal_status_t GPFSFSAL_clear(int fd,	/* IN */
			     uint64_t offset,	/* IN */
			     size_t buffer_size,	/* IN */
			     caddr_t buffer,	/* IN */
			     size_t *p_write_amount,	/* OUT */
			     bool *fsal_stable,	/* IN/OUT */
			     const struct req_op_context *p_context,
			     bool allocate);

fsal_status_t GPFSFSAL_close(int *p_file_descriptor);	/* IN */

fsal_status_t GPFSFSAL_dynamic_fsinfo(struct gpfs_file_handle *p_handle,
				int dirfd,	/* IN */
				fsal_dynamicfsinfo_t *p_dynamicinfo); /* OUT */

fsal_status_t GPFSFSAL_lookup(const struct req_op_context *p_context,	/* IN */
			      struct fsal_obj_handle *parent,
			      const char *p_filename,
			      struct attrlist *p_object_attr,
			      struct gpfs_file_handle *fh);

fsal_status_t GPFSFSAL_lookupPath(const char *p_path,	/* IN */
				int dirfd,	/* IN */
				struct gpfs_file_handle *object_handle,/* OUT */
				struct attrlist *p_object_attributes);	/* IO */

fsal_status_t GPFSFSAL_lookupJunction(struct gpfs_file_handle *p_handle,
				int dirfd,	/* IN */
				struct gpfs_file_handle *fsoot_hdl,/* OUT */
				struct attrlist *p_fsroot_attributes); /* IO */

fsal_status_t GPFSFSAL_lock_op(struct fsal_export *export,
			       struct fsal_obj_handle *obj_hdl,	/* IN */
			       void *p_owner,	/* IN */
			       fsal_lock_op_t lock_op,	/* IN */
			       fsal_lock_param_t request_lock,	/* IN */
			       fsal_lock_param_t *conflicting_lock); /* OUT */

fsal_status_t GPFSFSAL_share_op(int mntfd,	/* IN */
				int fd,	/* IN */
				void *p_owner,	/* IN */
				fsal_share_param_t request_share); /* IN */

fsal_status_t GPFSFSAL_rcp(struct gpfs_file_handle *filehandle,	/* IN */
			   int dirfd,	/* IN */
			   const char *p_local_path,	/* IN */
			   int transfer_opt);         /* IN */

fsal_status_t GPFSFSAL_rename(struct fsal_obj_handle *old_hdl,	/* IN */
			      const char *p_old_name,	/* IN */
			      struct fsal_obj_handle *new_hdl,	/* IN */
			      const char *p_new_name,	/* IN */
			      const struct req_op_context *p_context);	/* IN */

fsal_status_t GPFSFSAL_readlink(struct fsal_obj_handle *dir_hdl,	/* IN */
				const struct req_op_context *p_context,	/* IN */
				char *p_link_content,	/* OUT */
				size_t *link_len,	/* IN/OUT */
				struct attrlist *p_link_attributes);  /* IO */

fsal_status_t GPFSFSAL_symlink(struct fsal_obj_handle *dir_hdl,	/* IN */
			       const char *p_linkname,	/* IN */
			       const char *p_linkcontent,	/* IN */
			       const struct req_op_context *p_context,	/* IN */
			       uint32_t accessmode,	/* IN (ignored) */
			       struct gpfs_file_handle *p_link_handle, /* OUT */
			       struct attrlist *p_link_attributes);   /* IO */

int GPFSFSAL_handlecmp(struct gpfs_file_handle *handle1,
		       struct gpfs_file_handle *handle2,
		       fsal_status_t *status);

unsigned int GPFSFSAL_Handle_to_HashIndex(struct gpfs_file_handle *p_handle,
					  unsigned int cookie,
					  unsigned int alphabet_len,
					  unsigned int index_size);

unsigned int GPFSFSAL_Handle_to_RBTIndex(struct gpfs_file_handle *p_handle,
					 unsigned int cookie);

fsal_status_t GPFSFSAL_truncate(struct fsal_export *export,	/* IN */
				struct gpfs_file_handle *p_filehandle,	/* IN */
				const struct req_op_context *p_context,	/* IN */
				size_t length,	/* IN */
				struct attrlist *p_object_attributes);/* IO */

fsal_status_t GPFSFSAL_unlink(struct fsal_obj_handle *dir_hdl,	/* IN */
			      const char *p_object_name,	/* IN */
			      const struct req_op_context *p_context,	/* IN */
			      struct attrlist *p_parent_attributes); /* IO */

char *GPFSFSAL_GetFSName();

fsal_status_t GPFSFSAL_GetXAttrAttrs(struct gpfs_file_handle *obj_hdl, /* IN */
				     int dirfd,	/* IN */
				     unsigned int xattr_id,	/* IN */
				     struct attrlist *p_attrs);

fsal_status_t GPFSFSAL_ListXAttrs(struct gpfs_file_handle *obj_hdl, /* IN */
				  unsigned int cookie,	/* IN */
				  int dirfd,	/* IN */
				  fsal_xattrent_t *xattrs_tab,	/* IN/OUT */
				  unsigned int xattrs_tabsize,	/* IN */
				  unsigned int *p_nb_returned,	/* OUT */
				  int *end_of_list);	/* OUT */

fsal_status_t GPFSFSAL_GetXAttrValueById(struct gpfs_file_handle *objhdl,
					 unsigned int xattr_id,	/* IN */
					 int dirfd,	/* IN */
					 caddr_t buffer_addr,	/* IN/OUT */
					 size_t buffer_size,	/* IN */
					 size_t *p_output_size);

fsal_status_t GPFSFSAL_GetXAttrIdByName(struct gpfs_file_handle *objhdl,
					const char *xattr_name,	/* IN */
					int dirfd,	/* IN */
					unsigned int *pxattr_id); /* OUT */

fsal_status_t GPFSFSAL_GetXAttrValueByName(struct gpfs_file_handle *objhdl,
					   const char *xattr_name,	/* IN */
					   int dirfd,	/* IN */
					   caddr_t buffer_addr,	/* IN/OUT */
					   size_t buffer_size,	/* IN */
					   size_t *p_output_size); /* OUT */

fsal_status_t GPFSFSAL_SetXAttrValue(struct gpfs_file_handle *obj_hdl, /* IN */
				     const char *xattr_name,	/* IN */
				     int dirfd,	/* IN */
				     caddr_t buffer_addr,	/* IN */
				     size_t buffer_size,	/* IN */
				     int create);	/* IN */

fsal_status_t GPFSFSAL_RemoveXAttrByName(struct gpfs_file_handle *objhdl,
					 int dirfd,	/* IN */
					 const char *xattr_name);	/* IN */

int GPFSFSAL_GetXattrOffsetSetable(void);

unsigned int GPFSFSAL_GetFileno(int *pfile);

fsal_status_t GPFSFSAL_commit(int *p_file_descriptor, uint64_t offset,
			      size_t size);

struct glist_head gpfs_fsal_up_ctx_list;

void *GPFSFSAL_UP_Thread(void *Arg);

struct gpfs_fsal_up_ctx *gpfsfsal_find_fsal_up_context(struct gpfs_fsal_up_ctx
						       *ctx);
