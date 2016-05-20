/*
 * QEMU System Emulator block driver
 *
 * Copyright (c) 2003 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * [ A description of the changes introduced in this file]
 *
 * Copyright Rice University 2014
 *
 * Authors:
 *   Jie Zheng <zhengjie20009@rice.edu>
 *   T. S. Eugene Ng <eugeneng@rice.edu>
 *   Kunwadee Sripanidkulchai <kunwadee@gmail.com>
 *   Zhaolei Liu <zl10@rice.edu>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.    
 */
#ifndef BLOCK_INT_H
#define BLOCK_INT_H

#include "block.h"
#include "qemu-option.h"
#include "qemu-queue.h"
#include "qemu-timer.h"

#define BLOCK_FLAG_ENCRYPT	1
#define BLOCK_FLAG_COMPRESS	2
#define BLOCK_FLAG_COMPAT6	4

#define BLOCK_OPT_SIZE          "size"
#define BLOCK_OPT_ENCRYPT       "encryption"
#define BLOCK_OPT_COMPAT6       "compat6"
#define BLOCK_OPT_BACKING_FILE  "backing_file"
#define BLOCK_OPT_BACKING_FMT   "backing_fmt"
#define BLOCK_OPT_CLUSTER_SIZE  "cluster_size"
#define BLOCK_OPT_PREALLOC      "preallocation"

/* Pacer constants */
#define THROTTLE_DELAY 2048  //microsecond
#define BDRV_SECTORS_PER_BULK_CHUNK 2048 
#define BDRV_SECTORS_PER_BULK_CHUNK_BITS 11  
/* End */

typedef struct AIOPool {
    void (*cancel)(BlockDriverAIOCB *acb);
    int aiocb_size;
    BlockDriverAIOCB *free_aiocb;
} AIOPool;

struct BlockDriver {
    const char *format_name;
    int instance_size;
    int (*bdrv_probe)(const uint8_t *buf, int buf_size, const char *filename);
    int (*bdrv_probe_device)(const char *filename);
    int (*bdrv_open)(BlockDriverState *bs, int flags);
    int (*bdrv_file_open)(BlockDriverState *bs, const char *filename, int flags);
    int (*bdrv_read)(BlockDriverState *bs, int64_t sector_num,
                     uint8_t *buf, int nb_sectors);
    int (*bdrv_write)(BlockDriverState *bs, int64_t sector_num,
                      const uint8_t *buf, int nb_sectors);
    void (*bdrv_close)(BlockDriverState *bs);
    int (*bdrv_create)(const char *filename, QEMUOptionParameter *options);
    void (*bdrv_flush)(BlockDriverState *bs);
    int (*bdrv_is_allocated)(BlockDriverState *bs, int64_t sector_num,
                             int nb_sectors, int *pnum);
    int (*bdrv_set_key)(BlockDriverState *bs, const char *key);
    int (*bdrv_make_empty)(BlockDriverState *bs);
    /* aio */
    BlockDriverAIOCB *(*bdrv_aio_readv)(BlockDriverState *bs,
        int64_t sector_num, QEMUIOVector *qiov, int nb_sectors,
        BlockDriverCompletionFunc *cb, void *opaque);
    BlockDriverAIOCB *(*my_bdrv_aio_readv)(BlockDriverState *bs,
        int64_t sector_num, QEMUIOVector *qiov, int nb_sectors,
        BlockDriverCompletionFunc *cb, void *opaque);    // Pacer: add for latency measurement
    BlockDriverAIOCB *(*bdrv_aio_writev)(BlockDriverState *bs,
        int64_t sector_num, QEMUIOVector *qiov, int nb_sectors,
        BlockDriverCompletionFunc *cb, void *opaque);
    BlockDriverAIOCB *(*bdrv_aio_flush)(BlockDriverState *bs,
        BlockDriverCompletionFunc *cb, void *opaque);

    int (*bdrv_aio_multiwrite)(BlockDriverState *bs, BlockRequest *reqs,
        int num_reqs);
    int (*bdrv_merge_requests)(BlockDriverState *bs, BlockRequest* a,
        BlockRequest *b);


    const char *protocol_name;
    int (*bdrv_truncate)(BlockDriverState *bs, int64_t offset);
    int64_t (*bdrv_getlength)(BlockDriverState *bs);
    int64_t (*bdrv_get_totallength)(void); //Pacer: added for adaptive system
    
    int (*bdrv_write_compressed)(BlockDriverState *bs, int64_t sector_num,
                                 const uint8_t *buf, int nb_sectors);

