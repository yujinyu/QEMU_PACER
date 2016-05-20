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

#include "config-host.h"
#include "qemu-common.h"
#include "monitor.h"
#include "block_int.h"
#include "module.h"
#include "qemu-objects.h"
#include "qemu-timer.h"
#include "time.h"
#include <stdio.h>
#include <math.h>

#include <unistd.h> // used by Pacer for usleep()

#ifdef CONFIG_BSD
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/queue.h>
#ifndef __DragonFly__
#include <sys/disk.h>
#endif
#endif

#ifdef _WIN32
#include <windows.h>
#endif

static BlockDriverAIOCB *bdrv_aio_readv_em(BlockDriverState *bs,
        int64_t sector_num, QEMUIOVector *qiov, int nb_sectors,
        BlockDriverCompletionFunc *cb, void *opaque);
static BlockDriverAIOCB *bdrv_aio_writev_em(BlockDriverState *bs,
        int64_t sector_num, QEMUIOVector *qiov, int nb_sectors,
        BlockDriverCompletionFunc *cb, void *opaque);
static BlockDriverAIOCB *bdrv_aio_flush_em(BlockDriverState *bs,
        BlockDriverCompletionFunc *cb, void *opaque);
static BlockDriverAIOCB *bdrv_aio_noop_em(BlockDriverState *bs,
        BlockDriverCompletionFunc *cb, void *opaque);
static int bdrv_read_em(BlockDriverState *bs, int64_t sector_num,
                        uint8_t *buf, int nb_sectors);
static int bdrv_write_em(BlockDriverState *bs, int64_t sector_num,
                         const uint8_t *buf, int nb_sectors);

static QTAILQ_HEAD(, BlockDriverState) bdrv_states =
    QTAILQ_HEAD_INITIALIZER(bdrv_states);

static QLIST_HEAD(, BlockDriver) bdrv_drivers =
    QLIST_HEAD_INITIALIZER(bdrv_drivers);

/* If non-zero, use only whitelisted block drivers */
static int use_bdrv_whitelist;

/* Pacer indicators */
static int enable_throttling=0;
static int64_t throttle_usleep=0;
static int64_t throttle_starttime=0;
static int throttle_mechanism=1; //0: soft throttle 1: aggr throttl
static float ave_dirtyrate=0;
static int64_t throttle_dirtyrate=0;
static double prediction_dirtyrate=0;
/* End */

int path_is_absolute(const char *path)
{
    const char *p;
#ifdef _WIN32
    /* specific case for names like: "\\.\d:" */
    if (*path == '/' || *path == '\\')
        return 1;
#endif
    p = strchr(path, ':');
    if (p)
        p++;
    else
        p = path;
#ifdef _WIN32
    return (*p == '/' || *p == '\\');
#else
    return (*p == '/');
#endif
}

/* if filename is absolute, just copy it to dest. Otherwise, build a
   path to it by considering it is relative to base_path. URL are
   supported. */
void path_combine(char *dest, int dest_size,
                  const char *base_path,
                  const char *filename)
{
    const char *p, *p1;
    int len;

    if (dest_size <= 0)
        return;
    if (path_is_absolute(filename)) {
        pstrcpy(dest, dest_size, filename);
    } else {
        p = strchr(base_path, ':');
        if (p)
            p++;
        else
            p = base_path;
        p1 = strrchr(base_path, '/');
#ifdef _WIN32
        {
            const char *p2;
            p2 = strrchr(base_path, '\\');
            if (!p1 || p2 > p1)
                p1 = p2;
        }
#endif
        if (p1)
            p1++;
        else
            p1 = base_path;
        if (p1 > p)
            p = p1;
        len = p - base_path;
        if (len > dest_size - 1)
            len = dest_size - 1;
        memcpy(dest, base_path, len);
        dest[len] = '\0';
        pstrcat(dest, dest_size, filename);
    }
}

void bdrv_register(BlockDriver *bdrv)
{
    //printf("bdrv_register\n");
    if (!bdrv->bdrv_aio_readv) {
        /* add AIO emulation layer */
        bdrv->bdrv_aio_readv = bdrv_aio_readv_em;
        bdrv->bdrv_aio_writev = bdrv_aio_writev_em;
    } else if (!bdrv->bdrv_read) {
        /* add synchronous IO emulation layer */
        bdrv->bdrv_read = bdrv_read_em;
        bdrv->bdrv_write = bdrv_write_em;
    }

    if (!bdrv->bdrv_aio_flush)
        bdrv->bdrv_aio_flush = bdrv_aio_flush_em;

    QLIST_INSERT_HEAD(&bdrv_drivers, bdrv, list);
}

/* create a new block device (by default it is empty) */
BlockDriverState *bdrv_new(const char *device_name)
{
    BlockDriverState *bs;
//	fflush(stdout);
    bs = qemu_mallocz(sizeof(BlockDriverState));
    pstrcpy(bs->device_name, sizeof(bs->device_name), device_name);
    if (device_name[0] != '\0') {
        QTAILQ_INSERT_TAIL(&bdrv_states, bs, list);
    }
    return bs;
}

BlockDriver *bdrv_find_format(const char *format_name)
{
    BlockDriver *drv1;
//	fflush(stdout);
    QLIST_FOREACH(drv1, &bdrv_drivers, list) {
        if (!strcmp(drv1->format_name, format_name)) {
            return drv1;
        }
    }
    return NULL;
}

static int bdrv_is_whitelisted(BlockDriver *drv)
{
    static const char *whitelist[] = {
        CONFIG_BDRV_WHITELIST
    };
    const char **p;

    if (!whitelist[0])
        return 1;               /* no whitelist, anything goes */

    for (p = whitelist; *p; p++) {
        if (!strcmp(drv->format_name, *p)) {
            return 1;
        }
    }
    return 0;
}

BlockDriver *bdrv_find_whitelisted_format(const char *format_name)
{
    BlockDriver *drv = bdrv_find_format(format_name);
    return drv && bdrv_is_whitelisted(drv) ? drv : NULL;
}

int bdrv_create(BlockDriver *drv, const char* filename,
    QEMUOptionParameter *options)
{
    if (!drv->bdrv_create)
        return -ENOTSUP;

    return drv->bdrv_create(filename, options);
}

int bdrv_create_file(const char* filename, QEMUOptionParameter *options)
{
    BlockDriver *drv;

    drv = bdrv_find_protocol(filename);
    if (drv == NULL) {
        drv = bdrv_find_format("file");
    }

    return bdrv_create(drv, filename, options);
}

#ifdef _WIN32
void get_tmp_filename(char *filename, int size)
{
    char temp_dir[MAX_PATH];

    GetTempPath(MAX_PATH, temp_dir);
    GetTempFileName(temp_dir, "qem", 0, filename);
}
#else
void get_tmp_filename(char *filename, int size)
{
    int fd;
    const char *tmpdir;
    /* XXX: race condition possible */
    tmpdir = getenv("TMPDIR");
    if (!tmpdir)
        tmpdir = "/tmp";
    snprintf(filename, size, "%s/vl.XXXXXX", tmpdir);
    fd = mkstemp(filename);
    close(fd);
}
#endif

#ifdef _WIN32
static int is_windows_drive_prefix(const char *filename)
{
    return (((filename[0] >= 'a' && filename[0] <= 'z') ||
             (filename[0] >= 'A' && filename[0] <= 'Z')) &&
            filename[1] == ':');
}

int is_windows_drive(const char *filename)
{
    if (is_windows_drive_prefix(filename) &&
        filename[2] == '\0')
        return 1;
    if (strstart(filename, "\\\\.\\", NULL) ||
        strstart(filename, "//./", NULL))
        return 1;
    return 0;
}
#endif

/*
 * Detect host devices. By convention, /dev/cdrom[N] is always
 * recognized as a host CDROM.
 */
static BlockDriver *find_hdev_driver(const char *filename)
{
    int score_max = 0, score;
    BlockDriver *drv = NULL, *d;

    QLIST_FOREACH(d, &bdrv_drivers, list) {
        if (d->bdrv_probe_device) {
            score = d->bdrv_probe_device(filename);
            if (score > score_max) {
                score_max = score;
                drv = d;
            }
        }
    }

    return drv;
}

BlockDriver *bdrv_find_protocol(const char *filename)
{
    BlockDriver *drv1;
    char protocol[128];
    int len;
    const char *p;
    int is_drive;

    /* TODO Drivers without bdrv_file_open must be specified explicitly */

#ifdef _WIN32
    is_drive = is_windows_drive(filename) ||
        is_windows_drive_prefix(filename);
#else
    is_drive = 0;
#endif
    p = strchr(filename, ':');
    if (!p || is_drive) {
        drv1 = find_hdev_driver(filename);
        if (!drv1) {
            drv1 = bdrv_find_format("file");
        }
        return drv1;
    }
    len = p - filename;
    if (len > sizeof(protocol) - 1)
        len = sizeof(protocol) - 1;
    memcpy(protocol, filename, len);
    protocol[len] = '\0';
    QLIST_FOREACH(drv1, &bdrv_drivers, list) {
        if (drv1->protocol_name &&
            !strcmp(drv1->protocol_name, protocol)) {
            return drv1;
        }
    }
    return NULL;
}

static BlockDriver *find_image_format(const char *filename)
{
    int ret, score, score_max;
    BlockDriver *drv1, *drv;
    uint8_t buf[2048];
    BlockDriverState *bs;

    ret = bdrv_file_open(&bs, filename, 0);
    if (ret < 0)
        return NULL;

    /* Return the raw BlockDriver * to scsi-generic devices or empty drives */
    if (bs->sg || !bdrv_is_inserted(bs)) {
        bdrv_delete(bs);
        return bdrv_find_format("raw");
    }

    ret = bdrv_pread(bs, 0, buf, sizeof(buf));
    bdrv_delete(bs);
    if (ret < 0) {
        return NULL;
    }

    score_max = 0;
    drv = NULL;
    QLIST_FOREACH(drv1, &bdrv_drivers, list) {
        if (drv1->bdrv_probe) {
            score = drv1->bdrv_probe(buf, ret, filename);
            if (score > score_max) {
                score_max = score;
                drv = drv1;
            }
        }
    }
    return drv;
}

/**
 * Set the current 'total_sectors' value
 */
static int refresh_total_sectors(BlockDriverState *bs, int64_t hint)
{
    BlockDriver *drv = bs->drv;

    /* Do not attempt drv->bdrv_getlength() on scsi-generic devices */
    if (bs->sg)
        return 0;

    /* query actual device if possible, otherwise just trust the hint */
    if (drv->bdrv_getlength) {
        int64_t length = drv->bdrv_getlength(bs);
        if (length < 0) {
            return length;
        }
        hint = length >> BDRV_SECTOR_BITS;
    }

    bs->total_sectors = hint;
    return 0;
}

/*
 * Common part for opening disk images and files
 */
static int bdrv_open_common(BlockDriverState *bs, const char *filename,
    int flags, BlockDriver *drv)
{
    int ret, open_flags;

    assert(drv != NULL);

    bs->file = NULL;
    bs->total_sectors = 0;
    bs->is_temporary = 0;
    bs->encrypted = 0;
    bs->valid_key = 0;
    bs->open_flags = flags;

    /* Pacer modification: update indicators */
    bs->enable_throttle=0;
    bs->throlimit.time=-1;
    bs->timestamp_write=0; // for latency measurement for app io
    bs->migration=0; // for latency measurement for app io 
    /* End */

    /* buffer_alignment defaulted to 512, drivers can change this value */
    bs->buffer_alignment = 512;

    pstrcpy(bs->filename, sizeof(bs->filename), filename);
    
    /* Pacer modification */
    if(!strcmp(bs->device_name,"ide0-hd0")){
    	bs->enable_history_tracking=0;
    }
    else{
	bs->enable_history_tracking=0;
    }
    bs->enable_dirty_scheduling=0;
    bs->migration=0;    // to distinguish migration and app for latency measurement
    /* End */

    if (use_bdrv_whitelist && !bdrv_is_whitelisted(drv)) {
        return -ENOTSUP;
    }

    bs->drv = drv;
    bs->opaque = qemu_mallocz(drv->instance_size);

    /*
     * Yes, BDRV_O_NOCACHE aka O_DIRECT means we have to present a
     * write cache to the guest.  We do need the fdatasync to flush
     * out transactions for block allocations, and we maybe have a
     * volatile write cache in our backing device to deal with.
     */
    if (flags & (BDRV_O_CACHE_WB|BDRV_O_NOCACHE))
        bs->enable_write_cache = 1;

    /*
     * Clear flags that are internal to the block layer before opening the
     * image.
     */
    open_flags = flags & ~(BDRV_O_SNAPSHOT | BDRV_O_NO_BACKING);

    /*
     * Snapshots should be writeable.
     */
    if (bs->is_temporary) {
        open_flags |= BDRV_O_RDWR;
    }

    /* Open the image, either directly or using a protocol */
    if (drv->bdrv_file_open) {
        ret = drv->bdrv_file_open(bs, filename, open_flags);
    } else {
        ret = bdrv_file_open(&bs->file, filename, open_flags);
        if (ret >= 0) {
            ret = drv->bdrv_open(bs, open_flags);
        }
    }

    if (ret < 0) {
        goto free_and_fail;
    }

    bs->keep_read_only = bs->read_only = !(open_flags & BDRV_O_RDWR);

    ret = refresh_total_sectors(bs, bs->total_sectors);
    /* Pacer modification: update the additional variables */
    if(bs->enable_history_tracking){
    	bs->chunksize0=BDRV_SECTORS_PER_BULK_CHUNK;
    	bs->chunksize0_bit=BDRV_SECTORS_PER_BULK_CHUNK_BITS;
    	if(((bs->total_sectors) % (bs->chunksize0))!=0)	
	 	bs->writehistory_size0=((bs->total_sectors) >> (bs->chunksize0_bit))+1;
    	else
		bs->writehistory_size0=((bs->total_sectors) >> (bs->chunksize0_bit));
   	    bs->writehistory0=qemu_malloc(sizeof(WriteHistoryItem)*(bs->writehistory_size0));
    	WriteFreqItem* writefreq=qemu_malloc(sizeof(WriteFreqItem));
   	    writefreq->previousFreqItem=NULL;
    	writefreq->nextFreqItem=NULL;
    	writefreq->freq=0;
    	writefreq->head=bs->writehistory0;
    	bs->writefreqHead0=writefreq;
    	bs->writefreqTail0=writefreq;

    	bs->chunksize1=-1;
    	bs->chunksize1_bit=-1;
    	bs->writehistory_size1=-1;
    	bs->writehistory1=NULL;
    	bs->writefreqHead1=NULL;
    	bs->writefreqTail1=NULL;	

    	bs->current_chunksize=bs->chunksize0;	
    	bs->current_chunksize_bit=bs->chunksize0_bit;
    	bs->current_writehistory_size=bs->writehistory_size0;
    	bs->current_writehistory= bs->writehistory0;
    	bs->current_writefreqHead=bs->writefreqHead0;
    	bs->current_writefreqTail=bs->writefreqTail0;
   
    	bs->history_active_id=0;
	    bs->writeop_counter=0;

	    int i=0;
    	for(i=0;i<(bs->writehistory_size0);i++){
    		bs->writehistory0[i].freq=0;
    		bs->writehistory0[i].id=i;
    		bs->writehistory0[i].myFreqItem=bs->writefreqHead0;
    		if(i==0){
    			bs->writehistory0[i].previousWHItem=NULL;
    			bs->writehistory0[i].nextWHItem=&(bs->writehistory0[i+1]);
    		}
    		else if(i==(bs->writehistory_size0-1)){
    			bs->writehistory0[i].previousWHItem=&(bs->writehistory0[i-1]);
    			bs->writehistory0[i].nextWHItem=NULL;
    		}else{
    			bs->writehistory0[i].previousWHItem=&(bs->writehistory0[i-1]);
    			bs->writehistory0[i].nextWHItem=&(bs->writehistory0[i+1]);
    		}
    	}
    
	for(i=0;i<10;i++)
	{
		if(i==0)
		{
			bs->current_bmap.chunksize[0]=BDRV_SECTORS_PER_BULK_CHUNK;
			bs->current_bmap.chunksize_bit[0]=BDRV_SECTORS_PER_BULK_CHUNK_BITS;
		}
		else{
			bs->current_bmap.chunksize[i]=bs->current_bmap.chunksize[i-1]*2;
			bs->current_bmap.chunksize_bit[i]= ((bs->current_bmap.chunksize_bit[i-1])+1);
		}
        	if(((bs->total_sectors) % (bs->current_bmap.chunksize[i]))!=0)	
		  	bs->current_bmap.totalchunk[i]=((bs->total_sectors) >> (bs->current_bmap.chunksize_bit[i]))+1;
    		else
			bs->current_bmap.totalchunk[i]=((bs->total_sectors) >> (bs->current_bmap.chunksize_bit[i]));	
		int bitmap_size=0;
		if(((bs->current_bmap.totalchunk[i])%8)!=0)
			bitmap_size=((bs->current_bmap.totalchunk[i]) >> 3) +1;
		else
			bitmap_size=(bs->current_bmap.totalchunk[i]) >> 3;	
        bs->current_bmap.bitmap1[i]=qemu_malloc(bitmap_size);
		int k;
		for(k=0;k<bitmap_size;k++)
		{
			bs->current_bmap.bitmap1[i][k]=0;
		}
		if(i==0){
			bs->current_bmap.bitmap2=qemu_mallocz(bitmap_size);
		
			for(k=0;k<bitmap_size;k++)
			{
				bs->current_bmap.bitmap2[k]=0;
			}
	
    			bs->current_bmap.bp2accesschunk=0;
		}
		bs->current_bmap.bp1accesschunk[i]=0;
		bs->current_bmap.bp1storagerate[i]=0;
		bs->current_bmap.bp2coverchunk[i]=0;
		bs->current_bmap.bp2coverage[i]=0;
    	} 	
 	
	bs->dirty_writehistory=NULL;

   }
    bs->writeopsetsHead=qemu_malloc(sizeof(OneSetWriteOps));
    bs->writeopsetsHead->oneset=qemu_malloc(sizeof(WriteopInfo)*50000);
    bs->writeopsetsHead->index=0;
    bs->writeopsetsHead->nextset=NULL;	
    /* End */	
    if (ret < 0) {
        goto free_and_fail;
    }

#ifndef _WIN32
    if (bs->is_temporary) {
        unlink(filename);
    }
#endif
    return 0;

free_and_fail:
    if (bs->file) {
        bdrv_delete(bs->file);
        bs->file = NULL;
    }
    qemu_free(bs->opaque);
    bs->opaque = NULL;
    bs->drv = NULL;
    return ret;
}

