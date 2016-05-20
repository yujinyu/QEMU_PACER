/*
 * QEMU live block migration
 *
 * Copyright IBM, Corp. 2009
 *
 * Authors:
 *  Liran Schour   <lirans@il.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
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
 *
 */

#include "qemu-common.h"
#include "block_int.h"
#include "hw/hw.h"
#include "qemu-queue.h"
#include "qemu-timer.h"
#include "monitor.h"
#include "block-migration.h"
#include "migration.h"
#include <assert.h>
/* Libraries used by Pacer */
#include "time.h"
#include <zlib.h>
#include "pthread.h"
#include "qdict.h"
#include "qint.h"
#include "qjson.h"
 /* End */

#define BULK_BLOCK_SIZE (BDRV_SECTORS_PER_BULK_CHUNK << BDRV_SECTOR_BITS) 
#define DIRTY_BLOCK_SIZE (BDRV_SECTORS_PER_DIRTY_CHUNK << BDRV_SECTOR_BITS)		// Pacer Constant

#define BLK_MIG_FLAG_DEVICE_BLOCK       0x01
#define BLK_MIG_FLAG_EOS                0x02
#define BLK_MIG_FLAG_PROGRESS           0x04
#define BLK_MIG_FLAG_FS_BLOCKSIZE		0x08   									// Pacer Constant
#define BLK_MIG_FLAG_COMPRESSED_DEVICE_BLOCK 0x10								// Pacer Constant
#define BLK_MIG_COMPRESSION_LEVEL 9 											// Pacer Constant
#define MAX_IS_ALLOCATED_SEARCH 65536

//#define DEBUG_BLK_MIGRATION

