// ----------------------------------------------------------------------------
// Copyright IBM Corp. 2010, 2011
// All Rights Reserved
// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------
// Filename:    fsi_ipc_client.h
// Description: Samba VFS library - client definitions
// Author:      FSI IPC Team
// ----------------------------------------------------------------------------

#ifndef __FSI_IPC_CLIENT_H__
#define __FSI_IPC_CLIENT_H__

// Linux includes
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/time.h>
#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <sys/stat.h>
#include <sys/param.h>

// FSI IPC defines
#define FSI_CIFS_RESERVED_STREAMS   4   // CIFS does not allow handles 0-2

#define FSI_BLOCK_ALIGN(x, blocksize) \
(((x) % (blocksize)) ? (((x) / (blocksize)) * (blocksize)) : (x))

// FSI IPC getlock constants
#define FSI_IPC_GETLOCK_PTYPE 2
#define FSI_IPC_GETLOCK_PPID  0

#define PTFSAL_FILESYSTEM_NUMBER 77
#define FUSE_EXPORT_ID 281474976710656
#define FSI_IPC_FUSE_MSGID_BASE 5000000

// FSI Trace Level defines for Samba DEBUG level parm
// Samba default runtime level is 2, all trace <= level are logged
// Note: Though these defines map to Samba debug levels we are 
// using the level settings differently than Samba proper
#define FSI_FATAL    1  // TRC_FATAL - A fatal condition that prevents
                        //   the system continuing normal ops
#define FSI_ERR      2  // TRC_ERR - Warnings and error conditions
#define FSI_NOTICE   2  // TRC_NOTICE - meaningful events in system
#define FSI_STAT     2  // TRC_STAT - Can't really match separate trace
                        //   class but but use this for stats only
#define FSI_INFO     3  // TRC_INFO Detailed tracing of normal flow, yet not
                        //   as intensive as TRC_DEBUG
#define FSI_DEBUG    5  // TRC_DBUG - Very high frequency, can affect 
                        //   performance, user for debugging a component


#ifndef   __GNUC__
#define __attribute__(x) /*nothing*/
#endif // __GNUC__

// The following functions enable compile-time check with a cost of a function
// call. Ths function is empty, but due to its  __attribute__ declaration
// the compiler checks the format string which is passed to it by the
// FSI_TRACE...() mechanism.
extern void
compile_time_check_func(const char * fmt, ...)
__attribute__((format(printf, 1, 2)));  // 1=format 2=params


extern void ccl_log(int debugLevel, char *debugString);