/*
 * Opens a file using a protocol (file, host_device, nbd, ...)
 */
int bdrv_file_open(BlockDriverState **pbs, const char *filename, int flags)
{
    BlockDriverState *bs;
    BlockDriver *drv;
    int ret;

    drv = bdrv_find_protocol(filename);
    if (!drv) {
        return -ENOENT;
    }

    bs = bdrv_new("");
    ret = bdrv_open_common(bs, filename, flags, drv);
    if (ret < 0) {
        bdrv_delete(bs);
        return ret;
    }
    bs->growable = 1;
    *pbs = bs;
    return 0;
}

/*
 * Opens a disk image (raw, qcow2, vmdk, ...)
 */
int bdrv_open(BlockDriverState *bs, const char *filename, int flags,
              BlockDriver *drv)
{
    int ret;

    if (flags & BDRV_O_SNAPSHOT) {
        BlockDriverState *bs1;
        int64_t total_size;
        int is_protocol = 0;
        BlockDriver *bdrv_qcow2;
        QEMUOptionParameter *options;
        char tmp_filename[PATH_MAX];
        char backing_filename[PATH_MAX];

        /* if snapshot, we create a temporary backing file and open it
           instead of opening 'filename' directly */

        /* if there is a backing file, use it */
        bs1 = bdrv_new("");
        ret = bdrv_open(bs1, filename, 0, drv);
        if (ret < 0) {
            bdrv_delete(bs1);
            return ret;
        }
        total_size = bdrv_getlength(bs1) & BDRV_SECTOR_MASK;

        if (bs1->drv && bs1->drv->protocol_name)
            is_protocol = 1;

        bdrv_delete(bs1);

        get_tmp_filename(tmp_filename, sizeof(tmp_filename));

        /* Real path is meaningless for protocols */
        if (is_protocol)
            snprintf(backing_filename, sizeof(backing_filename),
                     "%s", filename);
        else if (!realpath(filename, backing_filename))
            return -errno;

        bdrv_qcow2 = bdrv_find_format("qcow2");
        options = parse_option_parameters("", bdrv_qcow2->create_options, NULL);

        set_option_parameter_int(options, BLOCK_OPT_SIZE, total_size);
        set_option_parameter(options, BLOCK_OPT_BACKING_FILE, backing_filename);
        if (drv) {
            set_option_parameter(options, BLOCK_OPT_BACKING_FMT,
                drv->format_name);
        }

        ret = bdrv_create(bdrv_qcow2, tmp_filename, options);
        free_option_parameters(options);
        if (ret < 0) {
            return ret;
        }

        filename = tmp_filename;
        drv = bdrv_qcow2;
        bs->is_temporary = 1;
    }

    /* Find the right image format driver */
    if (!drv) {
        drv = find_image_format(filename);
    }
      
    if (!drv) {
        ret = -ENOENT;
        goto unlink_and_fail;
    }

    /* Open the image */
    ret = bdrv_open_common(bs, filename, flags, drv);
    if (ret < 0) {
        goto unlink_and_fail;
    }

    /* If there is a backing file, use it */
    if ((flags & BDRV_O_NO_BACKING) == 0 && bs->backing_file[0] != '\0') {
        char backing_filename[PATH_MAX];
        int back_flags;
        BlockDriver *back_drv = NULL;

        bs->backing_hd = bdrv_new("");
        path_combine(backing_filename, sizeof(backing_filename),
                     filename, bs->backing_file);
        if (bs->backing_format[0] != '\0')
            back_drv = bdrv_find_format(bs->backing_format);

        /* backing files always opened read-only */
        back_flags =
            flags & ~(BDRV_O_RDWR | BDRV_O_SNAPSHOT | BDRV_O_NO_BACKING);

        ret = bdrv_open(bs->backing_hd, backing_filename, back_flags, back_drv);
        if (ret < 0) {
            bdrv_close(bs);
            return ret;
        }
        if (bs->is_temporary) {
            bs->backing_hd->keep_read_only = !(flags & BDRV_O_RDWR);
        } else {
            /* base image inherits from "parent" */
            bs->backing_hd->keep_read_only = bs->keep_read_only;
        }
    }

    if (!bdrv_key_required(bs)) {
        /* call the change callback */
        bs->media_changed = 1;
        if (bs->change_cb)
            bs->change_cb(bs->change_opaque);
    }

    return 0;

unlink_and_fail:
    if (bs->is_temporary) {
        unlink(filename);
    }
    return ret;
}

void bdrv_close(BlockDriverState *bs)
{
    if (bs->drv) {
        if (bs->backing_hd) {
            bdrv_delete(bs->backing_hd);
            bs->backing_hd = NULL;
        }
        bs->drv->bdrv_close(bs);
        qemu_free(bs->opaque);
#ifdef _WIN32
        if (bs->is_temporary) {
            unlink(bs->filename);
        }
#endif
        bs->opaque = NULL;
        bs->drv = NULL;

        if (bs->file != NULL) {
            bdrv_close(bs->file);
        }

        /* call the change callback */
        bs->media_changed = 1;
        if (bs->change_cb)
            bs->change_cb(bs->change_opaque);
    }
}

void bdrv_close_all(void)
{
    BlockDriverState *bs;

    QTAILQ_FOREACH(bs, &bdrv_states, list) {
        bdrv_close(bs);
    }
}

void bdrv_delete(BlockDriverState *bs)
{
    /* remove from list, if necessary */
    if (bs->device_name[0] != '\0') {
        QTAILQ_REMOVE(&bdrv_states, bs, list);
    }

    bdrv_close(bs);
    if (bs->file != NULL) {
        bdrv_delete(bs->file);
    }

    qemu_free(bs);
}

/*
 * Run consistency checks on an image
 *
 * Returns the number of errors or -errno when an internal error occurs
 */
int bdrv_check(BlockDriverState *bs)
{
    if (bs->drv->bdrv_check == NULL) {
        return -ENOTSUP;
    }

    return bs->drv->bdrv_check(bs);
}

/* commit COW file into the raw image */
int bdrv_commit(BlockDriverState *bs)
{
    BlockDriver *drv = bs->drv;
    int64_t i, total_sectors;
    int n, j, ro, open_flags;
    int ret = 0, rw_ret = 0;
    unsigned char sector[BDRV_SECTOR_SIZE];
    char filename[1024];
    BlockDriverState *bs_rw, *bs_ro;

    if (!drv)
        return -ENOMEDIUM;
    
    if (!bs->backing_hd) {
        return -ENOTSUP;
    }

    if (bs->backing_hd->keep_read_only) {
        return -EACCES;
    }
    
    ro = bs->backing_hd->read_only;
    strncpy(filename, bs->backing_hd->filename, sizeof(filename));
    open_flags =  bs->backing_hd->open_flags;

    if (ro) {
        /* re-open as RW */
        bdrv_delete(bs->backing_hd);
        bs->backing_hd = NULL;
        bs_rw = bdrv_new("");
        rw_ret = bdrv_open(bs_rw, filename, open_flags | BDRV_O_RDWR, drv);
        if (rw_ret < 0) {
            bdrv_delete(bs_rw);
            /* try to re-open read-only */
            bs_ro = bdrv_new("");
            ret = bdrv_open(bs_ro, filename, open_flags & ~BDRV_O_RDWR, drv);
            if (ret < 0) {
                bdrv_delete(bs_ro);
                /* drive not functional anymore */
                bs->drv = NULL;
                return ret;
            }
            bs->backing_hd = bs_ro;
            return rw_ret;
        }
        bs->backing_hd = bs_rw;
    }

    total_sectors = bdrv_getlength(bs) >> BDRV_SECTOR_BITS;
    for (i = 0; i < total_sectors;) {
        if (drv->bdrv_is_allocated(bs, i, 65536, &n)) {
            for(j = 0; j < n; j++) {
                if (bdrv_read(bs, i, sector, 1) != 0) {
                    ret = -EIO;
                    goto ro_cleanup;
                }

                if (bdrv_write(bs->backing_hd, i, sector, 1) != 0) {
                    ret = -EIO;
                    goto ro_cleanup;
                }
                i++;
	    }
	} else {
            i += n;
        }
    }

    if (drv->bdrv_make_empty) {
        ret = drv->bdrv_make_empty(bs);
        bdrv_flush(bs);
    }

    /*
     * Make sure all data we wrote to the backing device is actually
     * stable on disk.
     */
    if (bs->backing_hd)
        bdrv_flush(bs->backing_hd);

ro_cleanup:

    if (ro) {
        /* re-open as RO */
        bdrv_delete(bs->backing_hd);
        bs->backing_hd = NULL;
        bs_ro = bdrv_new("");
        ret = bdrv_open(bs_ro, filename, open_flags & ~BDRV_O_RDWR, drv);
        if (ret < 0) {
            bdrv_delete(bs_ro);
            /* drive not functional anymore */
            bs->drv = NULL;
            return ret;
        }
        bs->backing_hd = bs_ro;
        bs->backing_hd->keep_read_only = 0;
    }

    return ret;
}

void bdrv_commit_all(void)
{
    BlockDriverState *bs;

    QTAILQ_FOREACH(bs, &bdrv_states, list) {
        bdrv_commit(bs);
    }
}

/*
 * Return values:
 * 0        - success
 * -EINVAL  - backing format specified, but no file
 * -ENOSPC  - can't update the backing file because no space is left in the
 *            image file header
 * -ENOTSUP - format driver doesn't support changing the backing file
 */
int bdrv_change_backing_file(BlockDriverState *bs,
    const char *backing_file, const char *backing_fmt)
{
    BlockDriver *drv = bs->drv;

    if (drv->bdrv_change_backing_file != NULL) {
        return drv->bdrv_change_backing_file(bs, backing_file, backing_fmt);
    } else {
        return -ENOTSUP;
    }
}

static int bdrv_check_byte_request(BlockDriverState *bs, int64_t offset,
                                   size_t size)
{
    int64_t len;
    if (!bdrv_is_inserted(bs))
        return -ENOMEDIUM;
    if (bs->growable)
        return 0;
    len = bdrv_getlength(bs);

    if (offset < 0)
        return -EIO;
    
    if ((offset > len) || (len - offset < size))
        return -EIO;

    return 0;
}

static int bdrv_check_request(BlockDriverState *bs, int64_t sector_num,
                              int nb_sectors)
{
    return bdrv_check_byte_request(bs, sector_num * BDRV_SECTOR_SIZE,
                                   nb_sectors * BDRV_SECTOR_SIZE);
}

/* return < 0 if error. See bdrv_write() for the return codes */
int bdrv_read(BlockDriverState *bs, int64_t sector_num,
              uint8_t *buf, int nb_sectors)
{
    //printf("bdrv_read\n");
    BlockDriver *drv = bs->drv; 

    if (!drv)
        return -ENOMEDIUM;
    if (bdrv_check_request(bs, sector_num, nb_sectors))
        return -EIO;

    return drv->bdrv_read(bs, sector_num, buf, nb_sectors);
}


/* Pacer Function: add for verify the dirty freq list if necessary*/
void printDirtyWriteFreq(BlockDriverState *bs)
{
	WriteFreqItem* writefreq=bs->dirty_writefreqHead;
	while(writefreq!=NULL)
	{
		WriteHistoryItem* whitem=writefreq->head;
		printf("dirty freq %d ",writefreq->freq);
		if(writefreq->previousFreqItem==NULL)
		{
			printf("prev NULL ");
		}
		else
			printf("prev %d ",writefreq->previousFreqItem->freq);
		if(writefreq->nextFreqItem==NULL)
		{
			printf("next NULL ");
		}
		else
			printf("next %d\n",writefreq->nextFreqItem->freq);
		if(writefreq->freq==0)
		{
			printf("\n");
			break;
		}
		while(whitem!=NULL)
		{
			printf("id: %d freq %d\n",whitem->id, whitem->freq);
			whitem=whitem->nextWHItem;
		}
		writefreq=writefreq->nextFreqItem;
	}
}