#ifdef DEBUG_BLK_MIGRATION
#define DPRINTF(fmt, ...) \
    do { printf("blk_migration: " fmt, ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) \
    do { } while (0)
#endif

static int migr_progress; // Pacer variabl for prediction based on progress

typedef struct BlkMigDevState {
    BlockDriverState *bs;
    int bulk_completed;
    int shared_base;

    /* Pacer variables */
    int scheduling; // for enabling scheduling
    int dirty_scheduling; // for enableing dirty scheduling 
    WriteHistoryItem* currentHistoryItem;
    WriteFreqItem* currentFreqItem;
    int access_done;
    int chunksize;
    int chunksize_bit;
    int64_t current_chunksector;
    int current_chunklen;
    /* End */

    int64_t cur_sector;
    int64_t cur_dirty;
    int64_t completed_sectors;
    int64_t total_sectors;
    int64_t dirty;
    QSIMPLEQ_ENTRY(BlkMigDevState) entry;
} BlkMigDevState;

typedef struct BlkMigBlock {
    uint8_t *buf;
    BlkMigDevState *bmds;
    int64_t sector;
    int64_t nr_sectors; // Pacer variable
    struct iovec iov;
    QEMUIOVector qiov;
    BlockDriverAIOCB *aiocb;
    int ret;
    int64_t time;
    bool dirty;  // Pacer variable: flag to mark a dirty block
    QSIMPLEQ_ENTRY(BlkMigBlock) entry;
} BlkMigBlock;

/* additional struct used by Pacer */
typedef struct AccessChunk {
	int64_t chunkstart;
	int length;
	int64_t freq;
    QSIMPLEQ_ENTRY(AccessChunk) entry;
} AccessChunk;

typedef struct UnaccessChunks {
	int64_t chunkstart;
	int length;
	QSIMPLEQ_ENTRY(UnaccessChunks) entry;
} UnaccessChunks;
/* End */

typedef struct BlkMigState {
    int blk_enable;
    int shared_base;
    QSIMPLEQ_HEAD(bmds_list, BlkMigDevState) bmds_list;
    QSIMPLEQ_HEAD(blk_list, BlkMigBlock) blk_list;
    int submitted;
    int read_done;
    int transferred;
    uint64_t total_transferred; // Pacer variable for adaptive system
    int64_t total_sector_sum;
    int prev_progress;
    int bulk_completed;
    long double total_time;
    /* Pacer variables */
    int sparse; 
    int fs_bsize; 
    int compression;
    z_stream incoming_stream;
    z_stream outgoing_stream;
    int compress_init_send_called;
    int compress_init_recv_called;
    int64_t bulk_block_reads; 
    int64_t dirty_block_reads; 
    uint64_t saving_traffic;
    int64_t lastdirtyblk;
    int scheduling;
    int dscheduling;
    int throttling;
    int64_t migration_starttime;
    int64_t migration_dirtystarttime;
    int64_t migration_endtime;
    int64_t transfer_rate;
    int64_t chunksize;
    int64_t dirtyblocknum;
    int rr;
    QSIMPLEQ_HEAD(access_list, AccessChunk) access_list;
    QSIMPLEQ_HEAD(unaccess_list,UnaccessChunks) unaccess_list;	
    int writehistory_index;
    BlkMigDevState *migrated_bmds;	 	
    /* End */
} BlkMigState;


static BlkMigState block_mig_state;

/* Pacer Function: do_compression */
/* Description: Compress the data in the input_buffer to output_buffer. */
/* Return the length of compressed data. */
static uint32_t do_compression(uint8_t *input_buffer, uint32_t input_data_len, uint8_t *output_buffer, uint32_t output_bound)
{
		int status;

		block_mig_state.outgoing_stream.next_in = input_buffer;
		block_mig_state.outgoing_stream.avail_in = input_data_len;
		
		block_mig_state.outgoing_stream.next_out = output_buffer;
		block_mig_state.outgoing_stream.avail_out = output_bound;

		uint32_t compress_length=0;
		 
		status = deflate(&(block_mig_state.outgoing_stream), Z_FULL_FLUSH);
		switch (status) {
			case Z_OK:
				compress_length=output_bound - block_mig_state.outgoing_stream.avail_out;
				break;
			default:
				DPRINTF("Error in deflate: deflate returned %d\n", status);
				break;
		}
		
    	if(status!=Z_OK){
    			DPRINTF("Switch to non-compress. The left buffer is %d\n",block_mig_state.outgoing_stream.avail_out);
    			block_mig_state.compression=0;
		  	    return 0;
    	}else {
			return compress_length;
	}
}

/* Pacer Function: blk_send_with_len */
/* Description: send the data in the buffer+offset of the block with length of data_len. If compression, compress the data first */
/* Change the protocol: add the data length for each time of sending */
static void blk_send_with_len(QEMUFile *f, BlkMigBlock *blk, uint32_t offset, uint32_t data_len, bool compression)
{
	uint8_t dev_name_len;
	
	if(compression) {
		uint8_t* compressed_data;
		uint32_t compress_bufferbound= compressBound(data_len);
		compressed_data=qemu_malloc(compress_bufferbound);
		uint32_t compressed_data_len=do_compression(blk->buf+offset, data_len, compressed_data, compress_bufferbound);
		//if compression failed in any case, send the data without compression
		if(compressed_data_len==0)
		{
			blk_send_with_len(f, blk, offset, data_len, false);
		}
		else {
			qemu_put_be64(f, ((blk->sector+offset/BDRV_SECTOR_SIZE) << BDRV_SECTOR_BITS)
    		    			| BLK_MIG_FLAG_COMPRESSED_DEVICE_BLOCK);

			dev_name_len = strlen(blk->bmds->bs->device_name);
			qemu_put_byte(f, dev_name_len);
			qemu_put_buffer(f, (uint8_t *)blk->bmds->bs->device_name, dev_name_len);
			qemu_put_be32(f,compressed_data_len);
			qemu_put_buffer(f, compressed_data, compressed_data_len);
		}
		qemu_free(compressed_data);
	}
	else{
		qemu_put_be64(f, ((blk->sector+offset/BDRV_SECTOR_SIZE) << BDRV_SECTOR_BITS)
    		    			| BLK_MIG_FLAG_DEVICE_BLOCK);
		dev_name_len = strlen(blk->bmds->bs->device_name);
		qemu_put_byte(f, dev_name_len);
		qemu_put_buffer(f, (uint8_t *)blk->bmds->bs->device_name, dev_name_len);
		qemu_put_be32(f,data_len);
	    qemu_put_buffer(f, blk->buf+offset, data_len);
	}
}

/* Pacer Function: blk_send_nonzero_sectors */
/* Description: segmenting a bulk block (4MB) into file system blocks (4KB), remove the zero file system blocks, send the non-zero ones */
static void blk_send_nonzero_sectors(QEMUFile *f, BlkMigBlock * blk, bool compression,uint32_t block_size)
{
	int64_t scan_index=0;
	int64_t scan_start=0;
    int64_t not_zero_fsblock=0;
    bool last_not_zero=false;
    uint64_t* tmpbuf=(uint64_t*)(blk->buf);

	/* segment the bulk block into fs block size, check whether the fs block is zero */
    for(scan_index=0; scan_index<block_size; scan_index=scan_index+block_mig_state.fs_bsize)
    {
 	       int index=0;
		   /*scan every 64 bit to speed up the zero block checking*/
	 	   while(index < (block_mig_state.fs_bsize/sizeof(uint64_t)) ){
          		if(tmpbuf[scan_index/sizeof(uint64_t) + index]!= 0)
           			break;
				else
					index++;
           }
	      /*if not a zero block */
           if(index<(block_mig_state.fs_bsize/sizeof(uint64_t))){
			    /*if the previous block is not a zero block either, accumulate them*/
				if(last_not_zero==true){
					not_zero_fsblock++;		
				}
				else{
					/*if the previous block is a zero block, this block is the first non-zero block*/
					scan_start=scan_index;
					last_not_zero=true;
					not_zero_fsblock++;
				}
	       }
	       else{   
			   /* if this is a zero block, send the previous non-zero blocks*/
				if(last_not_zero==true){
					uint32_t data_len=not_zero_fsblock * block_mig_state.fs_bsize;
					blk_send_with_len(f, blk, scan_start, data_len, compression);
					last_not_zero=false;
					not_zero_fsblock=0;
				}
				block_mig_state.saving_traffic+=block_mig_state.fs_bsize;
		  }
	}
	
	/* if the last several blocks are non-zero blocks, need to send them*/
	if(last_not_zero==true){
		 uint32_t data_len=not_zero_fsblock * block_mig_state.fs_bsize;
		 blk_send_with_len(f, blk, scan_start, data_len,compression);
	}

} 

/* Pacer Function: blk_send */
/* Description: send the block according to its property, e.g. dirty block, bulk block, whether checking the sparse block, whether do compression */
static void blk_send(QEMUFile *f, BlkMigBlock * blk)
{
    
	if(blk->dirty==true) {	
		block_mig_state.dirtyblocknum++;
		if(block_mig_state.compression==1)
            		blk_send_with_len(f, blk, 0, ((blk->nr_sectors)<< BDRV_SECTOR_BITS), true);
		else
			blk_send_with_len(f, blk, 0, ((blk->nr_sectors)<< BDRV_SECTOR_BITS), false);
	}else{

		if(block_mig_state.sparse == 1){
			// if it is not a dirty block and sparse flag is set, check sparse block to send non-zero fs blocks only 
			if(block_mig_state.compression==1)
				blk_send_nonzero_sectors(f, blk, true,((blk->nr_sectors)<< BDRV_SECTOR_BITS));
			else
				blk_send_nonzero_sectors(f, blk, false,((blk->nr_sectors)<< BDRV_SECTOR_BITS));
		}else {
			// if not a dirty block and sparse flag is not set, send the bulk block 
			if(block_mig_state.compression==1)
				blk_send_with_len(f, blk, 0, ((blk->nr_sectors)<< BDRV_SECTOR_BITS), true);
			else
				blk_send_with_len(f, blk, 0, ((blk->nr_sectors)<< BDRV_SECTOR_BITS), false);
		}
		
	} 

}

/* Pacer Function: mig_compress_init_send */
/* Description: initialize the compression stream at the source site*/
/* if error occurs in the init function, switch to non-compression mode*/
static void mig_compress_init_send(int level)
{
	if (block_mig_state.compress_init_send_called == 1)
		deflateEnd(&block_mig_state.outgoing_stream);
	
	block_mig_state.compress_init_send_called = 1;

	/* allocate deflate state */
	block_mig_state.outgoing_stream.zalloc = Z_NULL;
	block_mig_state.outgoing_stream.zfree = Z_NULL;
	block_mig_state.outgoing_stream.opaque = Z_NULL;
	int ret = deflateInit(&(block_mig_state.outgoing_stream), level);
	if (ret != Z_OK)
	{
		DPRINTF("Deflate Init error (), switch to not compress\n");
		block_mig_state.compression=0;
	}
}

/* Pacer Function: mig_compress_init_recv */
/* Description: initialize the compression stream at the destination site*/
static void mig_compress_init_recv(void)
{
	if (block_mig_state.compress_init_recv_called == 1)
		inflateEnd(&(block_mig_state.incoming_stream));
	block_mig_state.compress_init_recv_called = 1;
	
	block_mig_state.incoming_stream.zalloc = Z_NULL;
    block_mig_state.incoming_stream.zfree = Z_NULL;
    block_mig_state.incoming_stream.opaque = Z_NULL;
    block_mig_state.incoming_stream.avail_in=0;
	block_mig_state.incoming_stream.next_in=Z_NULL;

	int ret=inflateInit(&(block_mig_state.incoming_stream));
	if (ret != Z_OK)
    {
         DPRINTF("Inflate Init error ()\n");
    }
}

/* Pacer Function: mig_compress_cleanup */
/* Description: clean up the compression functions */
static void mig_compress_cleanup(void)
{
	if (block_mig_state.compress_init_recv_called == 1 ) 
	{
		inflateEnd(&block_mig_state.incoming_stream);
		block_mig_state.compress_init_recv_called = 0;
	}
	if (block_mig_state.compress_init_send_called == 1 )
	{
		deflateEnd(&block_mig_state.outgoing_stream);
		block_mig_state.compress_init_send_called = 0;
	}
}

int blk_mig_active(void)
{
     return !QSIMPLEQ_EMPTY(&block_mig_state.bmds_list);
}

/* Pacer Function */
/* Description: return the total bytes transferred */
uint64_t my_blk_mig_bytes_transferred(void)
{
     uint64_t transferred= block_mig_state.total_transferred * BULK_BLOCK_SIZE;
     return transferred;
}

uint64_t blk_mig_bytes_transferred(void)
{
     BlkMigDevState *bmds;
     uint64_t sum = 0;

     QSIMPLEQ_FOREACH(bmds, &block_mig_state.bmds_list, entry) {
	    sum += bmds->completed_sectors;
     }
     return sum << BDRV_SECTOR_BITS;
}

uint64_t blk_mig_bytes_remaining(void)
{
	return blk_mig_bytes_total() - blk_mig_bytes_transferred();
}

/* Pacer Function: blk_mig_bytes_saving  */
/* Description: return the amount of data that are saved by sparse checking */
uint64_t blk_mig_bytes_saving(void)
{
	return block_mig_state.saving_traffic;
}

uint64_t blk_mig_bytes_total(void)
{
	BlkMigDevState *bmds;
	uint64_t sum = 0;

	QSIMPLEQ_FOREACH(bmds, &block_mig_state.bmds_list, entry) {
		sum += bmds->total_sectors;
	}
	return sum << BDRV_SECTOR_BITS;
}

static inline void add_avg_read_time(int64_t time, bool dirty)
{
	/* Pacer modification: increament accordingly */   
	if(!dirty)
		block_mig_state.bulk_block_reads++;
	else
		block_mig_state.dirty_block_reads++;
	/* End */

	block_mig_state.total_time += time;
}

static inline long double compute_read_bwidth(void)
{
	assert(block_mig_state.total_time != 0);

	/* Pacer modification: deal both dirty block reads and bulk block reads */    
    int64_t traffic1=block_mig_state.bulk_block_reads*BULK_BLOCK_SIZE;
    int64_t traffic2=block_mig_state.dirty_block_reads * DIRTY_BLOCK_SIZE;
    long double bwidth=(traffic1+traffic2)/ block_mig_state.total_time;
    /* End */

    return bwidth;
}

static void blk_mig_read_cb(void *opaque, int ret)
{
	
	BlkMigBlock *blk = opaque;

	blk->ret = ret;

	blk->time = qemu_get_clock_ns(rt_clock) - blk->time;
	add_avg_read_time(blk->time,blk->dirty);


	QSIMPLEQ_INSERT_TAIL(&block_mig_state.blk_list, blk, entry);

	block_mig_state.submitted--;
	block_mig_state.read_done++;
	assert(block_mig_state.submitted >= 0);
}

/* Pacer Function: getNextChunkSector */
/* Description: get the next chunk sector from freq list in bdrv */
static int64_t getNextChunkSector(int* pnr_sectors,BlkMigDevState* bmds)
{
	BlockDriverState *bs=bmds->bs;
	WriteHistoryItem* currentHistory;
	if(bmds->current_chunklen!=0)
	{
		*pnr_sectors=BDRV_SECTORS_PER_BULK_CHUNK;
		bmds->current_chunklen=bmds->current_chunklen-BDRV_SECTORS_PER_BULK_CHUNK;
		int64_t ret_sector= bmds->current_chunksector;
		bmds->current_chunksector=bmds->current_chunksector+BDRV_SECTORS_PER_BULK_CHUNK;
		return ret_sector;
	}
	if(bmds->access_done==0) 
	{
		//unaccess list
		if(bmds->currentFreqItem==NULL)
		{
			if(bs->history_active_id==1)
				bmds->currentFreqItem=bs->writefreqTail0;
			else
				bmds->currentFreqItem=bs->writefreqTail1;
			printf("bs->history_active_id %d\n",bs->history_active_id);

			currentHistory=bmds->currentFreqItem->head;
		}else{
			currentHistory=bmds->currentHistoryItem;
		}
		*pnr_sectors=BDRV_SECTORS_PER_BULK_CHUNK;
		bmds->current_chunklen=bmds->chunksize-BDRV_SECTORS_PER_BULK_CHUNK;
		int64_t ret_sector=(currentHistory->id)<<(bmds->chunksize_bit);
		bmds->current_chunksector=ret_sector+BDRV_SECTORS_PER_BULK_CHUNK;
		if(currentHistory->nextWHItem!=NULL)
			bmds->currentHistoryItem=currentHistory->nextWHItem;
		else{
			if(bmds->currentFreqItem->previousFreqItem==NULL)
				bmds->access_done=1;
			else{
				bmds->currentFreqItem=bmds->currentFreqItem->previousFreqItem;
				bmds->currentHistoryItem=bmds->currentFreqItem->head;
			}
		}
		return ret_sector;
	}else return -1;
	
}

/* Pacer Modified Function: mig_save_device_bulk */
/* Description: add the scheduling inside; update the additional Pacer variables */
static int mig_save_device_bulk(Monitor *mon, QEMUFile *f,
		BlkMigDevState *bmds)
{   
	int64_t cur_sector =-1;
	BlkMigBlock *blk;
	if((block_mig_state.scheduling==1)&&(bmds->scheduling==1))
	{
		int64_t total_sectors = bmds->total_sectors;
		int* pnr_sectors=qemu_malloc(sizeof(int));
		cur_sector = (int64_t)getNextChunkSector(pnr_sectors,bmds);
		int nr_sectors=*pnr_sectors;
		qemu_free(pnr_sectors);	
		if(cur_sector==-1)
			return 1;
		BlockDriverState *bs = bmds->bs;
	

		if (total_sectors <= cur_sector + nr_sectors ) {  
			nr_sectors = total_sectors - cur_sector;
		}
		bmds->completed_sectors = bmds->completed_sectors+nr_sectors;

		

		blk = qemu_malloc(sizeof(BlkMigBlock));
		blk->buf = qemu_malloc(nr_sectors* BDRV_SECTOR_SIZE);
		blk->bmds = bmds;
		blk->sector = cur_sector;
		blk->nr_sectors=nr_sectors;
		blk->dirty = false; 
		blk->iov.iov_base = blk->buf;
		blk->iov.iov_len = nr_sectors * BDRV_SECTOR_SIZE;
		qemu_iovec_init_external(&blk->qiov, &blk->iov, 1);

		blk->time = qemu_get_clock_ns(rt_clock);
                //modified from bdrv_aio_readv
		blk->aiocb = my_bdrv_aio_readv(bs, cur_sector, &blk->qiov,
				nr_sectors, blk_mig_read_cb, blk);
		if (!blk->aiocb) {
			goto error;
		}
		block_mig_state.submitted++;
                block_mig_state.total_transferred++;
		bdrv_reset_dirty(bs, cur_sector, nr_sectors); //NOTE: this can affect result, the migration and dirty communicates!
		return (bmds->cur_sector >= total_sectors);

	}else
	{
		int64_t total_sectors = bmds->total_sectors;
		cur_sector = bmds->cur_sector;
		BlockDriverState *bs = bmds->bs;
		int nr_sectors;

		if (bmds->shared_base) {
			while (cur_sector < total_sectors &&
					!bdrv_is_allocated(bs, cur_sector, MAX_IS_ALLOCATED_SEARCH,
						&nr_sectors)) {
				cur_sector += nr_sectors;
			}
		}

		if (cur_sector >= total_sectors) {
			bmds->cur_sector = bmds->completed_sectors = total_sectors;
			return 1;
		}

		bmds->completed_sectors = cur_sector;
        // Pacer modification: remove this line to transfer odd MB chunk, e.g. 9MB
		// cur_sector &= ~((int64_t)BDRV_SECTORS_PER_BULK_CHUNK - 1);  

		/* we are going to transfer a full block even if it is not allocated */
		nr_sectors = BDRV_SECTORS_PER_BULK_CHUNK; 

		if (total_sectors - cur_sector < BDRV_SECTORS_PER_BULK_CHUNK) {  
			nr_sectors = total_sectors - cur_sector;
		}

		blk = qemu_malloc(sizeof(BlkMigBlock));

		// modified for LINUX native aio; must alignment buffer when O_DIRECT
        // blk->buf = qemu_malloc(BULK_BLOCK_SIZE);

		blk->buf = qemu_memalign(512,BULK_BLOCK_SIZE);
                blk->bmds = bmds;
		blk->nr_sectors=nr_sectors;
		blk->sector = cur_sector;
		blk->dirty = false; 
		blk->iov.iov_base = blk->buf;
		blk->iov.iov_len = nr_sectors * BDRV_SECTOR_SIZE;
		qemu_iovec_init_external(&blk->qiov, &blk->iov, 1);

		blk->time = qemu_get_clock_ns(rt_clock);
                //modified from bdrv_aio_readv to my_bdrv_aio_readv for latency measurement 
		blk->aiocb = my_bdrv_aio_readv(bs, cur_sector, &blk->qiov,
				nr_sectors, blk_mig_read_cb, blk);
		if (!blk->aiocb) {
			goto error;
		}
		block_mig_state.submitted++;
        block_mig_state.total_transferred++; //added for adaptive system

		bdrv_reset_dirty(bs, cur_sector, nr_sectors);
		bmds->cur_sector = cur_sector + nr_sectors;

		return (bmds->cur_sector >= total_sectors);
	}
error:
	monitor_printf(mon, "Error reading sector %" PRId64 "\n", cur_sector);
	qemu_file_set_error(f);
	qemu_free(blk->buf);
	qemu_free(blk);
	return 0;
}

static void set_dirty_tracking(int enable)
{
	BlkMigDevState *bmds;

	QSIMPLEQ_FOREACH(bmds, &block_mig_state.bmds_list, entry) {
		bdrv_set_dirty_tracking(bmds->bs, enable);
	}
}

static void init_blk_migration_it(void *opaque, BlockDriverState *bs)
{
	Monitor *mon = opaque;
	BlkMigDevState *bmds;
	int64_t sectors;

	if (bs->type == BDRV_TYPE_HD) {
		sectors = bdrv_getlength(bs) >> BDRV_SECTOR_BITS;
		if (sectors == 0) {
			return;
		}

		bmds = qemu_mallocz(sizeof(BlkMigDevState));
		bmds->bs = bs;
		bmds->bulk_completed = 0;
		bmds->total_sectors = sectors;
		bmds->completed_sectors = 0;
		bmds->currentFreqItem=NULL;
		bmds->currentHistoryItem=NULL;

		bmds->shared_base = block_mig_state.shared_base;

		block_mig_state.total_sector_sum += sectors;

		if (bmds->shared_base) {
			monitor_printf(mon, "Start migration for %s with shared base "
					"image\n",
					bs->device_name);
		} else {
			monitor_printf(mon, "Start full migration for %s\n",
					bs->device_name);
		}

		QSIMPLEQ_INSERT_TAIL(&block_mig_state.bmds_list, bmds, entry);


	}
}

static void init_blk_migration(Monitor *mon, QEMUFile *f)
{
	block_mig_state.submitted = 0;
	block_mig_state.read_done = 0;
	block_mig_state.transferred = 0;
	block_mig_state.total_sector_sum = 0;
	block_mig_state.prev_progress = -1;
	block_mig_state.bulk_completed = 0;
	block_mig_state.total_time = 0;
	block_mig_state.bulk_block_reads = 0;
	block_mig_state.dirty_block_reads = 0;
	block_mig_state.saving_traffic = 0;
    block_mig_state.rr = 0;	
	/* Pacer modification: update additional variables */
	block_mig_state.migration_starttime=0;
	block_mig_state.migration_dirtystarttime=0;
	block_mig_state.migration_endtime=0;
	block_mig_state.lastdirtyblk=-1;
	block_mig_state.writehistory_index=0;
    block_mig_state.total_transferred=0;	
	/* End */
	bdrv_iterate(init_blk_migration_it, mon);
}

/* Pacer Function: get_progress */
int get_progress(void)
{
	return migr_progress;
}

static int blk_mig_save_bulked_block(Monitor *mon, QEMUFile *f)
{
	int64_t completed_sector_sum = 0;
	BlkMigDevState *bmds;
	int progress;
	int ret = 0;

	QSIMPLEQ_FOREACH(bmds, &block_mig_state.bmds_list, entry) {
		if (bmds->bulk_completed == 0) {
			if (mig_save_device_bulk(mon, f, bmds) == 1) {
				/* completed bulk section for this device */
				bmds->bulk_completed = 1;
			}
			completed_sector_sum += bmds->completed_sectors;
			ret = 1;
			break;
		} else {
			completed_sector_sum += bmds->completed_sectors;
		}
	}

	progress = completed_sector_sum * 100 / block_mig_state.total_sector_sum;
	if (progress != block_mig_state.prev_progress) {
		block_mig_state.prev_progress = progress;
		qemu_put_be64(f, (progress << BDRV_SECTOR_BITS)
				| BLK_MIG_FLAG_PROGRESS);
		monitor_printf(mon, "Completed %d %%\r", progress);
		migr_progress=progress;
		monitor_flush(mon);
	}

	return ret;
}

static void blk_mig_reset_dirty_cursor(void)
{
	BlkMigDevState *bmds;

	QSIMPLEQ_FOREACH(bmds, &block_mig_state.bmds_list, entry) {
		bmds->cur_dirty = 0;
	}
}

/* Pacer Function */
/* Description: get the next dirty sector from the dirty freq list */
static int64_t getNextDirtySector(BlkMigDevState* bmds)
{
	BlockDriverState *bs=bmds->bs;
	WriteHistoryItem* currentHistory;

	if(bs->dirty_writefreqTail->previousFreqItem==NULL)
		return -1;
	else{
		currentHistory=bs->dirty_writefreqTail->previousFreqItem->head;
		return (currentHistory->id)<<(bs->dirty_chunksize_bit);
	}
}

/* Pacer Modified Function */
/* Description: add the dirty scheduling inside */ 
static int mig_save_device_dirty(Monitor *mon, QEMUFile *f,
		BlkMigDevState *bmds, int is_async)
{
	BlkMigBlock *blk;
	int64_t total_sectors = bmds->total_sectors;
	int64_t sector;
	int nr_sectors;
	if((block_mig_state.scheduling==1)&&(bmds->dirty_scheduling==1))
	{
		
		sector = (int64_t)getNextDirtySector(bmds);
		if(sector==-1)
			return 1;
		if (total_sectors - sector < BDRV_SECTORS_PER_DIRTY_CHUNK) {
			nr_sectors = total_sectors - sector;
		} else {
			nr_sectors = BDRV_SECTORS_PER_DIRTY_CHUNK;
		}
		blk = qemu_malloc(sizeof(BlkMigBlock));
		blk->buf = qemu_malloc(DIRTY_BLOCK_SIZE); 
		blk->dirty = true; 
		blk->bmds = bmds;
		blk->sector = sector;
		blk->nr_sectors=nr_sectors;
		if (is_async) {
			blk->iov.iov_base = blk->buf;
			blk->iov.iov_len = nr_sectors * BDRV_SECTOR_SIZE;
			qemu_iovec_init_external(&blk->qiov, &blk->iov, 1);

			blk->time = qemu_get_clock_ns(rt_clock);
                        //modified from bdrv_aio_readv
			blk->aiocb = my_bdrv_aio_readv(bmds->bs, sector, &blk->qiov,
				nr_sectors, blk_mig_read_cb, blk);
			if (!blk->aiocb) {
				goto error;
			}
			block_mig_state.submitted++;
                         
		} else {
			if (bdrv_read(bmds->bs, sector, blk->buf,
				nr_sectors) < 0) {
				goto error;
			}
			blk_send(f, blk);

			qemu_free(blk->buf);
			qemu_free(blk);
		}
                block_mig_state.total_transferred++; //added for adaptive system
		bdrv_reset_dirty(bmds->bs, sector, nr_sectors);
		return 0;
	}else{
		for (sector = bmds->cur_dirty; sector < bmds->total_sectors;) {
			if (bdrv_get_dirty(bmds->bs, sector)) {

				if (total_sectors - sector < BDRV_SECTORS_PER_DIRTY_CHUNK) {
					nr_sectors = total_sectors - sector;
				} else {
					nr_sectors = BDRV_SECTORS_PER_DIRTY_CHUNK;
				}
				blk = qemu_malloc(sizeof(BlkMigBlock));
                                //modify for LINUX native aio
//				blk->buf = qemu_malloc(DIRTY_BLOCK_SIZE); 
				blk->buf=qemu_memalign(512,DIRTY_BLOCK_SIZE);
                                blk->dirty = true; 
				blk->bmds = bmds;
				blk->sector = sector;
				blk->nr_sectors=nr_sectors;

				if (is_async) {
					blk->iov.iov_base = blk->buf;
					blk->iov.iov_len = nr_sectors * BDRV_SECTOR_SIZE;
					qemu_iovec_init_external(&blk->qiov, &blk->iov, 1);

					blk->time = qemu_get_clock_ns(rt_clock);
                                        //modified from bdrv_aio_readv
					blk->aiocb = my_bdrv_aio_readv(bmds->bs, sector, &blk->qiov,
						nr_sectors, blk_mig_read_cb, blk);
					if (!blk->aiocb) {
						goto error;
					}
					block_mig_state.submitted++;
				} else {
					if (bdrv_read(bmds->bs, sector, blk->buf,
								nr_sectors) < 0) {
						goto error;
					}
					blk_send(f, blk);

					qemu_free(blk->buf);
					qemu_free(blk);
				}
                                block_mig_state.total_transferred++;
				bdrv_reset_dirty(bmds->bs, sector, nr_sectors);
				break;
			}
			sector += BDRV_SECTORS_PER_DIRTY_CHUNK;
			bmds->cur_dirty = sector;
		}
	
		return (bmds->cur_dirty >= bmds->total_sectors);
	}

error:
	monitor_printf(mon, "Error reading sector %" PRId64 "\n", sector);
	qemu_file_set_error(f);
	qemu_free(blk->buf);
	qemu_free(blk);
	return 0;
}

static int blk_mig_save_dirty_block(Monitor *mon, QEMUFile *f, int is_async)
{
	BlkMigDevState *bmds;
	int ret = 0;

	QSIMPLEQ_FOREACH(bmds, &block_mig_state.bmds_list, entry) {
		if (mig_save_device_dirty(mon, f, bmds, is_async) == 0) {
			ret = 1;
			break;
		}
	}

	return ret;
}


/* Pacer Function: is_zero_block */
/* Description: check whether a bulk block is a zero block. Zero block: return true, otherwise, return false */
static bool is_zero_block(BlkMigBlock * blk)
{
	uint32_t block_size = BULK_BLOCK_SIZE; 
	uint64_t tempsize=block_size/sizeof(uint64_t);
        uint64_t* tempbuf=(uint64_t*) blk->buf;
        while(tempsize--){
                if(*tempbuf++!= 0)
                        return false;
        }
        return true;
}


static void flush_blks(QEMUFile* f)
{
	BlkMigBlock *blk;

	DPRINTF("%s Enter submitted %d read_done %d transferred %d\n",
            __FUNCTION__, block_mig_state.submitted, block_mig_state.read_done,
            block_mig_state.transferred);

	while ((blk = QSIMPLEQ_FIRST(&block_mig_state.blk_list)) != NULL) {
		if (qemu_file_rate_limit(f)) {
			break;
		}
		if (blk->ret < 0) {
			qemu_file_set_error(f);
			break;
		}

		if((block_mig_state.sparse !=1)||(blk->dirty))
			blk_send(f, blk);
		else{
			int zb=is_zero_block(blk);
			if(!zb)
				blk_send(f,blk);
			else 
				block_mig_state.saving_traffic += BULK_BLOCK_SIZE;  
			
		}
		
		QSIMPLEQ_REMOVE_HEAD(&block_mig_state.blk_list, entry);
		qemu_free(blk->buf);
		qemu_free(blk);

		block_mig_state.read_done--;
		block_mig_state.transferred++;
		assert(block_mig_state.read_done >= 0);
	}
	
	 DPRINTF("%s Exit submitted %d read_done %d transferred %d\n", __FUNCTION__,
            block_mig_state.submitted, block_mig_state.read_done,
            block_mig_state.transferred);
}

int64_t get_remaining_dirty(void)
{
	BlkMigDevState *bmds;
	int64_t dirty = 0;

	QSIMPLEQ_FOREACH(bmds, &block_mig_state.bmds_list, entry) {
		dirty += bdrv_get_dirty_count(bmds->bs);
	}
	return dirty * DIRTY_BLOCK_SIZE;
}

static int is_stage2_completed(void)
{
	int64_t remaining_dirty;
	long double bwidth;

	if (block_mig_state.bulk_completed == 1) {

		remaining_dirty = get_remaining_dirty();
		if (remaining_dirty == 0) {
			return 1;
		}

		bwidth = compute_read_bwidth();
        if ((remaining_dirty / bwidth) <=
			migrate_max_downtime()) {
			/* finish stage2 because we think that we can finish remaing work
			   below max_downtime */
			return 1;
		}
	}

	return 0;
}

static void blk_mig_cleanup(Monitor *mon)
{
	BlkMigDevState *bmds;
	BlkMigBlock *blk;
	
	while ((bmds = QSIMPLEQ_FIRST(&block_mig_state.bmds_list)) != NULL) {
		QSIMPLEQ_REMOVE_HEAD(&block_mig_state.bmds_list, entry);
		qemu_free(bmds);
	}

	while ((blk = QSIMPLEQ_FIRST(&block_mig_state.blk_list)) != NULL) {
		QSIMPLEQ_REMOVE_HEAD(&block_mig_state.blk_list, entry);
		qemu_free(blk->buf);
		qemu_free(blk);
	}
               

	set_dirty_tracking(0);

	monitor_printf(mon, "\n");
	mig_compress_cleanup();
}

/* Pacer Function */
/* Description: enable the throttling */
void set_write_throttling(int enable,int64_t speed)
{
	BlkMigDevState *bmds;

	QSIMPLEQ_FOREACH(bmds, &block_mig_state.bmds_list, entry) {
		bdrv_set_write_throttling(bmds->bs,enable,speed/DIRTY_BLOCK_SIZE);
	}
}

/* Pacer Function */
/* Description: enable the history tracking to bdrv */
static void set_history_tracking(BlkMigDevState *bmds,int enable)
{
	bdrv_set_history_tracking(bmds->bs, enable);
}

/* Pacer Function */
/* Description: print the dirty block number at this moment */
static void printDirtyBlockCount(void)
{
	BlkMigDevState *bmds;
	QSIMPLEQ_FOREACH(bmds, &block_mig_state.bmds_list, entry) {
		printf("dirty block count %"PRId64"\n",  bdrv_get_dirty_count(bmds->bs));
	}
}

/* Pacer Function */
/* Description: return the new expire time */
int64_t getNewExpireTime(int64_t speed,int64_t remain_time)
{

	 BlkMigDevState *bmds;
        int64_t nextblock=block_mig_state.total_transferred;
        int64_t remaintime=0L;
        QSIMPLEQ_FOREACH(bmds, &block_mig_state.bmds_list, entry) {
                remaintime+=bdrv_getNewExpireTime(bmds->bs,nextblock,speed,remain_time);
        }
        return remaintime;

}

/* Pacer Function */
/* Description: get the average rate */
void getAveDirtyRate1and2(int64_t speed,int64_t *pdirtyrate1,int64_t *pdirtyrate2)
{
	BlkMigDevState *bmds;
        int64_t nextblock=block_mig_state.total_transferred; 
        QSIMPLEQ_FOREACH(bmds, &block_mig_state.bmds_list, entry) {
                bdrv_getAveDirtyRate2(bmds->bs,nextblock,speed,pdirtyrate1,pdirtyrate2);
        }
        return;
}

/* Pacer Function */
void printDBlockDist(void)
{
        BlkMigDevState *bmds;
        QSIMPLEQ_FOREACH(bmds, &block_mig_state.bmds_list, entry) {
                printWriteBlockDistance(bmds->bs);
        }

}

/* Pacer Function */
static void set_all_dirty(void)
{
 	BlkMigDevState *bmds;
        QSIMPLEQ_FOREACH(bmds, &block_mig_state.bmds_list, entry) {
                bdrv_set_dirty(bmds->bs,0,bmds->total_sectors);
	}
}

/* Pacer Function */
/* Description: prepare for scheduling */
static void getMigrateBMDS(void)
{
        //get the correct bs
        BlkMigDevState *bmds;

        QSIMPLEQ_FOREACH(bmds, &block_mig_state.bmds_list, entry) {
                set_history_tracking(bmds,0);
                if((bmds->bs->writeop_counter!=0)&&(block_mig_state.scheduling==1)){
                        block_mig_state.migrated_bmds=bmds;
                        bmds->scheduling=1;
                        if(block_mig_state.dscheduling==1){
                                bdrv_set_dirty_scheduling(bmds->bs,1);
                                bmds->dirty_scheduling=1;
                        }else{
                                bdrv_set_dirty_scheduling(bmds->bs,0);
                                bmds->dirty_scheduling=0;
                        }
			//get the correct freq set for scheduling
                        WriteFreqItem *currenttail;
                        if(bmds->bs->history_active_id==1){
                                currenttail=bmds->bs->writefreqTail0;
                                bmds->chunksize=bmds->bs->chunksize0;
                                bmds->chunksize_bit=bmds->bs->chunksize0_bit;
                        }
                        else{
                                currenttail=bmds->bs->writefreqTail1;
                                bmds->chunksize=bmds->bs->chunksize1;
                                bmds->chunksize_bit=bmds->bs->chunksize1_bit;
                        }

                        bmds->access_done=0;
                        bmds->current_chunksector=0;
                        bmds->current_chunklen=0;
                        printf("Migration getchunksize %d bit %d\n",bmds->chunksize,bmds->chunksize_bit);
                }else
                        bmds->scheduling=0;
        }
}

static int block_save_live(Monitor *mon, QEMUFile *f, int stage, void *opaque)
{
	

	 DPRINTF("Enter save live stage %d submitted %d transferred %d\n",
            stage, block_mig_state.submitted, block_mig_state.transferred);

	if (stage < 0) {
		blk_mig_cleanup(mon);
		return 0;
	}

	if (block_mig_state.blk_enable != 1) {
		/* no need to migrate storage */
		qemu_put_be64(f, BLK_MIG_FLAG_EOS);
		return 1;
	}

	if (stage == 1) {
		init_blk_migration(mon, f);
		block_mig_state.migration_starttime=qemu_get_clock_ns(rt_clock);
		/* Pacer modification: add for scheduling preparation */
        if(block_mig_state.scheduling==1)
	        getMigrateBMDS();
		
        /* start track dirty blocks */
		set_dirty_tracking(1);
        set_all_dirty();
        /* End */
	}

	flush_blks(f);


	if (qemu_file_has_error(f)) {
		blk_mig_cleanup(mon);
		return 0;
	}

	blk_mig_reset_dirty_cursor();
        
  	if (stage == 2) {
		/* control the rate of transfer */
		uint32_t block_size;
		if(block_mig_state.bulk_completed ==0)
			block_size = BULK_BLOCK_SIZE;
		else
			block_size = DIRTY_BLOCK_SIZE;

		while ((block_mig_state.submitted +
			block_mig_state.read_done) * block_size <
				qemu_file_get_rate_limit(f)) {
			//end
			if (block_mig_state.bulk_completed == 0) {
				/* first finish the bulk phase */
				if (blk_mig_save_bulked_block(mon, f) == 0) {
					/* finished saving bulk on all devices */
					block_mig_state.bulk_completed = 1;

					/* Pacer modification */
					block_size = DIRTY_BLOCK_SIZE;
					printDirtyBlockCount();
					block_mig_state.migration_dirtystarttime=qemu_get_clock_ns(rt_clock);
					block_mig_state.transfer_rate=((blk_mig_bytes_transferred()*1.0/(block_mig_state.migration_dirtystarttime-block_mig_state.migration_starttime))*1000000000);  
					printf("Migration speed: %"PRId64"\n",block_mig_state.transfer_rate);
					time_t rawtime;
    				struct tm * timeinfo;
    				time ( &rawtime );
    				timeinfo = localtime ( &rawtime );
    				printf("Current local time and date: %s", asctime (timeinfo));
                    if(block_mig_state.throttling)
						set_write_throttling(1,block_mig_state.transfer_rate/2);
					/* End */
				}
			} else {
				if (blk_mig_save_dirty_block(mon, f, 1) == 0) {
					/* no more dirty blocks */
					break;
				}
			}
		}

		flush_blks(f);
		
		if (qemu_file_has_error(f)) {
			blk_mig_cleanup(mon);
			return 0;
		}
	}

	if (stage == 3) {
		/* we know for sure that save bulk is completed and
		   all async read completed */
		assert(block_mig_state.submitted == 0);
		
		while (blk_mig_save_dirty_block(mon, f, 0) != 0);
		blk_mig_cleanup(mon);

		/* report completion */
		qemu_put_be64(f, (100 << BDRV_SECTOR_BITS) | BLK_MIG_FLAG_PROGRESS);

		if (qemu_file_has_error(f)) {
			return 0;
		}

		/* Pacer Modification */
		printf("Total Dirty block: %"PRId64"\n",block_mig_state.dirtyblocknum);
		monitor_printf(mon, "Block migration completed\n");
		block_mig_state.migration_endtime=qemu_get_clock_ns(rt_clock);
		uint64_t totalmtime=block_mig_state.migration_endtime-block_mig_state.migration_starttime;
		printf("Total migration time %"PRId64"\n",totalmtime);
		/* End */
	}

	qemu_put_be64(f, BLK_MIG_FLAG_EOS);

	return ((stage == 2) && is_stage2_completed());
}

/* Pacer Function: do_uncompression */
/* Description: uncompress the data in input_buf to uncompress_buf */
/* Return the amount of uncompressed data if successful. Otherwise, return 0 */
static int32_t do_uncompression(uint8_t *input_buf,uint32_t data_len, uint8_t *uncompress_buf, uint32_t uncompress_buf_length)
{
	int status;

	block_mig_state.incoming_stream.next_in = input_buf;
	block_mig_state.incoming_stream.avail_in = data_len;

	block_mig_state.incoming_stream.next_out = uncompress_buf;
	block_mig_state.incoming_stream.avail_out = uncompress_buf_length;

	status = inflate(&(block_mig_state.incoming_stream), Z_FULL_FLUSH);
	uint32_t uncompress_size= uncompress_buf_length-block_mig_state.incoming_stream.avail_out;		

	if(status != Z_OK){
		fprintf(stderr, "Error occurs in uncompression. Error Code: %d, %s", status, block_mig_state.incoming_stream.msg);
		return 0;
	}

	return uncompress_size;
}

static int block_load(QEMUFile *f, void *opaque, int version_id)
{
	static int banner_printed;
	int len, flags;
	char device_name[256];
	int64_t addr;
	BlockDriverState *bs;
	uint8_t *buf;

    do {
        addr = qemu_get_be64(f);

        flags = addr & ~BDRV_SECTOR_MASK;
        addr >>= BDRV_SECTOR_BITS;

        if ((flags & BLK_MIG_FLAG_DEVICE_BLOCK)||(flags & BLK_MIG_FLAG_COMPRESSED_DEVICE_BLOCK)) {   
           /* get device name */
	    	len = qemu_get_byte(f);
            qemu_get_buffer(f, (uint8_t *)device_name, len);
            device_name[len] = '\0';

            bs = bdrv_find(device_name);
	    	bdrv_set_history_tracking(bs, 0);
	
            if (!bs) {
                fprintf(stderr, "Error unknown block device %s\n",
                        device_name);
                return -EINVAL;
            }
	
            /* Pacer modification: add get length before receiving data, it is much flexible */
	    	uint32_t data_len = qemu_get_be32(f);  
            buf = qemu_malloc(data_len);
	    	qemu_get_buffer(f, buf, data_len);
			
	     	if(flags & BLK_MIG_FLAG_DEVICE_BLOCK) {
				bdrv_write(bs, addr, buf, data_len>>BDRV_SECTOR_BITS);
	      	}	else {
				uint32_t uncompress_buf_length=BULK_BLOCK_SIZE; //at most
				uint8_t *uncompress_buf= qemu_malloc(uncompress_buf_length);
				uint32_t uncompress_data_len= do_uncompression(buf,data_len,uncompress_buf,uncompress_buf_length);
				bdrv_write(bs,addr, uncompress_buf, uncompress_data_len>>BDRV_SECTOR_BITS);
				qemu_free(uncompress_buf);
	      	}    
	      	/* End */

            qemu_free(buf);
		}else if (flags & BLK_MIG_FLAG_PROGRESS) {
            	if (!banner_printed) {
                	printf("Receiving block device images\n");
                	banner_printed = 1;
            	}
            	printf("Completed %d %%%c", (int)addr, (addr == 100) ? '\n' : '\r');
            	fflush(stdout);
        }else if (!(flags & BLK_MIG_FLAG_EOS)) {
            	fprintf(stderr, "Unknown flags %d \n", flags);
            	return -EINVAL;
        }
        if (qemu_file_has_error(f)) {
            	return -EIO;
        }
    } while (!(flags & BLK_MIG_FLAG_EOS));

    return 0;
}


static void block_set_params(int blk_enable, int shared_base, int sparse, int fs_bsize, int compression, int scheduling, int dscheduling, int throttling, void *opaque)
{
    block_mig_state.blk_enable = blk_enable;
    block_mig_state.shared_base = shared_base;

   	/* shared base means that blk_enable = 1 */
    block_mig_state.blk_enable |= shared_base;

    /*Pacer modification: add for sparse, compression and scheduling with throttling*/
    block_mig_state.sparse = sparse;
    block_mig_state.fs_bsize = fs_bsize;
    block_mig_state.compression = compression;
    block_mig_state.scheduling = scheduling;
    block_mig_state.dscheduling = dscheduling;
    block_mig_state.throttling = throttling;
    /* End */
}

static void blk_mig_init_compressor_sparse(void)
{
	block_mig_state.compress_init_recv_called = 0;
        block_mig_state.compress_init_send_called = 0;
	
 	//prepare for compression
        mig_compress_init_recv();
        mig_compress_init_send(BLK_MIG_COMPRESSION_LEVEL);
}


void blk_mig_init(void)
{
    QSIMPLEQ_INIT(&block_mig_state.bmds_list);
    QSIMPLEQ_INIT(&block_mig_state.blk_list);

    register_savevm_live("block", 0, 1, block_set_params, block_save_live,
                         NULL, block_load, &block_mig_state);
	
    blk_mig_init_compressor_sparse();				// Pacer modification: prepare for compression
}
