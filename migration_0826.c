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
 */

#include "qemu-common.h"
#include "migration.h"
#include "monitor.h"
#include "buffered_file.h"
#include "sysemu.h"
#include "block.h"
#include "qemu_socket.h"
#include "block-migration.h"
#include "qemu-objects.h"
#include "pthread.h"
#include "block/raw-posix-aio.h"

//#define DEBUG_MIGRATION

#ifdef DEBUG_MIGRATION
#define DPRINTF(fmt, ...) \
    do { printf("migration: " fmt, ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) \
    do { } while (0)
#endif

/* Migration speed throttling */
static uint64_t max_throttle =  (10 << 20);

/* Pacer constants */
static uint64_t speed_step = (2 << 20);
/* End */

static MigrationState *current_migration;

//add for adaptive system by Pacer
//static int speed_change_interval=10000;

void qemu_start_incoming_migration(const char *uri)
{
    const char *p;

    if (strstart(uri, "tcp:", &p))
        tcp_start_incoming_migration(p);
#if !defined(WIN32)
    else if (strstart(uri, "exec:", &p))
        exec_start_incoming_migration(p);
    else if (strstart(uri, "unix:", &p))
        unix_start_incoming_migration(p);
    else if (strstart(uri, "fd:", &p))
        fd_start_incoming_migration(p);
#endif
    else
        fprintf(stderr, "unknown migration protocol: %s\n", uri);
}

int do_migrate(Monitor *mon, const QDict *qdict, QObject **ret_data)
{
//    printf("migrate thread %lu\n",(unsigned long int)pthread_self());    
    MigrationState *s = NULL;
    const char *p;
    int detach = qdict_get_int(qdict, "detach");
    const char *uri = qdict_get_str(qdict, "uri");

    if (current_migration &&
        current_migration->get_status(current_migration) == MIG_STATE_ACTIVE) {
        monitor_printf(mon, "migration already in progress\n");
        return -1;
    }
    /* Pacer modification: computing estimate migration speed */
    int has_mig_time = qdict_haskey(qdict, "mig_time");
    int mig_time=500;
   
    if(has_mig_time)
	mig_time=qdict_get_int(qdict,"mig_time");
    printf("migration requried time %d\n",mig_time);

    int metricopt=(int)qdict_get_int(qdict,"throughputrequired");
    int metricvalue=0;
    if(metricopt==1) {  
        int has_throughput = qdict_haskey(qdict,"required");
        if(has_throughput)
           metricvalue=qdict_get_int(qdict,"required");
        if(metricvalue!=0)
           printf("throughput required %d\n",metricvalue);  
    }else{
        int has_latency = qdict_haskey(qdict,"required");
        if(has_latency)
           metricvalue=qdict_get_int(qdict,"required");
        if(metricvalue!=0)
           printf("latency required %d\n",metricvalue);  
    }
    if(metricvalue==0){
         printf("no requirement\n");
         metricopt=2;
    }
    uint64_t memsize=ram_bytes_remaining();
    uint64_t disksize=bdrv_get_totallength();
    uint64_t speed;
     
    printf("ram size %"PRId64"\n",ram_bytes_remaining());
    printf("disk size %"PRId64"\n",bdrv_get_totallength());    
    
    if(mig_time!=0){
       speed=(disksize+memsize)/mig_time;
       max_throttle=speed;
       printf("migration speed0 %"PRId64"\n",speed);
    }
    //end
    if (strstart(uri, "tcp:", &p)) {
        s = tcp_start_outgoing_migration(mon, p, max_throttle, detach,
                                         (int)qdict_get_int(qdict, "blk"), 
                                         (int)qdict_get_int(qdict, "inc"),
                                         (int)qdict_get_int(qdict, "sparse"),
					 mig_time,
                                         metricopt, 
                                         metricvalue,                         
		                         (int)qdict_get_int(qdict,"compression"),
					 (int)qdict_get_int(qdict,"scheduling"),
					 (int)qdict_get_int(qdict,"dscheduling"),
					 (int)qdict_get_int(qdict,"throttling"));
#if !defined(WIN32)
    } else if (strstart(uri, "exec:", &p)) {
        s = exec_start_outgoing_migration(mon, p, max_throttle, detach,
                                          (int)qdict_get_int(qdict, "blk"), 
                                          (int)qdict_get_int(qdict, "inc"),
					  (int)qdict_get_int(qdict, "sparse"),
                                          mig_time,
                                          metricopt,
                                          metricvalue,
                                          (int)qdict_get_int(qdict,"compression"),
					  (int)qdict_get_int(qdict, "scheduling"),
					  (int)qdict_get_int(qdict,"dscheduling"),
					  (int)qdict_get_int(qdict,"throttling"));

    } else if (strstart(uri, "unix:", &p)) {
        s = unix_start_outgoing_migration(mon, p, max_throttle, detach,
					  (int)qdict_get_int(qdict, "blk"),
                                          (int)qdict_get_int(qdict, "inc"),
    					  (int)qdict_get_int(qdict, "sparse"),
                                          mig_time,
                                          metricopt,
                                          metricvalue,
                                          (int)qdict_get_int(qdict,"compression"),
					  (int)qdict_get_int(qdict, "scheduling"),
					  (int)qdict_get_int(qdict, "dscheduling"),
					  (int)qdict_get_int(qdict, "throttling"));
					
    } else if (strstart(uri, "fd:", &p)) {
        s = fd_start_outgoing_migration(mon, p, max_throttle, detach, 
                                        (int)qdict_get_int(qdict, "blk"), 
                                        (int)qdict_get_int(qdict, "inc"),
                                        (int)qdict_get_int(qdict, "sparse"),
                                        mig_time,
                                        metricopt,
                                        metricvalue,
                                        (int)qdict_get_int(qdict,"compression"),
					(int)qdict_get_int(qdict, "scheduling"),
					(int)qdict_get_int(qdict, "dscheduling"),
					(int)qdict_get_int(qdict, "throttling"));

#endif
    } else {
        monitor_printf(mon, "unknown migration protocol: %s\n", uri);
        return -1;
    }

    if (s == NULL) {
        monitor_printf(mon, "migration failed\n");
        return -1;
    }

    if (current_migration) {
        current_migration->release(current_migration);
    }

    current_migration = s;
    return 0;
}

int do_migrate_cancel(Monitor *mon, const QDict *qdict, QObject **ret_data)
{
    MigrationState *s = current_migration;

    if (s)
        s->cancel(s);

    return 0;
}

int do_migrate_set_perfvalue(Monitor *mon, const QDict *qdict, QObject **ret_data)
{
    double d;
    FdMigrationState *s;

    d = qdict_get_int(qdict, "value");
    d = MAX(0, d);

    s = migrate_to_fms(current_migration);
    if (s) {
       s->mig_state.metricvalue=d; 
    }

    return 0;
}

int do_migrate_set_speed(Monitor *mon, const QDict *qdict, QObject **ret_data)
{
    double d;
    FdMigrationState *s;

    d = qdict_get_double(qdict, "value");
    d = MAX(0, MIN(UINT32_MAX, d));
    max_throttle = d;

    s = migrate_to_fms(current_migration);
    if (s && s->file) {
        qemu_file_set_rate_limit(s->file, max_throttle);
    }

    return 0;
}

/* amount of nanoseconds we are willing to wait for migration to be down.
 * the choice of nanoseconds is because it is the maximum resolution that
 * get_clock() can achieve. It is an internal measure. All user-visible
 * units must be in seconds */
static uint64_t max_downtime = 30000000;

uint64_t migrate_max_downtime(void)
{
    return max_downtime;
}

int do_migrate_set_downtime(Monitor *mon, const QDict *qdict,
                            QObject **ret_data)
{
    double d;

    d = qdict_get_double(qdict, "value") * 1e9;
    d = MAX(0, MIN(UINT64_MAX, d));
    max_downtime = (uint64_t)d;

    return 0;
}

static void migrate_print_status(Monitor *mon, const char *name,
                                 const QDict *status_dict)
{
    QDict *qdict;

    qdict = qobject_to_qdict(qdict_get(status_dict, name));

   monitor_printf(mon, "transferred %s: %" PRIu64 " kbytes\n", name,
                        qdict_get_int(qdict, "transferred") >> 10);
    monitor_printf(mon, "remaining %s: %" PRIu64 " kbytes\n", name,
                        qdict_get_int(qdict, "remaining") >> 10);
    monitor_printf(mon, "total %s: %" PRIu64 " kbytes\n", name,
                        qdict_get_int(qdict, "total") >> 10);
    monitor_printf(mon, "sparse blocks %s: %" PRIu64 " kbytes\n", name,
			qdict_get_int(qdict, "saving") >> 10);

}

void do_info_migrate_print(Monitor *mon, const QObject *data)
{
    QDict *qdict;

    qdict = qobject_to_qdict(data);

    monitor_printf(mon, "Migration status: %s\n",
                   qdict_get_str(qdict, "status"));

    if (qdict_haskey(qdict, "ram")) {
        migrate_print_status(mon, "ram", qdict);
    }

    if (qdict_haskey(qdict, "disk")) {
        migrate_print_status(mon, "disk", qdict);
    }
}

static void migrate_put_status(QDict *qdict, const char *name,
                               uint64_t trans, uint64_t rem, uint64_t total, uint64_t saving)
{
    QObject *obj;

    obj = qobject_from_jsonf("{ 'transferred': %" PRId64 ", "
                               "'remaining': %" PRId64 ", "
                               "'total': %" PRId64 ","
							   "'saving': %" PRId64 "}", trans, rem, total, saving);
    qdict_put_obj(qdict, name, obj);
}

/**
 * do_info_migrate(): Migration status
 *
 * Return a QDict. If migration is active there will be another
 * QDict with RAM migration status and if block migration is active
 * another one with block migration status.
 *
 * The main QDict contains the following:
 *
 * - "status": migration status
 * - "ram": only present if "status" is "active", it is a QDict with the
 *   following RAM information (in bytes):
 *          - "transferred": amount transferred
 *          - "remaining": amount remaining
 *          - "total": total
 * - "disk": only present if "status" is "active" and it is a block migration,
 *   it is a QDict with the following disk information (in bytes):
 *          - "transferred": amount transferred
 *          - "remaining": amount remaining
 *          - "total": total
 *
 * Examples:
 *
 * 1. Migration is "completed":
 *
 * { "status": "completed" }
 *
 * 2. Migration is "active" and it is not a block migration:
 *
 * { "status": "active",
 *            "ram": { "transferred": 123, "remaining": 123, "total": 246 } }
 *
 * 3. Migration is "active" and it is a block migration:
 *
 * { "status": "active",
 *   "ram": { "total": 1057024, "remaining": 1053304, "transferred": 3720 },
 *   "disk": { "total": 20971520, "remaining": 20880384, "transferred": 91136 }}
 */
void do_info_migrate(Monitor *mon, QObject **ret_data)
{
    QDict *qdict;
    MigrationState *s = current_migration;

    if (s) {
        switch (s->get_status(s)) {
        case MIG_STATE_ACTIVE:
            qdict = qdict_new();
            qdict_put(qdict, "status", qstring_from_str("active"));

            migrate_put_status(qdict, "ram", ram_bytes_transferred(),
                               ram_bytes_remaining(), ram_bytes_total(), ram_bytes_saving());

            if (blk_mig_active()) {
                migrate_put_status(qdict, "disk", blk_mig_bytes_transferred(),
                                   blk_mig_bytes_remaining(),
                                   blk_mig_bytes_total(),
                                   blk_mig_bytes_saving());
            }

            *ret_data = QOBJECT(qdict);
            break;
        case MIG_STATE_COMPLETED:
            *ret_data = qobject_from_jsonf("{ 'status': 'completed' }");
            break;
        case MIG_STATE_ERROR:
            *ret_data = qobject_from_jsonf("{ 'status': 'failed' }");
            break;
        case MIG_STATE_CANCELLED:
            *ret_data = qobject_from_jsonf("{ 'status': 'cancelled' }");
            break;
        }
    }
}

/* shared migration helpers */

void migrate_fd_monitor_suspend(FdMigrationState *s, Monitor *mon)
{
    s->mon = mon;
    if (monitor_suspend(mon) == 0) {
        DPRINTF("suspending monitor\n");
    } else {
        monitor_printf(mon, "terminal does not allow synchronous "
                       "migration, continuing detached\n");
    }
}

void migrate_fd_error(FdMigrationState *s)
{
    DPRINTF("setting error state\n");
    s->state = MIG_STATE_ERROR;
    migrate_fd_cleanup(s);
}

int migrate_fd_cleanup(FdMigrationState *s)
{
    int ret = 0;

    qemu_set_fd_handler2(s->fd, NULL, NULL, NULL, NULL);

    if (s->file) {
        DPRINTF("closing file\n");
        if (qemu_fclose(s->file) != 0) {
            ret = -1;
        }
        s->file = NULL;
    }

    if (s->fd != -1)
        close(s->fd);

    /* Don't resume monitor until we've flushed all of the buffers */
    if (s->mon) {
        monitor_resume(s->mon);
    }

    s->fd = -1;

    return ret;
}

void migrate_fd_put_notify(void *opaque)
{
    FdMigrationState *s = opaque;

    qemu_set_fd_handler2(s->fd, NULL, NULL, NULL, NULL);
    qemu_file_put_notify(s->file);
}

ssize_t migrate_fd_put_buffer(void *opaque, const void *data, size_t size)
{
    FdMigrationState *s = opaque;
    ssize_t ret;
	
    do {
        ret = s->write(s, data, size);
    } while (ret == -1 && ((s->get_error(s)) == EINTR));

    if (ret == -1)
        ret = -(s->get_error(s));

    if (ret == -EAGAIN)
        qemu_set_fd_handler2(s->fd, NULL, NULL, migrate_fd_put_notify, s);
	
    return ret;
}
/*
static void sample_rate_tick(void *opaque)
{
   printf("sample_rate_tick\n");
   FdMigrationState *s = opaque;
 //  int64_t total_throughput = get_throughput();
 //  int64_t current_throughput=total_throughput-s->last_throughput;
 //  s->last_throughput = total_throughput;
 //  printf("during migration sample %"PRId64" current_throughput %"PRId64"\n",qemu_get_clock(rt_clock)-s->starttime,current_throughput/1024/1024/1);
 //  s->vmstop=1;
  int64_t total_latency = get_latency();
        int64_t current_total_latency = (total_latency-s->last_total_latency);
        s->last_total_latency = total_latency;

        int64_t total_ops = get_ops();
        int64_t current_ops = (total_ops-s->last_total_ops);
        s->last_total_ops = total_ops;

        int64_t current_latency = 0;
        if(current_ops>0)
              current_latency = current_total_latency / current_ops ;
        s->last_latency = current_latency;


        printf("current_total_latency %"PRId64" current ops %"PRId64" current latency %"PRId64" \n",current_total_latency,current_ops,current_latency);

   qemu_mod_timer(s->timer, qemu_get_clock(rt_clock)+30000);

   return;
}
*/

static int64_t maxvalue(int64_t x, int64_t y)
{
    if(x>=y)
       return x;
    else
       return y;
}

static void migration_rate_tick(void *opaque)
{
   FdMigrationState *s = opaque;
   int interval=30000;

   int64_t total_transfer =  my_blk_mig_bytes_transferred();
   int64_t current_transfer = total_transfer - s->last_transferred;
   int64_t real_speed = current_transfer*1000/s->last_interval; //Bytes per second 
   
   int64_t pasttime=(qemu_get_clock(rt_clock)-s->starttime)/1000L;
   printf("time %"PRId64" real_speed %"PRId64" ",pasttime,real_speed);

   int64_t memsize=ram_bytes_remaining();
   int64_t remaindisksize=get_remaining_dirty();
   int64_t speed = 0L;
   int64_t maxspeed=80L*1024L*1024L;
   int64_t restdisk=(bdrv_get_totallength()-total_transfer);
 
   //old version for dirtyrate which is the average rate comparing to time zero
 /*int64_t dirtyamount=(disksize-restdisk);
   int64_t dirtyrate=dirtyamount/pasttime;
   int64_t newdirtyrate=dirtyrate;
  */
 
   //old drity - transferred + generated = new dirty 
   int64_t newgenerate = remaindisksize + current_transfer - s->last_dirty;
   int64_t dirtyrate=newgenerate*1000/s->last_interval;
   int64_t newdirtyrate=dirtyrate;

   int64_t resttime=(uint64_t)(s->mig_state.mig_time)-pasttime;
   int64_t real_speed_MB = real_speed >> 20L;
   int64_t last_speed_MB = s->last_speed >> 20L;
   int64_t speed_MB = 0L;
 
   if(s->mig_state.mig_time <= pasttime)
   {
      //already over the time
      speed=maxspeed;
   }else { 
        /*pess*/      
     //  speed=(disksize+memsize+dirtyrate*resttime)/(resttime);
        /*opt*/
//      speed=(disksize+memsize)/resttime;
       /*pess-80*/
       if((bdrv_get_totallength()<=total_transfer)||(s->precopy==0))
       { 
          s->precopy=0;
       //   speed=maxspeed;
          interval=5000;

  /*        if(dirtyrate>s->last_dirtyrate)
                newdirtyrate=dirtyrate+(dirtyrate-s->last_dirtyrate)*disksize/(real_speed*s->last_interval/1000L);
          else{
                int64_t temprate=(s->last_dirtyrate-dirtyrate)*disksize/(real_speed*s->last_interval/1000L);
                if(temprate>dirtyrate)
                    newdirtyrate=0;
                else
                    newdirtyrate=dirtyrate-temprate;
          } */
          speed=(remaindisksize+memsize+newdirtyrate*resttime)/resttime;
       }
      else {
       //    newdirtyrate=dirtyrate;
   /*       if(dirtyrate>s->last_dirtyrate)
          	newdirtyrate=dirtyrate+(dirtyrate-s->last_dirtyrate)*restdisk/(real_speed*s->last_interval/1000L);
          else{
                int64_t temprate=(s->last_dirtyrate-dirtyrate)*restdisk/(real_speed*s->last_interval/1000L);
                if(temprate>dirtyrate)
                    newdirtyrate=0;
                else
                    newdirtyrate=dirtyrate-temprate;
          } */
          speed=(remaindisksize+memsize+newdirtyrate*resttime)/resttime;          
          if(restdisk<speed*30L) {
             uint64_t interval_64=restdisk*1000/speed; 
             interval=interval_64;
             printf("approaching pre-copy ending: interval %"PRId64"\n",interval_64);
          }
      }

      speed_MB = speed >> 20L; 
      if((real_speed_MB<last_speed_MB)&&(speed_MB>=real_speed_MB)){
          printf("extend speed from %"PRId64" ",speed);
          speed=speed*s->last_speed/real_speed;
          printf(" to %"PRId64" ",speed);
      }
      if(speed>maxspeed)
          speed =maxspeed;
   }
   speed_MB = speed >> 20L;
   printf("new generate %"PRId64" dirtyrate %"PRId64" new dirty rate %"PRId64" remaining disk %"PRId64" ram %"PRId64" \n",newgenerate,dirtyrate,newdirtyrate,remaindisksize,memsize);
     
    printf("real_speed_%"PRId64" last_speed_%"PRId64" speed_%"PRId64"\n",real_speed,s->last_speed,speed);
    printf("real_speed_MB %"PRId64" last_speed_MB %"PRId64" speed_MB %"PRId64"\n",real_speed_MB,last_speed_MB,speed_MB);

   if((s->mig_state.metricopt==1)||(s->mig_state.metricopt==2)){   
   	int64_t total_throughput = get_throughput();
   	int64_t current_throughput=(total_throughput-s->last_throughput)*1000/s->last_interval; //Bytes per second
   	s->last_throughput = total_throughput;

   	int64_t current_throughput_MB = current_throughput >> 20L; 

  	printf("current_throughput_MB %"PRId64"\n",current_throughput_MB);
  
        if(s->mig_state.metricopt==1){
  		if(current_throughput_MB>=s->mig_state.metricvalue)
  		{
     			printf("case 1 ");
                	printf("max speed %"PRId64" s->last_speed+step %"PRId64"\n",speed,s->last_speed+speed_step);
     			speed=maxvalue(speed,s->last_speed+speed_step);  
  		}
  		else
  		{
     			printf("case 2 ");
                        printf("max speed %"PRId64" s->last_speed-step %"PRId64"\n",speed,s->last_speed-speed_step);
     			speed=maxvalue(speed,s->last_speed-speed_step);
  		}
        }
  }
  if((s->mig_state.metricopt==0)||(s->mig_state.metricopt==2)){
        int64_t total_latency = get_latency();
        int64_t current_total_latency = (total_latency-s->last_total_latency); 
        s->last_total_latency = total_latency;
        
        int64_t total_ops = get_ops();
        int64_t current_ops = (total_ops-s->last_total_ops);
        s->last_total_ops = total_ops; 

        int64_t current_latency = 0;
        if(current_ops>0)
              current_latency = current_total_latency / current_ops ;
        s->last_latency = current_latency;        

        printf("current_latency %"PRId64"\n",current_latency);
        if(s->mig_state.metricopt==0){ 
        	if(current_latency <= s->mig_state.metricvalue)
        	{
                	printf("case 1 ");
                	printf("max speed %"PRId64" s->last_speed+step %"PRId64"\n",speed,s->last_speed+speed_step);
                	speed=maxvalue(speed,s->last_speed+speed_step);
        	}
        	else 
        	{
                	printf("case 2 ");
                	printf("max speed %"PRId64" s->last_speed-step %"PRId64"\n",speed,s->last_speed-speed_step);
                	speed=maxvalue(speed,s->last_speed-speed_step);
        	}
  	}
  }  
  printf("final speed %"PRId64"\n",speed);
  qemu_file_set_rate_limit(s->file,speed);
  qemu_mod_timer(s->timer1,qemu_get_clock(rt_clock)+interval);
  s->last_dirtyrate=dirtyrate;
  s->last_interval=interval;
  s->last_speed=speed;
  s->last_dirty=remaindisksize;
  s->last_transferred = total_transfer;
  return; 
}

/*
static void init_rate_tick(void *opaque)
{
  // printf("sample_rate_tick\n");
   FdMigrationState *s = opaque;
   uint64_t total_throughput = get_throughput();
   uint64_t current_throughput=total_throughput-s->last_throughput;
   s->last_throughput = total_throughput;
   printf("before migration sample %"PRId64" current_throughput %"PRId64"\n",qemu_get_clock(rt_clock)-s->starttime,current_throughput/1024L/1024L/1L);
   
   s->timer1= qemu_new_timer(rt_clock, migration_rate_tick, s);
   qemu_mod_timer(s->timer1, qemu_get_clock(rt_clock) + 30000);
   
   s->vmstop=0;
   qemu_del_timer(s->timer);
   qemu_free_timer(s->timer);
   s->timer=qemu_new_timer(rt_clock,sample_rate_tick,s);
  // qemu_mod_timer(s->timer, qemu_get_clock(rt_clock)+1000);
   return;
}*/


void migrate_fd_connect(FdMigrationState *s)
{
    int ret;
    printf("migrate_fd_connect\n");
    s->file = qemu_fopen_ops_buffered(s,
                                      s->bandwidth_limit,
                                      migrate_fd_put_buffer,
                                      migrate_fd_put_ready,
                                      migrate_fd_wait_for_unfreeze,
                                      migrate_fd_close);

    DPRINTF("beginning savevm\n");
    //add by Pacer for adaptive system
    if(s->mig_state.mig_time>0){
    	s->starttime=qemu_get_clock(rt_clock);
    	s->last_throughput=get_throughput();
    	s->last_transferred=0;
    	s->last_dirty=bdrv_get_totallength();
   // 	s->timer= qemu_new_timer(rt_clock, sample_rate_tick, s);
    	s->timer1= qemu_new_timer(rt_clock, migration_rate_tick, s);
    	s->precopy=1;
   //  	qemu_mod_timer(s->timer, qemu_get_clock(rt_clock) + 30000);
    	s->last_speed=(ram_bytes_remaining()+bdrv_get_totallength())/s->mig_state.mig_time;
        qemu_mod_timer(s->timer1,qemu_get_clock(rt_clock)+30000);
    	s->last_interval=30000L;
    	s->vmstop=0;
    }
    //end

    ret = qemu_savevm_state_begin(s->mon, s->file, s->mig_state.blk,
                                  s->mig_state.shared,s->mig_state.sparse,s->mig_state.mig_time,s->mig_state.compression,s->mig_state.scheduling,s->mig_state.dscheduling,s->mig_state.throttling);
    if (ret < 0) {
        DPRINTF("failed, %d\n", ret);
        migrate_fd_error(s);
        return;
    }
    
    migrate_fd_put_ready(s); 
}

void migrate_fd_put_ready(void *opaque)
{
//  printf("migrate_fd_put_ready\n");
    FdMigrationState *s = opaque;
    
    if (s->state != MIG_STATE_ACTIVE) {
        DPRINTF("put_ready returning because of non-active state\n");
        return;
    }

    //add by Pacer for adaptive system
    if (s->vmstop==1){
      //   printf("vmstop at %"PRId64"\n",qemu_get_clock(rt_clock));
         return;
    }
    //end

    if (qemu_savevm_state_iterate(s->mon, s->file) == 1) {
        int state;
        int old_vm_running = vm_running;

        DPRINTF("done iterating\n");
        time_t rawtime;
        struct tm * timeinfo;
        time ( &rawtime );
        timeinfo = localtime ( &rawtime );
        printf("vm stop at : %s", asctime (timeinfo));
		
        vm_stop(0);

        qemu_aio_flush();
        bdrv_flush_all();
        if ((qemu_savevm_state_complete(s->mon, s->file)) < 0) {
            if (old_vm_running) {
                vm_start();
            }
            state = MIG_STATE_ERROR;
        } else {
            state = MIG_STATE_COMPLETED;
            
        }
		if (migrate_fd_cleanup(s) < 0) {
            if (old_vm_running) {
                vm_start();
            }
            state = MIG_STATE_ERROR;
        }
        s->state = state;
    }
}

int migrate_fd_get_status(MigrationState *mig_state)
{
    FdMigrationState *s = migrate_to_fms(mig_state);
    return s->state;
}

void migrate_fd_cancel(MigrationState *mig_state)
{
    FdMigrationState *s = migrate_to_fms(mig_state);

    if (s->state != MIG_STATE_ACTIVE)
        return;

    DPRINTF("cancelling migration\n");

    s->state = MIG_STATE_CANCELLED;
    qemu_savevm_state_cancel(s->mon, s->file);

    migrate_fd_cleanup(s);
}

void migrate_fd_release(MigrationState *mig_state)
{
    FdMigrationState *s = migrate_to_fms(mig_state);

    DPRINTF("releasing state\n");
   
    if (s->state == MIG_STATE_ACTIVE) {
        s->state = MIG_STATE_CANCELLED;
        migrate_fd_cleanup(s);
    }
   qemu_free(s);
}

void migrate_fd_wait_for_unfreeze(void *opaque)
{
    FdMigrationState *s = opaque;
    int ret;

    DPRINTF("wait for unfreeze\n");
    if (s->state != MIG_STATE_ACTIVE)
        return;

    do {
        fd_set wfds;

        FD_ZERO(&wfds);
        FD_SET(s->fd, &wfds);

        ret = select(s->fd + 1, NULL, &wfds, NULL, NULL);
    } while (ret == -1 && (s->get_error(s)) == EINTR);
}

int migrate_fd_close(void *opaque)
{
    FdMigrationState *s = opaque;
    qemu_set_fd_handler2(s->fd, NULL, NULL, NULL, NULL);
    if(s->timer){
    	qemu_del_timer(s->timer);
    	qemu_free_timer(s->timer);
    }
    if(s->timer1){	
    	qemu_del_timer(s->timer1);
    	qemu_free_timer(s->timer1);
    }
    return s->close(s);
}
