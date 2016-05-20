/*
 * QEMU buffered QEMUFile
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
#include "hw/hw.h"
#include "qemu-timer.h"
#include "sysemu.h"
#include "qemu-char.h"
#include "buffered_file.h"

//#define DEBUG_BUFFERED_FILE

typedef struct QEMUFileBuffered
{
    BufferedPutFunc *put_buffer;
    BufferedPutReadyFunc *put_ready;
    BufferedWaitForUnfreezeFunc *wait_for_unfreeze;
    BufferedCloseFunc *close;
    void *opaque;
    QEMUFile *file;
    int has_error;
    int freeze_output;
    
    //Pacer variables
     size_t bytes_xfer;
     size_t xfer_limit;

    uint8_t *buffer;
    size_t buffer_size;
    size_t buffer_capacity;
    size_t buffer_startindex;
    size_t buffer_endindex; 
    size_t interval;
    QEMUTimer *timer;
} QEMUFileBuffered;

#ifdef DEBUG_BUFFERED_FILE
#define DPRINTF(fmt, ...) \
    do { printf("buffered-file: " fmt, ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) \
    do { } while (0)
#endif

//Pacer Function

static void my_buffered_append(QEMUFileBuffered *s,
                            const uint8_t *buf, size_t size)
{
    if (size > (s->buffer_capacity - s->buffer_size)) {
        void *tmp;

        DPRINTF("increasing buffer capacity from %zu by %zu\n",
                s->buffer_capacity, size + 1024*1024);
        
        size_t oldcapacity=s->buffer_capacity;
        s->buffer_capacity += size + 1024*1024;

        tmp = qemu_realloc(s->buffer, s->buffer_capacity);
        if (tmp == NULL) {
            fprintf(stderr, "qemu file buffer expansion failed\n");
            exit(1);
        }

        s->buffer = tmp;
        if((s->buffer_startindex>=s->buffer_endindex)&&(s->buffer_size>0))
	{
            size_t newstartindex=s->buffer_capacity-oldcapacity+s->buffer_startindex;
            memmove(s->buffer+newstartindex, s->buffer+s->buffer_startindex, oldcapacity-s->buffer_startindex);
            DPRINTF("1 startindex changed from %d to %d\n",(int)(s->buffer_startindex),(int)newstartindex);
            s->buffer_startindex=newstartindex;
            
	}
    }
    if(s->buffer_startindex>s->buffer_endindex)
    {
            memcpy(s->buffer+s->buffer_endindex,buf,size);
            s->buffer_size += size;
            s->buffer_endindex += size;     
            DPRINTF("2 endindex changed from %d to %d\n",(int)(s->buffer_endindex-size),(int)(s->buffer_endindex));
    }else{//startindex and endindex are equal has two possibilities
          //full or empty, impossible full here, it is empty
	    if(size>=(s->buffer_capacity-s->buffer_endindex))	
            {
               int firstsize=s->buffer_capacity-s->buffer_endindex;
               memcpy(s->buffer+s->buffer_endindex,buf,firstsize);
               memcpy(s->buffer,buf+firstsize,size-firstsize);
               s->buffer_size+=size;
               s->buffer_endindex = size-firstsize;
               DPRINTF("3 endindex changed from %d to %d\n",(int)(s->buffer_endindex-size),(int)(s->buffer_endindex));
            }else{
               memcpy(s->buffer+s->buffer_endindex,buf,size);
               s->buffer_size += size;
               s->buffer_endindex += size;
               DPRINTF("4 endindex changed from %d to %d\n",(int)(s->buffer_endindex-size),(int)(s->buffer_endindex));
	    }
    }
}


// Pacer function
static void my_buffered_flush(QEMUFileBuffered *s)
{
    size_t offset = 0;
    
    if (s->has_error) {
        DPRINTF("flush when error, bailing\n");
        return;
    }

    DPRINTF("flushing %zu byte(s) of data\n", s->buffer_size);

    while (offset < s->buffer_size) {
        ssize_t ret;
        //special        
        if((s->buffer_startindex)>(s->buffer_endindex)){
        	ret = s->put_buffer(s->opaque, s->buffer + s->buffer_startindex,
                           s->buffer_capacity-s->buffer_startindex);
	}
	else{
        	ret = s->put_buffer(s->opaque, s->buffer + s->buffer_startindex,
                            s->buffer_endindex-s->buffer_startindex);
        }
        if (ret == -EAGAIN) {
            DPRINTF("backend not ready, freezing\n");
            s->freeze_output = 1;
            break;
        }

        if (ret <= 0) {
            DPRINTF("error flushing data, %zd\n", ret);
            s->has_error = 1;
            break;
        } else {
            DPRINTF("flushed %zd byte(s)\n", ret);
            offset += ret;
            DPRINTF("0 startindex changed from %d ",(int)(s->buffer_startindex));
            s->buffer_startindex +=ret;
            if(s->buffer_startindex==s->buffer_capacity)
		s->buffer_startindex=0; 
            DPRINTF(" to %d\n",(int)(s->buffer_startindex));
        }
    }

    DPRINTF("flushed %zu of %zu byte(s)\n", offset, s->buffer_size);
    s->buffer_size -= offset;
       
}

/*
static void buffered_flush(QEMUFileBuffered *s)
{
    size_t offset = 0;

    if (s->has_error) {
        DPRINTF("flush when error, bailing\n");
        return;
    }

    DPRINTF("flushing %zu byte(s) of data\n", s->buffer_size);

    while (offset < s->buffer_size) {
        ssize_t ret;

        ret = s->put_buffer(s->opaque, s->buffer + offset,
                            s->buffer_size - offset);
        if (ret == -EAGAIN) {
            DPRINTF("backend not ready, freezing\n");
            s->freeze_output = 1;
            break;
        }

        if (ret <= 0) {
            DPRINTF("error flushing data, %zd\n", ret);
            s->has_error = 1;
            break;
        } else {
            DPRINTF("flushed %zd byte(s)\n", ret);
            offset += ret;
        }
    }

    DPRINTF("flushed %zu of %zu byte(s)\n", offset, s->buffer_size);
    memmove(s->buffer, s->buffer + offset, s->buffer_size - offset);
    s->buffer_size -= offset;
}
*/