/* Pacer Function: move the dirty block to clean block by updating the dirty freq list*/
void removeWriteFreq(BlockDriverState *bs, int chunk_id)
{
	WriteFreqItem* writefreq=bs->dirty_writehistory[chunk_id].myFreqItem;

	WriteHistoryItem* whitem=&(bs->dirty_writehistory[chunk_id]);
	if((whitem->nextWHItem==NULL)&&(whitem->previousWHItem==NULL)) //the only item in this freq
	{
		if(writefreq==bs->dirty_writefreqHead)
		{
			bs->dirty_writefreqHead=bs->dirty_writefreqHead->nextFreqItem;
			bs->dirty_writefreqHead->previousFreqItem=NULL;
			qemu_free(writefreq);
		}
		else
		{	
			writefreq->previousFreqItem->nextFreqItem=writefreq->nextFreqItem;
			writefreq->nextFreqItem->previousFreqItem=writefreq->previousFreqItem;
			qemu_free(writefreq);			
			
		}
	}else if((whitem->nextWHItem!=NULL)&&(whitem->previousWHItem==NULL)){  //not the only one, but the first one
		writefreq->head=whitem->nextWHItem;
		whitem->nextWHItem->previousWHItem=NULL;		
	}
	else{
		whitem->previousWHItem->nextWHItem=whitem->nextWHItem;
		if(whitem->nextWHItem!=NULL)
			whitem->nextWHItem->previousWHItem=whitem->previousWHItem;
	}

	WriteHistoryItem* prehead = bs->dirty_writefreqTail->head;
	prehead->previousWHItem=whitem;
	whitem->nextWHItem=prehead;
	whitem->previousWHItem=NULL;
	whitem->myFreqItem=bs->dirty_writefreqTail;
	bs->dirty_writefreqTail->head=whitem;
	return;	
}

/* Pacer Function: throttling mechanism, when throttle_mechanism=0 (soft), =1 (aggr)*/

static void set_throttle_delay(BlockDriverState *bs, int64_t sector_num, int nb_sectors)
{
   int64_t currenttime=my_qemu_get_clock_us(rt_clock); //get the us
    int64_t start, end;
    unsigned long val, idx, bit;

    start = sector_num / BDRV_SECTORS_PER_DIRTY_CHUNK;
    end = (sector_num + nb_sectors - 1) / BDRV_SECTORS_PER_DIRTY_CHUNK;
    int delta=0;
    for (; start <= end; start++) {
        idx = start / (sizeof(unsigned long) * 8);
        bit = start % (sizeof(unsigned long) * 8);
        val = bs->dirty_bitmap[idx];
        if (!(val & (1UL << bit))) {
                delta++;
        }
    }
        printf("detla %d\n",delta);
	if(delta==0)
	{
		bs->tempusleep=0;
		
	}else{
                if(bs->throlimit.nexttime<currenttime)
                {
                        bs->throlimit.time=currenttime;       
                        bs->throlimit.nexttime=bs->throlimit.time+(delta)*1000000.0/(throttle_dirtyrate);
                        bs->tempusleep=0;
                        
			       
                }else{
			if(throttle_mechanism==0){
				//soft throttling
				if(ave_dirtyrate>=(throttle_dirtyrate))	
                        		bs->tempusleep=(bs->throlimit.nexttime-currenttime);
				else
					bs->tempusleep=0;
			}else  //aggr throttling
			    bs->tempusleep=(bs->throlimit.nexttime-currenttime);
                        
                        bs->throlimit.time=currenttime+bs->tempusleep;
                        bs->throlimit.nexttime=bs->throlimit.time+delta*1000000.0/(throttle_dirtyrate);
		}		
	} 
	throttle_usleep=bs->tempusleep;
        printf("usleep %"PRId64"\n",throttle_usleep);
}

// Pacer Function
void printWriteBlockDistance(BlockDriverState *bs)
{
      long int count=bs->totalblock;
      long int i=0;
      for (i=0; i<count; i++) {
         if(bs->write_block_count[i]>=2)
         {
           long int ave_dist= bs->write_block_distance[i]/(bs->write_block_count[i]-1);
           printf("block %ld ave_dist %ld op %ld\n",i,ave_dist,bs->write_block_count[i]);
         }
    } 

}

int64_t bdrv_getNewExpireTime(BlockDriverState *bs, int64_t* pdirtyrate2,int64_t nextblock, int64_t speed, int64_t remain_time)
{
      int64_t currenttime=qemu_get_clock(rt_clock);

      int64_t blocksize= ((int64_t)BDRV_SECTORS_PER_BULK_CHUNK << (int64_t)BDRV_SECTOR_BITS);
      int64_t finaltime=currenttime+(bs->totalblock-nextblock)*blocksize*1000L/speed;
      uint64_t count=bs->totalblock;
      uint64_t i=0;
      uint64_t dirtyset_count=0;
      for (i=0; i<count; i++)
      {
         if(bs->write_block_count[i]>=2)
         { 
                //step1: compute ave dist
                uint64_t ave_dist= bs->write_block_distance[i]/(bs->write_block_count[i]-1);
                bs->write_block_avedist[i]=ave_dist;

              //step2: whether in the dirty set?
                if(i<nextblock)
                {
                        if(bdrv_get_dirty(bs, i*((int64_t)BDRV_SECTORS_PER_DIRTY_CHUNK)))
                        {  
                                bs->write_block_markmap[i]=2;
                                dirtyset_count++;
                        }else{
                                int found=0;
                                int64_t tmptime=bs->write_block_laccesstime[i];
                                while(tmptime<finaltime){
                                        if(tmptime>currenttime){
                                                found=1;
                                                break;
                                        }else
                                                tmptime+=bs->write_block_avedist[i];
                                }
                                if(found==1){
                                        bs->write_block_markmap[i]=2;   //dirty set
                                        dirtyset_count++;
                                }
                                else {
                                        bs->write_block_markmap[i]=1;   //clean but written set
                                }


 			}
                }else{
                        int64_t migrtime=currenttime+(i-nextblock)*blocksize*1000L/speed;
                        int found=0;
                        int64_t tmptime=bs->write_block_laccesstime[i];
                        while(tmptime<finaltime){
                                if(tmptime>migrtime){
                                        found=1;
                                        break;
                                }else
                                        tmptime+=bs->write_block_avedist[i];
                        }
                        if(found==1){
                                bs->write_block_markmap[i]=2;   //dirty set
                                dirtyset_count++;
                        }
                        else{
                                bs->write_block_markmap[i]=1;   //clean but written set 
                        }
                }
         }
    }
    double dirtyrate=0;
    uint64_t dirtyset_i=0;

    for(i=0;i<count;i++)
    {
        uint64_t currentdist=currenttime-bs->write_block_laccesstime[i];
        uint64_t variance=0;
        if(bs->write_block_count[i]>=2)
        {
                uint64_t E_x_2= bs->write_block_variance[i]/(bs->write_block_count[i]-1);
                uint64_t Ex_2= bs->write_block_avedist[i]*(bs->write_block_avedist[i]);
                variance=sqrt(E_x_2-Ex_2);

        }

        if(bs->write_block_markmap[i]==2)
        {
              double tmp=0;
              if(currentdist<=(bs->write_block_avedist[i]+2*variance))
                tmp=(dirtyset_count-dirtyset_i)*(1000.0/bs->write_block_avedist[i])/dirtyset_count;

              dirtyrate=dirtyrate+tmp;
              dirtyset_i++;
 
        }else if(bs->write_block_markmap[i]==1)
        {
              double tmp=0;
              if(currentdist<=(bs->write_block_avedist[i]+2*variance))
                tmp=(1000.0)/bs->write_block_avedist[i];

              dirtyrate=dirtyrate+tmp;
        }


    }
    
    if(dirtyset_i!=dirtyset_count)
        printf("ERROR, dirtyset_i!=dirtycount!!!!\n");
    dirtyrate=dirtyrate*blocksize;

    prediction_dirtyrate=dirtyrate;
    if(pdirtyrate2!=NULL)
    	*pdirtyrate2=prediction_dirtyrate;

    printf("totalblock %"PRId64" nextblock %"PRId64" dirtyset_count %"PRId64" dirtyrate %f\n",bs->totalblock,nextblock,dirtyset_count,prediction_dirtyrate);
   int64_t remaintime=0;
    int64_t precopy_remain=0;
    if(bs->totalblock>nextblock)
	precopy_remain=(bs->totalblock-nextblock)*blocksize/speed;

    if(precopy_remain>=remain_time)
    {
    	if((((int)prediction_dirtyrate)>>20)<(speed>>20)){
        	printf("case 1\n");
		remaintime=precopy_remain+dirtyset_count*blocksize/(speed-prediction_dirtyrate);
  	       printf("precopy_remain %"PRId64" dirtyset_count %"PRId64" speed %"PRId64" pred_drate %f remaintime %"PRId64"\n",precopy_remain,dirtyset_count,speed,prediction_dirtyrate,remaintime);
	}
    	else{
		printf("case 2\n");
        	remaintime=precopy_remain+dirtyset_count*blocksize/(speed-speed/5); 
        }
    }else{
        int64_t dirtytime1=remain_time-precopy_remain;
        if((dirtytime1*speed)>(dirtyset_count*blocksize))
        {
		printf("case 3\n");
		remaintime=remain_time;
	}else{
		printf("case 4\n");
		if(prediction_dirtyrate<speed)
                	remaintime=precopy_remain+dirtyset_count*blocksize/(speed-prediction_dirtyrate);
        	else
                	remaintime=precopy_remain+dirtyset_count*blocksize/(speed-speed/5);
	}		
    }
    return remaintime;
}

// Pacer Function
void bdrv_getAveDirtyRate2(BlockDriverState *bs, int64_t nextblock, int64_t speed,int64_t *pdirtyset_size,int64_t *pdirtyrate2)
{

      int64_t currenttime=qemu_get_clock(rt_clock);   
        
      int64_t blocksize= ((int64_t)BDRV_SECTORS_PER_BULK_CHUNK << (int64_t)BDRV_SECTOR_BITS); 
      int64_t finaltime=currenttime+(bs->totalblock-nextblock)*blocksize*1000L/speed; 
      uint64_t count=bs->totalblock;
      uint64_t i=0;
      uint64_t dirtyset_count=0;
      int64_t time0=my_qemu_get_clock_us(rt_clock);
      for (i=0; i<count; i++) 
      {
         if(bs->write_block_count[i]>=2)
         {  
         	//step1: compute ave dist
           	uint64_t ave_dist= bs->write_block_distance[i]/(bs->write_block_count[i]-1);        
             
                bs->write_block_avedist[i]=ave_dist;
                     
           	//step2: whether in the dirty set?
           	if(i<nextblock) 
           	{
              		if(bdrv_get_dirty(bs, i*((int64_t)BDRV_SECTORS_PER_DIRTY_CHUNK)))
	      		{  //for sure to be dirty
		  		bs->write_block_markmap[i]=2;
                                dirtyset_count++;
	      		}else{
		 	   //will dirty in the future?
                                
		 		int found=0;
                 		int64_t tmptime=bs->write_block_laccesstime[i];
                 		while(tmptime<finaltime){
                    			if(tmptime>currenttime){
                        			found=1;
                        			break;
		    			}else
						tmptime+=bs->write_block_avedist[i];
				}
                 		if(found==1){
		    			bs->write_block_markmap[i]=2;   //dirty set
					dirtyset_count++;

				}
                 		else {
		    			bs->write_block_markmap[i]=1;   //clean but written set
                                } 		 
	      		}
           	}else{
			int64_t migrtime=currenttime+(i-nextblock)*blocksize*1000L/speed;
                	int found=0;
                 	int64_t tmptime=bs->write_block_laccesstime[i];
                 	while(tmptime<finaltime){
                    		if(tmptime>migrtime){
                        		found=1;
                        		break;
                    		}else
                        		tmptime+=bs->write_block_avedist[i];  
                        }
                 	if(found==1){
                    		bs->write_block_markmap[i]=2;   //dirty set
                 		dirtyset_count++;
			}
			else{
                    		bs->write_block_markmap[i]=1;   //clean but written set 
			}
	   	}
         }
    }
    int64_t time1=my_qemu_get_clock_us(rt_clock); 
    double dirtyrate=0;
    uint64_t dirtyset_i=0;
    for(i=0;i<count;i++)
    {
        uint64_t currentdist=currenttime-bs->write_block_laccesstime[i];
        uint64_t variance=0;
        if(bs->write_block_count[i]>=2)
        {
                uint64_t E_x_2= bs->write_block_variance[i]/(bs->write_block_count[i]-1);
                uint64_t Ex_2= bs->write_block_avedist[i]*(bs->write_block_avedist[i]);
              	variance=sqrt(E_x_2-Ex_2);
        }         

	if(bs->write_block_markmap[i]==2)
	{
              double tmp=0;
              if(currentdist<=(bs->write_block_avedist[i]+2*variance))
                tmp=(dirtyset_count-dirtyset_i)*(1000.0/bs->write_block_avedist[i])/dirtyset_count;
              dirtyrate=dirtyrate+tmp;  
              dirtyset_i++;	
	}else if(bs->write_block_markmap[i]==1)
	{
              double tmp=0;
              if(currentdist<=(bs->write_block_avedist[i]+2*variance))
                tmp=(1000.0)/bs->write_block_avedist[i];
              dirtyrate=dirtyrate+tmp;  
	}	
    } 
    if(dirtyset_i!=dirtyset_count)
	printf("ERROR, dirtyset_i!=dirtycount!!!!\n"); 
    dirtyrate=dirtyrate * blocksize;
    *pdirtyrate2=dirtyrate/2;
    *pdirtyset_size=dirtyset_count*blocksize;
    int64_t time2=my_qemu_get_clock_us(rt_clock);
    printf("time_dirtyset %"PRId64" time_dirtyrate %"PRId64"\n",time1-time0,time2-time1);
    return;
}

//for manually setting dirty blocks for progress meter test
void set_write_block_distance(BlockDriverState *bs, int64_t sector_num, int nb_sectors, uint64_t count, uint64_t laccesstime, uint64_t distance,uint64_t variance)
{
    int64_t start, end;

    start = sector_num / BDRV_SECTORS_PER_DIRTY_CHUNK;
    end = (sector_num + nb_sectors - 1) / BDRV_SECTORS_PER_DIRTY_CHUNK;
    for (; start <= end; start++) {
           bs->write_block_laccesstime[start]=laccesstime;
           bs->write_block_count[start]=count;
           bs->write_block_distance[start]=distance;
	   bs->write_block_variance[start]=variance;
   }		
}

void update_write_block_distance(BlockDriverState *bs, int64_t sector_num, int nb_sectors)
{
    int64_t start, end;

    start = sector_num / BDRV_SECTORS_PER_DIRTY_CHUNK;
    end = (sector_num + nb_sectors - 1) / BDRV_SECTORS_PER_DIRTY_CHUNK;
    for (; start <= end; start++) {
         if(bs->write_block_count[start]==0)
         {
           bs->write_block_laccesstime[start]=qemu_get_clock(rt_clock);
           bs->write_block_count[start]=1;
           bs->write_block_distance[start]=0;
         }else{
           unsigned long currenttime=qemu_get_clock(rt_clock);
           unsigned long distance=currenttime-bs->write_block_laccesstime[start];
           if(distance>0)
           {
           	bs->write_block_distance[start]+=distance;
           	bs->write_block_count[start]+=1;
                bs->write_block_variance[start]+=(distance)*(distance);
           	bs->write_block_laccesstime[start]=currenttime;
           } 
         }
    } 
}