    int (*bdrv_snapshot_create)(BlockDriverState *bs,
                                QEMUSnapshotInfo *sn_info);
    int (*bdrv_snapshot_goto)(BlockDriverState *bs,
                              const char *snapshot_id);
    int (*bdrv_snapshot_delete)(BlockDriverState *bs, const char *snapshot_id);
    int (*bdrv_snapshot_list)(BlockDriverState *bs,
                              QEMUSnapshotInfo **psn_info);
    int (*bdrv_get_info)(BlockDriverState *bs, BlockDriverInfo *bdi);

    int (*bdrv_save_vmstate)(BlockDriverState *bs, const uint8_t *buf,
                             int64_t pos, int size);
    int (*bdrv_load_vmstate)(BlockDriverState *bs, uint8_t *buf,
                             int64_t pos, int size);

    int (*bdrv_change_backing_file)(BlockDriverState *bs,
        const char *backing_file, const char *backing_fmt);

    /* removable device specific */
    int (*bdrv_is_inserted)(BlockDriverState *bs);
    int (*bdrv_media_changed)(BlockDriverState *bs);
    int (*bdrv_eject)(BlockDriverState *bs, int eject_flag);
    int (*bdrv_set_locked)(BlockDriverState *bs, int locked);

    /* to control generic scsi devices */
    int (*bdrv_ioctl)(BlockDriverState *bs, unsigned long int req, void *buf);
    BlockDriverAIOCB *(*bdrv_aio_ioctl)(BlockDriverState *bs,
        unsigned long int req, void *buf,
        BlockDriverCompletionFunc *cb, void *opaque);

    /* List of options for creating images, terminated by name == NULL */
    QEMUOptionParameter *create_options;


    /* Returns number of errors in image, -errno for internal errors */
    int (*bdrv_check)(BlockDriverState* bs);

    void (*bdrv_debug_event)(BlockDriverState *bs, BlkDebugEvent event);

    /* Set if newly created images are not guaranteed to contain only zeros */
    int no_zero_init;

    QLIST_ENTRY(BlockDriver) list;
};

/* Pacer additional structs */
#define WHISTORYSIZE 20000
typedef struct HistoryItem {
	int operation; // 0 read 1 write
	int64_t sector_offset;
	int64_t access_length;
	QSIMPLEQ_ENTRY(HistoryItem) entry;
} HistoryItem;

typedef struct WriteHistoryItem {
	struct WriteHistoryItem *previousWHItem;
	int id;
	int freq;
	struct WriteFreqItem *myFreqItem;
	struct WriteHistoryItem *nextWHItem;
} WriteHistoryItem;

typedef struct WriteFreqItem {
	struct WriteFreqItem *previousFreqItem;
	int freq;
	struct WriteHistoryItem *head;
	struct WriteFreqItem *nextFreqItem;
} WriteFreqItem;

typedef struct WriteBitmap {
	int chunksize[10];
	int chunksize_bit[10];
	int totalchunk[10];
	char *bitmap1[10];
	int bp1accesschunk[10];
	float bp1storagerate[10];
	char *bitmap2;
	int bp2accesschunk;
	int bp2coverchunk[10];
	float bp2coverage[10];
} WriteBitmap;

typedef struct ThrottleLimit {
	int delta;
	int64_t time;
	int64_t nexttime;
} ThrottleLimit;

typedef struct WriteopInfo{
	int64_t time;
       int64_t sector_num;
       int length;
       int delta;
       int usleep;
       float ave_dirtyrate;
} WriteopInfo;

typedef struct OneSetWriteOps{
       struct WriteopInfo* oneset;
       int index;	
       struct OneSetWriteOps* nextset;		
} OneSetWriteOps;
/* End */


struct BlockDriverState {
    int64_t total_sectors; /* if we are reading a disk image, give its
                              size in sectors */
    int read_only; /* if true, the media is read only */
    int keep_read_only; /* if true, the media was requested to stay read only */
    int open_flags; /* flags used to open the file, re-used for re-open */
    int removable; /* if true, the media can be removed */
    int locked;    /* if true, the media cannot temporarily be ejected */
    int encrypted; /* if true, the media is encrypted */
    int valid_key; /* if true, a valid encryption key has been set */
    int sg;        /* if true, the device is a /dev/sg* */
    /* event callback when inserting/removing */
    void (*change_cb)(void *opaque);
    void *change_opaque;

    BlockDriver *drv; /* NULL means no media */
    void *opaque;

    char filename[1024];
    char backing_file[1024]; /* if non zero, the image is a diff of
                                this file image */
    char backing_format[16]; /* if non-zero and backing_file exists */
    int is_temporary;
    int media_changed;

    BlockDriverState *backing_hd;
    BlockDriverState *file;

    /* async read/write emulation */

    void *sync_aiocb;

