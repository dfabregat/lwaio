/*
 * Copyright (c) 2014-2015, Diego Fabregat-Traver
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   1. Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 *
 *   2. Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 *
 *   3. Neither the name of the copyright holder nor the names of its
 *   contributors may be used to endorse or promote products derived from this
 *   software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 *
 * Coded by Diego Fabregat-Traver (fabregat@aices.rwth-aachen.de)
 * December 2014, Version 0.1
 */

#define _GNU_SOURCE

#define MIN(a, b) (a) <= (b) ? (a) : (b)

/*static size_t MAX_IO_CHUNK = (1L<<30); // 2^30*/
#define MAX_IO_CHUNK ((size_t)(1L<<30))

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>

#include "lwaio.h"

typedef struct lwaio_queue {
    lwaio_task *head;
    lwaio_task *tail;
    pthread_mutex_t access;
} lwaio_queue;

typedef struct lwaio_control {
    lwaio_queue queue;
    sem_t       ntasks; // # of pending tasks
    pthread_t   io_thid;
    int         cpuid; // which core io thread is pinned to
} lwaio_control;

static lwaio_control lwaio_ctrl;

// Declaration of interal routines
void* lwaio_io_thread(void *data);
static void lwaio_sync_read( lwaio_task *task_p );
static void lwaio_sync_write( lwaio_task *task_p );
static void lwaio_queue_init( lwaio_queue *q );
static void lwaio_queue_finalize( lwaio_queue *q );
static void lwaio_queue_enqueue( lwaio_queue *q, lwaio_task *task );
static void lwaio_queue_dequeue( lwaio_queue *q, lwaio_task *task );
static void pin_thread( int cpuid );


/*
 * Implementation of interface routines
 */
void lwaio_init( void )
{
    char *cpuid_str, *end;
    long int cpuid;

    // Read cpuid from environment
    cpuid_str = getenv( "LWAIO_PIN_TO" );
    if ( cpuid_str == NULL )
    {
        cpuid = -1; // Default is -1 (no pinning)
    }
    else
    {
        errno = 0;
        cpuid = strtol( cpuid_str, &end, 10 );
        if ( errno == ERANGE || cpuid_str == end && strcmp( cpuid_str, "" ) )
        {
            fprintf( stderr, "[WARNING] Incorrect value for $LWAIO_PIN_TO\n" );
            cpuid = -1;
        }
    }

    lwaio_queue_init( &lwaio_ctrl.queue );
    sem_init( &lwaio_ctrl.ntasks, 0, 0 );
    lwaio_ctrl.cpuid = cpuid;
    pthread_create( &lwaio_ctrl.io_thid, NULL, lwaio_io_thread, NULL );
}

void lwaio_finalize( void )
{
    pthread_cancel( lwaio_ctrl.io_thid );
    sem_destroy( &lwaio_ctrl.ntasks );
    lwaio_queue_finalize( &lwaio_ctrl.queue );
}

void lwaio_read( lwaio_task *task )
{
    task->type = LWAIO_READ;
    sem_init( &task->done, 0, 0 );
    lwaio_queue_enqueue( &lwaio_ctrl.queue, task );
}

void lwaio_write( lwaio_task *task )
{
    task->type = LWAIO_WRITE;
    sem_init( &task->done, 0, 0 );
    lwaio_queue_enqueue( &lwaio_ctrl.queue, task );
}

void lwaio_wait( lwaio_task *task )
{
    sem_wait( &task->done );
    /*lwaio_queue_dequeue( &lwaio_ctrl.queue, task );*/
    sem_destroy( &task->done );
}

/*
 * Implementation of internals
 */
void* lwaio_io_thread(void *data)
{
    int cpuid = lwaio_ctrl.cpuid;
    lwaio_task *cur_task, *next_task;

    // We can cancel the thread any time
    pthread_setcanceltype( PTHREAD_CANCEL_ASYNCHRONOUS, NULL );

    // If pinning requested, pin
    if ( cpuid >= 0 )
        pin_thread( cpuid );

    // current task is head
    cur_task = lwaio_ctrl.queue.head;
    while ( 1 )
    {
        // Wait until there are tasks pending
        sem_wait( &lwaio_ctrl.ntasks );

        // If no tasks were pending after completing
        // last operation, take the first
        // Else, it is already pointing to the right place
        if ( cur_task == NULL )
            cur_task = lwaio_ctrl.queue.head;

        if ( cur_task->type == LWAIO_READ )
        {
            lwaio_sync_read( cur_task );
        }
        else if ( cur_task->type == LWAIO_WRITE )
        {
            lwaio_sync_write( cur_task );
        }
        else
        {
            // [TODO] error
        }

        // signal task completed
        next_task = cur_task->next;
        lwaio_queue_dequeue( &lwaio_ctrl.queue, cur_task );
        sem_post( &cur_task->done ); //
        cur_task = next_task;
    }

    pthread_exit(NULL);
}