// Our own trace macro that adds standard prefix to statements that includes
// the level and function name
#define FSI_TRACE2( level, format, ... )                                    \
{                                                                          \
  char logString[256];                                                    \
  compile_time_check_func( format, ## __VA_ARGS__ );                       \
  if (level <= FSI_DEBUG) {                                                \
    snprintf(logString ,1000, "[" #level "]: "   "%s: "  format "\n",      \
             __func__,  ## __VA_ARGS__ );                                  \
    printf("%s",logString);                                                \
    fflush(NULL);                                                              \
    ccl_log(level,logString);                                              \
  }                                                                        \
}

#define FSI_TRACE1( level, format, ... )                                    \
{                                                                          \
  char logString[256];                                                    \
  compile_time_check_func( format, ## __VA_ARGS__ );                       \
  if (level <= FSI_DEBUG) {                                                \
    snprintf(logString ,256, "[" #level "]: "   "%s: "  format ,      \
             __func__,  ## __VA_ARGS__ );                                  \
    ccl_log(level,logString);                                              \
  }                                                                        \
}

//Our own trace macro that adds standard prefix to statements that includes
// the level and function name
#define FSI_TRACE( level, format, ... )                                    \
{                                                                          \
  compile_time_check_func( format, ## __VA_ARGS__ );                       \
  if (level <= FSI_DEBUG) {                                                \
    printf("[" #level "]: "   "%s: "  format "\n",                         \
             __func__,  ## __VA_ARGS__ );                                  \
    fflush(NULL);                                                              \
  }                                                                        \
}

#define FSI_TRACE_COND_RC(rc, errVal, ... )                                \
{                                                                          \
  if ((errVal) == rc) {                                                    \
    FSI_TRACE(FSI_INFO, ## __VA_ARGS__);                                   \
  } else {                                                                 \
    FSI_TRACE(FSI_ERR, ## __VA_ARGS__);                                    \
  }                                                                        \
}

#define FSI_TRACE_HANDLE( handle)                                          \
{                                                                          \
  FSI_TRACE(FSI_NOTICE, "TODO implement trace handle here ");              \
}

#define NONIO_MSG_TYPE                                                     \
  ((g_multithreaded) ? (unsigned long)pthread_self() : g_client_pid )

#define WAIT_SHMEM_ATTACH()                                                \
{                                                                          \
  while (g_shm_at == 0) {                                                  \
    FSI_TRACE(FSI_INFO, "waiting for shmem attach");                       \
    sleep(1);                                                              \
  }                                                                        \
}

// FSI IPC includes
// #include "fsi_ipc_common.h"
// when compiled in syren look in the /.../ipc/client directory for this file
// in temporary ganesha environment this may have to be changed to look for it 
// in same directory as where fsi_ipc_ccl.h is placed
#include "../fsi_ipc_common.h"

#define FSAL_MAX_PATH_LEN PATH_MAX

extern int       g_shm_id;              // SHM ID
extern char    * g_shm_at;              // SHM Base Address
extern int       g_io_req_msgq;
extern int       g_io_rsp_msgq;
extern int       g_non_io_req_msgq;
extern int       g_non_io_rsp_msgq;
extern int       g_shmem_req_msgq;
extern int       g_shmem_rsp_msgq;
extern char      g_chdir_dirpath[PATH_MAX];
extern uint64_t  g_client_pid;
extern uint64_t  g_server_pid;
extern struct    file_handles_struct_t g_fsi_handles;        // FSI client handles
extern struct    dir_handles_struct_t  g_fsi_dir_handles;    // FSI client Dir handles
extern struct    acl_handles_struct_t  g_fsi_acl_handles;    // FSI client ACL handles
extern uint64_t  g_client_trans_id;  // FSI global transaction id
extern int       g_close_trace;      // FSI global trace of io rates at close
extern int       g_multithreaded;    // ganesha = true, samba = false

// from fsi_ipc_client_statistics.c
extern struct timeval            g_next_log_time;
extern struct timeval            g_curr_log_time;
extern struct timeval            g_last_log_time;
extern struct timeval            g_last_io_completed;
extern struct timeval            g_begin_io_idle_time;
extern struct ipc_client_stats_t g_client_bytes_read;
extern struct ipc_client_stats_t g_client_bytes_written;
extern struct ipc_client_stats_t g_ipc_vfs_xfr_time;    // usecs
extern struct ipc_client_stats_t g_client_io_wait_time;  // usecs
extern struct ipc_client_stats_t g_client_io_idle_time;  // usecs
extern uint64_t                  g_num_reads_in_progress;
extern uint64_t                  g_num_writes_in_progress;
extern uint64_t                  g_stat_log_interval;
extern char                      g_client_address[256]; // PTFSAL HACK

// BRUTAL HACK - must be fixed later - global non-io mutex
extern pthread_mutex_t g_non_io_mutex;
extern pthread_mutex_t g_dir_mutex; // dir handle mutex
extern pthread_mutex_t g_acl_mutex; // acl handle mutex
extern pthread_mutex_t g_handle_mutex; // file handle Processing mutex
extern pthread_mutex_t g_parseio_mutex; // only one thread can parse an io at a time
extern pthread_mutex_t g_transid_mutex; // only one thread can change global transid at a time


#define SAMBA_FSI_IPC_PARAM_NAME   "fsiparam"  // To designate as our parms
#define SAMBA_EXPORT_ID_PARAM_NAME "exportid"  // For ExportID
#define SAMBA_STATDELTA_PARAM_NAME "statdelta" // For Statistics Output 
#define MAX_FSI_PERF_COUNT         1000        // for m_perf_xxx counters

// enum for client buffer return code state
enum e_buf_rc_state {
  BUF_RC_STATE_UNKNOWN = 0,             // default
  BUF_RC_STATE_PENDING,                 // waiting on server Rc
  BUF_RC_STATE_RC_NOT_PROCESSED,        // received Rc, not processed by client
  BUF_RC_STATE_RC_PROCESSED             // client processed received Rc
};

enum e_fsi_name_enum {
  FSI_NAME_DEFAULT = 0,                 // default (normal file)
  FSI_NAME_DIR                          // name is a directory
};


// ----------------------------------------------------------------------------
/// @struct io_buf_status_t
/// @brief  contains I/O buffer status
// ----------------------------------------------------------------------------
struct io_buf_status_t {
  char    * m_p_shmem;                  // IPC shmem pointer
  int       m_this_io_op;               // enumerated I/O operation
                                        // (read/write/other I/O)
  int       m_buf_in_use;               // used to determine available buffers
                                        // a usable buffer is not in use
                                        // and not "not allocated"
  int       m_data_valid;               // set on read when data received
  int       m_bytes_in_buf;             // number of bytes of data in buffer
  int       m_buf_use_enum;             // BufUsexxx enumeration
  int       m_buf_rc_state;             // enum return code state BufRcXxx
  uint64_t  m_trans_id;                 // transaction id
};

// --------------------------------------------------------------------------
// file statistics structure
// --------------------------------------------------------------------------
// NOTE before 81020872 this use to be linux stat struct
// now it is defined to look very similiar but it does not
// align exactly like the linust struct stat, the field names
// are kept the same but it may impact ganesha protoype
//
// typedef struct stat fsi_stat_struct;
typedef struct fsi_stat_struct__ {
  uint64_t                st_dev;          // Device
  uint64_t                st_ino;          // File serial number
  uint64_t                st_mode;         // File mode
  uint64_t                st_nlink;        // Link count
  uint64_t                st_uid;          // User ID of the file's owner
  uint64_t                st_gid;          // Group ID of the file's group
  uint64_t                st_rdev;         // Device number, if device
  uint64_t                st_size;         // Size of file, in bytes
  uint64_t                st_atime_sec;        // Time of last access  sec only
  uint64_t                st_mtime_sec;        // Time of last modification  sec
  uint64_t                st_ctime_sec;        // Time of last change  sec
  //struct timespec         st_btime;        // Birthtime  not used
  uint64_t                st_blksize;      // Optimal block size for I/O
  uint64_t                st_blocks;       // Number of 512-byte blocks allocated
  struct PersistentHandle st_persistentHandle;
} fsi_stat_struct;


// ----------------------------------------------------------------------------
/// @struct file_handle_t
/// @brief  client file handle
// ----------------------------------------------------------------------------
struct file_handle_t {
  char                   m_filename[PATH_MAX];
                                              // full filename used with API
  int                    m_hndl_in_use;       // used to flag available entries
  int                    m_prev_io_op;        // enumerated I/O operation
                                              // (read/write/other I/O)
  struct io_buf_status_t m_writebuf_state[MAX_FSI_IPC_SHMEM_BUF_PER_STREAM *
                                          FSI_IPC_SHMEM_WRITEBUF_PER_BUF * 2];  // ganesha
                                              // one entry per write data buffer
  int                    m_writebuf_cnt;      // how many write buffers this
                                              // handle actually uses
  struct io_buf_status_t m_readbuf_state [MAX_FSI_IPC_SHMEM_BUF_PER_STREAM *
                                          FSI_IPC_SHMEM_READBUF_PER_BUF * 2]; // ganesha
                                              // one entry per read data buffer
  int                    m_readbuf_cnt;       // how many read buffers this
                                              // handle actually uses
  uint64_t               m_shm_handle[MAX_FSI_IPC_SHMEM_BUF_PER_STREAM];
                                              // SHM handle array
  int                    m_first_write_done;  // set if we are writing and first
                                              // write is complete
  int                    m_first_read_done;   // set if we completed first read
  int                    m_close_rsp_rcvd;    // IPC close file response received
  int                    m_read_at_eof;       // set if at EOF - only for read
  uint64_t               m_file_loc;          // used for writes and fstat
                                              // this is the location assuming
                                              // last read or write succeeded
                                              // this is the location the next
                                              // sequential write (not pwrite)
                                              // would use as an offset
  uint64_t               m_file_flags;        // flags
  fsi_stat_struct        m_stat;
  uint64_t               m_fs_handle;         // handle
  int                    m_deferred_io_rc;    // deferred io return code

  int                    m_dir_not_file_flag; // set if this handle represents a
                                              // directory instead of a file
                                              // (open must issue opendir
                                              // if the entity being opened
                                              // is a directory)
// FIX FIX FIX  SMB_STRUCT_DIR       * m_dirp;              // directory struct pointer (removed in ganesha) need to fix
  struct fsi_struct_dir_t * m_dirp; // PTFSAL VERSION
                                              // if m_dir_not_file_flag is
                                              // set
  uint64_t               m_resourceHandle;    // handle for resource management
  struct timeval         m_perf_pwrite_start[MAX_FSI_PERF_COUNT];
  struct timeval         m_perf_pwrite_end[MAX_FSI_PERF_COUNT];
  struct timeval         m_perf_aio_start[MAX_FSI_PERF_COUNT];
  struct timeval         m_perf_open_end;
  struct timeval         m_perf_close_end;
  uint64_t               m_perf_pwrite_count; // number of pwrite while open
  uint64_t               m_perf_pread_count;  // number of pread while open
  uint64_t               m_perf_aio_count;    // number of aio_force while open
  uint64_t               m_perf_fstat_count;  // number of fstat while open
};

// ----------------------------------------------------------------------------
/// @struct file_handles_struct_t
/// @brief  contains filehandles
// ----------------------------------------------------------------------------
struct file_handles_struct_t {
  struct file_handle_t m_handle[FSI_MAX_STREAMS + FSI_CIFS_RESERVED_STREAMS];
  int                  m_count;              // maximum handle used
};

// ----------------------------------------------------------------------------
/// @struct fsi_struct_dir_t
/// @brief  fsi unique directory information
// ----------------------------------------------------------------------------
struct fsi_struct_dir_t {
  uint64_t m_dir_handle_index;
  uint64_t m_last_ino;  // last inode we responded with
  char     dname[PATH_MAX];
};

// ----------------------------------------------------------------------------
/// @struct dir_handle_t
/// @brief  directory handle
// ----------------------------------------------------------------------------
struct dir_handle_t {
  int                     m_dir_handle_in_use; // used to flag available entries
  uint64_t                m_fs_dir_handle;     // fsi_facade handle
  struct fsi_struct_dir_t m_fsi_struct_dir;    // directory struct
  uint64_t                m_resourceHandle;    // server resource handle
};

// ----------------------------------------------------------------------------
/// @struct dir_handles_struct_t
/// @brief  contains directory handles
// ----------------------------------------------------------------------------
struct dir_handles_struct_t {
  struct dir_handle_t m_dir_handle[FSI_MAX_STREAMS];
  int                 m_count;
};


// ----------------------------------------------------------------------------
/// @struct acl_handle_t
/// @brief  ACL handle
// ----------------------------------------------------------------------------
struct acl_handle_t {
  int        m_acl_handle_in_use; // used to flag available entries
  uint64_t   m_acl_handle;        // acl handle             
  uint64_t   m_resourceHandle;    // server resource handle
};

// ----------------------------------------------------------------------------
/// @struct acl_handles_struct_t
/// @brief  contains ACL handles
// ----------------------------------------------------------------------------
struct acl_handles_struct_t {
  struct acl_handle_t m_acl_handle[FSI_MAX_STREAMS];
  int                 m_count;
};

// ----------------------------------------------------------------------------
// Structures for Ganesha use
// ----------------------------------------------------------------------------

// The context every call to CCL is made in
typedef struct {
  uint64_t  export_id;    // export id
  uint64_t  uid;          // user id of the connecting user
  uint64_t  gid;          // group id of the connecting user
  char  client_address[256]; // address of client

  //TODO check on if the next fiels are used by fsal or ccl
  // next 2 fields left over from prototype -
  // do not use these if not already using
  const char * param;     // incoming parameter
  int   handle_index;     // Samba's File descriptor fsp->fh->fd
                          //  or essentially or index into our
                          // global g_fsi_handles.m_handle[] array

} fsi_handle_struct;

#define uint32 uint32_t

// FSI IPC Statistics definitions
// ----------------------------------------------------------------------------
/// @struct ipc_client_stats_t
/// @brief  contains client statistics structure
// ----------------------------------------------------------------------------

// Statistics Logging interval of 5 minutes
#ifndef UNIT_TEST
#define FSI_IPC_CLIENT_STATS_LOG_INTERVAL 60 * 5
#else // UNIT_TEST
#define FSI_IPC_CLIENT_STATS_LOG_INTERVAL 2
#endif // UNIT_TEST

#define FSI_RETURN(x)          \
{                              \
  return x;                    \
}

struct ipc_client_stats_t {
  uint64_t count;
  uint64_t sum;
  uint64_t sumsq;
  uint64_t min;
  uint64_t max;
  uint64_t overflow_flag;
};

#define VARIANCE(pstat)                                                 \
  ( (pstat)->count > 1  ?                                               \
    (((pstat)->sumsq - (pstat)->sum * ((pstat)->sum /(pstat)->count)) / \
      ((pstat)->count - 1)) :                                           \
    0 )

// ----------------------------------------------------------------------------
// Defines for IO Idle time statistic collection,  this is time we are 
// idle waiting for user to send a read or write or doing other operations
// ----------------------------------------------------------------------------

#define START_IO_IDLE_CLOCK()                                                 \
{                                                                             \
  if (g_begin_io_idle_time.tv_sec != 0) {                                     \
    FSI_TRACE(FSI_ERR, "IDLE CLOCK was already started, distrust idle stat"); \
  }                                                                           \
  int rc = gettimeofday(&g_begin_io_idle_time, NULL);                         \
  if (rc != 0) {                                                              \
    FSI_TRACE(FSI_ERR, "gettimeofday rc = %d", rc);                           \
  }                                                                           \
}

#define END_IO_IDLE_CLOCK()                                                   \
{                                                                             \
  struct timeval curr_time;                                                   \
  struct timeval diff_time;                                                   \
  if (g_begin_io_idle_time.tv_sec == 0) {                                     \
    FSI_TRACE(FSI_ERR, "IDLE CLOCK already not running, distrust idle stat"); \
  }                                                                           \
  int rc = gettimeofday(&curr_time, NULL);                                    \
  if (rc != 0) {                                                              \
    FSI_TRACE(FSI_ERR, "gettimeofday rc = %d", rc);                           \
  } else {                                                                    \
    timersub(&curr_time, &g_begin_io_idle_time, &diff_time);                  \
    uint64_t delay = diff_time.tv_sec * 1000000 + diff_time.tv_usec;          \
    if (update_stats(&g_client_io_idle_time, delay)) {                        \
      FSI_TRACE(FSI_ERR, "IO Idle time stats sum square overflow");           \
    }                                                                         \
  }                                                                           \
  memset(&g_begin_io_idle_time, 0, sizeof(g_begin_io_idle_time));             \
}

#define IDLE_STAT_READ_START()                                                \
{                                                                             \
  g_num_reads_in_progress++;                                                  \
  if (((g_num_reads_in_progress + g_num_writes_in_progress) == 1) &&          \
      (g_begin_io_idle_time.tv_sec != 0)) {                                   \
    END_IO_IDLE_CLOCK();                                                      \
  }                                                                           \
}

#define IDLE_STAT_READ_END()                                                  \
{                                                                             \
  if (g_num_reads_in_progress == 0) {                                         \
    FSI_TRACE(FSI_ERR, "IO Idle read count off, distrust IDLE stat ");        \
  }                                                                           \
  g_num_reads_in_progress--;                                                  \
  if ((g_num_reads_in_progress + g_num_writes_in_progress) == 0) {            \
    START_IO_IDLE_CLOCK();                                                    \
  }                                                                           \
}

#define IDLE_STAT_WRITE_START()                                               \
{                                                                             \
  g_num_writes_in_progress++;                                                 \
  if (((g_num_reads_in_progress + g_num_writes_in_progress) == 1) &&          \
      (g_begin_io_idle_time.tv_sec != 0)) {                                   \
    END_IO_IDLE_CLOCK();                                                      \
  }                                                                           \
}

#define IDLE_STAT_WRITE_END()                                                 \
{                                                                             \
  if (g_num_writes_in_progress == 0) {                                        \
    FSI_TRACE(FSI_DEBUG, "IO Idle write count off, distrust IDLE stat ");       \
  }                                                                           \
  g_num_writes_in_progress--;                                                 \
  if ((g_num_reads_in_progress + g_num_writes_in_progress) == 0) {            \
    START_IO_IDLE_CLOCK();                                                    \
  }                                                                           \
}

int skel_open(fsi_handle_struct   * handle,
              char                  * path,
              int                   flags,
              mode_t                mode);

int skel_close(fsi_handle_struct * handle,
               int handle_index);

int add_acl_handle(uint64_t fs_acl_handle);
int add_dir_handle(uint64_t fs_dir_handle);
int add_fsi_handle(struct file_handle_t * p_new_handle);
void convert_fsi_name(fsi_handle_struct   * handle,
                      const char          * filename,
                      char                * sv_filename,
                      enum e_fsi_name_enum  fsi_name_type);
int delete_acl_handle(uint64_t aclHandle);
int delete_dir_handle(int dir_handle_index);
int delete_fsi_handle(int handle_index);
int ccl_cache_name_and_handle(char *handle, char *name);
int ccl_check_handle_index (int handle_index);
int ccl_find_handle_by_name(const char * filename);
int ccl_find_dir_handle_by_name(const char * filename);
int ccl_get_name_from_handle(char *handle, char *name);
int ccl_stat(fsi_handle_struct * handle,
             const char        * filename,
             fsi_stat_struct       * sbuf);
uint64_t get_acl_resource_handle(uint64_t aclHandle);
uint64_t get_export_id();
int have_pending_io_response(int handle_index);
int io_msgid_from_index (int index);
void ld_common_msghdr(struct CommonMsgHdr * p_msg_hdr,
                      uint64_t             transaction_type,
                      uint64_t             data_length,
                      uint64_t             export_id,
                      int            handle_index,
                      int            fs_handle,
                      int            use_crc,
                      int            is_IO_q);
void ld_uid_gid(uint64_t                * uid,
                uint64_t                * gid,
                fsi_handle_struct   * handle);
void load_shmem_hdr(struct CommonShmemDataHdr * p_shmem_hdr,
                    uint64_t             transaction_type,
                    uint64_t             data_length,
                    uint64_t             offset,
                    int                  handle_index,
                    uint64_t             transaction_id,
                    int                  use_crc);
int rcv_msg_nowait(int     msg_id,
                   void  * p_msg_buf,
                   size_t  msg_size,
                   long    msg_type,
                   int   * p_msg_error_code);
int rcv_msg_wait(int     msg_id,
                 void  * p_msg_buf,
                 size_t  msg_size,
                 long    msg_type,
                 int   * p_msg_error_code);
int send_msg(int          msg_id,
             const void * p_msg_buf,
             size_t       msg_size,
             int        * p_msg_error_code);
int ccl_chmod(fsi_handle_struct * handle,
               const char        * path,
               mode_t              mode);
int ccl_unlink(fsi_handle_struct  * handle,
                char               * path);
int ccl_rename(fsi_handle_struct * handle,
               const char        * old_name,
               const char        * new_name);
int ccl_opendir(fsi_handle_struct * handle,
                 const char        * filename,
                 const char        * mask,
                 uint32              attr);
int ccl_closedir(fsi_handle_struct * handle,
                  struct fsi_struct_dir_t * dirp);
int ccl_readdir(fsi_handle_struct * handle,
                 struct fsi_struct_dir_t * dirp,
                 fsi_stat_struct   * sbuf);
int ccl_fsync(fsi_handle_struct * handle,
               int handle_index);
int ccl_ftruncate(fsi_handle_struct * handle,
                   int handle_index,
                   uint64_t           offset);
ssize_t ccl_pread(fsi_handle_struct * handle,
                   void              * data,
                   size_t              n,
                   uint64_t            offset);
ssize_t ccl_pwrite(fsi_handle_struct * handle,
                    int handle_index,
                    const void        * data,
                    size_t              n,
                    uint64_t           offset);
int ccl_open(fsi_handle_struct   * handle,
              char                  * path,
              int                   flags,
              mode_t                mode);
int ccl_close(fsi_handle_struct * handle,
               int handle_index);
int merge_errno_rc(int rc_a,
                   int rc_b);
int get_all_io_responses(int     handle_index,
                         int   * combined_rc,
                         struct msg_t* p_msg);
int get_any_io_responses(int     handle_index,
                         int   * p_combined_rc,
                         struct msg_t* p_msg);
void issue_read_ahead(struct file_handle_t * p_pread_hndl,
                      int                    handle_index,
                      uint64_t               offset,
                      struct msg_t                * p_msg,
                      struct CommonMsgHdr         * p_pread_hdr,
                      struct ClientOpPreadReqMsg  * p_pread_req);
void load_deferred_io_rc(int handle_index,
                         int cur_error);
int parse_io_response(int     handle_index,
                      struct msg_t * p_msg);
int read_existing_data(struct file_handle_t * p_pread_hndl,
                       char                 * p_data,
                       uint64_t             * p_cur_offset,
                       uint64_t             * p_cur_length,
                       int                  * p_pread_rc,
                       int                  * p_pread_incomplete,
                       int                    handle_index);
int update_read_status(struct file_handle_t * p_pread_hndl,
                       int                    handle_index,
                       uint64_t               cur_offset,
                       char                 * data,
                       uint64_t               cur_length,
                       struct msg_t                * p_msg,
                       struct CommonMsgHdr         * p_pread_hdr,
                       struct ClientOpPreadReqMsg  * p_pread_req);
int verify_io_response(int                      transaction_type,
                       int                      cur_index,
                       struct CommonMsgHdr           * p_msg_hdr,
                       struct CommonShmemDataHdr     * p_shmem_hdr,
                       struct io_buf_status_t * p_io_buf_status);
int wait_free_write_buf(int     handle_index,
                        int   * p_combined_rc,
                        struct msg_t* p_msg);
void ccl_ipc_stats_init();
void ccl_ipc_stats_logger();
void ccl_ipc_stats_on_io_complete(struct timeval * done_time);
void ccl_ipc_stats_on_io_start(uint64_t delay);
void ccl_ipc_stats_on_read(uint64_t bytes);
void ccl_ipc_stats_on_write(uint64_t bytes);
uint64_t update_stats(struct ipc_client_stats_t * stat, uint64_t value);

// Prototypes - new for Ganesha 
int ccl_name_to_handle(fsi_handle_struct *pvfs_handle,
                       char *path, 
                       struct PersistentHandle *phandle);
int ccl_handle_to_name(fsi_handle_struct *pvfs_handle,
                       struct PersistentHandle *phandle,
                       char *path);
int ccl_dynamic_fsinfo(fsi_handle_struct *pvfs_handle,
                       char *path,
                       struct ClientOpDynamicFsInfoRspMsg *pfs_info);
int ccl_readlink(fsi_handle_struct *pvfs_handle,
                 char *path,
                 char *link_content);
int ccl_symlink(fsi_handle_struct *pvfs_handle,
                char *path,
                char *link_content);

#endif // ifndef __FSI_IPC_CLIENT_H__
