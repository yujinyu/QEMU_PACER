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

#include "math.h"
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
static uint64_t max_throttle =  (32 << 20);

/* Pacer constants */
static uint64_t speed_step = (2 << 20);
static uint64_t maxspeed = (80 << 20 );
static uint64_t countmaxspeed=0L;
static uint64_t setmaxspeed=(80<<20);
static uint64_t netmaxspeed = (50 <<20);  //1Gbps 12MB in io bottleneck at dest
static uint64_t shortinterval = 5000L;
static uint8_t vmstop_flag=0; 
static uint8_t allow_throttle=1;
static uint64_t rmaxspeed = 0L;
static uint64_t emaxspeed = 0L;
static uint64_t mswitch =0L;
static uint64_t throttle=0;
static bool disable_maxspeed_adjust=0;
static uint64_t transf_dsize_60s=0;
/* End */

static MigrationState *current_migration;

/* Pacer Function prototypes */
static void migration_rate_tick(void *opaque);
static void setdirtyratemem_tick(void *opaque);
static void migration_predict_rate_tick(void *opaque);
/* End */

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
    /*(1) get migration time*/
    int mig_time=mig_time=qdict_get_int(qdict,"mig_time");
    printf("migration requried time %d ",mig_time);
    /*(2) get performance requirement, throughput: -r+value; delay: empty+value; no: empty+empty */
    //metricopt: 0: latency: 1 throughput 2: none
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
    int prediction=(int)qdict_get_int(qdict,"prediction");
    printf("prediction %d\n",prediction); 
    if (strstart(uri, "tcp:", &p)) {
        s = tcp_start_outgoing_migration(mon, p, max_throttle, detach,
                                         (int)qdict_get_int(qdict, "blk"), 
                                         (int)qdict_get_int(qdict, "inc"),
                                         (int)qdict_get_int(qdict, "sparse"),
					 mig_time,
                                         metricopt, 
                                         metricvalue,
 					 (int)qdict_get_int(qdict, "policy"),                         
		                         (int)qdict_get_int(qdict,"compression"),
					 (int)qdict_get_int(qdict,"scheduling"),
					 (int)qdict_get_int(qdict,"dscheduling"),
					 (int)qdict_get_int(qdict,"throttling"),
					 (int)qdict_get_int(qdict,"prediction"));
#if !defined(WIN32)
    } else if (strstart(uri, "exec:", &p)) {
        s = exec_start_outgoing_migration(mon, p, max_throttle, detach,
                                          (int)qdict_get_int(qdict, "blk"), 
                                          (int)qdict_get_int(qdict, "inc"),
					  (int)qdict_get_int(qdict, "sparse"),
                                          mig_time,
                                          metricopt,
                                          metricvalue,
					  (int)qdict_get_int(qdict, "policy"),
                                          (int)qdict_get_int(qdict,"compression"),
					  (int)qdict_get_int(qdict, "scheduling"),
					  (int)qdict_get_int(qdict,"dscheduling"),
					  (int)qdict_get_int(qdict,"throttling"),
					  (int)qdict_get_int(qdict,"prediction"));

    } else if (strstart(uri, "unix:", &p)) {
        s = unix_start_outgoing_migration(mon, p, max_throttle, detach,
					  (int)qdict_get_int(qdict, "blk"),
                                          (int)qdict_get_int(qdict, "inc"),
    					  (int)qdict_get_int(qdict, "sparse"),
                                          mig_time,
                                          metricopt,
                                          metricvalue,
					  (int)qdict_get_int(qdict, "policy"),
                                          (int)qdict_get_int(qdict,"compression"),
					  (int)qdict_get_int(qdict, "scheduling"),
					  (int)qdict_get_int(qdict, "dscheduling"),
					  (int)qdict_get_int(qdict, "throttling"),
					  (int)qdict_get_int(qdict, "prediction"));
					
    } else if (strstart(uri, "fd:", &p)) {
        s = fd_start_outgoing_migration(mon, p, max_throttle, detach, 
                                        (int)qdict_get_int(qdict, "blk"), 
                                        (int)qdict_get_int(qdict, "inc"),
                                        (int)qdict_get_int(qdict, "sparse"),
                                        mig_time,
                                        metricopt,
                                        metricvalue,
                                        (int)qdict_get_int(qdict, "policy"),
                                        (int)qdict_get_int(qdict,"compression"),
					(int)qdict_get_int(qdict, "scheduling"),
					(int)qdict_get_int(qdict, "dscheduling"),
					(int)qdict_get_int(qdict, "throttling"),
					(int)qdict_get_int(qdict, "prediction"));

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

/* Pacer Function */
/* Descripstion: set preferred value */
int do_migrate_set_perfvalue(Monitor *mon, const QDict *qdict, QObject **ret_data)
{
    int d;
    FdMigrationState *s;

    d = qdict_get_int(qdict, "value");
    d = MAX(0, d);

    s = migrate_to_fms(current_migration);
    if (s) {
       s->mig_state.metricvalue=d; 
    }

    return 0;
}

/* Pacer Function */
/* Descripstion: sync multiple VMs */
void do_info_migrate_newtime(Monitor *mon,QObject **ret_data)
{
    if(current_migration!=NULL){
    	if(current_migration->new_expected_time==0){
		int64_t pasttime=(qemu_get_clock(rt_clock)-current_migration->starttime)/1000L;
    		int64_t transf_dsize =  my_blk_mig_bytes_transferred();
    		int64_t transf_msize = ram_block_bytes_transferred();
    		int64_t speed_real = (transf_dsize+transf_msize)/pasttime;
		printf("speed_real %"PRId64"\n",speed_real);
		if(speed_real!=0)
        		current_migration->new_expected_time=(bdrv_get_totallength()+ram_bytes_remaining())/speed_real;
	}
	monitor_printf(mon,"%"PRId64"\n",current_migration->new_expected_time);		
    }
}

/* Pacer Function */
/* Descripstion: set the migration time */
int do_migrate_set_migrtime(Monitor *mon, const QDict *qdict, QObject **ret_data)
{
    int d;
    FdMigrationState *s;

    d = qdict_get_int(qdict, "value");
    d = MAX(0, d);
    
    s = migrate_to_fms(current_migration);
    if (s) {
	if(s->mig_state.mig_time==0)
	{
		s->mig_state.mig_time=d;
		int64_t pasttime=(qemu_get_clock(rt_clock)-s->starttime);
		s->last_interval=pasttime;
		printf("pasttime %"PRId64"\n",pasttime);
		s->timer = qemu_new_timer(rt_clock, migration_rate_tick, s);
        	qemu_mod_timer(s->timer,qemu_get_clock(rt_clock)+1);
        	s->timer1=qemu_new_timer(rt_clock,setdirtyratemem_tick,s);
        	qemu_mod_timer(s->timer1,qemu_get_clock(rt_clock)+1000L);

	}else{
		s->mig_state.mig_time=d;  
		disable_maxspeed_adjust=1; 
        }      
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

static int64_t maxvalue(int64_t x, int64_t y)
{
    if(x>=y)
       return x;
    else
       return y;
}

/* Pacer Function */
/* Descripstion: return the expected speed */
uint64_t getExpectedSpeed(void* opaque,int stage,int64_t past_time, int64_t remain_time,int64_t remain_precopy_size,int64_t dirty_dsize,int64_t remain_msize,int64_t dirtyset_size,int64_t dirtyrate2,int64_t dirtyrate_mem,int policy)
{
  FdMigrationState *s = opaque;
  int64_t newmigrationtime=(int64_t)(s->mig_state.mig_time);
  s->mig_state.new_expected_time=newmigrationtime;
  int64_t speed=0L;
  int64_t throttle_value=0L;
  int64_t migrtime=past_time+remain_time;
  printf("Get expected speed: stage %d remain_time %"PRId64" remain_precopy_size %"PRId64" dirty_dsize %"PRId64" remain_msize %"PRId64" dirtyset_size %"PRId64" dirtyrate2 %"PRId64" dirtyrate_mem %"PRId64" policy %d\n",stage,remain_time,remain_precopy_size,dirty_dsize,remain_msize,dirtyset_size,dirtyrate2,dirtyrate_mem,policy);
  
  int64_t t3=0L;
  if(dirtyrate_mem>netmaxspeed)
  {
     int64_t x=70L;
     printf("------dirtyrate_mem > netmaxspeed-----------\n");
     t3=remain_msize*x/100L/(netmaxspeed-x*dirtyrate_mem/100L)+30L*0.008+remain_msize*(100L-x)/100L/netmaxspeed;
  }else{
     t3=remain_msize/(netmaxspeed-dirtyrate_mem);
  }
  printf("t3 %"PRId64" ",t3);
  if(remain_time<=t3){
        if(stage==1){
      		speed=setmaxspeed;
                //speed=maxspeed;
  		int64_t newremaintime=getNewExpireTime(maxspeed,remain_time);
                newmigrationtime=past_time+newremaintime+t3;
                printf("run out of time, precopy speed %"PRId64"\n",speed);
                printf("pasttime %"PRId64" expected finish time %"PRId64"\n",past_time,newmigrationtime);
                s->mig_state.new_expected_time=newmigrationtime;
		return speed;  
        }else {
                speed=setmaxspeed;
        //	speed=maxspeed;
                throttle_value=maxspeed/5;
        	if(dirtyrate2<throttle_value)
                {
                   throttle_value=dirtyrate2;
		}
                if(allow_throttle){
        		set_write_throttling(1,throttle_value);
                        throttle=1;
                }
                int64_t newremaintime=dirty_dsize/(maxspeed-throttle_value);
                newmigrationtime=past_time+newremaintime+t3;
                printf("pasttime %"PRId64" expected finish time %"PRId64"\n",past_time,newmigrationtime);
        	printf("run out of time! set throttling\n to %"PRId64" dirty iter speed %"PRId64"\n",throttle_value,speed);
                s->mig_state.new_expected_time=newmigrationtime;
		return speed;
        } 
  }else{
   	if(stage==2){
        	speed=dirty_dsize/(remain_time-t3)+dirtyrate2;
      		if(speed>maxspeed)
                {
			int64_t atleastspeed=dirty_dsize/(remain_time-t3);
                        if(maxspeed<atleastspeed){
                            //new migration time;
                            throttle_value=maxspeed/5;
                            if(dirtyrate2<throttle_value)
                               throttle_value=dirtyrate2;                          
                            int64_t newremaintime=dirty_dsize/(maxspeed-throttle_value);
                            newmigrationtime=past_time+newremaintime+t3; 
                            printf("pasttime %"PRId64" expected finish time %"PRId64"\n",past_time,newmigrationtime);
                        }  
                        else{
			    throttle_value=maxspeed-atleastspeed;
                            printf("pasttime %"PRId64" expected finish time %"PRId64"\n",past_time,migrtime);
                        }
                        if(allow_throttle){ 
                        	set_write_throttling(1,throttle_value);
                                throttle=1;
                        }
                        printf("Dirty iteration case 2: speed %"PRId64" maxspeed %"PRId64" atleastspeed %"PRId64" throttle_value %"PRId64"\n",speed,maxspeed,atleastspeed,throttle_value);
                	s->mig_state.new_expected_time=newmigrationtime;
                         return setmaxspeed;
                         //return maxspeed;
		}else{
                         if(throttle==1)
                         {
 			    throttle=0;
                            set_write_throttling(0,setmaxspeed);   
			 }
                         printf("pasttime %"PRId64" expected finish time %"PRId64"\n",past_time,migrtime);

			if(policy==1){
				return speed;
                         }
                        else 
				return setmaxspeed;
                                //return maxspeed;
		}
      }else{

                if(policy==1)
		{
       			int64_t remain_precopy_size_m=remain_precopy_size/(1024L*1024L);
   	  		int64_t dirtyset_size_m=dirtyset_size/(1024L*1024L);
   	  		int64_t dirtyrate2_m=dirtyrate2/(1024L*1024L);
	
                        int64_t a=0L-dirtyrate2_m;
                        int64_t b=dirtyrate2_m*(remain_time-t3)-remain_precopy_size_m-dirtyset_size_m;
                        int64_t c=dirtyset_size_m*(remain_time-t3);
        		printf("a=%"PRId64" b=%"PRId64" c=%"PRId64"\n",a,b,c);
                        int64_t delta=b*b-4L*a*c;
                        if(a==0)
                        {
				if(b!=0)
				{
					int64_t t2=(dirtyset_size_m*(remain_time-t3))/(remain_precopy_size_m+dirtyset_size_m);
                                        int64_t t1=remain_time-t3-t2;
                                        speed=remain_precopy_size_m/t1;
				}else
					speed=netmaxspeed>>20L;
 				printf("a=0, speed = %"PRId64"MB \n",speed);
                                
                                if(speed>(maxspeed >> 20L))
                                {
                                        //get the new finish time T'
                                        int64_t newremaintime=getNewExpireTime(maxspeed,remain_time-t3);
                                        printf("precopy case 3 speed%"PRId64" maxspeed %"PRId64" newremaintime %"PRId64" from %"PRId64"\n",speed,maxspeed,newremaintime,remain_time-t3);
                                        newmigrationtime=past_time+newremaintime+t3;
                                        printf("pasttime %"PRId64" expected finish time %"PRId64"\n",past_time,newmigrationtime);
                                        speed=setmaxspeed;
                                       // speed=maxspeed;
                                }else
				       printf("pasttime %"PRId64" expected finish time %"PRId64"\n",past_time,migrtime);
                	        s->mig_state.new_expected_time=newmigrationtime;
				speed=speed << 20L;
                                return speed;

                         }else { 
				int64_t t21=((0L-b)+sqrt(delta))/(2*a);
                                int64_t t22=((0L-b)-sqrt(delta))/(2*a);
                                int64_t t2_final=0l;
                                printf("delta=%"PRId64" x1=%"PRId64" x2=%"PRId64" \n",delta,t21,t22);
                                if((t21>=0) && (t21<(remain_time-t3))){ 
                                        int64_t t1=remain_time-t3-t21;
                                        t2_final=t21;
                                        speed=remain_precopy_size_m/t1;
                                        printf("speed1 %"PRId64"\n",speed);
                                        int64_t speed2=0;
                                        if(t21>0)
                                             speed2=(dirtyset_size_m+dirtyrate2_m*t21)/t21;
                                        printf("speed2=%"PRId64"\n",speed2);
                                        speed=speed << 20L;
                                 }
                                 else if((t22>=0)&&(t22<remain_time-t3)){
                                        int64_t t1=remain_time-t3-t22;
                                        t2_final=t22;
                                        speed=remain_precopy_size_m/t1;
                              		printf("speed1 %"PRId64"\n",speed);
                                        int64_t speed2=0;
                                        if(t22>0)
                                             speed2=(dirtyset_size_m+dirtyrate2_m*t22)/t22;
                               		printf("speed2=%"PRId64"\n",speed2);
                                        speed=speed << 20L;
                                 }else {   
                                       	printf("no solution\n");
                                        speed=setmaxspeed;
                                       // speed=maxspeed;
                                 }
                                 //case 3
                                 if(speed>maxspeed)
				 {
					//get the new finish time T'
                                        int64_t newremaintime=getNewExpireTime(maxspeed,remain_time-t3);
                                        speed=setmaxspeed;
                                       // speed=maxspeed;
                                        newmigrationtime=past_time+newremaintime+t3;
                                        printf("pasttime %"PRId64" expected finish time %"PRId64"\n",past_time,newmigrationtime);
				 }else
					printf("pasttime %"PRId64" expected finish time %"PRId64"\n",past_time,migrtime);   
				 
                	         s->mig_state.new_expected_time=newmigrationtime;
				 return speed;
			}
		}else{
            		int64_t t1=0;
        		int64_t t2=0;
                        if(maxspeed>dirtyrate2){         
              			t2=(dirtyset_size)/(maxspeed-dirtyrate2);
                                t1=remain_time-t3-t2;
              			if(t1>0){
                                    speed=remain_precopy_size/t1;
	   	                    printf("t1 %"PRId64" t2 %"PRId64" speed is %"PRId64"\n",t1,t2,speed);
                                    if(speed<=maxspeed)
                                    	return speed;  
                                }
                         }
              
                         int64_t newremaintime=getNewExpireTime(maxspeed,remain_time-t3);
                         printf("precopy case 3 speed%"PRId64" maxspeed %"PRId64" newremaintime %"PRId64" from %"PRId64"\n",speed,maxspeed,newremaintime,remain_time-t3);
                         newmigrationtime=past_time+newremaintime+t3;
                         printf("pasttime %"PRId64" expected finish time %"PRId64"\n",past_time,newmigrationtime);
                         speed=setmaxspeed; 
                      //   speed=maxspeed;
                         
                	 s->mig_state.new_expected_time=newmigrationtime;
  			 return speed;	
   	  	}
	}
  }
}

uint64_t getSpeedAdjusted(void *opaque,uint64_t speed_next_expected)
{
   FdMigrationState *s = opaque;
   if((s->mig_state.metricopt==1)||(s->mig_state.metricopt==2)){   
   	int64_t total_throughput = get_throughput();
   	int64_t current_throughput=(total_throughput-s->last_throughput)*1000/s->last_interval; //Bytes per second
   	s->last_throughput = total_throughput;

   	int64_t current_throughput_MB = current_throughput; 

  	printf("current_throughput %"PRId64" ",current_throughput_MB);
  
        if(s->mig_state.metricopt==1){
  		if(current_throughput_MB>s->mig_state.metricvalue)
  		{
     			printf("case_1 ");
                	printf("max speed %"PRId64" s->last_speed+step %"PRId64" ",speed_next_expected,s->speed_pre_expected+speed_step);
     			speed_next_expected=maxvalue(speed_next_expected,s->speed_pre_expected+speed_step);  
  		}
  		else
  		{
     			printf("case_2 ");
                      //  printf("max speed %"PRId64" s->last_speed-step %"PRId64" ",speed_next_expected,s->speed_pre_expected-speed_step);
     		//	speed_next_expected=speed_next_expected;
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

        printf("current_latency %"PRId64" interval %"PRId64"\n",current_latency,s->last_interval/1000L);
        if(s->mig_state.metricopt==0){ 
        	if(current_latency < s->mig_state.metricvalue)
        	{
                	printf("case_1 ");
                	printf("max speed %"PRId64" s->last_speed+step %"PRId64" ",speed_next_expected,s->speed_pre_expected+speed_step);
                	speed_next_expected=maxvalue(speed_next_expected,s->speed_pre_expected+speed_step);
        	}
        	else 
        	{
                	printf("case_2 ");
                //	printf("max speed %"PRId64" s->last_speed-step %"PRId64" ",speed_next_expected,s->speed_pre_expected-speed_step);
                //	speed_next_expected=speed_next_expected;
        	}
  	}
  }
  if(speed_next_expected>maxspeed)
	speed_next_expected=setmaxspeed;
  printf("speed after adjustment %"PRId64"\n",speed_next_expected);
  return speed_next_expected;  
}

uint64_t getSpeedScaleup(void *opaque,uint64_t speed_next_expected,uint64_t speed_real)
{
   FdMigrationState *s = opaque;
   if(s->speed_scaleup_flag==1)
   {
     //scaleup fails
     if(speed_real<=(s->speed_first_real))
     {
        s->speed_scaleup_flag=0;
     }else{
        //still need? Yes
        if(speed_real< s->speed_before_scaleup){  
          //can judge
          if((s->speed_first_real<=speed_next_expected)&&(speed_next_expected<maxspeed)){
             s->speed_before_scaleup=speed_next_expected;    
             speed_next_expected=speed_next_expected+(s->speed_first_expected-s->speed_first_real);
             if(speed_next_expected>maxspeed) 
                speed_next_expected=setmaxspeed;
             s->speed_scaleup_flag=1;
          }else{
             s->speed_scaleup_flag=0;
	  }
        }else{
          s->speed_scaleup_flag=0;
        } 
     }
   }else{
     //need scale up this time?
     if((speed_real<s->speed_pre_expected)&&(speed_next_expected<maxspeed))
     {
        s->speed_first_expected=s->speed_pre_expected;
        s->speed_first_real=speed_real;
        s->speed_before_scaleup=speed_next_expected; 
        speed_next_expected=speed_next_expected+(s->speed_first_expected-s->speed_first_real);
        if(speed_next_expected>maxspeed)
           speed_next_expected=setmaxspeed;
        s->speed_scaleup_flag=1;
     }else{
        s->speed_scaleup_flag=0;
     }
   }
   printf("speed after scaleup  %"PRId64"\n",speed_next_expected);
   return speed_next_expected;
}


static void getEstimateDirtyRate1and2(int64_t speed,int64_t *pdirtyset_size, int64_t *pdirtyrate2)
{
   return getAveDirtyRate1and2(speed,pdirtyset_size,pdirtyrate2);
}

static uint64_t getEstimateDirtyRateMEM(void *opaque)
{
   FdMigrationState *s = opaque;
   return s->estimateddirtyrate3;
}


static void setdirtyratemem_tick(void *opaque)
{
   FdMigrationState *s = opaque;
   if(ram_get_mem_mode()==1){
        if(s->samplemem==1){
   		int64_t unique=ram_get_unique_dirty();
                int64_t pasttime=(qemu_get_clock(rt_clock)-s->starttime); //ms
   		if((pasttime/1000L)<(s->mig_state.mig_time-3)){
        		double averate=ram_get_unique_ratio();
   			s->last_totalunique+=unique;
   	//		int64_t ave_unique = s->last_totalunique*1000L/pasttime*averate;
                        s->samplecount++;
                        int64_t ave_unique = s->last_totalunique/s->samplecount * averate; 	
    	//	        printf("sd2 time(MS) %"PRId64" unique %"PRId64" ave_unique %"PRId64" averate %f\n",pasttime,unique,ave_unique,averate);
   			s->estimateddirtyrate3=ave_unique; 
   			qemu_mod_timer(s->timer1,qemu_get_clock(rt_clock)+4000L);
                        s->samplemem=0;
        	}
	}else{
                s->samplemem=1;
		ram_set_clean_all();
                qemu_mod_timer(s->timer1,qemu_get_clock(rt_clock)+1000L);
	}
   }
}

static void migration_rate_tick(void *opaque)
{
//   int64_t time0=my_qemu_get_clock_us(rt_clock);
   FdMigrationState *s = opaque;
   int64_t speed_next_expected=0L;
   int interval=shortinterval;

   int64_t pasttime=(qemu_get_clock(rt_clock)-s->starttime)/1000L;
   int64_t remain_time=(uint64_t)(s->mig_state.mig_time)-pasttime;
   printf("time(s) %"PRId64" ",pasttime);
   int64_t speed_expected = s->speed_pre_expected;
   int64_t transf_dsize =  my_blk_mig_bytes_transferred();
   int64_t transf_msize = ram_block_bytes_transferred();
   int64_t speed_real = (transf_dsize-s->transf_pre_dsize+transf_msize-s->transf_pre_msize)*1000L/s->last_interval;  
   int64_t dirty_dsize=get_remaining_dirty();
   int64_t remain_msize=0;

   if(ram_get_mem_mode()==0){
	remain_msize= ram_bytes_remaining();
        printf("Reach mem migration, full speed\n");
        speed_next_expected=netmaxspeed;
        qemu_file_set_rate_limit(s->file,speed_next_expected);
        return;
   }
   else
	remain_msize=s->full_memsize;

   int64_t dirtyrate2=0L;
   int64_t dirtyset_size=0L;
   int64_t dirtyrate_mem=0L; 
   int64_t full_disk_size = bdrv_get_totallength();
   if(transf_dsize<full_disk_size) //PRECOPY
   {
      s->stage=1;
      s->remain_precopy_size=full_disk_size-transf_dsize;
   }else{
      if(s->stage==1){
     	rmaxspeed=0;
        emaxspeed=0;
        maxspeed= setmaxspeed;
        mswitch=1;
      }else
        mswitch=0;
      s->stage=2;
      s->remain_precopy_size=0L; 
   }
  //  int64_t time1=my_qemu_get_clock_us(rt_clock);
   if(s->stage==1)
   {
      dirty_dsize=dirty_dsize-s->remain_precopy_size;
      getEstimateDirtyRate1and2(s->speed_pre_expected,&dirtyset_size,&dirtyrate2);
      dirtyrate_mem=getEstimateDirtyRateMEM(s);
   }
   else
   {
      dirtyset_size=dirty_dsize;
      dirtyrate2=(dirty_dsize-s->dirty_pre_dsize+transf_dsize-s->transf_pre_dsize)*1000L/s->last_interval;
      if(ram_get_mem_mode()==0) { 
        dirtyrate_mem=(remain_msize+transf_msize-s->transf_pre_msize-s->remain_msize)*1000L/s->last_interval;
      }
      else {
        dirtyrate_mem=getEstimateDirtyRateMEM(s);
      } 
   }
  //  int64_t time2=my_qemu_get_clock_us(rt_clock);
    
   printf("dirtyset_size %"PRId64" dirtyrate2 %"PRId64" dirtyrate_mem %"PRId64"\n",dirtyset_size,dirtyrate2,dirtyrate_mem);
   s->dirty_pre_dsize=dirty_dsize;
   s->transf_pre_dsize=transf_dsize;
   s->transf_pre_msize=transf_msize;
   s->remain_msize=remain_msize;
    
   if(!disable_maxspeed_adjust) {
   	if((rmaxspeed==0)&&(emaxspeed==0))
   	{
       		if(speed_real<speed_expected)
       		{
             		rmaxspeed=speed_real;
             		emaxspeed=speed_expected;
       		}
   	}else{
       		if(speed_real<maxspeed)
       		{
           		if((rmaxspeed>=speed_real)&&(emaxspeed<=speed_expected))
           		{
               			countmaxspeed++;
                 		if(countmaxspeed==1)
                    			maxspeed=speed_real;
                 		else
                     			maxspeed=0.8*maxspeed+0.2*speed_real;
               			rmaxspeed=speed_real;
               			emaxspeed=speed_expected;
           		}else if(rmaxspeed<speed_real)
           		{ 
              			rmaxspeed=speed_real;
            	  		emaxspeed=speed_expected;  
           		}
       		}else
       		{
        		// summaxspeed+=speed_real;
         		countmaxspeed++;
       			//  maxspeed=summaxspeed/countmaxspeed; 
       			//  maxspeed=speed_real;
         		maxspeed=0.8*maxspeed+0.2*speed_real;
         		rmaxspeed=speed_real;
         		emaxspeed=speed_expected;  
       		}
   	}  
   }
   printf("rmaxspeed %"PRId64" emaxspeed %"PRId64" maxspeed %"PRId64"\n",rmaxspeed,emaxspeed,maxspeed);
   speed_next_expected=getExpectedSpeed(s,s->stage,pasttime,remain_time,s->remain_precopy_size,dirty_dsize,remain_msize,dirtyset_size,dirtyrate2,dirtyrate_mem,s->mig_state.policy);
  //  int64_t time3=my_qemu_get_clock_us(rt_clock);
   printf("speed_real %"PRId64" speed_expected %"PRId64" speed_next_expected %"PRId64" speed_scaleup_flag %d\n",speed_real,speed_expected,speed_next_expected,s->speed_scaleup_flag);

 // speed_next_expected=getSpeedAdjusted(s,speed_next_expected);

   if(mswitch==0)
   	speed_next_expected=getSpeedScaleup(s,speed_next_expected,speed_real);
  
/* 
   if(s->stage==2)
   {
       interval=shortinterval;
   }
   else {

       if(interval_step!=0)
          interval=maxvalue(s->mig_state.mig_time*1000L/interval_step,mininterval);
       else
          interval=100000; //infinite
      if(s->remain_precopy_size<(speed_next_expected*interval/1000L))
         interval=s->remain_precopy_size*1000L/speed_next_expected+1000;
   }
*/
   interval=shortinterval;

   if( (speed_next_expected >> 20L) > 0 ){
   	int64_t calinterval=1000/(speed_next_expected >> 20L);
        if(calinterval>0){
   		int64_t speed_next_m=1000/calinterval;
   		speed_next_expected=speed_next_m << 20L;
        }
   }
   printf("Final speed %"PRId64" interval %d\n",speed_next_expected,interval);
   if(speed_next_expected < (1*1024*1024))
      speed_next_expected = 1*1024*1024;
   qemu_file_set_rate_limit(s->file,speed_next_expected);
   qemu_mod_timer(s->timer,qemu_get_clock(rt_clock)+interval);
   s->last_interval=interval;
   s->speed_pre_expected=speed_next_expected;
 //   int64_t time4=my_qemu_get_clock_us(rt_clock);
  //  printf("time_dirty %"PRId64" time_compute %"PRId64" time_total %"PRId64"\n",time2-time1,time3-time2,time4-time0);
   return; 
}

static void migration_predict_rate_tick(void *opaque)
{
   //real migration speed
   FdMigrationState *s = opaque;
   int64_t transf_dsize =  my_blk_mig_bytes_transferred();
   int64_t transf_msize = ram_block_bytes_transferred();
   if(transf_dsize_60s==0L){
	transf_dsize_60s=transf_dsize;
        qemu_mod_timer(s->timer,qemu_get_clock(rt_clock)+s->last_interval);
        return; 
   }
  int64_t dirty_dsize=get_remaining_dirty();
  int64_t dirtyrate2=0L;

  int64_t pasttime=(qemu_get_clock(rt_clock)-s->starttime)/1000L;
  int64_t speed_real=(transf_dsize-transf_dsize_60s+transf_msize)/(pasttime-60L);
  int64_t remain_msize=0;
  int t3=0;
  int64_t newfinishtime=0;

  if(ram_get_mem_mode()==0){
       remain_msize= ram_bytes_remaining();
       t3=remain_msize/netmaxspeed; 	
       newfinishtime=pasttime+t3;
  }
  else{
      remain_msize=s->full_memsize; 	
      t3=remain_msize/netmaxspeed;
      if(transf_dsize>bdrv_get_totallength())     
      {
          dirtyrate2=(dirty_dsize-s->dirty_pre_dsize+transf_dsize-s->transf_pre_dsize)*1000L/s->last_interval;
	  if(dirtyrate2>speed_real)
	  	newfinishtime=s->mig_state.new_expected_time;
	  else
	  	newfinishtime=pasttime+dirty_dsize/(speed_real-dirtyrate2)+t3;
      }else{
           newfinishtime=pasttime+getNewExpireTime(speed_real,0)+t3;
      }
  }
  s->dirty_pre_dsize=dirty_dsize;
  s->transf_pre_dsize=transf_dsize;
  s->mig_state.new_expected_time=newfinishtime;
  printf("ave speed %"PRId64" dirtyrate2 %"PRId64"\n",speed_real,dirtyrate2);
  printf("%"PRId64" Predicted time is %"PRId64"\n",pasttime,newfinishtime);
  qemu_mod_timer(s->timer,qemu_get_clock(rt_clock)+s->last_interval);
}

void migrate_fd_connect(FdMigrationState *s)
{
    int ret;
    s->file = qemu_fopen_ops_buffered(s,
                                      s->bandwidth_limit,
                                      migrate_fd_put_buffer,
                                      migrate_fd_put_ready,
                                      migrate_fd_wait_for_unfreeze,
                                      migrate_fd_close);

    DPRINTF("beginning savevm\n");

	//add by Pacer 
        s->starttime=qemu_get_clock(rt_clock);
        s->dirty_pre_dsize=0;
        s->transf_pre_dsize=0;
        s->remain_precopy_size=bdrv_get_totallength();
        s->transf_pre_msize=0;
        s->remain_msize=ram_bytes_remaining();
        s->full_memsize=s->remain_msize;
        s->last_interval=shortinterval;
        s->last_total_latency=get_latency();
        s->last_total_ops=get_ops();
        s->last_throughput=get_throughput();
        s->stage=1;
        s->speed_scaleup_flag = 0;
        s->last_totalunique=0;
        s->last_maxunique=0;
        s->estimateddirtyrate3=0;
        s->samplemem=1;
        s->samplecount=0;
        s->mig_state.new_expected_time=0;
	s->mig_state.starttime=s->starttime;
        printf("mem size %"PRId64"\n",s->remain_msize);

    ret = qemu_savevm_state_begin(s->mon, s->file, s->mig_state.blk,
                                  s->mig_state.shared,s->mig_state.sparse,s->mig_state.mig_time,s->mig_state.compression,s->mig_state.scheduling,s->mig_state.dscheduling,s->mig_state.throttling);
    if (ret < 0) {
        DPRINTF("failed, %d\n", ret);
        migrate_fd_error(s);
        return;
    }
    
    //add for adaptive system by Pacer
    if(s->mig_state.mig_time>0){
        s->speed_pre_expected=getExpectedSpeed(s,s->stage,0L,s->mig_state.mig_time,s->remain_precopy_size,s->dirty_pre_dsize,s->remain_msize,0,0,0,s->mig_state.policy);
        if(s->speed_pre_expected==0L)
	{
           printf("Error: getExpectedSpeed return 0!!\n");
        }

        qemu_file_set_rate_limit(s->file,s->speed_pre_expected);
        s->timer = qemu_new_timer(rt_clock, migration_rate_tick, s);
        qemu_mod_timer(s->timer,qemu_get_clock(rt_clock)+s->last_interval);
        s->timer1=qemu_new_timer(rt_clock,setdirtyratemem_tick,s);
        qemu_mod_timer(s->timer1,qemu_get_clock(rt_clock)+1000L);   
   } else if (s->mig_state.prediction > 0){
        //not set control time, but need to report prediction time 
	s->timer = qemu_new_timer(rt_clock, migration_predict_rate_tick, s);
        qemu_mod_timer(s->timer,qemu_get_clock(rt_clock)+60000L);
   }	

   
   //end
    
    migrate_fd_put_ready(s); 
}

void migrate_fd_put_ready(void *opaque)
{
    FdMigrationState *s = opaque;
    
    if (s->state != MIG_STATE_ACTIVE) {
        DPRINTF("put_ready returning because of non-active state\n");
        return;
    }


    if (qemu_savevm_state_iterate(s->mon, s->file) == 1) {
        int state;
        int old_vm_running = vm_running;

        DPRINTF("done iterating\n");
        time_t rawtime;
        struct tm * timeinfo;
        time ( &rawtime );
        timeinfo = localtime ( &rawtime );
        printf("vm stop at : %s", asctime (timeinfo));
	if(vmstop_flag==0)	//add by Pacer for adaptive system
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
         
        //add by Pacer
        int64_t current_total_latency =get_latency();
        printf("current latency %"PRId64"  pre latency %"PRId64"\n",current_total_latency,s->last_total_latency);
        int64_t current_ops = (get_ops());
        printf("currrent ops %"PRId64" last op %"PRId64" \n",current_ops,s->last_total_ops);
        int64_t current_latency = 0;
        if(current_ops>0)
              current_latency = (current_total_latency-s->last_total_latency) / (current_ops-s->last_total_ops) ;
          
        printf("IO ave_latency during Migration %"PRId64" ",current_latency);

       
        int64_t pasttime=(qemu_get_clock(rt_clock)-s->starttime)/1000L;
        printf("pasttime %"PRId64"\n",pasttime);
        int64_t current_throughput=get_throughput();
        printf("current throughput %"PRId64" last throughput %"PRId64"\n",current_throughput,s->last_throughput);
        current_throughput=(current_throughput-s->last_throughput)/pasttime; //Bytes per second
        
        printf("IO throughput %"PRId64"\n",current_throughput);

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
    if (s->state != MIG_STATE_ACTIVE)
        return;

    DPRINTF("cancelling migration\n");

    s->state = MIG_STATE_CANCELLED;
    qemu_savevm_state_cancel(s->mon, s->file);

    migrate_fd_cleanup(s);
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
