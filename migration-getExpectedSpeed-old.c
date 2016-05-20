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
static uint64_t maxspeed_dirty= (80 << 20);
static uint64_t netmaxspeed = (125 <<20);  //1Gbps
static uint64_t mininterval = 5000L;
static uint64_t shortinterval = 5000L;
static uint8_t vmstop_flag=0; 
static uint64_t interval_step=20;
static uint8_t allow_throttle=0;
static uint8_t throttling=0;
/* End */

static MigrationState *current_migration;

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
					  (int)qdict_get_int(qdict, "policy"),
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
					  (int)qdict_get_int(qdict, "policy"),
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
                                        (int)qdict_get_int(qdict, "policy"),
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

static int64_t maxvalue(int64_t x, int64_t y)
{
    if(x>=y)
       return x;
    else
       return y;
}

uint64_t getExpectedSpeed(void* opaque,int stage,int64_t remain_time,int64_t remain_precopy_size,int64_t dirty_dsize,int64_t remain_msize,int64_t dirtyset_size,int64_t dirtyrate2,int64_t dirtyrate_mem,int policy)
{
  FdMigrationState *s = opaque;
  int64_t speed=0L;
  if((stage==2)&&((dirtyrate2>=maxspeed))&&(throttling==0))
  {
     speed=maxspeed; 
     throttling=1; 
  }else{ 
   	printf("Get expected speed: stage %d remain_time %"PRId64" remain_precopy_size %"PRId64" dirty_dsize %"PRId64" remain_msize %"PRId64" dirtyset_size %"PRId64" dirtyrate2 %"PRId64" dirtyrate_mem %"PRId64" policy %d\n",stage,remain_time,remain_precopy_size,dirty_dsize,remain_msize,dirtyset_size,dirtyrate2,dirtyrate_mem,policy);
   	int64_t t3=remain_msize/( (maxspeed) -dirtyrate_mem);
      printf("t3 %"PRId64" ",t3);
        if((remain_time<t3)||((remain_time==t3)&&(remain_precopy_size+dirty_dsize>0))){
           	speed=maxspeed;
                throttling=1;
        }else if(remain_time==t3)
 	{
		speed=netmaxspeed;
        }else {
   		if(policy==1){
      			if(stage==2)
      			{
         			speed=dirty_dsize/(remain_time-t3)+dirtyrate2;
      			}else{
          			remain_precopy_size=remain_precopy_size/(1024L*1024L);
          			dirty_dsize=dirty_dsize/(1024L*1024L);
   	  			remain_msize=remain_msize/(1024L*1024L);
   	  			dirtyset_size=dirtyset_size/(1024L*1024L);
   	  			dirtyrate2=dirtyrate2/(1024L*1024L);
   	  			dirtyrate_mem=dirtyrate_mem/(1024L*1024L);
	
                                int64_t a=0L-dirtyrate2;
                                int64_t b=dirtyrate2*(remain_time-t3)-remain_precopy_size-dirtyset_size;
                                int64_t c=dirtyset_size*(remain_time-t3);
        			printf("a=%"PRId64" b=%"PRId64" c=%"PRId64"\n",a,b,c);
                                int64_t delta=b*b-4L*a*c;
                                if(a==0)
                                {
					if(b!=0)
					{
						int64_t t2=(dirtyset_size*(remain_time-t3))/(remain_precopy_size+dirtyset_size);
                                                int64_t t1=remain_time-t3-t2;
                                                speed=remain_precopy_size/t1;
					}else
						speed=netmaxspeed>>20L;
 					printf("a=0, speed = %"PRId64"MB \n",speed);

                                        speed=speed << 20L; 
                                 }else { 
					int64_t t21=((0L-b)+sqrt(delta))/(2*a);
                                        int64_t t22=((0L-b)-sqrt(delta))/(2*a);
                                        printf("delta=%"PRId64" x1=%"PRId64" x2=%"PRId64" \n",delta,t21,t22);
                                        if((t21>=0) && (t21<(remain_time-t3))){ 
                                                int64_t t1=remain_time-t3-t21;
                                                speed=remain_precopy_size/t1;
                                                printf("speed1 %"PRId64"\n",speed);
                                                int64_t speed2=0;
                                                if(t21>0)
                                                  speed2=(dirtyset_size+dirtyrate2*t21)/t21;
                                                printf("speed2=%"PRId64"\n",speed2);
                                                speed=speed << 20L;
                                        }
                                        else if((t22>=0)&&(t22<c)){
                                                int64_t t1=remain_time-t3-t22;
                                                speed=remain_precopy_size/t1;
                                		printf("speed1 %"PRId64"\n",speed);
                                                int64_t speed2=0;
                                                if(t22>0)
                                                   speed2=(dirtyset_size+dirtyrate2*t22)/t22;
                                		printf("speed2=%"PRId64"\n",speed2);
                                                speed=speed << 20L;
                                        }else {
                                        	printf("no solution\n");
                                                speed=netmaxspeed;
                                                s->throttling=1;
                                        }   

				}
                                /*
       				int64_t c=remain_time-t3;
          			int64_t a=remain_precopy_size+dirty_dsize+dirtyrate1*c;      
          			int64_t b=dirtyrate2-dirtyrate1;
          			int64_t d=remain_precopy_size;
        			printf("a=%"PRId64" b=%"PRId64" c=%"PRId64" d=%"PRId64"\n",a,b,c,d);
          			int64_t delta=(b*c-a)*(b*c-a)+4L*b*(a*c-c*d);        
          			if(b==0)
          			{
            				if(c>0)
             					speed=(remain_precopy_size+dirty_dsize)/c;
            				else
             					speed=maxspeed>>20L;
          				printf("b=0, speed full = %"PRId64"MB \n",speed);

        			        speed=speed << 20L; 
                	         }else { 
        				int64_t t21=(a-b*c+sqrt(delta))/(0L-2L*b);
        				int64_t t22=(a-b*c-sqrt(delta))/(0L-2L*b);
        		printf("delta=%"PRId64" x1=%"PRId64" x2=%"PRId64" \n",delta,t21,t22);
        				if((t21>=0) && (t21<c)){ 
             					int64_t t1=remain_time-t3-t21;
             					speed=remain_precopy_size/t1;
           					printf("speed1 %"PRId64"\n",speed);
             					int64_t speed2=0;
                                                if(t21>0)
                                                  speed2=(dirty_dsize+dirtyrate1*t1+dirtyrate2*t21)/t21;
            				        printf("speed2=%"PRId64"\n",speed2);
                                        	speed=speed << 20L;
         				}
         				else if((t22>=0)&&(t22<c)){
             					int64_t t1=remain_time-t3-t22;
             					speed=remain_precopy_size/t1;
             			printf("speed1 %"PRId64"\n",speed);
             					int64_t speed2=0;
                                                if(t22>0)
                                                   speed2=(dirty_dsize+dirtyrate1*t1+dirtyrate2*t22)/t22;
             			printf("speed2=%"PRId64"\n",speed2);
                                        	speed=speed << 20L;
        				}else {
                                      	printf("no solution\n");
                                        	speed=maxspeed;
                                                s->throttling=1;
                                	}   
				}*/
      			}
   	    	}else{
            		int64_t t1=0;
        		int64_t t2=0;
      			if(stage==2)
        		{
           			speed=maxspeed;
        		}else{
                                  
              			t2=(dirtyset_size)/(maxspeed_dirty-dirtyrate2);
              			t1=remain_time-t3-t2;
              			if(t1<=0){
                			speed=netmaxspeed;
                                        s->throttling=1;
                                }
              			else
              				speed=remain_precopy_size/t1;
        
	   	             printf("t1 %"PRId64" t2 %"PRId64" speed is %"PRId64"\n",t1,t2,speed);
        		}
   	  	}
	}
   }
   if(speed>netmaxspeed){
     speed=netmaxspeed;
     throttling=1;
   }
   if(remain_time<0)
	allow_throttle=1;
   if(allow_throttle)
   	set_write_throttling(throttling,speed);
   if((throttling==1)&&(allow_throttle))
   { printf("enable throttling!!\n");
    interval_step=0;
   }else
    interval_step=20L;
   printf("compute speed %"PRId64"\n",speed);
   return speed;
}