// Pacer modified function
static void set_dirty_bitmap(BlockDriverState *bs, int64_t sector_num,
                             int nb_sectors, int dirty)
{
    int64_t start, end;
    unsigned long val, idx, bit;
    start = sector_num / BDRV_SECTORS_PER_DIRTY_CHUNK;  
    end = (sector_num + nb_sectors - 1) / BDRV_SECTORS_PER_DIRTY_CHUNK;
     for (; start <= end; start++) {
        idx = start / (sizeof(unsigned long) * 8);  
        bit = start % (sizeof(unsigned long) * 8);
	val = bs->dirty_bitmap[idx];
        if (dirty) { 
            // before, 1 << bit is wrong, 1L is must
            if (!(val & (1UL << bit))) {
                bs->dirty_count++;
                val |= 1UL << bit;
            }	
        } else {
            if (val & (1UL << bit)) {  
                bs->dirty_count--;
                val &= ~(1UL << bit);
            }
        }
        bs->dirty_bitmap[idx] = val;
    }
}

/* Pacer Function: add for dirty block freq list tracking*/
static void set_my_dirty_bitmap(BlockDriverState *bs, int64_t sector_num,
                             int nb_sectors, int dirty)
{
    int64_t start, end;
    start = sector_num >> (bs->dirty_chunksize_bit);  
    end = (sector_num + nb_sectors - 1) >> (bs->dirty_chunksize_bit);
    int delta=0;
    if(dirty){
    	for (; start <= end; start++) {
         	bs->dirty_writehistory[start].freq=bs->dirty_writehistory[start].freq+1;
	 	if(bs->dirty_writehistory[start].freq==1){
			bs->dirty_count++;
			delta++;
		}
	 	updateWriteFreq(bs,start,(bs->dirty_writehistory[start].freq)-1,1);
    	}
    }else{
	for (; start <= end; start++) {
	  if( bs->dirty_writehistory[start].freq!=0){
		bs->dirty_writehistory[start].freq=0;
	  	bs->dirty_count--;
          	removeWriteFreq(bs,start);
	  }
    	}
    }
}

/* Pacer Function: add for print write freq to verify the list is correct*/
void printWriteFreq(BlockDriverState *bs)
{
	WriteFreqItem* writefreq=bs->current_writefreqHead;
	while(writefreq!=NULL)
	{
		WriteHistoryItem* whitem=writefreq->head;
		printf("freq %d ",writefreq->freq);
		if(writefreq->previousFreqItem==NULL)
		{
			printf("prev NULL ");
		}
		else
			printf("prev %d ",writefreq->previousFreqItem->freq);
		if(writefreq->nextFreqItem==NULL)
		{
			printf("next NULL ");
		}
		else
			printf("next %d\n",writefreq->nextFreqItem->freq);
		if(writefreq->freq==0)
			break;
		while(whitem!=NULL)
		{
			printf("id: %d freq %d\n",whitem->id, whitem->freq);
			whitem=whitem->nextWHItem;
		}
		writefreq=writefreq->nextFreqItem;
	}
}

/* Pacer Function: update the write freq*/
void updateWriteFreq(BlockDriverState *bs, int chunk_id, int oldfreq, int flag)
{
	WriteFreqItem* writefreq=NULL;
	if(flag==0)
		writefreq=bs->current_writehistory[chunk_id].myFreqItem;
	else
		writefreq=bs->dirty_writehistory[chunk_id].myFreqItem;

	WriteHistoryItem* whitem=NULL;
	if(flag==0)
		whitem=&(bs->current_writehistory[chunk_id]);
	else
		whitem=&(bs->dirty_writehistory[chunk_id]);

	WriteFreqItem* writefreqhead=NULL;

	if(flag==0){
		writefreqhead=bs->current_writefreqHead;
	}else{
		writefreqhead=bs->dirty_writefreqHead;
	}
	if((whitem->nextWHItem==NULL)&&(whitem->previousWHItem==NULL)) //the only item in this freq
	{

		if(writefreq==writefreqhead)
		{
			writefreq->freq=oldfreq+1;
			return;
		}
		else
		{	
			if((writefreq->previousFreqItem->freq)>(oldfreq+1))
			{
				writefreq->freq=oldfreq+1;
				return;
			}else{
				WriteHistoryItem* prehead = writefreq->previousFreqItem->head;
				prehead->previousWHItem=whitem;
				whitem->nextWHItem=prehead;
				whitem->myFreqItem=writefreq->previousFreqItem;
				writefreq->previousFreqItem->head=whitem;
				writefreq->previousFreqItem->nextFreqItem=writefreq->nextFreqItem;
				if(writefreq->nextFreqItem!=NULL)
					writefreq->nextFreqItem->previousFreqItem=writefreq->previousFreqItem;
				else { //writefreq is the last one, need to be removed;
					if(flag==0)					
						bs->current_writefreqTail=writefreq->previousFreqItem;
					else
						bs->dirty_writefreqTail=writefreq->previousFreqItem;
				}				
				qemu_free(writefreq);
				return;
			}
		}
	}else if((whitem->nextWHItem!=NULL)&&(whitem->previousWHItem==NULL)){  //not the only one, but the first one
		writefreq->head=whitem->nextWHItem;
		whitem->nextWHItem->previousWHItem=NULL;		
	}
	else{
		whitem->previousWHItem->nextWHItem=whitem->nextWHItem;
		if(whitem->nextWHItem!=NULL)
			whitem->nextWHItem->previousWHItem=whitem->previousWHItem;
	}

	if(writefreq==writefreqhead)
	{
		WriteFreqItem* newwritefreq=qemu_malloc(sizeof(WriteFreqItem));
		newwritefreq->previousFreqItem=NULL;
    		newwritefreq->nextFreqItem=writefreq;
    		newwritefreq->freq=oldfreq+1;
    		newwritefreq->head=whitem;
		writefreq->previousFreqItem=newwritefreq;
		whitem->previousWHItem=NULL;
		whitem->nextWHItem=NULL;
		if(flag==0)
    			bs->current_writefreqHead=newwritefreq;
		else
			bs->dirty_writefreqHead=newwritefreq;
		whitem->myFreqItem=newwritefreq;
		return;
	}
	else
	{				
		if((writefreq->previousFreqItem->freq)>(oldfreq+1))
		{
			WriteFreqItem* newwritefreq=qemu_malloc(sizeof(WriteFreqItem));
			newwritefreq->previousFreqItem=writefreq->previousFreqItem;
    			newwritefreq->nextFreqItem=writefreq;
    			newwritefreq->freq=oldfreq+1;
    			newwritefreq->head=whitem;
			whitem->previousWHItem=NULL;
			whitem->nextWHItem=NULL;
    			writefreq->previousFreqItem->nextFreqItem=newwritefreq;
			writefreq->previousFreqItem=newwritefreq;
			whitem->myFreqItem=newwritefreq;
			return;
		}else{ 
			WriteHistoryItem* prehead = writefreq->previousFreqItem->head;
			prehead->previousWHItem=whitem;
			whitem->nextWHItem=prehead;
			whitem->previousWHItem=NULL;
			whitem->myFreqItem=writefreq->previousFreqItem;
			writefreq->previousFreqItem->head=whitem;
			return;
		}
	}	
	
}

/* Pacer Function: add for keeping the bitmap in the first half of the round*/
void set_writebitmap_first(BlockDriverState *bs, int64_t sector_num, int nb_sectors)
{
    int i;
    for(i=0;i<10;i++)
    {
	 int64_t start, end;
    	 int idx, bit;
		 char val;
         start = sector_num >> bs->current_bmap.chunksize_bit[i];
         end = (sector_num + nb_sectors - 1) >> bs->current_bmap.chunksize_bit[i];
	 
         int all_set=1;	
    	 for (; start <= end; start++) {
        	bit = start % (sizeof(char) * 8);
			idx = (start-bit) / (sizeof(char) * 8);
        	val = bs->current_bmap.bitmap1[i][idx];
        	
            	if (!(val & (1 << bit))) {
                	val |= 1 << bit;
			bs->current_bmap.bitmap1[i][idx] = val;
			all_set=0;
			bs->current_bmap.bp1accesschunk[i]++;
            	} 	
         } 
	 if(all_set)
		break; //because all the bigger chunk is all_set
	 else
		bs->current_bmap.bp1storagerate[i]=bs->current_bmap.bp1accesschunk[i]*1.0/bs->current_bmap.totalchunk[i];
	 
    }
   
}


/* Pacer Function: set the bitmap in the second half of the round*/

void set_writebitmap_second(BlockDriverState *bs, int64_t sector_num, int nb_sectors)
{   
    int64_t start, end;
    int idx, bit;
	char val;
    start = sector_num >> bs->current_bmap.chunksize_bit[0];
    end = (sector_num + nb_sectors - 1) >> bs->current_bmap.chunksize_bit[0];
	 
    int all_set=1;
    for (; start <= end; start++) {
       	bit = start % (sizeof(char) * 8);
		idx = (start-bit) / (sizeof(char) * 8);
       	val = bs->current_bmap.bitmap2[idx];
       
       	if (!(val & (1 << bit))) {
               	val |= 1 << bit;
		bs->current_bmap.bitmap2[idx] = val;
		all_set=0;
		bs->current_bmap.bp2accesschunk++;
		int i;
		for(i=0;i<10;i++)
		{

			int chunk_index=start >> i;
			int idx1,bit1;
			char val1;
       			bit1 = chunk_index % (sizeof(char) * 8);
				idx1 = (chunk_index-bit1) / (sizeof(char) * 8);
       			val1 = bs->current_bmap.bitmap1[i][idx1];
			if ((val1 & (1 << bit1)))  //in bitmap1,  covered
			{ 
				bs->current_bmap.bp2coverchunk[i]++;
			}
		}	
		
        } 	
    } 
    if(!all_set)
    {
	int j;
	for(j=0;j<10;j++)
		bs->current_bmap.bp2coverage[j]=bs->current_bmap.bp2coverchunk[j]*1.0/bs->current_bmap.bp2accesschunk;
    }
}

/* Pacer Function: add for gettting chunk size*/
int getchunksize(BlockDriverState *bs, int *chunksize_bit)
{
	int i, chunksize=0;
	float maxfl=0;
	for(i=0;i<10;i++){
		float sumfl=1-bs->current_bmap.bp1storagerate[i]+bs->current_bmap.bp2coverage[i];
		if(sumfl>maxfl){
			maxfl=sumfl;	
			chunksize=bs->current_bmap.chunksize[i];
			*chunksize_bit=bs->current_bmap.chunksize_bit[i];
		}
	}
	return chunksize;
}

/* Pacer Function: update the history*/
void updateHistory(int op, BlockDriverState *bs, int64_t sector_num, int nb_sectors)
{

	bs->writeop_counter++;
	int start=sector_num>>(bs->current_chunksize_bit); 
	int end=(sector_num+nb_sectors-1)>>(bs->current_chunksize_bit); 
	int i=start;
	int all_set=1;
	for(i=start;i<=end;i++)
	{
		bs->current_writehistory[i].freq=bs->current_writehistory[i].freq+1;
		updateWriteFreq(bs,i,((bs->current_writehistory[i].freq)-1),0);
		if(bs->current_writehistory[i].freq==1)
			all_set=0;
	}
	
	if((bs->writeop_counter)<=WHISTORYSIZE/2)
	{
		if(all_set==1) return;
		set_writebitmap_first(bs, sector_num, nb_sectors);
	}
	else{
		
		set_writebitmap_second(bs, sector_num, nb_sectors);
	}

	if((bs->writeop_counter)==WHISTORYSIZE)
	{
		int *chunksize_bit=qemu_malloc(sizeof(int));
		int chunksize=getchunksize(bs, chunksize_bit);
		if(chunksize==0)
		{
			printf("ERROR in getchunksize()");
			return;
		}else{
			printf("Success getchunksize: %d\n",chunksize);
		}
		bs->writeop_counter=0;
		if(bs->history_active_id==1){
			bs->writehistory1=bs->current_writehistory;
			bs->writefreqHead1=bs->current_writefreqHead;
    			bs->writefreqTail1=bs->current_writefreqTail;

			bs->chunksize0=chunksize;	
			bs->chunksize0_bit=*chunksize_bit;
			qemu_free(chunksize_bit);
			if(((bs->total_sectors) % (bs->chunksize0))!=0)	
	 			bs->writehistory_size0=((bs->total_sectors) >> (bs->chunksize0_bit))+1;
    			else
				bs->writehistory_size0=((bs->total_sectors) >> (bs->chunksize0_bit));
			qemu_free(bs->writehistory0);    			
			bs->writehistory0=qemu_malloc(sizeof(WriteHistoryItem)*(bs->writehistory_size0));
			WriteFreqItem* writefreq=bs->writefreqHead0;
			while(writefreq!=NULL)
			{	WriteFreqItem* writefreqnext=writefreq->nextFreqItem;
				qemu_free(writefreq);	
				writefreq=writefreqnext;
			}
			writefreq=qemu_malloc(sizeof(WriteFreqItem));
    			writefreq->previousFreqItem=NULL;
    			writefreq->nextFreqItem=NULL;
    			writefreq->freq=0;
    			writefreq->head=bs->writehistory0;
    			bs->writefreqHead0=writefreq;
    			bs->writefreqTail0=writefreq;

			bs->current_chunksize=bs->chunksize0;	
    			bs->current_chunksize_bit=bs->chunksize0_bit;
    			bs->current_writehistory_size=bs->writehistory_size0;
    			bs->current_writehistory= bs->writehistory0;
    			bs->current_writefreqHead=bs->writefreqHead0;
    			bs->current_writefreqTail=bs->writefreqTail0;
		}else{
			bs->writehistory0=bs->current_writehistory;
			bs->writefreqHead0=bs->current_writefreqHead;
    			bs->writefreqTail0=bs->current_writefreqTail;

			bs->chunksize1=chunksize;	
			bs->chunksize1_bit=*chunksize_bit;
			qemu_free(chunksize_bit);
			if(((bs->total_sectors) % (bs->chunksize1))!=0)	
	 			bs->writehistory_size1=((bs->total_sectors) >> (bs->chunksize1_bit))+1;
    			else
				bs->writehistory_size1=((bs->total_sectors) >> (bs->chunksize1_bit));
			if(bs->writehistory1!=NULL)			
				qemu_free(bs->writehistory1);    			
			bs->writehistory1=qemu_malloc(sizeof(WriteHistoryItem)*(bs->writehistory_size1));
			WriteFreqItem* writefreq=bs->writefreqHead1;
			while(writefreq!=NULL)
			{	WriteFreqItem* writefreqnext=writefreq->nextFreqItem;
				qemu_free(writefreq);	
				writefreq=writefreqnext;
			}
			writefreq=qemu_malloc(sizeof(WriteFreqItem));
    			writefreq->previousFreqItem=NULL;
    			writefreq->nextFreqItem=NULL;
    			writefreq->freq=0;
    			writefreq->head=bs->writehistory1;
    			bs->writefreqHead1=writefreq;
    			bs->writefreqTail1=writefreq;

			bs->current_chunksize=bs->chunksize1;	
    			bs->current_chunksize_bit=bs->chunksize1_bit;
    			bs->current_writehistory_size=bs->writehistory_size1;
    			bs->current_writehistory= bs->writehistory1;
    			bs->current_writefreqHead=bs->writefreqHead1;
    			bs->current_writefreqTail=bs->writefreqTail1;
		}
		int i=0;
		for(i=0;i<(bs->current_writehistory_size);i++){
			bs->current_writehistory[i].freq=0;
			bs->current_writehistory[i].id=i;
			bs->current_writehistory[i].myFreqItem=bs->current_writefreqHead;
			if(i==0){
				bs->current_writehistory[i].previousWHItem=NULL;
				bs->current_writehistory[i].nextWHItem=&(bs->current_writehistory[i+1]);
			}
			else if(i==(bs->current_writehistory_size-1)){
				bs->current_writehistory[i].previousWHItem=&(bs->current_writehistory[i-1]);
				bs->current_writehistory[i].nextWHItem=NULL;
			}else{
				bs->current_writehistory[i].previousWHItem=&(bs->current_writehistory[i-1]);
				bs->current_writehistory[i].nextWHItem=&(bs->current_writehistory[i+1]);
			}
    		}

		bs->history_active_id=1-bs->history_active_id;
		for(i=0;i<10;i++)
    		{
			int bitmap_size=0;
			if(((bs->current_bmap.totalchunk[i])%8)!=0)
				bitmap_size=((bs->current_bmap.totalchunk[i]) >> 3) +1;
			else
				bitmap_size=(bs->current_bmap.totalchunk[i]) >> 3;	
	
			bs->current_bmap.bp1accesschunk[i]=0;
			bs->current_bmap.bp1storagerate[i]=0;
			
			int k;
			for(k=0;k<bitmap_size;k++)
			{
				bs->current_bmap.bitmap1[i][k]=0;
			}

			if(i==0){	
			
				for(k=0;k<bitmap_size;k++)
				{
					bs->current_bmap.bitmap2[k]=0;
				}
				bs->current_bmap.bp2accesschunk=0;
			}
			bs->current_bmap.bp2coverchunk[i]=0;
			bs->current_bmap.bp2coverage[i]=0;
    		}	 	
		

	}
	
}



