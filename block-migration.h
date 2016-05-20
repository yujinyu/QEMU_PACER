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
 */

#ifndef BLOCK_MIGRATION_H
#define BLOCK_MIGRATION_H

void blk_mig_init(void);
int blk_mig_active(void);
uint64_t blk_mig_bytes_transferred(void);
uint64_t blk_mig_bytes_remaining(void);
uint64_t blk_mig_bytes_total(void);

/* Pacer Functions */
uint64_t my_blk_mig_bytes_transferred(void);
uint64_t blk_mig_bytes_saving(void);
int64_t get_remaining_dirty(void);
int get_progress(void);
void set_write_throttling(int,int64_t);
void set_dirtyblock_bitmap(int64_t cur_sector, int nr_sectors);
void auto_set_write_block_distance(int64_t cur_sector, int nr_sectors, uint64_t count, uint64_t laccesstime, uint64_t distance, uint64_t variance);
void printDBlockDist(void);
void getAveDirtyRate1and2(int64_t speed,int64_t *pdirtyset_size,int64_t *pdirtyrate2);
int64_t getNewExpireTime(int64_t speed,int64_t* pdirtyrate2, int64_t remain_time);
/* End */

#endif /* BLOCK_MIGRATION_H */