static void lwaio_sync_read( lwaio_task *task_p )
{
    off_t cur_pos = 0,
          end_pos = task_p->nbytes;
    size_t nbytes_chunk, nbytes_read;
    char *buff = (char *)task_p->buffer;

    lseek( fileno(task_p->fp), task_p->offset, SEEK_SET );
    while ( cur_pos < end_pos )
    {
        nbytes_chunk = MIN( MAX_IO_CHUNK, end_pos - cur_pos );
        nbytes_read = read( fileno(task_p->fp), (void*)&buff[cur_pos], nbytes_chunk );
        if ( nbytes_read == -1 )
        {
            perror("[ERROR] lwaio_sync_read:");
            exit( EXIT_FAILURE );
        }
        else if ( nbytes_read != nbytes_chunk )
        {
            fprintf( stderr, "[ERROR] lwaio_sync_read: read %zu (of %zu) bytes.\n", nbytes_read, nbytes_chunk );
            exit( EXIT_FAILURE );
        }
        cur_pos += nbytes_chunk;
    }
}

static void lwaio_sync_write( lwaio_task *task_p )
{
    off_t cur_pos = 0,
          end_pos = task_p->nbytes;
    size_t nbytes_chunk, nbytes_written;
    char *buff = (char *)task_p->buffer;

    lseek( fileno(task_p->fp), task_p->offset, SEEK_SET );
    while ( cur_pos < end_pos )
    {
        nbytes_chunk = MIN( MAX_IO_CHUNK, end_pos - cur_pos );
        nbytes_written = write( fileno(task_p->fp), (void*)&buff[cur_pos], nbytes_chunk );
        if ( nbytes_written == -1 )
        {
            perror("[ERROR] lwaio_sync_write:");
            exit( EXIT_FAILURE );
        }
        else if ( nbytes_written != nbytes_chunk )
        {
            fprintf( stderr, "[ERROR] lwaio_sync_write: wrote %zd (of %zu) bytes.\n", nbytes_written, nbytes_chunk );
            exit( EXIT_FAILURE );
        }
        cur_pos += nbytes_chunk;
    }
}

/*
 * Implementation of the queue
 */
static void lwaio_queue_init( lwaio_queue *q )
{
    q->head = NULL;
    q->tail = NULL;
    pthread_mutex_init( &q->access, NULL );
}

static void lwaio_queue_finalize( lwaio_queue *q )
{
    q->head = NULL;
    q->tail = NULL;
    pthread_mutex_destroy( &q->access );
}

static void lwaio_queue_enqueue( lwaio_queue *q, lwaio_task *task )
{
    // acquire mutex
    pthread_mutex_lock( &q->access );
    // enqueue
    if (q->tail == NULL ) // 0 tasks in queue
    {
        q->head = q->tail = task;
        task->prev = NULL;
        task->next = NULL;
    }
    else
    {
        task->prev = q->tail;
        task->next = NULL;
        q->tail->next = task;
        q->tail = task;
    }
    // release mutex
    pthread_mutex_unlock( &q->access );
    // signal new task
    sem_post( &lwaio_ctrl.ntasks );
}

static void lwaio_queue_dequeue( lwaio_queue *q, lwaio_task *task )
{
    // acquire mutex
    pthread_mutex_lock( &q->access );
    // dequeue
    if (task->prev == NULL ) // 1st task
    {
        q->head = task->next;
        if ( task->next == NULL ) // Also last task
            q->tail = NULL;
        else
            task->next->prev = NULL;
    }
    else
    {
        task->prev->next = task->next;
        if ( task->next == NULL ) // last task
            q->tail = task->prev;
        else
            task->next->prev = task->prev;
    }
    // release mutex
    pthread_mutex_unlock( &q->access );
    // Cleanup task
    task->prev = task->next = NULL;
    /*sem_destroy( &task->done );*/
}


/*
 * Utilities
 */
static void pin_thread( int cpuid )
{
    cpu_set_t cpuset;
    pthread_t th;
    int i, s;

    th = pthread_self();
    CPU_ZERO(&cpuset);
    CPU_SET(cpuid, &cpuset);
    s = pthread_setaffinity_np( th, sizeof(cpu_set_t), &cpuset );
    if (s != 0)
    {
        errno = s;
        perror("pthread_setaffinity_np");
    }
}