//Pacer Function: all write in VMMark is aio_write, so not go into this one
int bdrv_write(BlockDriverState *bs, int64_t sector_num,
               const uint8_t *buf, int nb_sectors)
{
    BlockDriver *drv = bs->drv;
    if (!bs->drv)
        return -ENOMEDIUM;
    if (bs->read_only)
        return -EACCES;
    if (bdrv_check_request(bs, sector_num, nb_sectors))
        return -EIO;

     if(bs->dirty_writehistory){
	  set_my_dirty_bitmap(bs, sector_num, nb_sectors, 1);
     }
     if (bs->dirty_bitmap) {
         set_dirty_bitmap(bs, sector_num, nb_sectors, 1);
     }

    if (bs->wr_highest_sector < sector_num + nb_sectors - 1) {
        bs->wr_highest_sector = sector_num + nb_sectors - 1;
    }
	if(bs->enable_history_tracking)
		updateHistory(1,bs, sector_num, nb_sectors);
	return drv->bdrv_write(bs, sector_num, buf, nb_sectors);
}

int bdrv_pread(BlockDriverState *bs, int64_t offset,
               void *buf, int count1)
{
    uint8_t tmp_buf[BDRV_SECTOR_SIZE];
    int len, nb_sectors, count;
    int64_t sector_num;
    int ret;

    count = count1;
    /* first read to align to sector start */
    len = (BDRV_SECTOR_SIZE - offset) & (BDRV_SECTOR_SIZE - 1);
    if (len > count)
        len = count;
    sector_num = offset >> BDRV_SECTOR_BITS;
    if (len > 0) {
        if ((ret = bdrv_read(bs, sector_num, tmp_buf, 1)) < 0)
            return ret;
        memcpy(buf, tmp_buf + (offset & (BDRV_SECTOR_SIZE - 1)), len);
        count -= len;
        if (count == 0)
            return count1;
        sector_num++;
        buf += len;
    }

    /* read the sectors "in place" */
    nb_sectors = count >> BDRV_SECTOR_BITS;
    if (nb_sectors > 0) {
        if ((ret = bdrv_read(bs, sector_num, buf, nb_sectors)) < 0)
            return ret;
        sector_num += nb_sectors;
        len = nb_sectors << BDRV_SECTOR_BITS;
        buf += len;
        count -= len;
    }

    /* add data from the last sector */
    if (count > 0) {
        if ((ret = bdrv_read(bs, sector_num, tmp_buf, 1)) < 0)
            return ret;
        memcpy(buf, tmp_buf, count);
    }
    return count1;
}

int bdrv_pwrite(BlockDriverState *bs, int64_t offset,
                const void *buf, int count1)
{
    uint8_t tmp_buf[BDRV_SECTOR_SIZE];
    int len, nb_sectors, count;
    int64_t sector_num;
    int ret;

    count = count1;
    /* first write to align to sector start */
    len = (BDRV_SECTOR_SIZE - offset) & (BDRV_SECTOR_SIZE - 1);
    if (len > count)
        len = count;
    sector_num = offset >> BDRV_SECTOR_BITS;
    if (len > 0) {
        if ((ret = bdrv_read(bs, sector_num, tmp_buf, 1)) < 0)
            return ret;
        memcpy(tmp_buf + (offset & (BDRV_SECTOR_SIZE - 1)), buf, len);
        if ((ret = bdrv_write(bs, sector_num, tmp_buf, 1)) < 0)
            return ret;
        count -= len;
        if (count == 0)
            return count1;
        sector_num++;
        buf += len;
    }

    /* write the sectors "in place" */
    nb_sectors = count >> BDRV_SECTOR_BITS;
    if (nb_sectors > 0) {
        if ((ret = bdrv_write(bs, sector_num, buf, nb_sectors)) < 0)
            return ret;
        sector_num += nb_sectors;
        len = nb_sectors << BDRV_SECTOR_BITS;
        buf += len;
        count -= len;
    }

    /* add data from the last sector */
    if (count > 0) {
        if ((ret = bdrv_read(bs, sector_num, tmp_buf, 1)) < 0)
            return ret;
        memcpy(tmp_buf, buf, count);
        if ((ret = bdrv_write(bs, sector_num, tmp_buf, 1)) < 0)
            return ret;
    }
    return count1;
}

/**
 * Truncate file to 'offset' bytes (needed only for file protocols)
 */
int bdrv_truncate(BlockDriverState *bs, int64_t offset)
{
    BlockDriver *drv = bs->drv;
    int ret;
    if (!drv)
        return -ENOMEDIUM;
    if (!drv->bdrv_truncate)
        return -ENOTSUP;
    if (bs->read_only)
        return -EACCES;
    ret = drv->bdrv_truncate(bs, offset);
    if (ret == 0) {
        ret = refresh_total_sectors(bs, offset >> BDRV_SECTOR_BITS);
    }
    return ret;
}

/**
 * Length of a file in bytes. Return < 0 if error or unknown.
 */
int64_t bdrv_getlength(BlockDriverState *bs)
{
    BlockDriver *drv = bs->drv;
    if (!drv)
        return -ENOMEDIUM;

    /* Fixed size devices use the total_sectors value for speed instead of
       issuing a length query (like lseek) on each call.  Also, legacy block
       drivers don't provide a bdrv_getlength function and must use
       total_sectors. */
    if (!bs->growable || !drv->bdrv_getlength) {
        return bs->total_sectors * BDRV_SECTOR_SIZE;
    }
    return drv->bdrv_getlength(bs);
}

/* return 0 as number of sectors if no device present or error */
void bdrv_get_geometry(BlockDriverState *bs, uint64_t *nb_sectors_ptr)
{
    int64_t length;
    length = bdrv_getlength(bs);
    if (length < 0)
        length = 0;
    else
        length = length >> BDRV_SECTOR_BITS;
    *nb_sectors_ptr = length;
}

struct partition {
        uint8_t boot_ind;           /* 0x80 - active */
        uint8_t head;               /* starting head */
        uint8_t sector;             /* starting sector */
        uint8_t cyl;                /* starting cylinder */
        uint8_t sys_ind;            /* What partition type */
        uint8_t end_head;           /* end head */
        uint8_t end_sector;         /* end sector */
        uint8_t end_cyl;            /* end cylinder */
        uint32_t start_sect;        /* starting sector counting from 0 */
        uint32_t nr_sects;          /* nr of sectors in partition */
} __attribute__((packed));

/* try to guess the disk logical geometry from the MSDOS partition table. Return 0 if OK, -1 if could not guess */
static int guess_disk_lchs(BlockDriverState *bs,
                           int *pcylinders, int *pheads, int *psectors)
{
    uint8_t buf[BDRV_SECTOR_SIZE];
    int ret, i, heads, sectors, cylinders;
    struct partition *p;
    uint32_t nr_sects;
    uint64_t nb_sectors;

    bdrv_get_geometry(bs, &nb_sectors);

    ret = bdrv_read(bs, 0, buf, 1);
    if (ret < 0)
        return -1;
    /* test msdos magic */
    if (buf[510] != 0x55 || buf[511] != 0xaa)
        return -1;
    for(i = 0; i < 4; i++) {
        p = ((struct partition *)(buf + 0x1be)) + i;
        nr_sects = le32_to_cpu(p->nr_sects);
        if (nr_sects && p->end_head) {
            /* We make the assumption that the partition terminates on
               a cylinder boundary */
            heads = p->end_head + 1;
            sectors = p->end_sector & 63;
            if (sectors == 0)
                continue;
            cylinders = nb_sectors / (heads * sectors);
            if (cylinders < 1 || cylinders > 16383)
                continue;
            *pheads = heads;
            *psectors = sectors;
            *pcylinders = cylinders;
#if 0
            printf("guessed geometry: LCHS=%d %d %d\n",
                   cylinders, heads, sectors);
#endif
            return 0;
        }
    }
    return -1;
}

void bdrv_guess_geometry(BlockDriverState *bs, int *pcyls, int *pheads, int *psecs)
{
    int translation, lba_detected = 0;
    int cylinders, heads, secs;
    uint64_t nb_sectors;

    /* if a geometry hint is available, use it */
    bdrv_get_geometry(bs, &nb_sectors);
    bdrv_get_geometry_hint(bs, &cylinders, &heads, &secs);
    translation = bdrv_get_translation_hint(bs);
    if (cylinders != 0) {
        *pcyls = cylinders;
        *pheads = heads;
        *psecs = secs;
    } else {
        if (guess_disk_lchs(bs, &cylinders, &heads, &secs) == 0) {
            if (heads > 16) {
                /* if heads > 16, it means that a BIOS LBA
                   translation was active, so the default
                   hardware geometry is OK */
                lba_detected = 1;
                goto default_geometry;
            } else {
                *pcyls = cylinders;
                *pheads = heads;
                *psecs = secs;
                /* disable any translation to be in sync with
                   the logical geometry */
                if (translation == BIOS_ATA_TRANSLATION_AUTO) {
                    bdrv_set_translation_hint(bs,
                                              BIOS_ATA_TRANSLATION_NONE);
                }
            }
        } else {
        default_geometry:
            /* if no geometry, use a standard physical disk geometry */
            cylinders = nb_sectors / (16 * 63);

            if (cylinders > 16383)
                cylinders = 16383;
            else if (cylinders < 2)
                cylinders = 2;
            *pcyls = cylinders;
            *pheads = 16;
            *psecs = 63;
            if ((lba_detected == 1) && (translation == BIOS_ATA_TRANSLATION_AUTO)) {
                if ((*pcyls * *pheads) <= 131072) {
                    bdrv_set_translation_hint(bs,
                                              BIOS_ATA_TRANSLATION_LARGE);
                } else {
                    bdrv_set_translation_hint(bs,
                                              BIOS_ATA_TRANSLATION_LBA);
                }
            }
        }
        bdrv_set_geometry_hint(bs, *pcyls, *pheads, *psecs);
    }
}

void bdrv_set_geometry_hint(BlockDriverState *bs,
                            int cyls, int heads, int secs)
{
    bs->cyls = cyls;
    bs->heads = heads;
    bs->secs = secs;
}

void bdrv_set_type_hint(BlockDriverState *bs, int type)
{
    bs->type = type;
    bs->removable = ((type == BDRV_TYPE_CDROM ||
                      type == BDRV_TYPE_FLOPPY));
}

void bdrv_set_translation_hint(BlockDriverState *bs, int translation)
{
    bs->translation = translation;
}

void bdrv_get_geometry_hint(BlockDriverState *bs,
                            int *pcyls, int *pheads, int *psecs)
{
    *pcyls = bs->cyls;
    *pheads = bs->heads;
    *psecs = bs->secs;
}

int bdrv_get_type_hint(BlockDriverState *bs)
{
    return bs->type;
}

int bdrv_get_translation_hint(BlockDriverState *bs)
{
    return bs->translation;
}

void bdrv_set_on_error(BlockDriverState *bs, BlockErrorAction on_read_error,
                       BlockErrorAction on_write_error)
{
    bs->on_read_error = on_read_error;
    bs->on_write_error = on_write_error;
}

BlockErrorAction bdrv_get_on_error(BlockDriverState *bs, int is_read)
{
    return is_read ? bs->on_read_error : bs->on_write_error;
}

int bdrv_is_removable(BlockDriverState *bs)
{
    return bs->removable;
}

int bdrv_is_read_only(BlockDriverState *bs)
{
    return bs->read_only;
}

int bdrv_is_sg(BlockDriverState *bs)
{
    return bs->sg;
}

int bdrv_enable_write_cache(BlockDriverState *bs)
{
    return bs->enable_write_cache;
}

/* XXX: no longer used */
void bdrv_set_change_cb(BlockDriverState *bs,
                        void (*change_cb)(void *opaque), void *opaque)
{
    bs->change_cb = change_cb;
    bs->change_opaque = opaque;
}

int bdrv_is_encrypted(BlockDriverState *bs)
{
    if (bs->backing_hd && bs->backing_hd->encrypted)
        return 1;
    return bs->encrypted;
}

int bdrv_key_required(BlockDriverState *bs)
{
    BlockDriverState *backing_hd = bs->backing_hd;

    if (backing_hd && backing_hd->encrypted && !backing_hd->valid_key)
        return 1;
    return (bs->encrypted && !bs->valid_key);
}

int bdrv_set_key(BlockDriverState *bs, const char *key)
{
    int ret;
    if (bs->backing_hd && bs->backing_hd->encrypted) {
        ret = bdrv_set_key(bs->backing_hd, key);
        if (ret < 0)
            return ret;
        if (!bs->encrypted)
            return 0;
    }
    if (!bs->encrypted) {
        return -EINVAL;
    } else if (!bs->drv || !bs->drv->bdrv_set_key) {
        return -ENOMEDIUM;
    }
    ret = bs->drv->bdrv_set_key(bs, key);
    if (ret < 0) {
        bs->valid_key = 0;
    } else if (!bs->valid_key) {
        bs->valid_key = 1;
        /* call the change callback now, we skipped it on open */
        bs->media_changed = 1;
        if (bs->change_cb)
            bs->change_cb(bs->change_opaque);
    }
    return ret;
}