    /* I/O stats (display with "info blockstats"). */
    uint64_t rd_bytes;
    uint64_t wr_bytes;
    uint64_t rd_ops;
    uint64_t wr_ops;
    uint64_t wr_highest_sector;

	/* Pacer variables */
	QSIMPLEQ_HEAD(history_list, HistoryItem) history_list;
	int enable_history_tracking;
	int history_size;

	int enable_throttle;
	int throttle_delay;
	
 	int enable_dirty_scheduling;
	int chunksize0;
	int chunksize0_bit;	
	int writehistory_size0;
	WriteHistoryItem* writehistory0;
	WriteFreqItem* writefreqHead0;
	WriteFreqItem* writefreqTail0;
	
	int chunksize1;	
	int chunksize1_bit;	
	int writehistory_size1;
	WriteHistoryItem* writehistory1;
	WriteFreqItem* writefreqHead1;
	WriteFreqItem* writefreqTail1;

	int current_chunksize;	
	int current_chunksize_bit;	
	int current_writehistory_size;
	WriteHistoryItem* current_writehistory;
	WriteFreqItem* current_writefreqHead;
	WriteFreqItem* current_writefreqTail;
	
	WriteBitmap current_bmap;
	int history_active_id;
	int writeop_counter;

	int dirty_chunksize;	
	int dirty_chunksize_bit;	
	int dirty_writehistory_size;
	WriteHistoryItem* dirty_writehistory;
	WriteFreqItem* dirty_writefreqHead;
	WriteFreqItem* dirty_writefreqTail;
	
    int64_t throttle_dirtyrate; 
	ThrottleLimit throlimit;	  
    int64_t tempusleep;
	int64_t timestamp_write;
    int migration;
	int64_t zeroop;
    int64_t totalopafterfcopy;
	OneSetWriteOps* writeopsetsHead;
	/* End */

    /* Whether the disk can expand beyond total_sectors */
    int growable;

    /* the memory alignment required for the buffers handled by this driver */
    int buffer_alignment;

    /* do we need to tell the quest if we have a volatile write cache? */
    int enable_write_cache;

   /* NOTE: the following infos are only hints for real hardware
       drivers. They are not used by the block driver */
    int cyls, heads, secs, translation;
    int type;
    BlockErrorAction on_read_error, on_write_error;
    char device_name[32];
    unsigned long *dirty_bitmap;

    int64_t dirty_count;

    /* Pacer variables */
    uint64_t *write_block_laccesstime; // for adaptive system
    uint64_t *write_block_distance;  // for adaptive system
    uint64_t *write_block_count;  // adaptive system
    uint64_t *write_block_variance; // for adaptive system
    uint64_t totalblock;  // adaptive system
    uint64_t *write_block_avedist; // adaptive system
    uint8_t *write_block_markmap; // adaptive system
    uint64_t *write_block_dirty_accesstime; // adaptive system, prediction, discrete solution
	/* End */

    QTAILQ_ENTRY(BlockDriverState) list;
    void *private;
};

struct BlockDriverAIOCB {
    AIOPool *pool;
    BlockDriverState *bs;
    BlockDriverCompletionFunc *cb;
    void *opaque;
    BlockDriverAIOCB *next;
};

void get_tmp_filename(char *filename, int size);

void *qemu_aio_get(AIOPool *pool, BlockDriverState *bs,
                   BlockDriverCompletionFunc *cb, void *opaque);
void qemu_aio_release(void *p);

void *qemu_blockalign(BlockDriverState *bs, size_t size);

#ifdef _WIN32
int is_windows_drive(const char *filename);
#endif

struct DriveInfo;

typedef struct BlockConf {
    struct DriveInfo *dinfo;
    uint16_t physical_block_size;
    uint16_t logical_block_size;
    uint16_t min_io_size;
    uint32_t opt_io_size;
} BlockConf;

static inline unsigned int get_physical_block_exp(BlockConf *conf)
{
    unsigned int exp = 0, size;

    for (size = conf->physical_block_size; size > 512; size >>= 1) {
        exp++;
    }

    return exp;
}

#define DEFINE_BLOCK_PROPERTIES(_state, _conf)                          \
    DEFINE_PROP_DRIVE("drive", _state, _conf.dinfo),                    \
    DEFINE_PROP_UINT16("logical_block_size", _state,                    \
                       _conf.logical_block_size, 512),                  \
    DEFINE_PROP_UINT16("physical_block_size", _state,                   \
                       _conf.physical_block_size, 512),                 \
    DEFINE_PROP_UINT16("min_io_size", _state, _conf.min_io_size, 512),  \
    DEFINE_PROP_UINT32("opt_io_size", _state, _conf.opt_io_size, 512)

#endif /* BLOCK_INT_H */