uint64_t getSpeedAdjusted(void *opaque,uint64_t speed_next_expected)
{
   FdMigrationState *s = opaque;
   if((s->mig_state.metricopt==1)||(s->mig_state.metricopt==2)){   
   	int64_t total_throughput = get_throughput();
   	int64_t current_throughput=(total_throughput-s->last_throughput)*1000/s->last_interval; //Bytes per second
   	s->last_throughput = total_throughput;

   	int64_t current_throughput_MB = current_throughput >> 20L; 

  	printf("current_throughput_MB %"PRId64" ",current_throughput_MB);
  
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

        printf("current_latency %"PRId64"\n",current_latency);
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
	speed_next_expected=maxspeed;
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
                speed_next_expected=maxspeed;
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
           speed_next_expected=maxspeed;
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
   FdMigrationState *s = opaque;
   int64_t speed_next_expected=0L;
   int interval=shortinterval;
   //printDBlockDist();

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
      /*  if(((dirty_dsize+remain_msize)/maxspeed>=remain_time)&&(vmstop_flag==0))
	{
           vm_stop(0);
           vmstop_flag=1;
           printf("stop vm at mem migration\n");
        }
      */      
        speed_next_expected=netmaxspeed;
        qemu_file_set_rate_limit(s->file,speed_next_expected);
        return;
   }
   else
	remain_msize=s->full_memsize;
 /*  if(((dirty_dsize+remain_msize)/maxspeed>=remain_time)||(pasttime>s->mig_state.mig_time))
   {
       if(vmstop_flag==0){
       	 vm_stop(0);
      	 printf("No enough time, stop vm, full speed\n");
         vmstop_flag=1;
       } 
       speed_next_expected=maxspeed;
       qemu_file_set_rate_limit(s->file,speed_next_expected);
       if(vmstop_flag==0)
       	qemu_mod_timer(s->timer,qemu_get_clock(rt_clock)+interval);
       s->speed_pre_expected=speed_next_expected;
       //migrate at full speed until finish
       return;
   } */
 

 //  printf("mem transf %"PRId64" pre_transf %"PRId64" remain_msize %"PRId64" pre_remain %"PRId64"\n",transf_msize,s->transf_pre_msize,remain_msize,s->remain_msize);
   int64_t dirtyrate2=0L;
   int64_t dirtyset_size=0L;
   int64_t dirtyrate_mem=0L; 
   int64_t full_disk_size = bdrv_get_totallength();
   if(transf_dsize<full_disk_size) //PRECOPY
   {
      s->stage=1;
      s->remain_precopy_size=full_disk_size-transf_dsize;
   }else{
      s->stage=2;
      s->remain_precopy_size=0L; 
   }

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
   //     printf("remain_msize %"PRId64" transf_msize %"PRId64" transf_pre_msize %"PRId64" pre_remain_msize %"PRId64"\n",remain_msize,transf_msize,s->transf_pre_msize,s->remain_msize);
     //  printf("dirtyrate_mem %"PRId64"\n",dirtyrate_mem); 
      }
      else {
        dirtyrate_mem=getEstimateDirtyRateMEM(s);
      //  printf("estimateMEM %"PRId64"\n",dirtyrate_mem);
        
       
      } 
   }
   printf("dirtyset_size %"PRId64" dirtyrate2 %"PRId64" dirtyrate_mem %"PRId64"\n",dirtyset_size,dirtyrate2,dirtyrate_mem);
   s->dirty_pre_dsize=dirty_dsize;
   s->transf_pre_dsize=transf_dsize;
   s->transf_pre_msize=transf_msize;
   s->remain_msize=remain_msize;
   speed_next_expected=getExpectedSpeed(s,s->stage,remain_time,s->remain_precopy_size,dirty_dsize,remain_msize,dirtyset_size,dirtyrate2,dirtyrate_mem,s->mig_state.policy);
   
   printf("speed_real %"PRId64" speed_expected %"PRId64" speed_next_expected %"PRId64" speed_scaleup_flag %d\n",speed_real,speed_expected,speed_next_expected,s->speed_scaleup_flag);

   speed_next_expected=getSpeedAdjusted(s,speed_next_expected);

   speed_next_expected=getSpeedScaleup(s,speed_next_expected,speed_real);
   
   if(s->stage==2)
   {
       interval=shortinterval;
   }
   else {

       if(interval_step!=0)
          interval=maxvalue(s->mig_state.mig_time*1000L/interval_step,mininterval);
       else
          interval=shortinterval;
//       if(s->remain_precopy_size<(speed_next_expected*interval/1000L))
  //        interval=s->remain_precopy_size*1000L/speed_next_expected;
   }
   printf("Final speed %"PRId64" interval %d\n",speed_next_expected,interval);
   qemu_file_set_rate_limit(s->file,speed_next_expected);
   qemu_mod_timer(s->timer,qemu_get_clock(rt_clock)+interval);
   s->last_interval=interval;
   s->speed_pre_expected=speed_next_expected;
   return; 
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
    //add for adaptive system by Pacer
    if(s->mig_state.mig_time>0){
    	s->starttime=qemu_get_clock(rt_clock);
        s->dirty_pre_dsize=0;
        s->transf_pre_dsize=0;
    	s->remain_precopy_size=bdrv_get_totallength();
    	s->transf_pre_msize=0;
        s->remain_msize=ram_bytes_remaining();
        s->full_memsize=s->remain_msize;
    	s->last_interval=maxvalue(s->mig_state.mig_time*1000L/interval_step,mininterval);
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
        s->speed_pre_expected=getExpectedSpeed(s,s->stage,s->mig_state.mig_time,s->remain_precopy_size,s->dirty_pre_dsize,s->remain_msize,0,0,0,s->mig_state.policy);
        if(s->speed_pre_expected==0L)
	{
           printf("Error: getExpectedSpeed return 0!!\n");
        }

        qemu_file_set_rate_limit(s->file,s->speed_pre_expected);
        s->timer = qemu_new_timer(rt_clock, migration_rate_tick, s);
        qemu_mod_timer(s->timer,qemu_get_clock(rt_clock)+s->last_interval);
        s->timer1=qemu_new_timer(rt_clock,setdirtyratemem_tick,s);
        qemu_mod_timer(s->timer1,qemu_get_clock(rt_clock)+1000L);   
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