void bdrv_get_format(BlockDriverState *bs, char *buf, int buf_size)
{
    if (!bs->drv) {
        buf[0] = '\0';
    } else {
        pstrcpy(buf, buf_size, bs->drv->format_name);
    }
}

void bdrv_iterate_format(void (*it)(void *opaque, const char *name),
                         void *opaque)
{
    BlockDriver *drv;

    QLIST_FOREACH(drv, &bdrv_drivers, list) {
        it(opaque, drv->format_name);
    }
}

BlockDriverState *bdrv_find(const char *name)
{
    BlockDriverState *bs;

    QTAILQ_FOREACH(bs, &bdrv_states, list) {
        if (!strcmp(name, bs->device_name)) {
            return bs;
        }
    }
    return NULL;
}

BlockDriverState *bdrv_next(BlockDriverState *bs)
{
    if (!bs) {
        return QTAILQ_FIRST(&bdrv_states);
    }
    return QTAILQ_NEXT(bs, list);
}

void bdrv_iterate(void (*it)(void *opaque, BlockDriverState *bs), void *opaque)
{
    BlockDriverState *bs;

    QTAILQ_FOREACH(bs, &bdrv_states, list) {
        it(opaque, bs);
    }
}
//Pacer function: for getting the total size of disk for adaptive system
int64_t bdrv_get_totallength(void)
{
    BlockDriverState *bs;
    int64_t length=0;
    QTAILQ_FOREACH(bs, &bdrv_states, list) {
        length=length+bdrv_getlength(bs);
    }
    return length;
}

const char *bdrv_get_device_name(BlockDriverState *bs)
{
    return bs->device_name;
}

void bdrv_flush(BlockDriverState *bs)
{
    if (bs->open_flags & BDRV_O_NO_FLUSH) {
        return;
    }

    if (bs->drv && bs->drv->bdrv_flush)
        bs->drv->bdrv_flush(bs);
}

void bdrv_flush_all(void)
{
    BlockDriverState *bs;

    QTAILQ_FOREACH(bs, &bdrv_states, list) {
        if (bs->drv && !bdrv_is_read_only(bs) &&
            (!bdrv_is_removable(bs) || bdrv_is_inserted(bs))) {
            bdrv_flush(bs);
        }
    }
}

int bdrv_has_zero_init(BlockDriverState *bs)
{
    assert(bs->drv);

    if (bs->drv->no_zero_init) {
        return 0;
    } else if (bs->file) {
        return bdrv_has_zero_init(bs->file);
    }

    return 1;
}

/*
 * Returns true iff the specified sector is present in the disk image. Drivers
 * not implementing the functionality are assumed to not support backing files,
 * hence all their sectors are reported as allocated.
 *
 * 'pnum' is set to the number of sectors (including and immediately following
 * the specified sector) that are known to be in the same
 * allocated/unallocated state.
 *
 * 'nb_sectors' is the max value 'pnum' should be set to.
 */
int bdrv_is_allocated(BlockDriverState *bs, int64_t sector_num, int nb_sectors,
	int *pnum)
{
    int64_t n;
    if (!bs->drv->bdrv_is_allocated) {
        if (sector_num >= bs->total_sectors) {
            *pnum = 0;
            return 0;
        }
        n = bs->total_sectors - sector_num;
        *pnum = (n < nb_sectors) ? (n) : (nb_sectors);
        return 1;
    }
    return bs->drv->bdrv_is_allocated(bs, sector_num, nb_sectors, pnum);
}

void bdrv_mon_event(const BlockDriverState *bdrv,
                    BlockMonEventAction action, int is_read)
{
    QObject *data;
    const char *action_str;

    switch (action) {
    case BDRV_ACTION_REPORT:
        action_str = "report";
        break;
    case BDRV_ACTION_IGNORE:
        action_str = "ignore";
        break;
    case BDRV_ACTION_STOP:
        action_str = "stop";
        break;
    default:
        abort();
    }

    data = qobject_from_jsonf("{ 'device': %s, 'action': %s, 'operation': %s }",
                              bdrv->device_name,
                              action_str,
                              is_read ? "read" : "write");
    monitor_protocol_event(QEVENT_BLOCK_IO_ERROR, data);

    qobject_decref(data);
}

static void bdrv_print_dict(QObject *obj, void *opaque)
{
    QDict *bs_dict;
    Monitor *mon = opaque;

    bs_dict = qobject_to_qdict(obj);

    monitor_printf(mon, "%s: type=%s removable=%d",
                        qdict_get_str(bs_dict, "device"),
                        qdict_get_str(bs_dict, "type"),
                        qdict_get_bool(bs_dict, "removable"));

    if (qdict_get_bool(bs_dict, "removable")) {
        monitor_printf(mon, " locked=%d", qdict_get_bool(bs_dict, "locked"));
    }

    if (qdict_haskey(bs_dict, "inserted")) {
        QDict *qdict = qobject_to_qdict(qdict_get(bs_dict, "inserted"));

        monitor_printf(mon, " file=");
        monitor_print_filename(mon, qdict_get_str(qdict, "file"));
        if (qdict_haskey(qdict, "backing_file")) {
            monitor_printf(mon, " backing_file=");
            monitor_print_filename(mon, qdict_get_str(qdict, "backing_file"));
        }
        monitor_printf(mon, " ro=%d drv=%s encrypted=%d",
                            qdict_get_bool(qdict, "ro"),
                            qdict_get_str(qdict, "drv"),
                            qdict_get_bool(qdict, "encrypted"));
    } else {
        monitor_printf(mon, " [not inserted]");
    }

    monitor_printf(mon, "\n");
}

void bdrv_info_print(Monitor *mon, const QObject *data)
{
    qlist_iter(qobject_to_qlist(data), bdrv_print_dict, mon);
}

void bdrv_info(Monitor *mon, QObject **ret_data)
{
    QList *bs_list;
    BlockDriverState *bs;

    bs_list = qlist_new();

    QTAILQ_FOREACH(bs, &bdrv_states, list) {
        QObject *bs_obj;
        const char *type = "unknown";

        switch(bs->type) {
        case BDRV_TYPE_HD:
            type = "hd";
            break;
        case BDRV_TYPE_CDROM:
            type = "cdrom";
            break;
        case BDRV_TYPE_FLOPPY:
            type = "floppy";
            break;
        }

        bs_obj = qobject_from_jsonf("{ 'device': %s, 'type': %s, "
                                    "'removable': %i, 'locked': %i }",
                                    bs->device_name, type, bs->removable,
                                    bs->locked);

        if (bs->drv) {
            QObject *obj;
            QDict *bs_dict = qobject_to_qdict(bs_obj);

            obj = qobject_from_jsonf("{ 'file': %s, 'ro': %i, 'drv': %s, "
                                     "'encrypted': %i }",
                                     bs->filename, bs->read_only,
                                     bs->drv->format_name,
                                     bdrv_is_encrypted(bs));
            if (bs->backing_file[0] != '\0') {
                QDict *qdict = qobject_to_qdict(obj);
                qdict_put(qdict, "backing_file",
                          qstring_from_str(bs->backing_file));
            }

            qdict_put_obj(bs_dict, "inserted", obj);
        }
        qlist_append_obj(bs_list, bs_obj);
    }

    *ret_data = QOBJECT(bs_list);
}

static void bdrv_stats_iter(QObject *data, void *opaque)
{
    QDict *qdict;
    Monitor *mon = opaque;

    qdict = qobject_to_qdict(data);
    monitor_printf(mon, "%s:", qdict_get_str(qdict, "device"));

    qdict = qobject_to_qdict(qdict_get(qdict, "stats"));
    monitor_printf(mon, " rd_bytes=%" PRId64
                        " wr_bytes=%" PRId64
                        " rd_operations=%" PRId64
                        " wr_operations=%" PRId64
                        "\n",
                        qdict_get_int(qdict, "rd_bytes"),
                        qdict_get_int(qdict, "wr_bytes"),
                        qdict_get_int(qdict, "rd_operations"),
                        qdict_get_int(qdict, "wr_operations"));
}

void bdrv_stats_print(Monitor *mon, const QObject *data)
{
    qlist_iter(qobject_to_qlist(data), bdrv_stats_iter, mon);
}

static QObject* bdrv_info_stats_bs(BlockDriverState *bs)
{
    QObject *res;
    QDict *dict;

    res = qobject_from_jsonf("{ 'stats': {"
                             "'rd_bytes': %" PRId64 ","
                             "'wr_bytes': %" PRId64 ","
                             "'rd_operations': %" PRId64 ","
                             "'wr_operations': %" PRId64 ","
                             "'wr_highest_offset': %" PRId64
                             "} }",
                             bs->rd_bytes, bs->wr_bytes,
                             bs->rd_ops, bs->wr_ops,
                             bs->wr_highest_sector *
                             (uint64_t)BDRV_SECTOR_SIZE);
    dict  = qobject_to_qdict(res);

    if (*bs->device_name) {
        qdict_put(dict, "device", qstring_from_str(bs->device_name));
    }

    if (bs->file) {
        QObject *parent = bdrv_info_stats_bs(bs->file);
        qdict_put_obj(dict, "parent", parent);
    }

    return res;
}

void bdrv_info_stats(Monitor *mon, QObject **ret_data)
{
    QObject *obj;
    QList *devices;
    BlockDriverState *bs;

    devices = qlist_new();

    QTAILQ_FOREACH(bs, &bdrv_states, list) {
        obj = bdrv_info_stats_bs(bs);
        qlist_append_obj(devices, obj);
    }

    *ret_data = QOBJECT(devices);
}

const char *bdrv_get_encrypted_filename(BlockDriverState *bs)
{
    if (bs->backing_hd && bs->backing_hd->encrypted)
        return bs->backing_file;
    else if (bs->encrypted)
        return bs->filename;
    else
        return NULL;
}

void bdrv_get_backing_filename(BlockDriverState *bs,
                               char *filename, int filename_size)
{
    if (!bs->backing_file) {
        pstrcpy(filename, filename_size, "");
    } else {
        pstrcpy(filename, filename_size, bs->backing_file);
    }
}

int bdrv_write_compressed(BlockDriverState *bs, int64_t sector_num,
                          const uint8_t *buf, int nb_sectors)
{
    BlockDriver *drv = bs->drv;
    if (!drv)
        return -ENOMEDIUM;
    if (!drv->bdrv_write_compressed)
        return -ENOTSUP;
    if (bdrv_check_request(bs, sector_num, nb_sectors))
        return -EIO;

   if(bs->dirty_writehistory){
	  set_my_dirty_bitmap(bs, sector_num, nb_sectors, 1);
    }

    if (bs->dirty_bitmap) 
         set_dirty_bitmap(bs, sector_num, nb_sectors, 1);

    if(bs->enable_history_tracking)
	updateHistory(1,bs, sector_num, nb_sectors);

    return drv->bdrv_write_compressed(bs, sector_num, buf, nb_sectors);
}

int bdrv_get_info(BlockDriverState *bs, BlockDriverInfo *bdi)
{
    BlockDriver *drv = bs->drv;
    if (!drv)
        return -ENOMEDIUM;
    if (!drv->bdrv_get_info)
        return -ENOTSUP;
    memset(bdi, 0, sizeof(*bdi));
    return drv->bdrv_get_info(bs, bdi);
}

int bdrv_save_vmstate(BlockDriverState *bs, const uint8_t *buf,
                      int64_t pos, int size)
{
    BlockDriver *drv = bs->drv;
    if (!drv)
        return -ENOMEDIUM;
    if (drv->bdrv_save_vmstate)
        return drv->bdrv_save_vmstate(bs, buf, pos, size);
    if (bs->file)
        return bdrv_save_vmstate(bs->file, buf, pos, size);
    return -ENOTSUP;
}

int bdrv_load_vmstate(BlockDriverState *bs, uint8_t *buf,
                      int64_t pos, int size)
{
    BlockDriver *drv = bs->drv;
    if (!drv)
        return -ENOMEDIUM;
    if (drv->bdrv_load_vmstate)
        return drv->bdrv_load_vmstate(bs, buf, pos, size);
    if (bs->file)
        return bdrv_load_vmstate(bs->file, buf, pos, size);
    return -ENOTSUP;
}

void bdrv_debug_event(BlockDriverState *bs, BlkDebugEvent event)
{
    BlockDriver *drv = bs->drv;

    if (!drv || !drv->bdrv_debug_event) {
        return;
    }

    return drv->bdrv_debug_event(bs, event);

}

/**************************************************************/
/* handling of snapshots */

int bdrv_can_snapshot(BlockDriverState *bs)
{
    BlockDriver *drv = bs->drv;
    if (!drv || bdrv_is_removable(bs) || bdrv_is_read_only(bs)) {
        return 0;
    }

    if (!drv->bdrv_snapshot_create) {
        if (bs->file != NULL) {
            return bdrv_can_snapshot(bs->file);
        }
        return 0;
    }

    return 1;
}

int bdrv_snapshot_create(BlockDriverState *bs,
                         QEMUSnapshotInfo *sn_info)
{
    BlockDriver *drv = bs->drv;
    if (!drv)
        return -ENOMEDIUM;
    if (drv->bdrv_snapshot_create)
        return drv->bdrv_snapshot_create(bs, sn_info);
    if (bs->file)
        return bdrv_snapshot_create(bs->file, sn_info);
    return -ENOTSUP;
}

int bdrv_snapshot_goto(BlockDriverState *bs,
                       const char *snapshot_id)
{
    BlockDriver *drv = bs->drv;
    int ret, open_ret;

    if (!drv)
        return -ENOMEDIUM;
    if (drv->bdrv_snapshot_goto)
        return drv->bdrv_snapshot_goto(bs, snapshot_id);

    if (bs->file) {
        drv->bdrv_close(bs);
        ret = bdrv_snapshot_goto(bs->file, snapshot_id);
        open_ret = drv->bdrv_open(bs, bs->open_flags);
        if (open_ret < 0) {
            bdrv_delete(bs->file);
            bs->drv = NULL;
            return open_ret;
        }
        return ret;
    }

    return -ENOTSUP;
}

int bdrv_snapshot_delete(BlockDriverState *bs, const char *snapshot_id)
{
    BlockDriver *drv = bs->drv;
    if (!drv)
        return -ENOMEDIUM;
    if (drv->bdrv_snapshot_delete)
        return drv->bdrv_snapshot_delete(bs, snapshot_id);
    if (bs->file)
        return bdrv_snapshot_delete(bs->file, snapshot_id);
    return -ENOTSUP;
}

int bdrv_snapshot_list(BlockDriverState *bs,
                       QEMUSnapshotInfo **psn_info)
{
    BlockDriver *drv = bs->drv;
    if (!drv)
        return -ENOMEDIUM;
    if (drv->bdrv_snapshot_list)
        return drv->bdrv_snapshot_list(bs, psn_info);
    if (bs->file)
        return bdrv_snapshot_list(bs->file, psn_info);
    return -ENOTSUP;
}

#define NB_SUFFIXES 4

char *get_human_readable_size(char *buf, int buf_size, int64_t size)
{
    static const char suffixes[NB_SUFFIXES] = "KMGT";
    int64_t base;
    int i;

    if (size <= 999) {
        snprintf(buf, buf_size, "%" PRId64, size);
    } else {
        base = 1024;
        for(i = 0; i < NB_SUFFIXES; i++) {
            if (size < (10 * base)) {
                snprintf(buf, buf_size, "%0.1f%c",
                         (double)size / base,
                         suffixes[i]);
                break;
            } else if (size < (1000 * base) || i == (NB_SUFFIXES - 1)) {
                snprintf(buf, buf_size, "%" PRId64 "%c",
                         ((size + (base >> 1)) / base),
                         suffixes[i]);
                break;
            }
            base = base * 1024;
        }
    }
    return buf;
}