static int buffered_put_buffer(void *opaque, const uint8_t *buf, int64_t pos, int size)
{
    QEMUFileBuffered *s = opaque;
    int offset = 0;
    ssize_t ret;

    DPRINTF("putting %d bytes at %" PRId64 "\n", size, pos);

    if (s->has_error) {
        DPRINTF("flush when error, bailing\n");
        return -EINVAL;
    }

    DPRINTF("unfreezing output\n");
    s->freeze_output = 0;
    // Pacer modification
    my_buffered_flush(s);

    while (!s->freeze_output && offset < size) {
        if (s->bytes_xfer > s->xfer_limit) {
            DPRINTF("transfer limit exceeded when putting\n");
            break;
        }

        ret = s->put_buffer(s->opaque, buf + offset, size - offset);
        if (ret == -EAGAIN) {
            DPRINTF("backend not ready, freezing\n");
            s->freeze_output = 1;
            break;
        }

        if (ret <= 0) {
            DPRINTF("error putting\n");
            s->has_error = 1;
            offset = -EINVAL;
            break;
        }

        DPRINTF("put %zd byte(s)\n", ret);
        offset += ret;
        s->bytes_xfer += ret;
    }

    if (offset >= 0) {
        DPRINTF("buffering %d bytes\n", size - offset);
        my_buffered_append(s, buf + offset, size - offset); // Pacer modification
        offset = size;
    }

    return offset;
}

static int buffered_close(void *opaque)
{
    QEMUFileBuffered *s = opaque;
    int ret;

    DPRINTF("closing\n");

    while (!s->has_error && s->buffer_size) {
        my_buffered_flush(s); // Pacer modification
        if (s->freeze_output)
            s->wait_for_unfreeze(s);
    }

    ret = s->close(s->opaque);

    qemu_del_timer(s->timer);
    qemu_free_timer(s->timer);
    qemu_free(s->buffer);
    qemu_free(s);

    return ret;
}

static int buffered_rate_limit(void *opaque)
{
    QEMUFileBuffered *s = opaque;

    if (s->has_error)
        return 0;

    if (s->freeze_output)
        return 1;

    if (s->bytes_xfer > s->xfer_limit)
        return 1;

    return 0;
}

static size_t buffered_set_rate_limit(void *opaque, size_t new_rate)
{
    QEMUFileBuffered *s = opaque;

    if (s->has_error)
        goto out;

//Pacer modification for adaptive system

      s->interval=1000*1024*1024/(new_rate);
      s->xfer_limit=1*1024*1024; //1MB 
      printf("buffered_set_rate_limit to interval %d xfer_limit %d\n",(int)s->interval,(int)s->xfer_limit);     
out:
    return s->xfer_limit;
}

static size_t buffered_get_rate_limit(void *opaque)
{
    QEMUFileBuffered *s = opaque;
  
    return s->xfer_limit;
}

static void buffered_rate_tick(void *opaque)
{


    QEMUFileBuffered *s = opaque;

    if (s->has_error)
        return;
 
    //Pacer modification for adaptive system
    qemu_mod_timer(s->timer, qemu_get_clock(rt_clock) + s->interval);
    
    if (s->freeze_output)
        return;

    s->bytes_xfer = 0;
    // Pacewr modification
    my_buffered_flush(s);

    /* Add some checks around this */
    s->put_ready(s->opaque);
}

QEMUFile *qemu_fopen_ops_buffered(void *opaque,
                                  size_t bytes_per_sec,
                                  BufferedPutFunc *put_buffer,
                                  BufferedPutReadyFunc *put_ready,
                                  BufferedWaitForUnfreezeFunc *wait_for_unfreeze,
                                  BufferedCloseFunc *close)
{
    QEMUFileBuffered *s;

    s = qemu_mallocz(sizeof(*s));

    s->opaque = opaque;
    // Pacer modification for adaptive system
    s->xfer_limit = 1*1024*1024;
    s->put_buffer = put_buffer;
    s->put_ready = put_ready;
    s->wait_for_unfreeze = wait_for_unfreeze;
    s->close = close;

    s->file = qemu_fopen_ops(s, buffered_put_buffer, NULL,
                             buffered_close, buffered_rate_limit,
                             buffered_set_rate_limit,
			     buffered_get_rate_limit);

    s->timer = qemu_new_timer(rt_clock, buffered_rate_tick, s);

    s->interval=1000*1024*1024/bytes_per_sec;
    qemu_mod_timer(s->timer, qemu_get_clock(rt_clock) + s->interval);

    return s->file;
}
