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
 */

#include "qemu-common.h"
#include "qemu_socket.h"
#include "migration.h"
#include "qemu-char.h"
#include "sysemu.h"
#include "buffered_file.h"
#include "block.h"
#include "time.h"

#define DEBUG_MIGRATION_TCP

#ifdef DEBUG_MIGRATION_TCP
#define DPRINTF(fmt, ...) \
    do { printf("migration-tcp: " fmt, ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) \
    do { } while (0)
#endif

static int socket_errno(FdMigrationState *s)
{
    return socket_error();
}

static int socket_write(FdMigrationState *s, const void * buf, size_t size)
{
    return send(s->fd, buf, size, 0);
}

static int tcp_close(FdMigrationState *s)
{
    DPRINTF("tcp_close\n");
    if (s->fd != -1) {
        close(s->fd);
        s->fd = -1;
    }
    return 0;
}


static void tcp_wait_for_connect(void *opaque)
{
    FdMigrationState *s = opaque;
    int val, ret;
    socklen_t valsize = sizeof(val);

    DPRINTF("connect completed\n");
    do {
        ret = getsockopt(s->fd, SOL_SOCKET, SO_ERROR, (void *) &val, &valsize);
    } while (ret == -1 && (s->get_error(s)) == EINTR);

    if (ret < 0) {
        migrate_fd_error(s);
        return;
    }

    qemu_set_fd_handler2(s->fd, NULL, NULL, NULL, NULL);

    if (val == 0)
        migrate_fd_connect(s);
    else {
        DPRINTF("error connecting %d\n", val);
        migrate_fd_error(s);
    }
}

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
					     int progressprediction)
{
    struct sockaddr_in addr;
    FdMigrationState *s;
    int ret;
    
#ifdef DEBUG_MIGRATION_TCP
    int64_t migrate_start=qemu_get_clock_ns(rt_clock);
    DPRINTF("Migration starting at %" PRId64 "\n",migrate_start);
    time_t rawtime;
    struct tm * timeinfo;
    time ( &rawtime );
    timeinfo = localtime ( &rawtime );
    DPRINTF( "Current local time and date: %s", asctime (timeinfo) );
#endif

    if (parse_host_port(&addr, host_port) < 0)
        return NULL;

    s = qemu_mallocz(sizeof(*s));

    s->get_error = socket_errno;
    s->write = socket_write;
    s->close = tcp_close;
    s->mig_state.cancel = migrate_fd_cancel;
    s->mig_state.get_status = migrate_fd_get_status;
    s->mig_state.release = migrate_fd_release;

    s->mig_state.blk = blk;
    s->mig_state.shared = inc;
    s->mig_state.sparse = sparse;
    s->mig_state.mig_time = mig_time;
    s->mig_state.metricopt = metricopt;
    s->mig_state.metricvalue = metricvalue;
    s->mig_state.compression = compression;
    s->mig_state.scheduling = scheduling;
    s->mig_state.dscheduling = dscheduling;
    s->mig_state.throttling = throttling;
    s->mig_state.policy = policy;
    s->mig_state.prediction = prediction;
    s->mig_state.progressprediction = progressprediction;   
 
    s->sent_traffic=0; 

    s->state = MIG_STATE_ACTIVE;
    s->mon = NULL;
    s->bandwidth_limit = bandwidth_limit;
    s->fd = qemu_socket(PF_INET, SOCK_STREAM, 0);
    if (s->fd == -1) {
        qemu_free(s);
        return NULL;
    }

    socket_set_nonblock(s->fd);

    if (!detach) {
        migrate_fd_monitor_suspend(s, mon);
    }

    do {
        ret = connect(s->fd, (struct sockaddr *)&addr, sizeof(addr));
        if (ret == -1)
            ret = -(s->get_error(s));

        if (ret == -EINPROGRESS || ret == -EWOULDBLOCK)
            qemu_set_fd_handler2(s->fd, NULL, NULL, tcp_wait_for_connect, s);
    } while (ret == -EINTR);

    if (ret < 0 && ret != -EINPROGRESS && ret != -EWOULDBLOCK) {
        DPRINTF("connect failed\n");
		migrate_fd_error(s);
    } else if (ret >= 0)
        migrate_fd_connect(s);

    return &s->mig_state;
}

static void tcp_accept_incoming_migration(void *opaque)
{
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    int s = (unsigned long)opaque;
    QEMUFile *f;
    int c, ret;

    do {
        c = qemu_accept(s, (struct sockaddr *)&addr, &addrlen);
    } while (c == -1 && socket_error() == EINTR);

    DPRINTF("accepted migration\n");
	
#ifdef DEBUG_MIGRATION_TCP
    int64_t migrate_start=qemu_get_clock_ns(rt_clock);
    DPRINTF("Migration starting at %" PRId64 "\n",migrate_start);
    time_t rawtime;
    struct tm * timeinfo;
    time ( &rawtime );
    timeinfo = localtime ( &rawtime );
    DPRINTF( "Current local time and date: %s", asctime (timeinfo) );
#endif

    if (c == -1) {
        fprintf(stderr, "could not accept migration connection\n");
        return;
    }

    f = qemu_fopen_socket(c);
    if (f == NULL) {
        fprintf(stderr, "could not qemu_fopen socket\n");
        goto out;
    }

    ret = qemu_loadvm_state(f);
    
    int64_t migrate_load_end=qemu_get_clock_ns(rt_clock);
    DPRINTF("qemu_loadvm_state end at %" PRId64 "\n",migrate_load_end);
 
    if (ret < 0) {
        fprintf(stderr, "load of migration failed\n");
        goto out_fopen;
    }
    
    qemu_announce_self();
    DPRINTF("successfully loaded vm state\n");
    migrate_load_end=qemu_get_clock_ns(rt_clock);
    DPRINTF("qemu_announce_self end,vm_start at %" PRId64 "\n",migrate_load_end); 
    
    if (autostart)
        vm_start();
#ifdef DEBUG_MIGRATION_TCP     
 int64_t migrate_end=qemu_get_clock_ns(rt_clock);
 DPRINTF("vm_start end, Migration end at %" PRId64 "\n",migrate_end);
int64_t migrate_time= (migrate_end-migrate_start)/1000000000;
DPRINTF("Migration time %" PRId64 "\n",migrate_time);

    time ( &rawtime );
    timeinfo = localtime ( &rawtime );
    DPRINTF( "Current local time and date: %s", asctime (timeinfo) );
#endif


out_fopen:
    qemu_fclose(f);
out:
    qemu_set_fd_handler2(s, NULL, NULL, NULL, NULL);
    close(s);
    close(c);
}

int tcp_start_incoming_migration(const char *host_port)
{
    struct sockaddr_in addr;
    int val;
    int s;

    if (parse_host_port(&addr, host_port) < 0) {
        fprintf(stderr, "invalid host/port combination: %s\n", host_port);
        return -EINVAL;
    }

    s = qemu_socket(PF_INET, SOCK_STREAM, 0);
    if (s == -1)
        return -socket_error();

    val = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&val, sizeof(val));

    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) == -1)
        goto err;

    if (listen(s, 1) == -1)
        goto err;

    qemu_set_fd_handler2(s, NULL, tcp_accept_incoming_migration, NULL,
                         (void *)(unsigned long)s);

    return 0;

err:
    close(s);
    return -socket_error();
}