char *bdrv_snapshot_dump(char *buf, int buf_size, QEMUSnapshotInfo *sn)
{
    char buf1[128], date_buf[128], clock_buf[128];
#ifdef _WIN32
    struct tm *ptm;
#else
    struct tm tm;
#endif
    time_t ti;
    int64_t secs;

    if (!sn) {
        snprintf(buf, buf_size,
                 "%-10s%-20s%7s%20s%15s",
                 "ID", "TAG", "VM SIZE", "DATE", "VM CLOCK");
    } else {
        ti = sn->date_sec;
#ifdef _WIN32
        ptm = localtime(&ti);
        strftime(date_buf, sizeof(date_buf),
                 "%Y-%m-%d %H:%M:%S", ptm);
#else
        localtime_r(&ti, &tm);
        strftime(date_buf, sizeof(date_buf),
                 "%Y-%m-%d %H:%M:%S", &tm);
#endif
        secs = sn->vm_clock_nsec / 1000000000;
        snprintf(clock_buf, sizeof(clock_buf),
                 "%02d:%02d:%02d.%03d",
                 (int)(secs / 3600),
                 (int)((secs / 60) % 60),
                 (int)(secs % 60),
                 (int)((sn->vm_clock_nsec / 1000000) % 1000));
        snprintf(buf, buf_size,
                 "%-10s%-20s%7s%20s%15s",
                 sn->id_str, sn->name,
                 get_human_readable_size(buf1, sizeof(buf1), sn->vm_state_size),
                 date_buf,
                 clock_buf);
    }
    return buf;
}


/**************************************************************/
// Pacer function: add for latency measurement for app io
BlockDriverAIOCB *my_bdrv_aio_readv(BlockDriverState *bs, int64_t sector_num,
                                 QEMUIOVector *qiov, int nb_sectors,
                                 BlockDriverCompletionFunc *cb, void *opaque)
{
     bs->migration=1;
     BlockDriverAIOCB *mycb=bdrv_aio_readv(bs,sector_num,qiov,nb_sectors,cb,opaque);
     bs->migration=0;
     return mycb;
}

//add refer to qemu maillist for AIO write dirty block setting after write finish.

typedef struct BlockCompleteData {
    BlockDriverCompletionFunc *cb;
    void *opaque;
    BlockDriverState *bs;
    int64_t sector_num;
    int nb_sectors;
} BlockCompleteData;

static void block_complete_cb(void *opaque, int ret)
{
    BlockCompleteData *b = opaque;

    if (b->bs->dirty_bitmap) {
        set_dirty_bitmap(b->bs, b->sector_num, b->nb_sectors, 1);
    }

    if (b->bs->write_block_laccesstime){
        update_write_block_distance(b->bs,b->sector_num,b->nb_sectors);
    }
    b->cb(b->opaque, ret);
    qemu_free(b);
}

static BlockCompleteData *blk_dirty_cb_alloc(BlockDriverState *bs,
                                             int64_t sector_num,
                                             int nb_sectors,
                                             BlockDriverCompletionFunc *cb,
                                             void *opaque)
{
    BlockCompleteData *blkdata = qemu_mallocz(sizeof(BlockCompleteData));

    blkdata->bs = bs;
    blkdata->cb = cb;
    blkdata->opaque = opaque;
    blkdata->sector_num = sector_num;
    blkdata->nb_sectors = nb_sectors;

    return blkdata;
}

//end

/* async I/Os */

BlockDriverAIOCB *bdrv_aio_readv(BlockDriverState *bs, int64_t sector_num,
                                 QEMUIOVector *qiov, int nb_sectors,
                                 BlockDriverCompletionFunc *cb, void *opaque)
{
    BlockDriver *drv = bs->drv;
    BlockDriverAIOCB *ret;
    if(bs->migration==0)
    	bs->timestamp_write=my_qemu_get_clock_us(rt_clock);
    else
        bs->timestamp_write=0;
    
    if (!drv){
        return NULL;

    }

    if (bdrv_check_request(bs, sector_num, nb_sectors)){
          return NULL;
    }
    
    ret = drv->bdrv_aio_readv(bs, sector_num, qiov, nb_sectors,
                              cb, opaque);
    if (ret) {
	/* Update stats even though technically transfer has not happened. */
	bs->rd_bytes += (unsigned) nb_sectors * BDRV_SECTOR_SIZE;
	bs->rd_ops ++;
    }
   
    return ret;
}


//Pacer modification: VMMark write is this one, so focus on this one
BlockDriverAIOCB *bdrv_aio_writev(BlockDriverState *bs, int64_t sector_num,
                                  QEMUIOVector *qiov, int nb_sectors,
                                  BlockDriverCompletionFunc *cb, void *opaque)
{
    bs->enable_throttle=enable_throttling;

    bs->timestamp_write=my_qemu_get_clock_us(rt_clock);

    BlockDriver *drv = bs->drv;
    BlockDriverAIOCB *ret;
    BlockCompleteData *blk_cb_data;

    if (!drv){
        return NULL;
    }

    if (bs->read_only){
	return NULL;
    }
   
    if (bdrv_check_request(bs, sector_num, nb_sectors))
    {
	return NULL;
    }
     if(bs->dirty_writehistory)
	set_my_dirty_bitmap(bs, sector_num, nb_sectors, 1);
     if (bs->dirty_bitmap){ 
        // set_dirty_bitmap(bs, sector_num, nb_sectors, 1);
         blk_cb_data = blk_dirty_cb_alloc(bs, sector_num, nb_sectors, cb,
                                         opaque);
         cb = &block_complete_cb;
         opaque = blk_cb_data;

     }
     if(bs->enable_throttle&&bs->dirty_bitmap)
	set_throttle_delay(bs,sector_num,nb_sectors);
     bs->tempusleep=throttle_usleep;
	
     if(bs->enable_history_tracking)
   	 updateHistory(1,bs, sector_num, nb_sectors);


     ret = drv->bdrv_aio_writev(bs, sector_num, qiov, nb_sectors,
                               cb, opaque);

     if (ret) {
        /* Update stats even though technically transfer has not happened. */
	
		bs->wr_bytes += (unsigned) nb_sectors * BDRV_SECTOR_SIZE;
       	 	bs->wr_ops ++;
        	if (bs->wr_highest_sector < sector_num + nb_sectors - 1) {
            		bs->wr_highest_sector = sector_num + nb_sectors - 1;
        	}
     }

    return ret;
}


typedef struct MultiwriteCB {
    int error;
    int num_requests;
    int num_callbacks;
    struct {
        BlockDriverCompletionFunc *cb;
        void *opaque;
        QEMUIOVector *free_qiov;
        void *free_buf;
    } callbacks[];
} MultiwriteCB;

static void multiwrite_user_cb(MultiwriteCB *mcb)
{
    int i;

    for (i = 0; i < mcb->num_callbacks; i++) {
        mcb->callbacks[i].cb(mcb->callbacks[i].opaque, mcb->error);
        if (mcb->callbacks[i].free_qiov) {
            qemu_iovec_destroy(mcb->callbacks[i].free_qiov);
        }
        qemu_free(mcb->callbacks[i].free_qiov);
        qemu_vfree(mcb->callbacks[i].free_buf);
    }
}

static void multiwrite_cb(void *opaque, int ret)
{
    MultiwriteCB *mcb = opaque;

    if (ret < 0 && !mcb->error) {
        mcb->error = ret;
        multiwrite_user_cb(mcb);
    }

    mcb->num_requests--;
    if (mcb->num_requests == 0) {
        if (mcb->error == 0) {
            multiwrite_user_cb(mcb);
        }
        qemu_free(mcb);
    }
}

static int multiwrite_req_compare(const void *a, const void *b)
{
    const BlockRequest *req1 = a, *req2 = b;

    /*
     * Note that we can't simply subtract req2->sector from req1->sector
     * here as that could overflow the return value.
     */
    if (req1->sector > req2->sector) {
        return 1;
    } else if (req1->sector < req2->sector) {
        return -1;
    } else {
        return 0;
    }
}

/*
 * Takes a bunch of requests and tries to merge them. Returns the number of
 * requests that remain after merging.
 */
static int multiwrite_merge(BlockDriverState *bs, BlockRequest *reqs,
    int num_reqs, MultiwriteCB *mcb)
{
    int i, outidx;

    // Sort requests by start sector
    qsort(reqs, num_reqs, sizeof(*reqs), &multiwrite_req_compare);

    // Check if adjacent requests touch the same clusters. If so, combine them,
    // filling up gaps with zero sectors.
    outidx = 0;
    for (i = 1; i < num_reqs; i++) {
        int merge = 0;
        int64_t oldreq_last = reqs[outidx].sector + reqs[outidx].nb_sectors;

        // This handles the cases that are valid for all block drivers, namely
        // exactly sequential writes and overlapping writes.
        if (reqs[i].sector <= oldreq_last) {
            merge = 1;
        }

        // The block driver may decide that it makes sense to combine requests
        // even if there is a gap of some sectors between them. In this case,
        // the gap is filled with zeros (therefore only applicable for yet
        // unused space in format like qcow2).
        if (!merge && bs->drv->bdrv_merge_requests) {
            merge = bs->drv->bdrv_merge_requests(bs, &reqs[outidx], &reqs[i]);
        }

        if (reqs[outidx].qiov->niov + reqs[i].qiov->niov + 1 > IOV_MAX) {
            merge = 0;
        }

        if (merge) {
            size_t size;
            QEMUIOVector *qiov = qemu_mallocz(sizeof(*qiov));
            qemu_iovec_init(qiov,
                reqs[outidx].qiov->niov + reqs[i].qiov->niov + 1);

            // Add the first request to the merged one. If the requests are
            // overlapping, drop the last sectors of the first request.
            size = (reqs[i].sector - reqs[outidx].sector) << 9;
            qemu_iovec_concat(qiov, reqs[outidx].qiov, size);

            // We might need to add some zeros between the two requests
            if (reqs[i].sector > oldreq_last) {
                size_t zero_bytes = (reqs[i].sector - oldreq_last) << 9;
                uint8_t *buf = qemu_blockalign(bs, zero_bytes);
                memset(buf, 0, zero_bytes);
                qemu_iovec_add(qiov, buf, zero_bytes);
                mcb->callbacks[i].free_buf = buf;
            }

            // Add the second request
            qemu_iovec_concat(qiov, reqs[i].qiov, reqs[i].qiov->size);

            reqs[outidx].nb_sectors = qiov->size >> 9;
            reqs[outidx].qiov = qiov;

            mcb->callbacks[i].free_qiov = reqs[outidx].qiov;
        } else {
            outidx++;
            reqs[outidx].sector     = reqs[i].sector;
            reqs[outidx].nb_sectors = reqs[i].nb_sectors;
            reqs[outidx].qiov       = reqs[i].qiov;
        }
    }

    return outidx + 1;
}

/*
 * Submit multiple AIO write requests at once.
 *
 * On success, the function returns 0 and all requests in the reqs array have
 * been submitted. In error case this function returns -1, and any of the
 * requests may or may not be submitted yet. In particular, this means that the
 * callback will be called for some of the requests, for others it won't. The
 * caller must check the error field of the BlockRequest to wait for the right
 * callbacks (if error != 0, no callback will be called).
 *
 * The implementation may modify the contents of the reqs array, e.g. to merge
 * requests. However, the fields opaque and error are left unmodified as they
 * are used to signal failure for a single request to the caller.
 */
int bdrv_aio_multiwrite(BlockDriverState *bs, BlockRequest *reqs, int num_reqs)
{
    //printf("bdrv_aio_multiwrite\n");
    BlockDriverAIOCB *acb;
    MultiwriteCB *mcb;
    int i;

    if (num_reqs == 0) {
        return 0;
    }

    // Create MultiwriteCB structure
    mcb = qemu_mallocz(sizeof(*mcb) + num_reqs * sizeof(*mcb->callbacks));
    mcb->num_requests = 0;
    mcb->num_callbacks = num_reqs;

    for (i = 0; i < num_reqs; i++) {
        mcb->callbacks[i].cb = reqs[i].cb;
        mcb->callbacks[i].opaque = reqs[i].opaque;
    }

    // Check for mergable requests
    num_reqs = multiwrite_merge(bs, reqs, num_reqs, mcb);

    // Run the aio requests
    for (i = 0; i < num_reqs; i++) {
        acb = bdrv_aio_writev(bs, reqs[i].sector, reqs[i].qiov,
            reqs[i].nb_sectors, multiwrite_cb, mcb);

        if (acb == NULL) {
            // We can only fail the whole thing if no request has been
            // submitted yet. Otherwise we'll wait for the submitted AIOs to
            // complete and report the error in the callback.
            if (mcb->num_requests == 0) {
                reqs[i].error = -EIO;
                goto fail;
            } else {
                mcb->num_requests++;
                multiwrite_cb(mcb, -EIO);
                break;
            }
        } else {
            mcb->num_requests++;
        }
    }

    return 0;

fail:
    qemu_free(mcb);
    return -1;
}

BlockDriverAIOCB *bdrv_aio_flush(BlockDriverState *bs,
        BlockDriverCompletionFunc *cb, void *opaque)
{
    BlockDriver *drv = bs->drv;

    if (bs->open_flags & BDRV_O_NO_FLUSH) {
        return bdrv_aio_noop_em(bs, cb, opaque);
    }

    if (!drv)
        return NULL;
    return drv->bdrv_aio_flush(bs, cb, opaque);
}

void bdrv_aio_cancel(BlockDriverAIOCB *acb)
{
    acb->pool->cancel(acb);
}


/**************************************************************/
/* async block device emulation */

typedef struct BlockDriverAIOCBSync {
    BlockDriverAIOCB common;
    QEMUBH *bh;
    int ret;
    /* vector translation state */
    QEMUIOVector *qiov;
    uint8_t *bounce;
    int is_write;
} BlockDriverAIOCBSync;

static void bdrv_aio_cancel_em(BlockDriverAIOCB *blockacb)
{
    BlockDriverAIOCBSync *acb =
        container_of(blockacb, BlockDriverAIOCBSync, common);
    qemu_bh_delete(acb->bh);
    acb->bh = NULL;
    qemu_aio_release(acb);
}

static AIOPool bdrv_em_aio_pool = {
    .aiocb_size         = sizeof(BlockDriverAIOCBSync),
    .cancel             = bdrv_aio_cancel_em,
};

static void bdrv_aio_bh_cb(void *opaque)
{
    BlockDriverAIOCBSync *acb = opaque;

    if (!acb->is_write)
        qemu_iovec_from_buffer(acb->qiov, acb->bounce, acb->qiov->size);
    qemu_vfree(acb->bounce);
    acb->common.cb(acb->common.opaque, acb->ret);
    qemu_bh_delete(acb->bh);
    acb->bh = NULL;
    qemu_aio_release(acb);
}

