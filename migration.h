/*
 * QEMU live migration
 *
 * Copyright IBM, Corp. 2008
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
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

#ifndef QEMU_MIGRATION_H
#define QEMU_MIGRATION_H

#include "qdict.h"
#include "qemu-common.h"

#define MIG_STATE_ERROR		-1
#define MIG_STATE_COMPLETED	0
#define MIG_STATE_CANCELLED	1
#define MIG_STATE_ACTIVE	2

#define DEFAULT_FS_BLOCKSIZE 4096

typedef struct MigrationState MigrationState;

struct MigrationState
{
    /* FIXME: add more accessors to print migration info */
    void (*cancel)(MigrationState *s);
    int (*get_status)(MigrationState *s);
    void (*release)(MigrationState *s);
    int blk;
    int shared;
    int sparse;
    int mig_time; 
    int64_t new_expected_time;
    int64_t starttime;
    int compression;
    int scheduling;
    int dscheduling;
    int throttling;
    int metricopt;
    int metricvalue;
    int policy;
    int prediction;
    int progressprediction;
    int reachthreshold; //for sync concurrent migration
    int couldfinish; //for sync concurrent migration
    int wait; //for speed 0 coordination
};

typedef struct FdMigrationState FdMigrationState;

struct FdMigrationState
{
    MigrationState mig_state;
    int64_t bandwidth_limit;
    int64_t sent_traffic; 
    QEMUFile *file;
    int fd;
    Monitor *mon;
    int state;
    int (*get_error)(struct FdMigrationState*);
    int (*close)(struct FdMigrationState*);
    int (*write)(struct FdMigrationState*, const void *, size_t);
    void *opaque;
    int64_t starttime;
    uint64_t transf_pre_dsize;
    uint64_t dirty_pre_dsize;
    uint64_t remain_precopy_size;
    uint64_t transf_pre_msize;
    uint64_t remain_msize;     
    uint64_t last_interval;
    uint64_t speed_pre_expected;
    uint64_t speed_first_expected;
    uint64_t speed_first_real;
    uint64_t speed_before_scaleup;
    uint64_t last_throughput;
    uint64_t last_total_latency;
    uint64_t last_total_ops;
    uint64_t last_totalunique;
    uint64_t last_maxunique;
    uint64_t estimateddirtyrate3;
    uint64_t full_memsize;
    uint8_t throttling;
    uint8_t samplemem;
    uint64_t samplecount;
    QEMUTimer *timer;
    QEMUTimer *timer1;
    QEMUTimer *timer2;
    int stage;
    int speed_scaleup_flag;
};


uint64_t getExpectedSpeed(void *opaque,int stage,int64_t past_time,int64_t remain_time,int64_t remain_precopy_size,int64_t dirty_dsize,int64_t remain_msize,int64_t dirtyrate1,int64_t dirtyrate2, int64_t dirtyrate_mem,int policy);

uint64_t getSpeedAdjusted(void *opaque,uint64_t speed_next_expected);
uint64_t getSpeedScaleup(void *opaque,uint64_t speed_next_expected,uint64_t speed_real);
void qemu_start_incoming_migration(const char *uri);

int do_migrate(Monitor *mon, const QDict *qdict, QObject **ret_data);

int do_migrate_cancel(Monitor *mon, const QDict *qdict, QObject **ret_data);

int do_migrate_set_speed(Monitor *mon, const QDict *qdict, QObject **ret_data);

int do_migrate_set_perfvalue(Monitor *mon, const QDict *qdict, QObject **ret_data);

int do_migrate_set_migrtime(Monitor *mon, const QDict *qdict, QObject **ret_data);

uint64_t migrate_max_downtime(void);


int do_migrate_set_wait(Monitor *mon, const QDict *qdict,
                            QObject **ret_data);
int do_migrate_set_couldfinish(Monitor *mon, const QDict *qdict,
                            QObject **ret_data);

int do_migrate_set_downtime(Monitor *mon, const QDict *qdict,
                            QObject **ret_data);

void do_info_migrate_print(Monitor *mon, const QObject *data);

void do_info_migrate_reachthreshold(Monitor *mon, QObject **ret_data);

void do_info_migrate_dirtyset(Monitor *mon, QObject **ret_data);

void do_info_migrate_dirtyrate(Monitor *mon, QObject **ret_data);

void do_info_migrate_newtime(Monitor *mon, QObject **ret_data);

void do_info_migrate(Monitor *mon, QObject **ret_data);

int exec_start_incoming_migration(const char *host_port);

MigrationState *exec_start_outgoing_migration(Monitor *mon,
                                              const char *host_port,
					      int64_t bandwidth_limit,
					      int detach,
					      int blk,
					      int inc,
					      int sparse,
					      int mig_time,
                                              int metricopt,
                                              int metricvalue,
					      int policy,
                                              int compression,
					      int scheduling,
					      int dscheduling,
					      int throttling,
					      int prediction,
					      int progressprediction);

int tcp_start_incoming_migration(const char *host_port);

MigrationState *tcp_start_outgoing_migration(Monitor *mon,
                                             const char *host_port,
					     int64_t bandwidth_limit,
					     int detach,
					     int blk,
					     int inc,
					     int sparse,
					     int mig_time,
                                             int metricopt,
                                             int metricvalue,
                                             int policy,
					     int compression,
					     int scheduling,
					     int dscheduling,
					     int throttling,
					     int prediction,
				             int progressprediction);

int unix_start_incoming_migration(const char *path);

MigrationState *unix_start_outgoing_migration(Monitor *mon,
                                              const char *path,
					      int64_t bandwidth_limit,
					      int detach,
					      int blk,
					      int inc,
					      int sparse,
					      int mig_time,
                                              int metricopt,
                                              int metricvalue,
                                              int policy,
					      int compression,
					      int scheduling,
					      int dscheduling,
					      int throttling,
					      int prediction,
					      int progressprediction);

int fd_start_incoming_migration(const char *path);

MigrationState *fd_start_outgoing_migration(Monitor *mon,
					    const char *fdname,
					    int64_t bandwidth_limit,
					    int detach,
					    int blk,
					    int inc,
					    int sparse,
					    int mig_time,
                                            int metricopt,
                                            int metricvalue,
                                            int policy,
					    int compression,
					    int scheduling,
					    int dscheduling,
					    int throttling,
					    int prediction,
					    int progressprediction);

void migrate_fd_monitor_suspend(FdMigrationState *s, Monitor *mon);

void migrate_fd_error(FdMigrationState *s);

int migrate_fd_cleanup(FdMigrationState *s);

void migrate_fd_put_notify(void *opaque);

ssize_t migrate_fd_put_buffer(void *opaque, const void *data, size_t size);

void migrate_fd_connect(FdMigrationState *s);

void migrate_fd_put_ready(void *opaque);

int migrate_fd_get_status(MigrationState *mig_state);

void migrate_fd_cancel(MigrationState *mig_state);

void migrate_fd_release(MigrationState *mig_state);

void migrate_fd_wait_for_unfreeze(void *opaque);

int migrate_fd_close(void *opaque);

static inline FdMigrationState *migrate_to_fms(MigrationState *mig_state)
{
    return container_of(mig_state, FdMigrationState, mig_state);
}

#endif
