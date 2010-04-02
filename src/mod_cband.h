/*
 * $Id: mod_cband.h,v 1.5 2006/05/28 18:58:37 dembol Exp $
 *
 * mod_cband - A per-user, per-virtualhost and per-destination bandwidth limiter for the Apache HTTP Server Version 2
 *
 * Copyright (c) 2005 Lukasz Dembinski <dembol@cband.linux.pl>
 * All Rights Reserved
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *	     
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *		     
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *					 
 */

#include "httpd.h"
#include "http_main.h"
#include "http_core.h"
#include "http_config.h"
#include "http_request.h"
#include "http_protocol.h"
#include "http_log.h"
#include "apr_strings.h"
#include "apr_file_io.h"
#include "apr_file_info.h"
#include "apr_signal.h"
#include "apr_hash.h"
#include "apr_time.h"
#include "apr_thread_mutex.h"
#include "util_filter.h"
#include "util_cfgtree.h"
#include <sys/shm.h>
#include <unistd.h>

#include "libpatricia.c"

#define MAX_CLASS_STR_LEN		16
#define MAX_DST_LEN			16
#define MAX_VIRTUALHOST_NAME		0x100
#define MAX_PERIOD_LEN			0x20
#define MAX_TRAFFIC_LEN			0x100
#define MAX_REMOTE_HOSTS		8192
#define MAX_HASH_TABLE_LEN		0x100
#define MAX_SHMEM_SEGMENTS		0x1000
#define MAX_SHMEM_ENTRIES		0x1000
#define MAX_SLOW_REMOTE_LOOPS		5
#define MAX_DELAY_LOOPS			100
#define MAX_OVERLIMIT_DELAY		10
#define MAX_PULSE_LEN			250000
#define MAX_PULSES			4
#define MAX_CHUNK_LEN			0x8000
#define CONST_PULSE_LEN			1000000
#define MAX_SLEEP_TIME			100000
#define MAX_REMOTE_HOST_LIFE		10
#define MIN_SPEED			1024
#define MIN_SLEEP_TIME			50000
#define PERIOD_LEN			1
#define DEFAULT_REFRESH			15
#define CBAND_HANDLER_ALL		0
#define CBAND_HANDLER_ME		1

#if (defined(__GNU_LIBRARY__) && !defined(_SEM_SEMUN_UNDEFINED)) || defined(__FreeBSD__)
/* union semun is defined by including <sys/sem.h> */
#else
/* according to X/OPEN we have to define it ourselves */
union semun {
    int val;
    struct semid_ds *buf;
    unsigned short *array;
    struct seminfo *__buf;
};					            
#endif

typedef struct mod_cband_virtualhost_config_entry mod_cband_virtualhost_config_entry;
typedef struct mod_cband_user_config_entry mod_cband_user_config_entry;
typedef struct mod_cband_class_config_entry mod_cband_class_config_entry;

typedef struct {
    unsigned long long total_bytes; 			/* in bytes - total traffic */
    unsigned long long class_bytes[DST_CLASS]; 		/* in bytes - class traffic */
    unsigned long start_time;
    long score_flush_count;
    int was_request;
} mod_cband_scoreboard_entry;

typedef struct {
    unsigned long kbps, rps, max_conn;
} mod_cband_speed;

typedef struct {
    mod_cband_speed max_speed;
    mod_cband_speed over_speed;
    mod_cband_speed curr_speed;
    mod_cband_speed remote_speed;
    unsigned long shared_kbps, shared_connections, total_conn;
    unsigned long total_last_refresh;
    unsigned long total_last_time;
    mod_cband_scoreboard_entry total_usage;
    float current_TX, old_TX;
    float current_conn, old_conn;
    int overlimit;
    unsigned long time_delta;
} mod_cband_shmem_data;

typedef struct {
    int shmem_id;
    int shmem_entry_idx;
    void *shmem_data;
} mod_cband_shmem_segment;

struct mod_cband_virtualhost_config_entry {
    char *virtual_name;
    apr_port_t virtual_port;
    unsigned virtual_defn_line;
    char *virtual_limit_exceeded;
    char *virtual_scoreboard;
    char *virtual_user;
    unsigned long virtual_limit;  			/* in units of *_mult bytes - total limit  */
    unsigned long virtual_class_limit[DST_CLASS];	/* in units of *_mult bytes - class limits */
    unsigned long refresh_time;				/* in seconds */
    unsigned long slice_len;				/* in seconds */
    unsigned int virtual_limit_mult;
    unsigned int virtual_class_limit_mult[DST_CLASS];
    mod_cband_speed virtual_class_speed[DST_CLASS];
    mod_cband_shmem_data *shmem_data;
    mod_cband_virtualhost_config_entry *next;
};

struct mod_cband_user_config_entry {
    char *user_name;
    char *user_limit_exceeded;
    char *user_scoreboard;
    unsigned long user_limit;  				/* in units of *_mult bytes - total limit  */
    unsigned long user_class_limit[DST_CLASS]; 		/* in units of *_mult bytes - class limits */
    unsigned long refresh_time;	
    unsigned long slice_len;	
    unsigned int user_limit_mult;
    unsigned int user_class_limit_mult[DST_CLASS];
    mod_cband_speed user_class_speed[DST_CLASS];
    mod_cband_shmem_data *shmem_data;			/* in seconds */
    mod_cband_user_config_entry *next;
};

struct mod_cband_class_config_entry {
    char *class_name;
    unsigned int class_nr;
    mod_cband_shmem_data *shmem_data;
    mod_cband_class_config_entry *next;
};

typedef struct mod_cband_remote_host {
    int used;
    unsigned long remote_addr;
    unsigned long remote_conn;
    unsigned long remote_kbps, remote_max_conn;
    unsigned long remote_last_time;
    unsigned long remote_last_refresh;
    unsigned long remote_total_conn;
    char *virtual_name;
} mod_cband_remote_host;

typedef struct mod_cband_remote_hosts {
    int shmem_id;
    int sem_id;
    struct mod_cband_remote_host *hosts;
} mod_cband_remote_hosts;

typedef struct {
    mod_cband_virtualhost_config_entry *next_virtualhost;
    mod_cband_user_config_entry *next_user;
    mod_cband_class_config_entry *next_class;
    apr_pool_t *p;
    char *default_limit_exceeded;
    int default_limit_exceeded_code;
    patricia_tree_t *tree;
    unsigned long start_time;				/* in seconds */
    int sem_id;
    mod_cband_shmem_segment shmem_seg[MAX_SHMEM_SEGMENTS];
    mod_cband_remote_hosts remote_hosts;
    int shmem_seg_idx;
    unsigned long score_flush_period;
    unsigned long random_pulse;
    unsigned long max_chunk_len;
} mod_cband_config_header;

typedef struct mod_cband_brigade_ctx {
    apr_bucket_brigade *bb;
} mod_cband_brigade_ctx;

typedef struct {
    unsigned long limit;
    unsigned long slice_limit;
    unsigned long class_limit;
    unsigned long class_slice_limit;
    unsigned long long usage;
    unsigned long long class_usage;
    unsigned int limit_mult;
    unsigned int class_limit_mult;
    char *limit_exceeded;
    char *scoreboard;
} mod_cband_limits_usages;