static BlockDriverAIOCB *bdrv_aio_rw_vector(BlockDriverState *bs,
                                            int64_t sector_num,
                                            QEMUIOVector *qiov,
                                            int nb_sectors,
                                            BlockDriverCompletionFunc *cb,
                                            void *opaque,
                                            int is_write)

{
    BlockDriverAIOCBSync *acb;

    acb = qemu_aio_get(&bdrv_em_aio_pool, bs, cb, opaque);
    acb->is_write = is_write;
    acb->qiov = qiov;
    acb->bounce = qemu_blockalign(bs, qiov->size);

    if (!acb->bh)
        acb->bh = qemu_bh_new(bdrv_aio_bh_cb, acb);

    if (is_write) {
        qemu_iovec_to_buffer(acb->qiov, acb->bounce);
        acb->ret = bdrv_write(bs, sector_num, acb->bounce, nb_sectors);
    } else {
        acb->ret = bdrv_read(bs, sector_num, acb->bounce, nb_sectors);
    }

    qemu_bh_schedule(acb->bh);

    return &acb->common;
}

static BlockDriverAIOCB *bdrv_aio_readv_em(BlockDriverState *bs,
        int64_t sector_num, QEMUIOVector *qiov, int nb_sectors,
        BlockDriverCompletionFunc *cb, void *opaque)
{
    printf("bdrv_aio_readv_em\n");
    return bdrv_aio_rw_vector(bs, sector_num, qiov, nb_sectors, cb, opaque, 0);
}

static BlockDriverAIOCB *bdrv_aio_writev_em(BlockDriverState *bs,
        int64_t sector_num, QEMUIOVector *qiov, int nb_sectors,
        BlockDriverCompletionFunc *cb, void *opaque)
{
    return bdrv_aio_rw_vector(bs, sector_num, qiov, nb_sectors, cb, opaque, 1);
}

static BlockDriverAIOCB *bdrv_aio_flush_em(BlockDriverState *bs,
        BlockDriverCompletionFunc *cb, void *opaque)
{
    BlockDriverAIOCBSync *acb;

    acb = qemu_aio_get(&bdrv_em_aio_pool, bs, cb, opaque);
    acb->is_write = 1; /* don't bounce in the completion hadler */
    acb->qiov = NULL;
    acb->bounce = NULL;
    acb->ret = 0;

    if (!acb->bh)
        acb->bh = qemu_bh_new(bdrv_aio_bh_cb, acb);

    bdrv_flush(bs);
    qemu_bh_schedule(acb->bh);
    return &acb->common;
}

static BlockDriverAIOCB *bdrv_aio_noop_em(BlockDriverState *bs,
        BlockDriverCompletionFunc *cb, void *opaque)
{
    BlockDriverAIOCBSync *acb;

    acb = qemu_aio_get(&bdrv_em_aio_pool, bs, cb, opaque);
    acb->is_write = 1; /* don't bounce in the completion handler */
    acb->qiov = NULL;
    acb->bounce = NULL;
    acb->ret = 0;

    if (!acb->bh) {
        acb->bh = qemu_bh_new(bdrv_aio_bh_cb, acb);
    }

    qemu_bh_schedule(acb->bh);
    return &acb->common;
}

/**************************************************************/
/* sync block device emulation */

static void bdrv_rw_em_cb(void *opaque, int ret)
{
    *(int *)opaque = ret;
}

#define NOT_DONE 0x7fffffff

static int bdrv_read_em(BlockDriverState *bs, int64_t sector_num,
                        uint8_t *buf, int nb_sectors)
{
    printf("bdrv_read_em\n");
    int async_ret;
    BlockDriverAIOCB *acb;
    struct iovec iov;
    QEMUIOVector qiov;

    async_context_push();

    async_ret = NOT_DONE;
    iov.iov_base = (void *)buf;
    iov.iov_len = nb_sectors * BDRV_SECTOR_SIZE;
    qemu_iovec_init_external(&qiov, &iov, 1);
    acb = bdrv_aio_readv(bs, sector_num, &qiov, nb_sectors,
        bdrv_rw_em_cb, &async_ret);
    if (acb == NULL) {
        async_ret = -1;
        goto fail;
    }

    while (async_ret == NOT_DONE) {
        qemu_aio_wait();
    }


fail:
    async_context_pop();
    return async_ret;
}

static int bdrv_write_em(BlockDriverState *bs, int64_t sector_num,
                         const uint8_t *buf, int nb_sectors)
{
    int async_ret;
    BlockDriverAIOCB *acb;
    struct iovec iov;
    QEMUIOVector qiov;

    async_context_push();

    async_ret = NOT_DONE;
    iov.iov_base = (void *)buf;
    iov.iov_len = nb_sectors * BDRV_SECTOR_SIZE;
    qemu_iovec_init_external(&qiov, &iov, 1);
    acb = bdrv_aio_writev(bs, sector_num, &qiov, nb_sectors,
        bdrv_rw_em_cb, &async_ret);
    if (acb == NULL) {
        async_ret = -1;
        goto fail;
    }
    while (async_ret == NOT_DONE) {
        qemu_aio_wait();
    }

fail:
    async_context_pop();
    return async_ret;
}

void bdrv_init(void)
{
    module_call_init(MODULE_INIT_BLOCK);
}

void bdrv_init_with_whitelist(void)
{
    use_bdrv_whitelist = 1;
    bdrv_init();
}

void *qemu_aio_get(AIOPool *pool, BlockDriverState *bs,
                   BlockDriverCompletionFunc *cb, void *opaque)
{
    BlockDriverAIOCB *acb;

    if (pool->free_aiocb) {
        acb = pool->free_aiocb;
        pool->free_aiocb = acb->next;
    } else {
        acb = qemu_mallocz(pool->aiocb_size);
        acb->pool = pool;
    }
    acb->bs = bs;
    acb->cb = cb;
    acb->opaque = opaque;
    return acb;
}

void qemu_aio_release(void *p)
{
    BlockDriverAIOCB *acb = (BlockDriverAIOCB *)p;
    AIOPool *pool = acb->pool;
    acb->next = pool->free_aiocb;
    pool->free_aiocb = acb;
}

/**************************************************************/
/* removable device support */

/**
 * Return TRUE if the media is present
 */
int bdrv_is_inserted(BlockDriverState *bs)
{
    BlockDriver *drv = bs->drv;
    int ret;
    if (!drv)
        return 0;
    if (!drv->bdrv_is_inserted)
        return 1;
    ret = drv->bdrv_is_inserted(bs);
    return ret;
}

/**
 * Return TRUE if the media changed since the last call to this
 * function. It is currently only used for floppy disks
 */
int bdrv_media_changed(BlockDriverState *bs)
{
    BlockDriver *drv = bs->drv;
    int ret;

    if (!drv || !drv->bdrv_media_changed)
        ret = -ENOTSUP;
    else
        ret = drv->bdrv_media_changed(bs);
    if (ret == -ENOTSUP)
        ret = bs->media_changed;
    bs->media_changed = 0;
    return ret;
}

/**
 * If eject_flag is TRUE, eject the media. Otherwise, close the tray
 */
int bdrv_eject(BlockDriverState *bs, int eject_flag)
{
    BlockDriver *drv = bs->drv;
    int ret;

    if (bs->locked) {
        return -EBUSY;
    }

    if (!drv || !drv->bdrv_eject) {
        ret = -ENOTSUP;
    } else {
        ret = drv->bdrv_eject(bs, eject_flag);
    }
    if (ret == -ENOTSUP) {
        if (eject_flag)
            bdrv_close(bs);
        ret = 0;
    }

    return ret;
}

int bdrv_is_locked(BlockDriverState *bs)
{
    return bs->locked;
}

/**
 * Lock or unlock the media (if it is locked, the user won't be able
 * to eject it manually).
 */
void bdrv_set_locked(BlockDriverState *bs, int locked)
{
    BlockDriver *drv = bs->drv;

    bs->locked = locked;
    if (drv && drv->bdrv_set_locked) {
        drv->bdrv_set_locked(bs, locked);
    }
}

/* needed for generic scsi interface */

int bdrv_ioctl(BlockDriverState *bs, unsigned long int req, void *buf)
{
    BlockDriver *drv = bs->drv;

    if (drv && drv->bdrv_ioctl)
        return drv->bdrv_ioctl(bs, req, buf);
    return -ENOTSUP;
}

BlockDriverAIOCB *bdrv_aio_ioctl(BlockDriverState *bs,
        unsigned long int req, void *buf,
        BlockDriverCompletionFunc *cb, void *opaque)
{
    BlockDriver *drv = bs->drv;

    if (drv && drv->bdrv_aio_ioctl)
        return drv->bdrv_aio_ioctl(bs, req, buf, cb, opaque);
    return NULL;
}



void *qemu_blockalign(BlockDriverState *bs, size_t size)
{
    return qemu_memalign((bs && bs->buffer_alignment) ? bs->buffer_alignment : 512, size);
}


/* Pacer Function:set dirty tracking*/
void bdrv_set_dirty_tracking(BlockDriverState *bs, int enable)
{
    long int bitmap_size;
    bs->dirty_count = 0;
    if (enable) {
	if(!(bs->enable_dirty_scheduling)){
        	if (!bs->dirty_bitmap) {
            		bitmap_size = (bdrv_getlength(bs) >> BDRV_SECTOR_BITS) +
               	    	BDRV_SECTORS_PER_DIRTY_CHUNK * 8 - 1;
            		bitmap_size /= BDRV_SECTORS_PER_DIRTY_CHUNK * 8;
            		bs->dirty_bitmap = qemu_mallocz(bitmap_size);
                        int i=0;
                        for(i=0;i<bitmap_size/8;i++)
                           bs->dirty_bitmap[i]=0;

                        bs->totalblock=bitmap_size*8;

                        bs->write_block_laccesstime=qemu_mallocz((bs->totalblock)*sizeof(unsigned long));
                        bs->write_block_distance=qemu_mallocz((bs->totalblock)*sizeof(unsigned long));
                        bs->write_block_count=qemu_mallocz((bs->totalblock)*sizeof(unsigned long));
                        bs->write_block_avedist=qemu_mallocz((bs->totalblock)*sizeof(unsigned long));
                        bs->write_block_markmap=qemu_malloc((bs->totalblock)*sizeof(unsigned char));
                        bs->write_block_variance=qemu_malloc((bs->totalblock)*sizeof(unsigned long));
			bs->write_block_dirty_accesstime=qemu_mallocz((bs->totalblock)*sizeof(unsigned long));

                        for(i=0;i<bs->totalblock;i++)
                        {
                            bs->write_block_laccesstime[i]=0;
                            bs->write_block_distance[i]=0;
                            bs->write_block_count[i]=0; 
                            bs->write_block_avedist[i]=0;
                            bs->write_block_markmap[i]=0;
                            bs->write_block_variance[i]=0;
			    bs->write_block_dirty_accesstime[i]=0;
                        }                         
 
        	}
	}else{
	 if (!bs->dirty_writehistory) {
		bs->dirty_chunksize=BDRV_SECTORS_PER_DIRTY_CHUNK;
	    	bs->dirty_chunksize_bit=BDRV_SECTORS_PER_DIRTY_CHUNK_BIT;
    		if(((bs->total_sectors) % (bs->dirty_chunksize))!=0)	
		 	bs->dirty_writehistory_size=((bs->total_sectors) >> (bs->dirty_chunksize_bit))+1;
    		else
			bs->dirty_writehistory_size=((bs->total_sectors) >> (bs->dirty_chunksize_bit));
   		bs->dirty_writehistory=qemu_malloc(sizeof(WriteHistoryItem)*(bs->dirty_writehistory_size));
    		WriteFreqItem* writefreq=qemu_malloc(sizeof(WriteFreqItem));
   		writefreq->previousFreqItem=NULL;
    		writefreq->nextFreqItem=NULL;
    		writefreq->freq=0;
    		writefreq->head=bs->dirty_writehistory;
    		bs->dirty_writefreqHead=writefreq;
    		bs->dirty_writefreqTail=writefreq;
		
		int i=0;
    		for(i=0;i<(bs->dirty_writehistory_size);i++){
			bs->dirty_writehistory[i].freq=0;
			bs->dirty_writehistory[i].id=i;
			bs->dirty_writehistory[i].myFreqItem=bs->dirty_writefreqHead;
			if(i==0){
				bs->dirty_writehistory[i].previousWHItem=NULL;
				bs->dirty_writehistory[i].nextWHItem=&(bs->dirty_writehistory[i+1]);
			}
			else if(i==(bs->dirty_writehistory_size-1)){
				bs->dirty_writehistory[i].previousWHItem=&(bs->dirty_writehistory[i-1]);
				bs->dirty_writehistory[i].nextWHItem=NULL;
			}else{
				bs->dirty_writehistory[i].previousWHItem=&(bs->dirty_writehistory[i-1]);
				bs->dirty_writehistory[i].nextWHItem=&(bs->dirty_writehistory[i+1]);
			}
		}
	   }
	}
    } else {

        if (bs->dirty_writehistory) {
            qemu_free(bs->dirty_writehistory);
           bs->dirty_writehistory = NULL;
        }
	if (bs->dirty_bitmap) {
	    qemu_free(bs->dirty_bitmap);
	    bs->dirty_bitmap = NULL;
	}
    }
}

/* Pacer Function: set write throttling*/
void bdrv_set_write_throttling(BlockDriverState *bs, int enable, int64_t speed)
{
	if(enable){
            throttle_dirtyrate=speed;
	    enable_throttling=1;	
	    printf("set throttling dirty rate to be %"PRId64"\n",throttle_dirtyrate);
	    throttle_starttime=my_qemu_get_clock_us(rt_clock);
          
	}else{
           enable_throttling=0;
           throttle_usleep=0;
        }
}
void bdrv_set_history_tracking(BlockDriverState *bs, int enable)
{
   bs->enable_history_tracking = enable;
}
void bdrv_set_dirty_scheduling(BlockDriverState *bs, int enable)
{
   bs->enable_dirty_scheduling = enable;
}


int bdrv_get_dirty(BlockDriverState *bs, int64_t sector)
{
    int64_t chunk = sector / (int64_t)BDRV_SECTORS_PER_DIRTY_CHUNK;

    if (bs->dirty_bitmap &&
        (sector << BDRV_SECTOR_BITS) < bdrv_getlength(bs)) { 
			//Pacer: when 2000/2048,chunk1 is dirty, but chunk 0 is clean that is wrong, when get chunk 0
        return !!(bs->dirty_bitmap[chunk / (sizeof(unsigned long) * 8)] &
            (1UL << (chunk % (sizeof(unsigned long) * 8))));
    } else {
        return 0;
    }
}


void bdrv_set_dirty(BlockDriverState *bs, int64_t cur_sector,
                      int nr_sectors)
{
        if(bs->enable_dirty_scheduling)
            set_my_dirty_bitmap(bs, cur_sector, nr_sectors, 1);
        else
            set_dirty_bitmap(bs, cur_sector, nr_sectors, 1);
}


void bdrv_reset_dirty(BlockDriverState *bs, int64_t cur_sector,
                      int nr_sectors)
{
	if(bs->enable_dirty_scheduling)
   	    set_my_dirty_bitmap(bs, cur_sector, nr_sectors, 0);
	else
	    set_dirty_bitmap(bs, cur_sector, nr_sectors, 0);
}


int64_t bdrv_get_dirty_count(BlockDriverState *bs)
{
    return bs->dirty_count;
}
