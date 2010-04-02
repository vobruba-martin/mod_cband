/*
 * $Id: mod_cband.c,v 1.20 2006/05/28 18:58:35 dembol Exp $
 *
 * mod_cband - A per-user, per-virtualhost and per-destination bandwidth limiter for the Apache HTTP Server Version 2
 *
 * Copyright (c) 2005 Lukasz Dembinski <dembol@cband.linux.pl>
 * All Rights Reserved
 * 
 * Date:	2006/05/28
 * Info:	mod_cband Apache 2 module
 * Contact:	mailto: <dembol@cband.linux.pl>
 * Version:     0.9.7.5
 * Phase:       stabilization
 *
 * Authors:
 * - Lukasz Dembinski <dembol@cband.linux.pl>
 * - Sergey V. Beduev <shaman@interdon.net>
 * - Kyle Poulter <kyle@unixowns.us>
 * - Adam Dawidowski <drake@oomkill.net>
 * - Arvind Srinivasan <arvind@madtux.org>
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
#include "apr_pools.h"
#include "apr_shm.h"
#include "util_filter.h"
#include "util_cfgtree.h"
#include <sys/shm.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <unistd.h>

#include "mod_cband.h"

#if defined(__sun) || defined(__sun__)
static inline float logf (float x) { return log (x); }
static inline float sqrtf (float x) { return sqrt (x); }
static inline float floorf (float x) { return floor (x); }
static inline float powf (float x, float y) { return pow (x,y); }
#endif

#if defined(__FreeBSD__) || defined(__OpenBSD__) || \
defined(__sun) || defined(__sun__) // no truncf on FreeBSD, OpenBSD and SUN
inline float truncf (float d) {
    return (d < 0) ? -floorf(-d) : floorf(d);
}
#endif

static mod_cband_config_header *config = NULL;
static const char mod_cband_filter_name[] = "CBAND_FILTER";
ap_filter_rec_t *mod_cband_output_filter_handle;
apr_status_t patricia_cleanup(void *);
const void mod_cband_signal_handler(int);
unsigned long mod_cband_conf_get_period_sec(char *period);
unsigned long mod_cband_conf_get_limit_kb(char *limit, unsigned int *mult);
unsigned long mod_cband_conf_get_speed_kbps(char *speed);

module AP_MODULE_DECLARE_DATA cband_module;

int mod_cband_shmem_seg_new(void)
{
    int seg_idx;
    int shmem_id;

    seg_idx  = ++config->shmem_seg_idx;
    shmem_id = config->shmem_seg[seg_idx].shmem_id;
    
    if (shmem_id == 0) {
	shmem_id = shmget(IPC_PRIVATE, sizeof(mod_cband_shmem_data) * MAX_SHMEM_ENTRIES, IPC_CREAT | 0666);
	
	if (shmem_id < 0) {
	    fprintf(stderr, "apache2_mod_cband: cannot create shared memory segment for virtual hosts\n");
	    fflush(stderr);
	    return -1;
	}
	
        config->shmem_seg[seg_idx].shmem_id = shmem_id;
	config->shmem_seg[seg_idx].shmem_data = (mod_cband_shmem_data *)shmat(shmem_id, 0, 0);
	memset(config->shmem_seg[seg_idx].shmem_data, 0, sizeof(mod_cband_shmem_data) * MAX_SHMEM_ENTRIES);
    }

    config->shmem_seg[seg_idx].shmem_entry_idx = 0;

    return seg_idx;
}

mod_cband_shmem_data *mod_cband_shmem_init(void)
{
    mod_cband_shmem_data *data;
    int seg_idx, entry_idx;
    
    seg_idx = config->shmem_seg_idx;
    if ((seg_idx < 0) || (config->shmem_seg[seg_idx].shmem_entry_idx >= MAX_SHMEM_ENTRIES - 1))
	config->shmem_seg_idx = seg_idx = mod_cband_shmem_seg_new();

    if (seg_idx < 0)
	return NULL;

    entry_idx = config->shmem_seg[seg_idx].shmem_entry_idx++;
    data = (mod_cband_shmem_data *)(config->shmem_seg[seg_idx].shmem_data + entry_idx * sizeof(mod_cband_shmem_data));
    data->total_last_refresh = apr_time_now();

    return data;
}

void mod_cband_shmem_remove(int shmem_id)
{
    shmctl(shmem_id, IPC_RMID, 0);
}

void mod_cband_sem_init(int sem_id)
{
    union semun arg;
    unsigned short values[1];
    
    values[0] = 1;
    arg.val   = 1;
    arg.array = values;
    
    semctl(sem_id, 0, SETALL, arg);    
    
    /* 
     * We should also set owner of the semaphore ...
     */
}

void mod_cband_sem_remove(int sem_id)
{
    union semun arg;

    arg.val   = 0;    
    semctl(sem_id, 0, IPC_RMID, arg);    
}

void mod_cband_sem_down(int sem_id)
{
    struct sembuf sops;
    
    sops.sem_num  = 0;
    sops.sem_op   = -1;
    sops.sem_flg  = SEM_UNDO;
    
    semop(sem_id, &sops, 1);
}

void mod_cband_sem_up(int sem_id)
{
    struct sembuf sops;
        
    sops.sem_num  = 0;
    sops.sem_op   = 1;
    sops.sem_flg  = SEM_UNDO;
    
    semop(sem_id, &sops, 1);
}

int mod_cband_remote_hosts_init(void)
{
    int shmem_id, sem_id;
    int seg_size;

    shmem_id = config->remote_hosts.shmem_id;
    seg_size = sizeof(mod_cband_remote_host) * MAX_REMOTE_HOSTS;

    if (shmem_id == 0) {
	config->remote_hosts.shmem_id = shmem_id = shmget(IPC_PRIVATE, seg_size , IPC_CREAT | 0666);
        if (shmem_id < 0) {
	    fprintf(stderr, "apache2_mod_cband: cannot create shared memory segment for remote hosts\n");
	    fflush(stderr);
	    return -1;
	}
	
        config->remote_hosts.hosts = (mod_cband_remote_host *)shmat(shmem_id, 0, 0);
    }
    
    if (config->remote_hosts.hosts != NULL)
	memset(config->remote_hosts.hosts, 0, seg_size);
    
    config->remote_hosts.sem_id = sem_id = semget(IPC_PRIVATE, 1, IPC_CREAT | 0666);
    mod_cband_sem_init(sem_id);
    
    return 0;
}

/**
 * get virtualhost entry or create new one
 */
mod_cband_virtualhost_config_entry *mod_cband_get_virtualhost_entry_(char *virtualhost, apr_port_t port, unsigned line, int create)
{
    mod_cband_virtualhost_config_entry *entry;
    mod_cband_virtualhost_config_entry *new_entry;
    int i;

    if (virtualhost == NULL || config == NULL)
	return NULL;
    
    entry = config->next_virtualhost;
    
    while(entry != NULL) {

        if (!strcmp(entry->virtual_name, virtualhost) && (line == entry->virtual_defn_line))
	    return entry;
    
	if (entry->next == NULL)
	    break;
	    
	entry = entry->next;
    }

    if (create) {
	if ((new_entry = apr_palloc(config->p, sizeof(mod_cband_virtualhost_config_entry))) == NULL) {
	    fprintf(stderr, "apache2_mod_cband: cannot alloc memory for virtualhost entry\n");
	    fflush(stderr);
	    return NULL;
	}
	
	memset(new_entry, 0, sizeof(mod_cband_virtualhost_config_entry));
	new_entry->virtual_name       = virtualhost;
	new_entry->virtual_defn_line  = line;
	new_entry->virtual_port       = port;
	new_entry->virtual_limit_mult = 1024;
	
	if (new_entry->shmem_data == NULL)
	    new_entry->shmem_data = mod_cband_shmem_init();

	for (i = 0; i < DST_CLASS; i++)
	    new_entry->virtual_class_limit_mult[i] = 1024;
	
	if (entry == NULL)
	    config->next_virtualhost = new_entry;
	else
	    entry->next = new_entry;
	
	return new_entry;    
    }
    
    return NULL;    
}
 
mod_cband_virtualhost_config_entry *mod_cband_get_virtualhost_entry(server_rec *s, ap_conf_vector_t *module_config, int create)
{
    char *virtualhost;
    apr_port_t port;
    unsigned line;

    if (s == NULL)
	return NULL;

    virtualhost = s->server_hostname;
    port        = s->port;
    line        = s->defn_line_number;

    return mod_cband_get_virtualhost_entry_(virtualhost, port, line, create);
}

/**
 * get user entry or create new one
 */
mod_cband_user_config_entry *mod_cband_get_user_entry(char *user, ap_conf_vector_t *module_config, int create)
{
    mod_cband_user_config_entry *entry;
    mod_cband_user_config_entry *new_entry;
    int i;

    if (user == NULL || config == NULL)
	return NULL;
    
    entry = config->next_user;
    
    while(entry != NULL) {
	if (!strcmp(entry->user_name, user))
	    return entry;
    
	if (entry->next == NULL)
	    break;
	    
	entry = entry->next;
    }
    
    if (create) {
	if ((new_entry = apr_palloc(config->p, sizeof(mod_cband_user_config_entry))) == NULL) {
	    fprintf(stderr, "apache2_mod_cband: cannot alloc memory for user entry\n");
	    fflush(stderr);
	    return NULL;
	}
	
	memset(new_entry, 0, sizeof(mod_cband_user_config_entry));
	new_entry->user_name       = user;
	new_entry->user_limit_mult = 1024;

	if (new_entry->shmem_data == NULL)
	    new_entry->shmem_data = mod_cband_shmem_init();

	for (i = 0; i < DST_CLASS; i++)
	    new_entry->user_class_limit_mult[i] = 1024;

	if (entry == NULL)
	    config->next_user = new_entry;
	else
	    entry->next = new_entry;
	
	return new_entry;    
    }
	
    return NULL;    
}

/**
 * get class entry or create new one
 */
mod_cband_class_config_entry *mod_cband_get_class_entry(char *dest, ap_conf_vector_t *module_config, int create)
{
    mod_cband_class_config_entry *entry;
    mod_cband_class_config_entry *new_entry;

    if (dest == NULL || config == NULL)
	return NULL;
    
    entry = config->next_class;
    
    while(entry != NULL) {
	if (!strcmp(entry->class_name, dest))
	    return entry;
    
	if (entry->next == NULL)
	    break;
	    
	entry = entry->next;
    }

    if (create) {
	if ((new_entry = apr_palloc(config->p, sizeof(mod_cband_class_config_entry))) == NULL) {
	    fprintf(stderr, "apache2_mod_cband: cannot alloc memory for class entry\n");
	    fflush(stderr);
	    return NULL;
	}
	
	memset(new_entry, 0, sizeof(mod_cband_class_config_entry));
	new_entry->class_name = dest;
    
	if (entry == NULL)
	    config->next_class = new_entry;
	else
	    entry->next = new_entry;
	
	return new_entry;    
    }
    
    return NULL;    
}

static char *username_arg  = NULL;
static char *classname_arg = NULL;
int class_nr = -1;

int mod_cband_check_duplicate(void *ptr, const char *command, const char *arg, server_rec *s)
{
    if (ptr != NULL) {
	if (s->server_hostname != NULL)
	    ap_log_error(APLOG_MARK, APLOG_WARNING, 0, s, "Duplicate command '%s' for %s:%d", (char *)command, s->server_hostname, s->defn_line_number);
	else
	    ap_log_error(APLOG_MARK, APLOG_WARNING, 0, s, "Duplicate command '%s'", (char *)command);
	
	return 1;
    }
    
    return 0;
}

int mod_cband_check_virtualhost_command(mod_cband_virtualhost_config_entry **entry, cmd_parms *parms, const char *command)
{
    if ((*entry = mod_cband_get_virtualhost_entry(parms->server, parms->server->module_config, 1)) == NULL) {
	ap_log_error(APLOG_MARK, APLOG_WARNING, 0, parms->server, "Invalid command '%s', undefined virtualhost name", command);
	return 0;
    }

    return 1;
}

int mod_cband_check_virtualhost_class_command(mod_cband_virtualhost_config_entry **entry_virtual, mod_cband_class_config_entry **entry, cmd_parms *parms, const char *command, const char *arg)
{
    if ((*entry = mod_cband_get_class_entry((char *)arg, parms->server->module_config, 0)) == NULL) {
    	ap_log_error(APLOG_MARK, APLOG_WARNING, 0, parms->server, "Invalid command '%s', undefined class name", (char *)command);
	return 0;
    }

    if (!mod_cband_check_virtualhost_command(entry_virtual, parms, command))
	return 0;

    return 1;
}

int mod_cband_check_user_command(mod_cband_user_config_entry **entry, cmd_parms *parms, const char *command, const char **err)
{
    *err = NULL;
    
    if ((*err = ap_check_cmd_context(parms, GLOBAL_ONLY)) != NULL)
	return 0;
    
    if ((*entry = mod_cband_get_user_entry(username_arg, parms->server->module_config, 0)) == NULL) {
    	ap_log_error(APLOG_MARK, APLOG_WARNING, 0, parms->server, "Invalid command '%s', undefined user name", (char *)command);
	return 0;
    }

    return 1;
}

static const char *mod_cband_set_default_url(cmd_parms *parms, void *mconfig, const char *arg)
{
    if (!mod_cband_check_duplicate(config->default_limit_exceeded, "CBandDefaultExceededURL", arg, parms->server))
	config->default_limit_exceeded = (char *)arg;
      
    return NULL;
}

static const char *mod_cband_set_default_code(cmd_parms *parms, void *mconfig, const char *arg)
{
    config->default_limit_exceeded_code = atoi((char *)arg);
      
    return NULL;
}

static const char *mod_cband_set_random_pulse(cmd_parms *parms, void *mconfig, int flag)
{
    const char *flag_str;

    if (flag)
	flag_str = "On";
    else
	flag_str = "Off";

    if (!mod_cband_check_duplicate((void *)config->random_pulse, "CBandRandomPulse", flag_str, parms->server))
	config->random_pulse = (unsigned long)flag;
      
    return NULL;
}

static const char *mod_cband_set_score_flush_period(cmd_parms *parms, void *mconfig, const char *arg)
{
    if (!mod_cband_check_duplicate((void *)config->score_flush_period, "CBandScoreFlushPeriod", arg, parms->server))
	config->score_flush_period = atol((char *)arg);
      
    return NULL;
}

static const char *mod_cband_set_limit(cmd_parms *parms, void *mconfig, const char *arg)
{
    mod_cband_virtualhost_config_entry *entry;

    if (mod_cband_check_virtualhost_command(&entry, parms, "CBandLimit") &&
       (!mod_cband_check_duplicate((void *)entry->virtual_limit, "CBandLimit", arg, parms->server)))
	    entry->virtual_limit = mod_cband_conf_get_limit_kb((char *)arg, &entry->virtual_limit_mult);
    
    return NULL;
}

static const char *mod_cband_set_period(cmd_parms *parms, void *mconfig, const char *arg)
{
    mod_cband_virtualhost_config_entry *entry;

    if (mod_cband_check_virtualhost_command(&entry, parms, "CBandPeriod") &&
       (!mod_cband_check_duplicate((void *)entry->refresh_time, "CBandPeriod", arg, parms->server)))
	    entry->refresh_time = mod_cband_conf_get_period_sec((char *)arg);
    
    return NULL;
}

static const char *mod_cband_set_period_slice(cmd_parms *parms, void *mconfig, const char *arg)
{
    mod_cband_virtualhost_config_entry *entry;

    if (mod_cband_check_virtualhost_command(&entry, parms, "CBandPeriodSlice") &&
       (!mod_cband_check_duplicate((void *)entry->slice_len, "CBandPeriodSlice", arg, parms->server)))
	    entry->slice_len = mod_cband_conf_get_period_sec((char *)arg);
    
    return NULL;
}

static const char *mod_cband_set_url(cmd_parms *parms, void *mconfig, const char *arg)
{
    mod_cband_virtualhost_config_entry *entry;

    if (mod_cband_check_virtualhost_command(&entry, parms, "CBandExceededURL") &&
       (!mod_cband_check_duplicate(entry->virtual_limit_exceeded, "CBandExceededURL", arg, parms->server)))
	    entry->virtual_limit_exceeded = (char *)arg;
      
    return NULL;
}

static const char *mod_cband_set_speed(cmd_parms *parms, void *mconfig, const char *arg1, const char *arg2, const char *arg3)
{
    mod_cband_virtualhost_config_entry *entry;

    if (mod_cband_check_virtualhost_command(&entry, parms, "CBandSpeed") &&
       (!mod_cband_check_duplicate((void *)entry->shmem_data->max_speed.kbps, "CBandSpeed", arg1, parms->server))) {
    	entry->shmem_data->max_speed.kbps     = entry->shmem_data->curr_speed.kbps     = mod_cband_conf_get_speed_kbps((char *)arg1);
	entry->shmem_data->max_speed.rps      = entry->shmem_data->curr_speed.rps      = atol((char *)arg2);
	entry->shmem_data->max_speed.max_conn = entry->shmem_data->curr_speed.max_conn = atol((char *)arg3);
	entry->shmem_data->shared_kbps        = entry->shmem_data->curr_speed.kbps;
    }
    
    return NULL;
}

static const char *mod_cband_set_remote_speed(cmd_parms *parms, void *mconfig, const char *arg1, const char *arg2, const char *arg3)
{
    mod_cband_virtualhost_config_entry *entry;

    if (mod_cband_check_virtualhost_command(&entry, parms, "CBandRemoteSpeed") &&
       (!mod_cband_check_duplicate((void *)entry->shmem_data->remote_speed.kbps, "CBandRemoteSpeed", arg1, parms->server))) {
    	entry->shmem_data->remote_speed.kbps     = mod_cband_conf_get_speed_kbps((char *)arg1);
	entry->shmem_data->remote_speed.rps      = atol((char *)arg2);
	entry->shmem_data->remote_speed.max_conn = atol((char *)arg3);
    }
    
    return NULL;
}

char *mod_cband_get_next_char(const char *str, char val)
{
    int i;

    if (str == NULL)
	return NULL;

    for (i = 0; i < strlen(str); i++) {
	if (str[i] == val) {
	    return (char *)(str + i);
	}
    }
    
    return NULL;
}

char *mod_cband_get_next_notchar(const char *str, char val, int offset)
{
    int i;
    char *ptr;

    if (str == NULL)
	return NULL;

    if (offset)
	str += strlen(str) + 1;

    for (i = 0; i < strlen(str); i++) {
	if (str[i] != val) {
	    if ((ptr = mod_cband_get_next_char(str, val)) != NULL)
		*ptr = 0;
	    	    
	    return (char *)(str + i);
	}
    }
    
    return NULL;
}

static const char *mod_cband_set_class_remote_speed(cmd_parms *parms, void *mconfig, const char *args)
{
    mod_cband_class_config_entry *entry;
    mod_cband_virtualhost_config_entry *entry_virtual;
    char *arg1, *arg2, *arg3, *arg4;
    
    arg1 = mod_cband_get_next_notchar(args, ' ', 0);
    arg2 = mod_cband_get_next_notchar((const char *)arg1, ' ', 1);
    arg3 = mod_cband_get_next_notchar((const char *)arg2, ' ', 1);
    arg4 = mod_cband_get_next_notchar((const char *)arg3, ' ', 1);
    
    if (arg1 == NULL || arg2 == NULL || arg3 == NULL || arg4 == NULL) {
	ap_log_error(APLOG_MARK, APLOG_WARNING, 0, parms->server, "CBandClassRemoteSpeed takes four arguments");
	return NULL;
    }
    
    if (mod_cband_check_virtualhost_class_command(&entry_virtual, &entry, parms, "CBandClassRemoteSpeed", arg1)) {
	entry_virtual->virtual_class_speed[entry->class_nr].kbps     = mod_cband_conf_get_speed_kbps((char *)arg2);
        entry_virtual->virtual_class_speed[entry->class_nr].rps      = atol((char *)arg3);
	entry_virtual->virtual_class_speed[entry->class_nr].max_conn = atol((char *)arg4);
    }
    
    return NULL;
}

static const char *mod_cband_set_exceeded_speed(cmd_parms *parms, void *mconfig, const char *arg1, const char *arg2, const char *arg3)
{
    mod_cband_virtualhost_config_entry *entry;

    if (mod_cband_check_virtualhost_command(&entry, parms, "CBandExceededSpeed") &&
       (!mod_cband_check_duplicate((void *)entry->shmem_data->over_speed.kbps, "CBandExceededSpeed", arg1, parms->server))) {
    	entry->shmem_data->over_speed.kbps     = mod_cband_conf_get_speed_kbps((char *)arg1);
	entry->shmem_data->over_speed.rps      = atol((char *)arg2);
	entry->shmem_data->over_speed.max_conn = atol((char *)arg3);
    }
    
    return NULL;
}

static const char *mod_cband_set_scoreboard(cmd_parms *parms, void *mconfig, const char *arg)
{
    mod_cband_virtualhost_config_entry *entry;
    
    if (mod_cband_check_virtualhost_command(&entry, parms, "CBandScoreboard") &&
       (!mod_cband_check_duplicate(entry->virtual_scoreboard, "CBandScoreboard", arg, parms->server)))
	entry->virtual_scoreboard = (char *)arg;
      
    return NULL;
}

static const char *mod_cband_set_user(cmd_parms *parms, void *mconfig, const char *arg)
{
    mod_cband_virtualhost_config_entry *entry;
    
    if ((entry = mod_cband_get_virtualhost_entry(parms->server, parms->server->module_config, 1)) == NULL) {
    	ap_log_error(APLOG_MARK, APLOG_WARNING, 0, parms->server, "Invalid command 'CBandUser %s', undefined virtualhost name", (char *)arg);
	return NULL;
    }

    if (mod_cband_get_user_entry((char *)arg, parms->server->module_config, 0) == NULL)  {
    	ap_log_error(APLOG_MARK, APLOG_WARNING, 0, parms->server, "Invalid command 'CBandUser %s', undefined user", (char *)arg);
	return NULL;
    }
    
    if (!mod_cband_check_duplicate(entry->virtual_user, "CBandUser", arg, parms->server))
	entry->virtual_user = (char *)arg;
      
    return NULL;
}

static const char *mod_cband_user_section(cmd_parms *parms, void *mconfig, const char *arg)
{
    mod_cband_user_config_entry *entry;
    const char *endp = ap_strrchr_c(arg, '>');
    char *username;
    const char *err;
    
    if ((err = ap_check_cmd_context(parms, GLOBAL_ONLY)) != NULL)
	return err;

    if (endp == NULL)
	return apr_pstrcat(parms->pool, parms->cmd->name, "> directive missing closing '>'", NULL);

    username = apr_pstrndup(parms->pool, arg, endp - arg);

#ifdef DEBUG
    fprintf(stderr, "apache2_mod_cband: user %s\n", username);
    fflush(stderr);
#endif

    if ((mod_cband_get_user_entry(username, parms->server->module_config, 0)) != NULL) 
	return apr_pstrcat(parms->pool, parms->cmd->name, " ", username, "> duplicate user definition", NULL);

    if ((entry = mod_cband_get_user_entry(username, parms->server->module_config, 1)) == NULL) 
        return ap_walk_config(parms->directive->first_child, parms, parms->context);

    entry->user_name = username;
    username_arg = username;

    return ap_walk_config(parms->directive->first_child, parms, parms->context);
}

static const char *mod_cband_set_user_limit(cmd_parms *parms, void *mconfig, const char *arg)
{
    mod_cband_user_config_entry *entry;
    const char *err;

    if (mod_cband_check_user_command(&entry, parms, "CBandUserLimit", &err) &&
       (!mod_cband_check_duplicate((void *)entry->user_limit, "CBandUserLimit", arg, parms->server)))
	entry->user_limit = mod_cband_conf_get_limit_kb((char *)arg, &entry->user_limit_mult);

    return err;
}

static const char *mod_cband_set_user_period(cmd_parms *parms, void *mconfig, const char *arg)
{
    mod_cband_user_config_entry *entry;
    const char *err;

    if (mod_cband_check_user_command(&entry, parms, "CBandUserPeriod", &err) &&
       (!mod_cband_check_duplicate((void *)entry->refresh_time, "CBandUserPeriod", arg, parms->server)))
	entry->refresh_time = mod_cband_conf_get_period_sec((char *)arg);
    
    return err;
}

static const char *mod_cband_set_user_period_slice(cmd_parms *parms, void *mconfig, const char *arg)
{
    mod_cband_user_config_entry *entry;
    const char *err;

    if (mod_cband_check_user_command(&entry, parms, "CBandUserPeriodSlice", &err) &&
       (!mod_cband_check_duplicate((void *)entry->slice_len, "CBandUserPeriodSlice", arg, parms->server)))
	entry->slice_len = mod_cband_conf_get_period_sec((char *)arg);
    
    return err;
}

static const char *mod_cband_set_user_url(cmd_parms *parms, void *mconfig, const char *arg)
{
    mod_cband_user_config_entry *entry;
    const char *err;
    
    if (mod_cband_check_user_command(&entry, parms, "CBandUserExceededURL", &err) &&
       (!mod_cband_check_duplicate(entry->user_limit_exceeded, "CBandUserExceededURL", arg, parms->server)))
	entry->user_limit_exceeded = (char *)arg;

    return err;
}

static const char *mod_cband_set_user_speed(cmd_parms *parms, void *mconfig, const char *arg1, const char *arg2, const char *arg3)
{
    mod_cband_user_config_entry *entry;
    const char *err;
    
    if (mod_cband_check_user_command(&entry, parms, "CBandUserSpeed", &err) &&
       (!mod_cband_check_duplicate((void *)entry->shmem_data->max_speed.kbps, "CBandUserSpeed", arg1, parms->server))) {
    	entry->shmem_data->max_speed.kbps     = entry->shmem_data->curr_speed.kbps     = mod_cband_conf_get_speed_kbps((char *)arg1);
	entry->shmem_data->max_speed.rps      = entry->shmem_data->curr_speed.rps      = atol((char *)arg2);
	entry->shmem_data->max_speed.max_conn = entry->shmem_data->curr_speed.max_conn = atol((char *)arg3);
	entry->shmem_data->shared_kbps        = entry->shmem_data->curr_speed.kbps;
    }

    return err;
}

static const char *mod_cband_set_user_remote_speed(cmd_parms *parms, void *mconfig, const char *arg1, const char *arg2, const char *arg3)
{
    mod_cband_user_config_entry *entry;
    const char *err;

    if (mod_cband_check_user_command(&entry, parms, "CBandUserRemoteSpeed", &err) &&
       (!mod_cband_check_duplicate((void *)entry->shmem_data->max_speed.kbps, "CBandUserRemoteSpeed", arg1, parms->server))) {
    	entry->shmem_data->remote_speed.kbps     = mod_cband_conf_get_speed_kbps((char *)arg1);
	entry->shmem_data->remote_speed.rps      = atol((char *)arg2);
	entry->shmem_data->remote_speed.max_conn = atol((char *)arg3);
    }
    
    return err;
}

static const char *mod_cband_set_user_exceeded_speed(cmd_parms *parms, void *mconfig, const char *arg1, const char *arg2, const char *arg3)
{
    mod_cband_user_config_entry *entry;
    const char *err;
    
    if (mod_cband_check_user_command(&entry, parms, "CBandUserExceededSpeed", &err) &&
       (!mod_cband_check_duplicate((void *)entry->shmem_data->over_speed.kbps, "CBandUserExceededSpeed", arg1, parms->server))) {
    	entry->shmem_data->over_speed.kbps     = mod_cband_conf_get_speed_kbps((char *)arg1);
	entry->shmem_data->over_speed.rps      = atol((char *)arg2);
	entry->shmem_data->over_speed.max_conn = atol((char *)arg3);
    }

    return err;
}

static const char *mod_cband_set_user_scoreboard(cmd_parms *parms, void *mconfig, const char *arg)
{
    mod_cband_user_config_entry *entry;
    const char *err;
    
    if (mod_cband_check_user_command(&entry, parms, "CBandUserScoreboard", &err) &&
       (!mod_cband_check_duplicate(entry->user_scoreboard, "CBandUserScoreboard", arg, parms->server)))
	entry->user_scoreboard = (char *)arg;
      
    return err;
}

int mod_cband_check_IP(char *addr)
{
    int i;
    int dig, dot, mask;
    int len;
    
    len = (strlen(addr) > MAX_DST_LEN)?MAX_DST_LEN:strlen(addr);
    
    dig = 0;
    dot = 0;
    for (i = 0; i < len; i++) {
	if (addr[i] >= '0' && addr[i] <= '9') {
	    if (++dig > 3)
		return 0;
	} else
	if (addr[i] == '.') {
	    if (dig == 0)
		return 0;
	
	    if (++dot > 3)
		return 0;
		
	    dig = 0;
	} else
	if (addr[i] == '/') {
	    if (dig == 0)
		return 0;
	
	    mask = atoi(&addr[i + 1]);
	    if (mask < 0 || mask > 32)
		return 0;
	
	    return 1;
	} else
	    return 0;
    }

    return 1;
}

static const char *mod_cband_class_section(cmd_parms *parms, void *mconfig, const char *arg)
{
    mod_cband_class_config_entry *entry;
    const char *endp = ap_strrchr_c(arg, '>');
    char *classname;
    const char *err;
    
    class_nr++;
    
    if (class_nr >= DST_CLASS)
        return ap_walk_config(parms->directive->first_child, parms, parms->context);
    
    if ((err = ap_check_cmd_context(parms, GLOBAL_ONLY)) != NULL)
	return err;

    if (endp == NULL)
	return apr_pstrcat(parms->pool, parms->cmd->name, "> directive missing closing '>'", NULL);

    classname = apr_pstrndup(parms->pool, arg, endp - arg);
    
#ifdef DEBUG
    fprintf(stderr, "apache2_mod_cband: class %s (class %d)\n", classname, class_nr);
    fflush(stderr);
#endif

    if ((mod_cband_get_class_entry(classname, parms->server->module_config, 0)) != NULL) 
	return apr_pstrcat(parms->pool, parms->cmd->name, " ", classname, "> duplicate class definition", NULL);

    if ((entry = mod_cband_get_class_entry(classname, parms->server->module_config, 1)) == NULL) 
        return ap_walk_config(parms->directive->first_child, parms, parms->context);

    entry->class_name = classname;
    entry->class_nr   = class_nr;
    classname_arg     = classname;

    return ap_walk_config(parms->directive->first_child, parms, parms->context);
}

static const char *mod_cband_set_class_dst(cmd_parms *parms, void *mconfig, const char *arg)
{
    patricia_node_t *node;
    char class_nr_str[MAX_CLASS_STR_LEN];

    if (config->tree == NULL)
	config->tree = New_Patricia(32); 

    if ((class_nr < DST_CLASS) && (mod_cband_check_IP((char *)arg))) {
#ifdef DEBUG
	fprintf(stderr, "apache2_mod_cband: class dst %s (class %d)\n", (char *)arg, class_nr);
	fflush(stderr);
#endif
    
        sprintf(class_nr_str, "%d", class_nr);
	node = make_and_lookup(config->tree, (char *)arg);
		
	if (node)
    	    node->user1 = apr_pstrdup(config->p, class_nr_str);
    } else {
	if (class_nr >= DST_CLASS) {
	    ap_log_error(APLOG_MARK, APLOG_WARNING, 0, parms->server, "You can define only %d destination classes", DST_CLASS);
	} else {
	    ap_log_error(APLOG_MARK, APLOG_WARNING, 0, parms->server, "Invalid IP address %s", arg);
	}
    }

    return NULL;
}

static const char *mod_cband_set_class_limit(cmd_parms *parms, void *mconfig, const char *arg, const char *limit)
{
    mod_cband_class_config_entry *entry;
    mod_cband_virtualhost_config_entry *entry_virtual;
    
    if ((entry = mod_cband_get_class_entry((char *)arg, parms->server->module_config, 0)) == NULL) {
    	ap_log_error(APLOG_MARK, APLOG_WARNING, 0, parms->server, "Invalid command 'CBandClassLimit %s %s', undefined class name", (char *)arg, (char *)limit);
	return NULL;
    }
    
    if ((entry_virtual = mod_cband_get_virtualhost_entry(parms->server, parms->server->module_config, 1)) == NULL) {
	ap_log_error(APLOG_MARK, APLOG_WARNING, 0, parms->server, "Invalid command 'CBandClassLimit %s %s', undefined virtualhost name", (char *)arg, (char *)limit);
	return NULL;
    }

    entry_virtual->virtual_class_limit[entry->class_nr] = mod_cband_conf_get_limit_kb((char *)limit,
	&entry_virtual->virtual_class_limit_mult[entry->class_nr]);
    
    return NULL;
}

static const char *mod_cband_set_user_class_limit(cmd_parms *parms, void *mconfig, const char *arg, const char *limit)
{
    mod_cband_class_config_entry *entry;
    mod_cband_user_config_entry *entry_user;
    const char *err;
    
    if ((err = ap_check_cmd_context(parms, GLOBAL_ONLY)) != NULL)
	return err;
    
    if ((entry = mod_cband_get_class_entry((char *)arg, parms->server->module_config, 0)) == NULL) {
    	ap_log_error(APLOG_MARK, APLOG_WARNING, 0, parms->server, "Invalid command 'CBandUserClassLimit %s %s', undefined class name", (char *)arg, (char *)limit);
	return NULL;
    }

    if ((entry_user = mod_cband_get_user_entry(username_arg, parms->server->module_config, 1)) == NULL) {
    	ap_log_error(APLOG_MARK, APLOG_WARNING, 0, parms->server, "Invalid command 'CBandUserClassLimit %s %s', undefined user name", (char *)arg, (char *)limit);
	return NULL;
    }

    entry_user->user_class_limit[entry->class_nr] = mod_cband_conf_get_limit_kb((char *)limit,
	&entry_user->user_class_limit_mult[entry->class_nr]);
    
    return NULL;
}

unsigned long mod_cband_conf_get_period_sec(char *period)
{
    unsigned long val;
    char unit;

    sscanf(period, "%lu%c", &val, &unit);
    
    if (unit == 's' || unit == 'S')
	return val;
    else
    if (unit == 'm' || unit == 'M')
	return val * 60;
    else
    if (unit == 'h' || unit == 'H')
	return val * 60 * 60;
    else
    if (unit == 'd' || unit == 'D')
	return val * 60 * 60 * 24;
    else
    if (unit == 'w' || unit == 'W')
	return val * 60 * 60 * 24 * 7;

    return atol(period);
}

unsigned long mod_cband_conf_get_limit_kb(char *limit, unsigned int *mult)
{
    unsigned long val;
    char unit, unit2 = 0;

    sscanf(limit, "%lu%c%c", &val, &unit, &unit2);
    if (unit2 == 'i' || unit2 == 'I')
	*mult = 1024;
    else
	*mult = 1000;

    if (unit == 'k' || unit == 'K')
	return val;
    else
    if (unit == 'm' || unit == 'M')
	return val * *mult;
    else
    if (unit == 'g' || unit == 'G')
	return val * *mult * *mult;

    return atol(limit);
}

unsigned long mod_cband_conf_get_speed_kbps(char *speed)
{
    unsigned long val;
    char unit1, unit2 = 'p';

    sscanf(speed, "%lu%cb%cs", &val, &unit1, &unit2);

    /* kibibytes per second */
    if (unit2 == '/')
	val = val*8;
    else if (unit2 != 'p') {
	/* rzucic jakims bledem :) */
    }

    if (unit1 == 'k' || unit1 == 'K')
	return val;
    else
    if (unit1 == 'm' || unit1 == 'M')
	return val * 1024;
    else
    if (unit1 == 'g' || unit1 == 'G')
	return val * 1024 * 1024;

    return atol(speed);
}

char *mod_cband_create_time(apr_pool_t *p, unsigned long sec)
{
    unsigned h, m, s, d, w;
    char period[MAX_PERIOD_LEN];

    s = sec % 60;
    sec /= 60;
    m = sec % 60;
    sec /= 60;
    h = sec % 24;
    sec /= 24;
    d = sec % 7;
    sec /= 7;
    w = sec;

    sprintf(period, "%uW ", w);
    sprintf(period + strlen(period), "%uD ", d);
    sprintf(period + strlen(period), "%02u:%02u:%02u", h, m, s);
	
    return apr_pstrndup(p, period, strlen(period));
}

char *mod_cband_create_traffic_size(apr_pool_t *p, unsigned long kb, char *unit, int mult)
{
    char traffic[MAX_TRAFFIC_LEN];
    char dest_unit[3] = {0, 0, 0};
    float v;
    unsigned long vi;

    if (mult <= 0)
	mult = 1000;

    if ((unit != "" && unit[0] == 'G') || (unit == "" && kb >= mult*mult)) {
	dest_unit[0] = 'G';
	v = (float)kb / (mult * mult);
    } else
    if ((unit != "" && unit[0] == 'M') || (unit == "" && kb >= mult)) {
	dest_unit[0] = 'M';
	v = (float)kb / mult;
    } else {
	dest_unit[0] = 'K';
	v = kb;
    }
    if (mult == 1024)
	dest_unit[1] = 'i';

    vi = (unsigned long)truncf(v * 100);
    v  = (float)vi / 100;
    
    if (vi % 100)
	sprintf(traffic, "%0.2f%sB", v, dest_unit);
    else
    	sprintf(traffic, "%0.0f%sB", v, dest_unit);

    return apr_pstrndup(p, traffic, strlen(traffic));
}

char *mod_cband_create_period(apr_pool_t *p, unsigned long start, unsigned long refresh)
{
    unsigned sec;

    if (start == 0 || refresh == 0)
	return "never";

    sec = start + refresh;
    sec -= (unsigned long)(apr_time_now() / 1e6);
    
    return mod_cband_create_time(p, sec);
}

static const command_rec mod_cband_cmds[] =
{
  AP_INIT_TAKE1(
      "CBandDefaultExceededURL",
      mod_cband_set_default_url,
      NULL,
      RSRC_CONF,
      "CBandDefaultExceededURL - The URL to redirect when bandwidth is exceeded."
    ),

  AP_INIT_TAKE1(
      "CBandDefaultExceededCode",
      mod_cband_set_default_code,
      NULL,
      RSRC_CONF,
      "CBandDefaultExceededCode - The http code sent to user when the limit is exceeded"
    ),

  AP_INIT_FLAG(
      "CBandRandomPulse",
      mod_cband_set_random_pulse,
      NULL,
      RSRC_CONF,
      "CBandRandomPulse - Sets random pulse for FBS algorithm."
    ),
  
  AP_INIT_TAKE1(
      "CBandScoreFlushPeriod",
      mod_cband_set_score_flush_period,
      NULL,
      RSRC_CONF,
      "CBandScoreFlushPeriod"
    ),
  
  AP_INIT_TAKE1(
      "CBandLimit",
      mod_cband_set_limit,
      NULL,
      RSRC_CONF,
      "CBandLimit - The limit bandwidth in KB for virtualhost."
    ),

  AP_INIT_TAKE1(
      "CBandPeriod",
      mod_cband_set_period,
      NULL,
      RSRC_CONF,
      "CBandPeriod - The time after the scoreboard will be cleared"
    ),

  AP_INIT_TAKE1(
      "CBandPeriodSlice",
      mod_cband_set_period_slice,
      NULL,
      RSRC_CONF,
      "CBandPeriodSlice - Specifies number of bandwidth slices"
    ),

  AP_INIT_TAKE1(
      "CBandExceededURL",
      mod_cband_set_url,
      NULL,
      RSRC_CONF,
      "CBandExceededURL - The URL to redirect when virtualhost's bandwidth is exceeded."
    ),

  AP_INIT_TAKE3 (
      "CBandSpeed",
      mod_cband_set_speed,
      NULL,
      RSRC_CONF,
      "CBandSpeed - Maximal speed for virtualhost."
    ),

  AP_INIT_TAKE3 (
      "CBandRemoteSpeed",
      mod_cband_set_remote_speed,
      NULL,
      RSRC_CONF,
      "CBandRemoteSpeed - Maximal speed for remote clients."
    ),

  AP_INIT_RAW_ARGS (
      "CBandClassRemoteSpeed",
      mod_cband_set_class_remote_speed,
      NULL,
      RSRC_CONF,
      "CBandClassRemoteSpeed - Maximal speed for remote class."
    ),

  AP_INIT_TAKE3 (
      "CBandExceededSpeed",
      mod_cband_set_exceeded_speed,
      NULL,
      RSRC_CONF,
      "CBandExceededSpeed - Over limit speed for virtualhost."
    ),

  AP_INIT_TAKE1(
      "CBandScoreboard",
      mod_cband_set_scoreboard,
      NULL,
      RSRC_CONF,
      "CBandScoreboard - The path to the virtualhost's scoreboard file."
    ),

  AP_INIT_TAKE1(
      "CBandUser",
      mod_cband_set_user,
      NULL,
      RSRC_CONF,
      "CBandUser - virtualhost user owner."
    ),

  AP_INIT_RAW_ARGS(
      "<CBandUser", 
      mod_cband_user_section, 
      NULL,
      RSRC_CONF,
      "CBandUser section"
    ),

  AP_INIT_TAKE1(
      "CBandUserLimit",
      mod_cband_set_user_limit,
      NULL,
      RSRC_CONF,
      "CBandUserLimit - The limit bandwidth in KB for user."
    ),

  AP_INIT_TAKE1(
      "CBandUserPeriod",
      mod_cband_set_user_period,
      NULL,
      RSRC_CONF,
      "CBandUserPeriod - The time after the scoreboard will be cleared"
    ),

  AP_INIT_TAKE1(
      "CBandUserPeriodSlice",
      mod_cband_set_user_period_slice,
      NULL,
      RSRC_CONF,
      "CBandPeriodSlice - Specifies number of bandwidth slices"
    ),
    
  AP_INIT_TAKE1(
      "CBandUserExceededURL",
      mod_cband_set_user_url,
      NULL,
      RSRC_CONF,
      "CBandUserExceededURL - The URL to redirect when user's bandwidth is exceeded."
    ),

  AP_INIT_TAKE3(
      "CBandUserSpeed",
      mod_cband_set_user_speed,
      NULL,
      RSRC_CONF,
      "CBandUserSpeed - Maximal speed for user."
    ),

  AP_INIT_TAKE3(
      "CBandUserRemoteSpeed",
      mod_cband_set_user_remote_speed,
      NULL,
      RSRC_CONF,
      "CBandUserRemoteSpeed - Maximal remote speed for user."
    ),

  AP_INIT_TAKE3(
      "CBandUserExceededSpeed",
      mod_cband_set_user_exceeded_speed,
      NULL,
      RSRC_CONF,
      "CBandUserExceededSpeed - Over limit speed for user."
    ),

  AP_INIT_TAKE1(
      "CBandUserScoreboard",
      mod_cband_set_user_scoreboard,
      NULL,
      RSRC_CONF,
      "CBandUserScoreboard - The path to the user's scoreboard file."
    ),

  AP_INIT_RAW_ARGS(
      "<CBandClass", 
      mod_cband_class_section, 
      NULL,
      RSRC_CONF,
      "CBandClass section"
    ),

  AP_INIT_TAKE1(
      "CBandClassDst",
      mod_cband_set_class_dst,
      NULL,
      RSRC_CONF,
      "CBandClassDst"
    ),

  AP_INIT_TAKE2(
      "CBandClassLimit",
      mod_cband_set_class_limit,
      NULL,
      RSRC_CONF,
      ""
    ),

  AP_INIT_TAKE2(
      "CBandUserClassLimit",
      mod_cband_set_user_class_limit,
      NULL,
      RSRC_CONF,
      ""
    ),

    {NULL}
};

apr_status_t patricia_cleanup(void *p)
{
    patricia_tree_t *t = (patricia_tree_t *) p;
    Clear_Patricia(t,NULL);
    
    return APR_SUCCESS;
}

int mod_cband_get_dst(request_rec *r) 
{
    patricia_node_t *node;
    prefix_t p;
    char *leaf;
	      
    if (config->tree == NULL)
	return -1;
    
    p.bitlen = 32;
    p.ref_count = 0;
    p.family = AF_INET;
    p.add.sin.s_addr = inet_addr(r->connection->remote_ip);
		      
    node = patricia_search_best(config->tree, &p);
				    
    if (node) {
        leaf = node->user1;
					            
        if (leaf) {
#ifdef DEBUG
            fprintf(stderr,"%s leaf %s\n",r->connection->remote_ip,leaf);
            fflush(stderr);
#endif
	    return atoi(leaf);
        }
    }
    
    return -1;
}

int mod_cband_get_remote_host(struct conn_rec *c, int create, mod_cband_virtualhost_config_entry *entry)
{
    int i;
    mod_cband_remote_host *hosts;
    unsigned long time_now, time_delta;
    in_addr_t addr;
    
    if (entry == NULL)
	return -1;
    
    if (c->remote_ip != NULL)
	addr = inet_addr(c->remote_ip);    
    else
	addr = c->remote_addr->sa.sin.sin_addr.s_addr;
	
    time_now = apr_time_now();     
    hosts = config->remote_hosts.hosts;

    if (hosts == NULL)
	return -1;
    
    /* BEGIN CRITICAL SECTION */
    mod_cband_sem_down(config->remote_hosts.sem_id);
    for (i = 0; i < MAX_REMOTE_HOSTS; i++) {
	time_delta = (time_now - hosts[i].remote_last_time) / 1e6;
	if (hosts[i].used && ((time_delta <= MAX_REMOTE_HOST_LIFE) || (hosts[i].remote_conn > 0)) &&
	   (hosts[i].remote_addr == addr) && (hosts[i].virtual_name == entry->virtual_name)) {
	    mod_cband_sem_up(config->remote_hosts.sem_id);
	    /* END CRITICAL SECTION */
	    return i; 
	}
    }

    if (create) {    
	for (i = 0; i < MAX_REMOTE_HOSTS; i++) {
	    time_delta = (time_now - hosts[i].remote_last_time) / 1e6;
	    if ((hosts[i].used == 0) || ((time_delta > MAX_REMOTE_HOST_LIFE) && (hosts[i].remote_conn <= 0))) {
		memset(&hosts[i], 0, sizeof(mod_cband_remote_host));
		hosts[i].used                = 1;
		hosts[i].remote_addr         = addr;
		hosts[i].remote_last_time    = time_now;
		hosts[i].remote_last_refresh = time_now;
		hosts[i].virtual_name        = entry->virtual_name;
		mod_cband_sem_up(config->remote_hosts.sem_id);
		/* END CRITICAL SECTION */
		return i; 
	    }
	}
    }
    mod_cband_sem_up(config->remote_hosts.sem_id);
    /* END CRITICAL SECTION */

    return -1;
}

void mod_cband_safe_change(unsigned long *val, int diff)
{
    if (val == NULL)
	return;

    if ((diff > 0) || (diff < 0 && *val >= -diff))
	*val += diff;
    else
	*val = 0;
}

int mod_cband_change_remote_connections_lock(int index, int diff)
{
    if (index < 0)
	return -1;

    /* BEGIN CRITICAL SECTION */
    mod_cband_sem_down(config->remote_hosts.sem_id);
    mod_cband_safe_change(&config->remote_hosts.hosts[index].remote_conn, diff);
    mod_cband_sem_up(config->remote_hosts.sem_id);
    /* END CRITICAL SECTION */

    return 0;
}

/* 
 * Nie trzeba semafora, poniewaz operacje na liczbach 32-bit sa atomowe
 */
int mod_cband_set_remote_request_time(int index, unsigned long time)
{
    if (index < 0)
	return -1;

    config->remote_hosts.hosts[index].remote_last_time = time;

    return 0;
}

/* 
 * Nie trzeba semafora, poniewaz operacje na liczbach 32-bit sa atomowe
 */
int mod_cband_set_remote_current_speed(int index, unsigned long kbps)
{
    if (index < 0)
	return -1;

    config->remote_hosts.hosts[index].remote_kbps = kbps;

    return 0;
}

/* 
 * Nie trzeba semafora, poniewaz operacje na liczbach 32-bit sa atomowe
 */
int mod_cband_set_remote_max_connections(int index, unsigned long max)
{
    if (index < 0)
	return -1;

    config->remote_hosts.hosts[index].remote_max_conn = max;

    return 0;
}

/* 
 * Nie trzeba semafora, poniewaz operacje na liczbach 32-bit sa atomowe
 */
int mod_cband_set_remote_last_refresh(int index, unsigned long time)
{
    if (index < 0)
	return -1;

    config->remote_hosts.hosts[index].remote_last_refresh = time;

    return 0;
}

/* 
 * Nie trzeba semafora, poniewaz operacje na liczbach 32-bit sa atomowe
 */
int mod_cband_set_remote_total_connections(int index, unsigned long set)
{
    if (index < 0)
	return -1;

    config->remote_hosts.hosts[index].remote_total_conn = set;

    return 0;
}

/* 
 * Nie trzeba semafora, poniewaz operacje na liczbach 32-bit sa atomowe
 */
int mod_cband_get_remote_total_connections(int index)
{
    if (index < 0)
	return -1;

    return config->remote_hosts.hosts[index].remote_conn;
}

float mod_cband_get_remote_connections_speed_lock(int index)
{
    unsigned long time_now;
    float time_delta;
    float rps = 0;
    
    if (index < 0)
	return 0;

    time_now = apr_time_now();
    
    /* BEGIN CRITICAL SECTION */
    mod_cband_sem_down(config->remote_hosts.sem_id);
    time_delta = (float)(time_now - config->remote_hosts.hosts[index].remote_last_refresh) / 1e6;
    if (time_delta > 0)
	rps = (float)(config->remote_hosts.hosts[index].remote_total_conn) / time_delta;
    mod_cband_sem_up(config->remote_hosts.sem_id);
    /* END CRITICAL SECTION */

    return rps;
}

int mod_cband_change_remote_total_connections_lock(int index, unsigned long diff)
{
    if (index < 0)
	return -1;

    /* BEGIN CRITICAL SECTION */
    mod_cband_sem_down(config->remote_hosts.sem_id);
    mod_cband_safe_change(&config->remote_hosts.hosts[index].remote_total_conn, diff);
    mod_cband_sem_up(config->remote_hosts.sem_id);
    /* END CRITICAL SECTION */

    return 0;
}

/* 
 * Nie trzeba semafora, poniewaz operacje na liczbach 32-bit sa atomowe
 */
unsigned long mod_cband_get_remote_connection_time(int index)
{
    if (index < 0)
	return 0;

    return config->remote_hosts.hosts[index].remote_last_time;
}

/* 
 * Nie trzeba semafora, poniewaz operacje na liczbach 32-bit sa atomowe
 */
int mod_cband_get_remote_connections(int index)
{
    if (index < 0)
	return 0;
	
    return config->remote_hosts.hosts[index].remote_conn;
}

/* 
 * Nie trzeba semafora, poniewaz operacje na liczbach 32-bit sa atomowe
 */
int mod_cband_remove_remote_host(int index)
{
    if (index < 0)
	return -1;

    config->remote_hosts.hosts[index].used = 0;

    return 0;
}

int mod_cband_get_dst_speed_lock(mod_cband_virtualhost_config_entry *entry, mod_cband_user_config_entry *entry_user, unsigned long *remote_kbps, unsigned long *remote_rps, unsigned long *remote_max_conn, int dst)
{
    unsigned long virtualhost_kbps = 0;
    unsigned long user_kbps = 0;
    unsigned long virtualhost_rps = 0;
    unsigned long user_rps = 0;
    unsigned long virtualhost_max_conn = 0;
    unsigned long user_max_conn = 0;

    if (entry != NULL) {
        /* BEGIN CRITICAL SECTION */
	mod_cband_sem_down(config->sem_id);
        virtualhost_kbps     = entry->shmem_data->remote_speed.kbps;
	virtualhost_rps      = entry->shmem_data->remote_speed.rps;	
	virtualhost_max_conn = entry->shmem_data->remote_speed.max_conn;	
        mod_cband_sem_up(config->sem_id);
	/* BEGIN CRITICAL SECTION */

	if (dst >= 0 && dst <= DST_CLASS) {
	    if (entry->virtual_class_speed[dst].kbps > 0)
		virtualhost_kbps = entry->virtual_class_speed[dst].kbps;
		
	    if (entry->virtual_class_speed[dst].rps > 0)
		virtualhost_rps  = entry->virtual_class_speed[dst].rps;

	    if (entry->virtual_class_speed[dst].max_conn > 0)
		virtualhost_max_conn  = entry->virtual_class_speed[dst].max_conn;
	}
    }

    if (entry_user != NULL) {
        /* BEGIN CRITICAL SECTION */
	mod_cband_sem_down(config->sem_id);
        user_kbps     = entry_user->shmem_data->remote_speed.kbps;
	user_rps      = entry_user->shmem_data->remote_speed.rps;	
	user_max_conn = entry_user->shmem_data->remote_speed.max_conn;	
        mod_cband_sem_up(config->sem_id);
	/* BEGIN CRITICAL SECTION */
	
	if (dst >= 0 && dst <= DST_CLASS) {
	    if (entry_user->user_class_speed[dst].kbps > 0)
		user_kbps = entry_user->user_class_speed[dst].kbps;
	    
	    if (entry_user->user_class_speed[dst].rps > 0)
		user_rps  = entry_user->user_class_speed[dst].rps;

	    if (entry_user->user_class_speed[dst].max_conn > 0)
		user_max_conn  = entry_user->user_class_speed[dst].max_conn;
	}
    }

    if (remote_kbps != NULL) {
	if ((user_kbps > 0) && (virtualhost_kbps > user_kbps)) 
	    *remote_kbps = user_kbps;
        else
	if (virtualhost_kbps > 0)
	    *remote_kbps = virtualhost_kbps;
        else
	    *remote_kbps = user_kbps;
    }

    if (remote_rps != NULL) {
	if ((user_rps > 0) && (virtualhost_rps > user_rps)) 
	    *remote_rps = virtualhost_rps;
        else
	if (virtualhost_rps > 0)
	    *remote_rps = virtualhost_rps;
        else
    	    *remote_rps = user_rps;
    }

    if (remote_max_conn != NULL) {
    	if ((user_max_conn > 0) && (virtualhost_max_conn > user_max_conn)) 
	    *remote_max_conn = virtualhost_max_conn;
        else
	if (virtualhost_max_conn > 0)
	    *remote_max_conn = virtualhost_max_conn;
        else
    	    *remote_max_conn = user_max_conn;
    }
    
    return 0;
}

/* 
 * Nie trzeba semafora, poniewaz operacje na liczbach 32-bit sa atomowe
 */
int mod_cband_get_score(server_rec *s, char *path, unsigned long long *val, int dst, mod_cband_shmem_data *shmem_data)
{
    if (val == NULL || shmem_data == NULL)
	return -1;

    if (dst < 0)
	*val = shmem_data->total_usage.total_bytes;
    else
	*val = shmem_data->total_usage.class_bytes[dst];
    
    return 0;
}


/*
 * semafor opuszczany w mod_cband_update_score_cache
 */
int mod_cband_get_score_all(server_rec *s, char *path, mod_cband_scoreboard_entry *val)
{
    apr_file_t *fd;
    apr_size_t nbuf;
    apr_pool_t *subpool;
    
    if (path == NULL || val == NULL)
	return -1;
    
    apr_pool_create(&subpool, config->p);
    
    if ((apr_file_open(&fd, path, APR_READ | APR_BINARY, 0, subpool)) != APR_SUCCESS) {
	apr_pool_destroy(subpool);
	return -1;
    }
    
    nbuf = sizeof(mod_cband_scoreboard_entry);
    apr_file_read(fd, val, &nbuf);
    apr_file_close(fd);
    apr_pool_destroy(subpool);
    
    return 0;
}

/* 
 * semafor opuszczany w mod_cband_save_score_cache i mod_cband_flush_score_lock
 */
int mod_cband_save_score(char *path, mod_cband_scoreboard_entry *scoreboard)
{
    apr_file_t *fd;
    apr_size_t nbuf;
    apr_pool_t *subpool;
    
    if (path == NULL || scoreboard == NULL || scoreboard->was_request == 0)
	return -1;
	
    apr_pool_create(&subpool, config->p);

    if ((apr_file_open(&fd, path,
		APR_CREATE | APR_READ | APR_WRITE | APR_BINARY, APR_UREAD | APR_UWRITE, subpool)) != APR_SUCCESS) {
	
	fprintf(stderr, "apache2_mod_cband: cannot open scoreboard file %s\n", path);
	fflush(stderr);
	
	return -1;
    }
   
    apr_file_lock(fd, APR_FLOCK_EXCLUSIVE);
    nbuf = sizeof(mod_cband_scoreboard_entry);
    apr_file_write(fd, scoreboard, &nbuf);
    apr_file_unlock(fd);
    apr_file_close(fd);
    apr_pool_destroy(subpool);
    
    return 0;
}

int mod_cband_flush_score_lock(char *path, mod_cband_scoreboard_entry *scoreboard)
{
    if ((path == NULL) || (scoreboard == NULL))
	return -1;

    /* BEGIN CRITICAL SECTION */
    mod_cband_sem_down(config->sem_id);
    
    scoreboard->was_request = 1;
    if (--(scoreboard->score_flush_count) <= 0) {
	mod_cband_save_score(path, scoreboard);
	scoreboard->score_flush_count = config->score_flush_period;
    }
    
    mod_cband_sem_up(config->sem_id);
    /* END CRITICAL SECTION */        
    
    return 0;
}

/*
 * semafor opuszczany poza funkcje mod_cband_log_bucket
 */
int mod_cband_update_score(char *path, unsigned long long *bytes_served, int dst, mod_cband_scoreboard_entry *scoreboard)
{
    if (scoreboard == NULL || bytes_served == NULL)
	return -1;

    scoreboard->total_bytes	     += (unsigned long long)(*bytes_served);
    if (dst >= 0)
	scoreboard->class_bytes[dst] += (unsigned long long)(*bytes_served);

    return 0;
}

int mod_cband_clear_score_lock(mod_cband_scoreboard_entry *scoreboard)
{
    if (scoreboard == NULL)
	return -1;

    /* BEGIN CRITICAL SECTION */        
    mod_cband_sem_down(config->sem_id);
    memset(scoreboard, 0, sizeof(mod_cband_scoreboard_entry));    
    mod_cband_sem_up(config->sem_id);
    /* END CRITICAL SECTION */        

    return 0;
}

/* semafor opuszczany przez funkcje mod_cband_post_config */
int mod_cband_update_score_cache(server_rec *s)
{
    mod_cband_virtualhost_config_entry *entry = NULL;
    mod_cband_user_config_entry *entry_user = NULL;

    entry = config->next_virtualhost;
    while(entry != NULL) {
        mod_cband_get_score_all(s, entry->virtual_scoreboard, &(entry->shmem_data->total_usage));
        if ((entry = entry->next) == NULL)
	    break;
    }

    entry_user = config->next_user;
    while(entry_user != NULL) {
        mod_cband_get_score_all(s, entry_user->user_scoreboard, &(entry_user->shmem_data->total_usage));
        if ((entry_user = entry_user->next) == NULL)
	    break;
    }
    
    return OK;
}

int mod_cband_save_score_cache(void)
{
    mod_cband_virtualhost_config_entry *entry = NULL;
    mod_cband_user_config_entry *entry_user = NULL;

    entry = config->next_virtualhost;
    while(entry != NULL) {
        mod_cband_save_score(entry->virtual_scoreboard, &(entry->shmem_data->total_usage));
        if ((entry = entry->next) == NULL)
	    break;
    }

    entry_user = config->next_user;
    while(entry_user != NULL) {
        mod_cband_save_score(entry_user->user_scoreboard, &(entry_user->shmem_data->total_usage));
        if ((entry_user = entry_user->next) == NULL)
	    break;
    }
    
    return OK;
}

unsigned long mod_cband_get_start_time(mod_cband_scoreboard_entry *scoreboard)
{
    if (scoreboard == NULL)
	return 0;

    return scoreboard->start_time;
}

int mod_cband_set_start_time(mod_cband_scoreboard_entry *scoreboard, unsigned long start_time)
{
    if (scoreboard == NULL)
	return 0;

    scoreboard->start_time = start_time;
    
    return 0;
}

/*
 * speed aproximation function
 */
int mod_cband_get_speed_lock(mod_cband_shmem_data *shmem_data, float *bps, float *rps)
{
    float time_delta;

    if (shmem_data == NULL)
	return -1;

    /* BEGIN CRITICAL SECTION */
    mod_cband_sem_down(config->sem_id);

    time_delta = (float)(shmem_data->time_delta) / 1e6;

    if (time_delta <= 0)
	time_delta = PERIOD_LEN;

    if (bps != NULL)
	*bps = ((float)(shmem_data->old_TX * 8)) / time_delta;    
    
    if (rps != NULL)
	*rps = ((float)(shmem_data->old_conn)) / time_delta;

    mod_cband_sem_up(config->sem_id);
    /* END CRITICAL SECTION */

    return 0;
}

/* 
 * semafor opuszczany przez funkcje check_connections_speed
 */
int mod_cband_get_real_speed(mod_cband_shmem_data *shmem_data, float *bps, float *rps)
{
    if (shmem_data == NULL)
	return -1;

    if (bps != NULL)
	*bps = ((float)(shmem_data->current_TX * 8) / PERIOD_LEN);  
    
    if (rps != NULL)
        *rps = ((float)(shmem_data->current_conn) / PERIOD_LEN);    

    return 0;
}

/*
 * semafor opuszczany przez mod_cband_log_bucket, mod_cband_update_speed_lock, mod_cband_check_connections_speed
 */
int mod_cband_update_speed(mod_cband_shmem_data *shmem_data, unsigned long bytes_served, int new_connection, int remote_idx)
{
    unsigned long time_delta;
    unsigned long time_last_request;
    unsigned long time_now;
    unsigned long time_delta_real;
    
    if (shmem_data == NULL)
	return -1;
    
    time_now          = apr_time_now();
    time_delta_real   = time_now - shmem_data->total_last_refresh;
    time_delta        = (time_now - shmem_data->total_last_refresh) / 1e6;
    time_last_request = (time_now - shmem_data->total_last_time) / 1e6;
    
    if (bytes_served > 0)
	shmem_data->current_TX += bytes_served;
    
    if (new_connection) {
    	shmem_data->total_last_time = time_now;
        mod_cband_set_remote_request_time(remote_idx, time_now);
	mod_cband_change_remote_total_connections_lock(remote_idx, 1);
        shmem_data->current_conn += new_connection;
    }

    if (time_delta > PERIOD_LEN) {    
	shmem_data->total_last_refresh = time_now;
	mod_cband_set_remote_total_connections(remote_idx, 0);
        mod_cband_set_remote_last_refresh(remote_idx, time_now);
	shmem_data->time_delta   = time_delta_real;
    }
    
    if (time_delta > PERIOD_LEN && time_delta < 2 * PERIOD_LEN) {
        shmem_data->old_TX       = shmem_data->current_TX;
	shmem_data->old_conn     = shmem_data->current_conn;
        shmem_data->current_TX   = 0;
	shmem_data->current_conn = 0;
    } else 
    if (time_delta >= 2 * PERIOD_LEN) {
	shmem_data->old_TX       = shmem_data->current_TX;
	shmem_data->old_conn     = shmem_data->current_conn;
        shmem_data->current_TX   = 0;
	shmem_data->current_conn = 0;
    }
        
    return 0;
}

int mod_cband_update_speed_lock(mod_cband_shmem_data *shmem_data, unsigned long bytes_served, int new_connection, int remote_idx)
{
    /* BEGIN CRITICAL SECTION */
    mod_cband_sem_down(config->sem_id);
    mod_cband_update_speed(shmem_data, bytes_served, new_connection, remote_idx);
    mod_cband_sem_up(config->sem_id);
    /* END CRITICAL SECTION */

    return 0;
}

int mod_cband_set_overlimit_speed(mod_cband_shmem_data *shmem_data)
{
    if (shmem_data == NULL)
	return -1;

    shmem_data->curr_speed.kbps     = shmem_data->over_speed.kbps;
    shmem_data->curr_speed.rps      = shmem_data->over_speed.rps;
    shmem_data->curr_speed.max_conn = shmem_data->over_speed.max_conn;
    shmem_data->shared_kbps         = shmem_data->over_speed.kbps;
    shmem_data->overlimit           = 1;

    return 0;
}

int mod_cband_set_overlimit_speed_lock(mod_cband_shmem_data *shmem_data)
{
    if (shmem_data == NULL)
	return -1;

    /* BEGIN CRITICAL SECTION */
    mod_cband_sem_down(config->sem_id);
    mod_cband_set_overlimit_speed(shmem_data);
    mod_cband_sem_up(config->sem_id);
    /* END CRITICAL SECTION */

    return 0;
}

int mod_cband_set_normal_speed(mod_cband_shmem_data *shmem_data)
{
    if (shmem_data == NULL)
	return -1;

    shmem_data->curr_speed.kbps     = shmem_data->max_speed.kbps;
    shmem_data->curr_speed.rps      = shmem_data->max_speed.rps;
    shmem_data->curr_speed.max_conn = shmem_data->max_speed.max_conn;
    shmem_data->shared_kbps         = shmem_data->max_speed.kbps;
    shmem_data->overlimit           = 0;

    return 0;
}

int mod_cband_set_normal_speed_lock(mod_cband_shmem_data *shmem_data)
{
    if (shmem_data == NULL)
	return -1;

    /* BEGIN CRITICAL SECTION */
    mod_cband_sem_down(config->sem_id);
    mod_cband_set_normal_speed(shmem_data);
    mod_cband_sem_up(config->sem_id);
    /* END CRITICAL SECTION */

    return 0;
}

void mod_cband_check_virtualhost_refresh(mod_cband_virtualhost_config_entry *entry_virtual, unsigned long sec)
{
    mod_cband_scoreboard_entry *scoreboard;

    if (entry_virtual == NULL || entry_virtual->refresh_time == 0)
	return;

    scoreboard = &(entry_virtual->shmem_data->total_usage);

    if (mod_cband_get_start_time(scoreboard) < 0)
    	mod_cband_set_start_time(scoreboard, sec);
    
    if ((mod_cband_get_start_time(scoreboard) + entry_virtual->refresh_time) < sec) {
    	mod_cband_clear_score_lock(scoreboard);
	mod_cband_set_normal_speed_lock(entry_virtual->shmem_data);
	mod_cband_set_start_time(scoreboard, sec);
    }
}

void mod_cband_check_user_refresh(mod_cband_user_config_entry *entry_user, unsigned long sec)
{
    mod_cband_scoreboard_entry *scoreboard;

    if (entry_user == NULL || entry_user->refresh_time == 0)
	return;

    scoreboard = &(entry_user->shmem_data->total_usage);

    if (mod_cband_get_start_time(scoreboard) < 0)
    	mod_cband_set_start_time(scoreboard, sec);
    
    if ((mod_cband_get_start_time(scoreboard) + entry_user->refresh_time) < sec) {
    	mod_cband_clear_score_lock(scoreboard);
	mod_cband_set_normal_speed_lock(entry_user->shmem_data);
	mod_cband_set_start_time(scoreboard, sec);
    }
}

int mod_cband_reset(mod_cband_shmem_data *shmem_data)
{
    if (shmem_data == NULL)
	return -1;

    mod_cband_clear_score_lock(&(shmem_data->total_usage));
    mod_cband_set_start_time(&(shmem_data->total_usage), (unsigned long)(apr_time_now() / 1e6));
    mod_cband_set_normal_speed_lock(shmem_data);

    return 0;
}

int mod_cband_reset_virtualhost(char *name)
{
    mod_cband_virtualhost_config_entry *entry;
    char virtualhost[MAX_VIRTUALHOST_NAME];
    unsigned port, line;

    if (name == NULL)
	return -1;

    if (!strcasecmp(name, "all")) {
        entry = config->next_virtualhost;
    
	while(entry != NULL) {
	    mod_cband_reset(entry->shmem_data);

	    if (entry->next == NULL)
		break;
	    
	    entry = entry->next;
        }
    } else {
	sscanf(name, "%[^:]:%u:%u", virtualhost, &port, &line);
	
	if ((entry = mod_cband_get_virtualhost_entry_(virtualhost, (apr_port_t)port, line, 0)) != NULL)
	    mod_cband_reset(entry->shmem_data);
    }
    
    return 0;
}

int mod_cband_reset_user(char *name)
{
    mod_cband_user_config_entry *entry;

    if (name == NULL)
	return -1;

    if (!strcasecmp(name, "all")) {
        entry = config->next_user;
    
	while(entry != NULL) {
	    mod_cband_reset(entry->shmem_data);
    
	    if (entry->next == NULL)
		break;
	    
	    entry = entry->next;
        }
    } else {
	if ((entry = mod_cband_get_user_entry(name, NULL, 0)) != NULL)
	    mod_cband_reset(entry->shmem_data);
    }
    
    return 0;
}

unsigned long mod_cband_get_slice_limit(unsigned long start_time, unsigned long refresh_time, 
	unsigned long slice_len, unsigned long limit)
{
    unsigned long slice_limit, slice;
    unsigned int slice_no;

    if (slice_len > 0 && refresh_time > 0) {
        slice_limit = (unsigned long)(((float)slice_len / refresh_time) * limit);
	slice_no    = (((unsigned long)(apr_time_now() / 1e6) - start_time) / slice_len) + 1;
	slice       = slice_no * slice_limit;
	
	if (slice > limit)
	    slice = limit;
	
	return slice;
    }
    
    return limit;
}

void mod_cband_status_print_limit(request_rec *req, unsigned long limit, unsigned long usage, char *unit, unsigned int mult, unsigned long slice_limit)
{
    unsigned char normal_r, normal_g, normal_b;
    unsigned char exceeded_r, exceeded_g, exceeded_b;
    unsigned char r, g, b;
    const char *color;
    float u;

    if (slice_limit == 0)
	slice_limit = limit;
        
    exceeded_r = 0xff;
    exceeded_g = 0x30;
    exceeded_b = 0x50;
    normal_r = 0x30;
    normal_g = 0xf0;
    normal_b = 0x30;
        
    if (limit == (unsigned long)0) 
        ap_rprintf(req, "<td style=\"background-color: yellow\">U/U/%s</td>\n", mod_cband_create_traffic_size(req->pool, usage, unit, mult));
    else {
	if (usage < limit) {
	    r = normal_r;
	    g = normal_g;
	    b = normal_b;
	    if (usage > 0) {
		u = (float)usage / limit;
		r += (unsigned char)((float)(exceeded_r - normal_r) * u);
		g -= (unsigned char)((float)(normal_g - exceeded_g) * u);
		b += (unsigned char)((float)(exceeded_b - normal_b) * u);
	    }
	} else {
	    r = exceeded_r;
	    g = exceeded_g;
	    b = exceeded_b;
	}
    
	if (usage < limit / 2)
	    color = "black";
	else
	    color = "white";
	    
	ap_rprintf(req, "<td style=\"color: %s; background-color: #%02X%02X%02X\">%s/%s/%s</td>\n", color, r, g, b, mod_cband_create_traffic_size(req->pool, limit, unit, mult), mod_cband_create_traffic_size(req->pool, slice_limit, unit, mult), mod_cband_create_traffic_size(req->pool, usage, unit, mult));
    }
}

void mod_cband_status_print_speed(request_rec *req, unsigned long limit, float usage)
{
    unsigned char normal_r, normal_g, normal_b;
    unsigned char exceeded_r, exceeded_g, exceeded_b;
    unsigned char r, g, b;
    const char *color;
    float u;
    
    exceeded_r = 0xff;
    exceeded_g = 0x20;
    exceeded_b = 0x20;
    normal_r = 0xf0;
    normal_g = 0xa1;
    normal_b = 0xa1;
        
    if (limit == (unsigned long)0) 
        ap_rprintf(req, "<td class=\"speed\">U/%0.2f</td>\n", usage);
    else {
	if (usage < limit) {
	    r = normal_r;
	    g = normal_g;
	    b = normal_b;
	
	    if (usage > 0) {
		u = (float)usage / limit;
		g -= (unsigned char)((float)(normal_g - exceeded_g) * u);
	    	b -= (unsigned char)((float)(normal_b - exceeded_b) * u);
	    }
	} else {
	    r = exceeded_r;
	    g = exceeded_g;
	    b = exceeded_b;
	}

	if (usage < limit / 2)
	    color = "black";
	else
	    color = "white";
    
	ap_rprintf(req, "<td style=\"color: %s; background-color: #%02X%02X%02X\">%lu/%0.2f</td>\n", color, r, g, b, limit, usage);
    }
}

void mod_cband_status_print_connections(request_rec *req, unsigned long limit, unsigned long usage)
{
    unsigned char normal_r, normal_g, normal_b;
    unsigned char exceeded_r, exceeded_g, exceeded_b;
    unsigned char r, g, b;
    const char *color;
    float u;

    exceeded_r = 0x36;
    exceeded_g = 0x55;
    exceeded_b = 0xad;
    normal_r = 0xb4;
    normal_g = 0xbf;
    normal_b = 0xff;
        
    if (limit == (unsigned long)0) 
        ap_rprintf(req, "<td class=remote_odd>U/%lu</td>\n", usage);
    else {
	if (usage < limit) {
	    r = normal_r;
	    g = normal_g;
	    b = normal_b;
	    if (usage > 0) {
		u = (float)usage / limit;
		r -= (unsigned char)((float)(normal_r - exceeded_r) * u);
		g -= (unsigned char)((float)(normal_g - exceeded_g) * u);
		b -= (unsigned char)((float)(normal_b - exceeded_b) * u);
	    }
	} else {
	    r = exceeded_r;
	    g = exceeded_g;
	    b = exceeded_b;
	}

	if (usage <= limit / 2)
	    color = "black";
	else
	    color = "white";
    
	ap_rprintf(req, "<td style=\"color: %s; background-color: #%02X%02X%02X\">%lu/%lu</td>\n", color, r, g, b, limit, usage);
    }
}

void mod_cband_status_print_virtualhost_row(request_rec *r, mod_cband_virtualhost_config_entry *entry,
	int handler_type, int refresh, char *unit, unsigned long long *total_traffic) {
    
    mod_cband_scoreboard_entry *virtual_usage;
    unsigned long slice_limit;
    float bps, rps;
    int i;

    virtual_usage = &entry->shmem_data->total_usage;
	    
    ap_rputs("<tr>\n", r);
    ap_rprintf(r, "<td><a href=\"http://%s\">%s</a>:%d:(%d)</td>\n", entry->virtual_name, entry->virtual_name, entry->virtual_port, entry->virtual_defn_line);	
    if (handler_type == CBAND_HANDLER_ALL)
	ap_rprintf(r, "<td><a href=\"?reset=%s:%d:%d&amp;refresh=%d&amp;unit=%s\">reset</a></td>\n", entry->virtual_name, entry->virtual_port, entry->virtual_defn_line, refresh, unit);
    ap_rprintf(r, "<td class=\"refresh\">%s</td>\n", mod_cband_create_period(r->pool, virtual_usage->start_time, entry->refresh_time));	

    slice_limit = mod_cband_get_slice_limit(entry->shmem_data->total_usage.start_time, entry->refresh_time, entry->slice_len, entry->virtual_limit);
    mod_cband_status_print_limit(r, entry->virtual_limit, (unsigned long)(virtual_usage->total_bytes / entry->virtual_limit_mult),
	unit, entry->virtual_limit_mult, slice_limit);

    for (i = 0; i < DST_CLASS; i++) {
        slice_limit = mod_cband_get_slice_limit(entry->shmem_data->total_usage.start_time, entry->refresh_time, entry->slice_len, entry->virtual_class_limit[i]);
	mod_cband_status_print_limit(r, entry->virtual_class_limit[i], (unsigned long)(virtual_usage->class_bytes[i] / entry->virtual_class_limit_mult[i]),
	    unit, entry->virtual_class_limit_mult[i], slice_limit);
    }

    mod_cband_update_speed_lock(entry->shmem_data, 0, 0, -1);
    mod_cband_get_speed_lock(entry->shmem_data, &bps, &rps);
    mod_cband_status_print_speed(r, entry->shmem_data->curr_speed.kbps, bps / 1024);
    mod_cband_status_print_speed(r, entry->shmem_data->curr_speed.rps,  rps);
    mod_cband_status_print_connections(r, entry->shmem_data->curr_speed.max_conn, entry->shmem_data->total_conn);

    if (entry->virtual_user)
        ap_rprintf(r, "<td>%s</td>\n", entry->virtual_user);	
    else
	ap_rprintf(r, "<td>none</td>\n");	

    ap_rputs("</tr>\n", r);
    
    *total_traffic = virtual_usage->total_bytes;
}

void mod_cband_status_print_user_row(request_rec *r, mod_cband_user_config_entry *entry_user,
	int handler_type, int refresh, char *unit) {
    
    mod_cband_scoreboard_entry *user_usage;
    unsigned long slice_limit;
    float bps, rps;
    int i;

    user_usage = &entry_user->shmem_data->total_usage;
	    
    ap_rputs("<tr>\n", r);
    ap_rprintf(r, "<td>%s</td>\n", entry_user->user_name);	
    if (handler_type == CBAND_HANDLER_ALL)
	ap_rprintf(r, "<td><a href=\"?reset_user=%s&amp;refresh=%d&amp;unit=%s\">reset</a></td>\n", entry_user->user_name, refresh, unit);
    ap_rprintf(r, "<td class=\"refresh\">%s</td>\n", mod_cband_create_period(r->pool, user_usage->start_time, entry_user->refresh_time));

    slice_limit = mod_cband_get_slice_limit(entry_user->shmem_data->total_usage.start_time, entry_user->refresh_time, entry_user->slice_len, entry_user->user_limit);
    mod_cband_status_print_limit(r, entry_user->user_limit, (unsigned long)(user_usage->total_bytes / entry_user->user_limit_mult),
	unit, entry_user->user_limit_mult, slice_limit);

    for (i = 0; i < DST_CLASS; i++) {
        slice_limit = mod_cband_get_slice_limit(entry_user->shmem_data->total_usage.start_time, entry_user->refresh_time, entry_user->slice_len, entry_user->user_class_limit[i]);
	mod_cband_status_print_limit(r, entry_user->user_class_limit[i], (unsigned long)(user_usage->class_bytes[i] / entry_user->user_class_limit_mult[i]),
	    unit, entry_user->user_class_limit_mult[i], slice_limit);
    }

    mod_cband_update_speed_lock(entry_user->shmem_data, 0, 0, -1);
    mod_cband_get_speed_lock(entry_user->shmem_data, &bps, &rps);
    mod_cband_status_print_speed(r, entry_user->shmem_data->curr_speed.kbps, bps / 1024);
    mod_cband_status_print_speed(r, entry_user->shmem_data->curr_speed.rps,  rps);
    mod_cband_status_print_connections(r, entry_user->shmem_data->curr_speed.max_conn, entry_user->shmem_data->total_conn);

    ap_rputs("</tr>\n", r);
}

void mod_cband_status_print_virtualhost_XML_row(request_rec *r, mod_cband_virtualhost_config_entry *entry,
	int handler_type) {

    float bps, rps;
    mod_cband_class_config_entry *entry_class;
    mod_cband_scoreboard_entry *virtual_usage;
    int i;

    virtual_usage = &entry->shmem_data->total_usage;

    mod_cband_update_speed_lock(entry->shmem_data, 0, 0, -1);
    mod_cband_get_speed_lock(entry->shmem_data, &bps, &rps);
	    
    ap_rprintf(r, "\t\t<%s>\n", entry->virtual_name);
    ap_rprintf(r, "\t\t\t<port>%d</port>\n", entry->virtual_port);
    ap_rprintf(r, "\t\t\t<line>%d</line>\n", entry->virtual_defn_line);
    ap_rprintf(r, "\t\t\t<limits>\n");
 	    
    ap_rprintf(r, "\t\t\t\t<total>%lu%s</total>\n", entry->virtual_limit,
	      (entry->virtual_limit_mult == 1024 ? "KiB" : "KB"));

    entry_class = config->next_class;
    i = 0;
    while(entry_class != NULL) {
	ap_rprintf(r, "\t\t\t\t<%s>%lu%s</%s>\n", entry_class->class_name, entry->virtual_class_limit[i],
        (entry->virtual_class_limit_mult[i] == 1024 ? "KiB" : "KB"), entry_class->class_name);
	i++;
        entry_class = entry_class->next;
    }
    ap_rprintf(r, "\t\t\t\t<kbps>%lu</kbps>\n", entry->shmem_data->curr_speed.kbps);
    ap_rprintf(r, "\t\t\t\t<rps>%lu</rps>\n", entry->shmem_data->curr_speed.rps);
    ap_rprintf(r, "\t\t\t\t<connections>%lu</connections>\n", entry->shmem_data->curr_speed.max_conn);
    ap_rprintf(r, "\t\t\t</limits>\n");

    ap_rprintf(r, "\t\t\t<usages>\n");
    ap_rprintf(r, "\t\t\t\t<total>%luKiB</total>\n", (unsigned long)(virtual_usage->total_bytes / 1024));
    entry_class = config->next_class;
    i = 0;
    while(entry_class != NULL) {
        ap_rprintf(r, "\t\t\t\t<%s>%lu%s</%s>\n", entry_class->class_name,
	    (unsigned long)(virtual_usage->class_bytes[i] / entry->virtual_class_limit_mult[i]),
	    (entry->virtual_class_limit_mult[i] == 1024 ? "KiB" : "KB"), entry_class->class_name);
        i++;
        entry_class = entry_class->next;
    }
    ap_rprintf(r, "\t\t\t\t<kbps>%0.2f</kbps>\n", bps / 1024);
    ap_rprintf(r, "\t\t\t\t<rps>%0.2f</rps>\n", rps);
    ap_rprintf(r, "\t\t\t\t<connections>%lu</connections>\n", entry->shmem_data->total_conn);
    ap_rprintf(r, "\t\t\t</usages>\n");

    ap_rprintf(r, "<time_to_refresh>%s</time_to_refresh>", mod_cband_create_period(r->pool, virtual_usage->start_time, entry->refresh_time));
    
    if (entry->virtual_user)
	ap_rprintf(r, "\t\t\t<user>%s</user>\n", entry->virtual_user);
    else
    	ap_rprintf(r, "\t\t\t<user>none</user>\n");	

    if (entry->virtual_scoreboard)
	ap_rprintf(r, "\t\t\t<scoreboard>%s</scoreboard>\n", entry->virtual_scoreboard);	
    else
    	ap_rprintf(r, "\t\t\t<scoreboard>none</scoreboard>\n");	

    if (entry->virtual_limit_exceeded)
	ap_rprintf(r, "\t\t\t<limit_exceeded_URL>%s</limit_exceeded_URL>\n", entry->virtual_limit_exceeded);	
    else
    	ap_rprintf(r, "\t\t\t<limit_exceeded_URL>none</limit_exceeded_URL>\n");	

    ap_rprintf(r, "\t\t</%s>\n", entry->virtual_name);
}

void mod_cband_status_print_user_XML_row(request_rec *r, mod_cband_user_config_entry *entry_user,
	int handler_type) {
    
    float bps, rps;
    mod_cband_class_config_entry *entry_class;
    mod_cband_scoreboard_entry *user_usage;
    int i;

    user_usage = &entry_user->shmem_data->total_usage;

    mod_cband_update_speed_lock(entry_user->shmem_data, 0, 0, -1);
    mod_cband_get_speed_lock(entry_user->shmem_data, &bps, &rps);
    
    ap_rprintf(r, "\t\t<%s>\n", entry_user->user_name);	
    ap_rprintf(r, "\t\t\t<limits>\n");
    ap_rprintf(r, "\t\t\t\t<total>%lu%s</total>\n", entry_user->user_limit,
    (entry_user->user_limit_mult == 1024 ? "KiB" : "KB"));

    entry_class = config->next_class;
    i = 0;
    while(entry_class != NULL) {
	ap_rprintf(r, "\t\t\t\t<%s>%lu%s</%s>\n", entry_class->class_name, entry_user->user_class_limit[i],
	          (entry_user->user_class_limit_mult[i] == 1024 ? "KiB" : "KB"), entry_class->class_name);
	i++;
        entry_class = entry_class->next;
    }
    
    ap_rprintf(r, "\t\t\t\t<kbps>%lu</kbps>\n", entry_user->shmem_data->curr_speed.kbps);
    ap_rprintf(r, "\t\t\t\t<rps>%lu</rps>\n", entry_user->shmem_data->curr_speed.rps);
    ap_rprintf(r, "\t\t\t\t<connections>%lu</connections>\n", entry_user->shmem_data->curr_speed.max_conn);
    ap_rprintf(r, "\t\t\t</limits>\n");

    ap_rprintf(r, "\t\t\t<usages>\n");
    ap_rprintf(r, "\t\t\t\t<total>%luKiB</total>\n", (unsigned long)(user_usage->total_bytes / 1024));
    entry_class = config->next_class;
    i = 0;
    while(entry_class != NULL) {
        ap_rprintf(r, "\t\t\t\t<%s>%lu%s</%s>\n", entry_class->class_name,
	    (unsigned long)(user_usage->class_bytes[i] / entry_user->user_class_limit_mult[i]),
	    (entry_user->user_class_limit_mult[i] == 1024 ? "KiB" : "KB"), entry_class->class_name);
        i++;
        entry_class = entry_class->next;
    }
    ap_rprintf(r, "\t\t\t\t<kbps>%0.2f</kbps>\n", bps / 1024);
    ap_rprintf(r, "\t\t\t\t<rps>%0.2f</rps>\n", rps);
    ap_rprintf(r, "\t\t\t\t<connections>%lu</connections>\n", entry_user->shmem_data->total_conn);
    ap_rprintf(r, "\t\t\t</usages>\n");

    ap_rprintf(r, "<time_to_refresh>%s</time_to_refresh>", mod_cband_create_period(r->pool, user_usage->start_time, entry_user->refresh_time));

    if (entry_user->user_limit_exceeded)
	ap_rprintf(r, "\t\t\t<limit_exceeded_URL>%s</limit_exceeded_URL>\n", entry_user->user_limit_exceeded);	
    else
    	ap_rprintf(r, "\t\t\t<limit_exceeded_URL>none</limit_exceeded_URL>\n");	
	    
    if (entry_user->user_scoreboard)
	ap_rprintf(r, "\t\t\t<scoreboard>%s</scoreboard>\n", entry_user->user_scoreboard);	
    else
    	ap_rprintf(r, "\t\t\t<scoreboard>none</scoreboard>\n");	

    ap_rprintf(r, "\t\t</%s>\n", entry_user->user_name);
}

static const char mod_cband_status_handler_style[] = 
"\n<style type=\"text/css\">\n"
"body 		{ font-family: sans-serif; font-size: 0.6em; }\n"
"table		{ font-family: tachoma, helvetica, verdana, sans-serif; border: 1px solid #d0d0d0; }\n"
"tr		{ font-family: tachoma, helvetica, verdana, sans-serif; border: 1px solid #d0d0d0; }\n"
"td		{ padding-left: 0.5em; padding-right: 0.5em; }\n"
"td.refresh	{ background-color: #0de2cb; text-align: right; }\n"
"td.speed	{ background-color: #ffa1a1; text-align: left; }\n"
"td.speedc	{ background-color: #ffb1b1; text-align: left; }\n"
"td.remote_odd	{ background-color: #b4bfff; text-align: left; }\n"
"td.remote_even	{ background-color: #bfcfff; text-align: left; }\n"
"a		{ text-decoration: none; color: #606060; }\n"
"a:hover	{ text-decoration: underline; }\n"
"h1, h2		{ font-family: tachoma, helvetica, verdana, sans-serif}\n"
".small 	{ font-size: smaller; }\n"
"div.section	{ margin-top: 1.5em; margin-bottom: 0.5em; }\n"
"div.footer	{ margin-top: 2.5em; text-align: center; font-size: smaller; }\n"
"</style>\n\n";

static const char mod_cband_status_handler_foot[] = 
"\n<div class=\"footer\"><br>\n"
"<p><a href=\"http://cband.linux.pl\">mod_cband 0.9.7.5</a><br>\n"
"Copyright 2005 by <a href=\"mailto: dembol _at_ cband.linux.pl\">Lukasz Dembinski</a><br>\n"
"All rights reserved.<br>\n"
"<br><a href=\"http://validator.w3.org/check?uri=http%3A%2F%2Fdembol.nasa.pl%2Fcband-status\">[HTML 4.0 Strict]</a>\n"
"</p></div>\n";

/**
 * /cband-status handler output in HTML
 */
static int mod_cband_status_handler_HTML (request_rec *r, int handler_type)
{
    mod_cband_virtualhost_config_entry *entry = NULL, *entry_me = NULL;
    mod_cband_user_config_entry *entry_user, *entry_user_me = NULL;
    mod_cband_class_config_entry *entry_class;
    unsigned long uptime, sec;
    unsigned i;
    char *arg;
    char *val, *key;
    int refresh = -1;
    char *unit = "";
    int vhosts_number = 0, users_number = 0;
    unsigned long long traffic, total_traffic = 0;
    unsigned long total_connections = 0;
    float bps, current_speed_bps = 0;
    float rps, current_speed_rps = 0;
    struct in_addr remote_addr;
    unsigned long time_now, time_delta;
    int odd = 0, ok = 0;
    const char *odd_str;

    if (r->args != NULL) {

	arg = r->args;
	while(*arg != 0) {
	    val = ap_getword_nc(r->pool, &arg, '&');
	    
	    if (val == NULL)
		break;
	
	    key = ap_getword_nc(r->pool, &val, '=');
	
	    if (key == NULL)
		break;
		
	    apr_table_setn(r->notes, key, val);
	}
    }

    if ((arg = (char *)apr_table_get(r->notes, "refresh")) != NULL)
	refresh = atoi(arg);

    if ((arg = (char *)apr_table_get(r->notes, "unit")) != NULL) {
	if (arg[0] == 'G' || arg[0] == 'g' || arg[0] == 'M' || arg[0] == 'm' || arg[0] == 'K' || arg[0] == 'k') {
	    arg[0] = toupper(arg[0]);
	    if (arg[1] == 'I' || arg[1] == 'i') {
		arg[1] = 'i';
		arg[2] = 0;
		unit = arg;
	    } else {
		arg[1] = 0;
		unit = arg;
	    }
	}
    }
    
    if (refresh < 0)
	refresh = DEFAULT_REFRESH;
	
    if (handler_type == CBAND_HANDLER_ALL) {
	if ((arg = (char *)apr_table_get(r->notes, "reset")) != NULL) {
    	    mod_cband_reset_virtualhost(arg);
	
	    apr_table_setn(r->headers_out, "Location", apr_psprintf(r->pool, "%s?refresh=%d&unit=%s", r->uri, refresh, unit));
	    return HTTP_MOVED_PERMANENTLY;
	}

	if ((arg = (char *)apr_table_get(r->notes, "reset_user")) != NULL) {
    	    mod_cband_reset_user(arg);
	
	    apr_table_setn(r->headers_out, "Location", apr_psprintf(r->pool, "%s?refresh=%d&unit=%s", r->uri, refresh, unit));
	    return HTTP_MOVED_PERMANENTLY;
	}
    } else {
    	if ((entry_me = mod_cband_get_virtualhost_entry(r->server, r->server->module_config, 0)) != NULL) {
	    if (entry_me->virtual_user != NULL)  
		entry_user_me = mod_cband_get_user_entry(entry_me->virtual_user, r->server->module_config, 0);
	    else
		entry_user_me = NULL;
	}
    }

    sec = (unsigned long)(apr_time_now() / 1e6);
    uptime = (unsigned long)(sec - config->start_time);

    apr_table_setn(r->headers_out, "Refresh", apr_psprintf(r->pool, "%d", refresh));
    ap_set_content_type(r, "text/html");

    ap_rputs(DOCTYPE_HTML_4_0S "<html>\n<head>\n<title>mod_cband status</title>\n", r);
    ap_rputs("<meta http-equiv=\"Content-Type\" content=\"text/html;charset=utf-8\">\n", r);
    ap_rputs(mod_cband_status_handler_style, r);
    ap_rputs("</head>\n", r);
    ap_rputs("<body>\n", r);
    ap_rputs("<h1 style=\"text-align: center\"><a href=\"http://cband.linux.pl\">mod_cband</a> status page</h1>\n", r);
    ap_rprintf(r, "<h2 style=\"text-align: center\">Server uptime %s</h2>\n", mod_cband_create_time(r->pool, uptime));
    ap_rputs("<div class=\"section\">", r);
    ap_rputs("<h2 style=\"display: inline;\">Virtual hosts</h2>\n", r);
    ap_rprintf(r, "<p style=\"display: inline;\"><a href=\"?refresh=%d&amp;unit=%s\">[refresh]</a> &nbsp; <a href=\"?refresh=%d\">[human-readable]</a> <a href=\"?refresh=%d&amp;unit=G\">[GB]</a> <a href=\"?refresh=%d&amp;unit=M\">[MB]</a> <a href=\"?refresh=%d&amp;unit=K\">[KB]</a></p>\n", refresh, unit, refresh, refresh, refresh, refresh);
    ap_rputs("</div>\n", r);
    
    if (((handler_type == CBAND_HANDLER_ME) && (entry_me == NULL)) || 
        ((handler_type == CBAND_HANDLER_ALL) && (config->next_virtualhost == NULL)))
	ap_rputs("<p>There are no limits set for virtual hosts.</p>\n", r);
    else {
	ap_rputs("<table cellspacing=\"0\" cellpadding=\"0\">\n", r);
	ap_rputs("<tr>\n<td>Virtual host name</td>\n", r);

	if (handler_type == CBAND_HANDLER_ALL) {
	    ap_rputs("<td>", r);
	    ap_rprintf(r, "<a href=\"?reset=all&amp;refresh=%d&amp;unit=%s\">reset all</a></td>\n", refresh, unit);
	}
	
	ap_rputs("<td>time to refresh</td>\n", r);
	ap_rprintf(r,"<td>Total<br>Limit/Slice/Used</td>\n");
	entry_class = config->next_class;
	
	i = 0;
	while(entry_class != NULL) {
	    ap_rprintf(r,"<td>%s<br>Limit/Slice/Used</td>\n", entry_class->class_name);
	
	    entry_class = entry_class->next;
            i++;
	}
	
	for (; i < DST_CLASS; i++)
	    ap_rprintf(r,"<td>Class %d<br>Limit/Slice/Used</td>\n",i);
		
	ap_rprintf(r,"<td>kbps<br>Limit/Current</td>\n");
	ap_rprintf(r,"<td>rps<br>Limit/Current</td>\n");
	ap_rprintf(r,"<td>Connections<br>Limit/Current</td>\n");
	
	ap_rputs("<td>user</td>\n", r);
	ap_rputs("</tr>\n", r);
	
	current_speed_bps = 0;
	current_speed_rps = 0;
	if (handler_type == CBAND_HANDLER_ALL) {	
	    entry = config->next_virtualhost;
	    while(entry != NULL) {
		mod_cband_check_virtualhost_refresh(entry, sec);
	        mod_cband_status_print_virtualhost_row(r, entry, handler_type, refresh, unit, &traffic);
		total_traffic += traffic;
		total_connections += entry->shmem_data->total_conn;
		vhosts_number++;
		
		mod_cband_get_speed_lock(entry->shmem_data, &bps, &rps);
		current_speed_bps += bps;
		current_speed_rps += rps;
	    	    
		if ((entry = entry->next) == NULL)
		    break;
	    }
	} else {
	    if ((entry_user_me != NULL) && (entry_user_me->user_name != NULL)) {
	        entry = config->next_virtualhost;
		while(entry != NULL) {
		    if ((entry->virtual_user != NULL) && !strcasecmp(entry->virtual_user, entry_user_me->user_name)) {
			mod_cband_check_virtualhost_refresh(entry, sec);
		        mod_cband_status_print_virtualhost_row(r, entry, handler_type, refresh, unit, &traffic);
		    }
		    
		    if ((entry = entry->next) == NULL)
			break;
		}
	    } else	
	    if (entry_me != NULL) {
	    	mod_cband_check_virtualhost_refresh(entry_me, sec);
	        mod_cband_status_print_virtualhost_row(r, entry_me, handler_type, refresh, unit, &traffic);
	    }
	}
	
	ap_rputs("</table>\n", r);
    }

    ap_rputs("<div class=\"section\">", r);
    ap_rputs("<br><h2 style=\"display: inline;\">Users</h2>\n", r);
    ap_rprintf(r, "<p style=\"display: inline;\"><a href=\"?refresh=%d&amp;unit=%s\">[refresh]</a> &nbsp; <a href=\"?refresh=%d\">[human-readable]</a> <a href=\"?refresh=%d&amp;unit=G\">[GB]</a> <a href=\"?refresh=%d&amp;unit=M\">[MB]</a> <a href=\"?refresh=%d&amp;unit=K\">[KB]</a></p>\n", refresh, unit, refresh, refresh, refresh, refresh);
    ap_rputs("</div>\n", r);

    if (((handler_type == CBAND_HANDLER_ME) && (entry_user_me == NULL)) || 
        ((handler_type == CBAND_HANDLER_ALL) && (config->next_user == NULL)))
        ap_rputs("<p>There are no limits set for users.</p>\n", r);
    else {
	ap_rputs("<table cellspacing=\"0\" cellpadding=\"0\">\n", r);
	ap_rputs("<tr>\n<td>Username</td>\n", r);
	
	if (handler_type == CBAND_HANDLER_ALL) {
	    ap_rputs("<td>", r);
	    ap_rprintf(r, "<a href=\"?reset_user=all&amp;refresh=%d&amp;unit=%s\">reset all</a></td>\n", refresh, unit);
	}
	
	ap_rputs("<td>time to refresh</td>\n", r);    
	ap_rprintf(r,"<td>Total<br>Limit/Slice/Used</td>\n");
	entry_class = config->next_class;
	
	i = 0;
	while(entry_class != NULL) {
	    ap_rprintf(r,"<td>%s<br>Limit/Slice/Used</td>\n", entry_class->class_name);
	
	    entry_class = entry_class->next;
            i++;
	}
	
	for (; i < DST_CLASS; i++)
	    ap_rprintf(r,"<td>Class %d<br>Limit/Slice/Used</td>\n",i);

	ap_rprintf(r,"<td>kbps<br>Limit/Current</td>\n");
	ap_rprintf(r,"<td>rps<br>Limit/Current</td>\n");
	ap_rprintf(r,"<td>Connections<br>Limit/Current</td>\n");
	
	ap_rputs("</tr>\n", r);
	
	if (handler_type == CBAND_HANDLER_ALL) {	
	    entry_user = config->next_user;
	
	    while(entry_user != NULL) {
		mod_cband_check_user_refresh(entry_user, sec);
		mod_cband_status_print_user_row(r, entry_user, handler_type, refresh, unit);
		users_number++;
		
		if ((entry_user = entry_user->next) == NULL)
		    break;
	    }
	} else {
	    if (entry_user_me != NULL) {
	        mod_cband_check_user_refresh(entry_user_me, sec);
		mod_cband_status_print_user_row(r, entry_user_me, handler_type, refresh, unit);
	    }
	}
	
	ap_rputs("</table>\n", r);
    }

    ap_rputs("<table width=\"100%\" cellspacing=\"0\" cellpadding=\"0\" style=\"border: 0; margin: 0; padding: 0\">", r);
    ap_rputs("<tr>", r);
    ap_rputs("<td valign=top>", r);

    ap_rputs("<div class=\"section\">", r);
    ap_rputs("<br><h2 style=\"display: inline;\">Remote clients</h2>\n", r);
    ap_rprintf(r, "<p style=\"display: inline;\"><a href=\"?refresh=%d&amp;unit=%s\">[refresh]</a> &nbsp; <a href=\"?refresh=%d\">[human-readable]</a> <a href=\"?refresh=%d&amp;unit=G\">[GB]</a> <a href=\"?refresh=%d&amp;unit=M\">[MB]</a> <a href=\"?refresh=%d&amp;unit=K\">[KB]</a></p>\n", refresh, unit, refresh, refresh, refresh, refresh);
    ap_rputs("</div>", r);
    ap_rputs("<table width=500 cellspacing=\"0\" cellpadding=\"0\">", r);

    ap_rputs("<tr>", r);
    ap_rprintf(r, "<td>Remote IP</td>");
    ap_rprintf(r, "<td>Virtualhost</td>");
    ap_rprintf(r, "<td>Connections<br>Limit/Current</td>");
    ap_rprintf(r, "<td>Last speed/conn [kbps]</td>");
    ap_rputs("</tr>", r);

    time_now = apr_time_now();
    for (i = 0; i < MAX_REMOTE_HOSTS; i++) {
        time_delta = (time_now - config->remote_hosts.hosts[i].remote_last_time) / 1e6;
	
	if ((!config->remote_hosts.hosts[i].used) || ((time_delta > MAX_REMOTE_HOST_LIFE) && (config->remote_hosts.hosts[i].remote_conn <= 0)))
	    continue;
    
	if (handler_type != CBAND_HANDLER_ALL) {	
	    ok = 0;

	    if ((entry_user_me != NULL) && (entry_user_me->user_name != NULL)) {
	    
		entry = config->next_virtualhost;
	        while(entry != NULL) {
		
		    if ((entry->virtual_name != NULL) && (config->remote_hosts.hosts[i].virtual_name != NULL) && 
		        (entry->virtual_user != NULL) &&
			(!strcasecmp(entry->virtual_name, config->remote_hosts.hosts[i].virtual_name)) &&
			(!strcasecmp(entry_user_me->user_name, entry->virtual_user))) {
			ok = 1;
			break;
		    }
		    
		    if ((entry = entry->next) == NULL)
			break;
		}
	    } else 
	    if (!strcasecmp(r->server->server_hostname, config->remote_hosts.hosts[i].virtual_name))
		ok = 1;
	    
	    if (!ok)
		continue;
	}

	if (odd == 0) {
	    odd_str = "even";
	    odd = 1;
	} else {
	    odd_str = "odd";
	    odd = 0;
	}
	    	    
	remote_addr.s_addr = config->remote_hosts.hosts[i].remote_addr;
        ap_rputs("<tr>", r);
	ap_rprintf(r, "<td class=remote_%s>%s</td>", odd_str, inet_ntoa((struct in_addr)remote_addr));
        ap_rprintf(r, "<td class=remote_%s>%s</td>", odd_str, config->remote_hosts.hosts[i].virtual_name);
	mod_cband_status_print_connections(r, config->remote_hosts.hosts[i].remote_max_conn, config->remote_hosts.hosts[i].remote_conn);
	
	ap_rprintf(r, "<td class=remote_%s>%lu</td>", odd_str, config->remote_hosts.hosts[i].remote_kbps);
	ap_rputs("</tr>", r);
	
	if ((time_delta > (MAX_REMOTE_HOST_LIFE / 3)) && (config->remote_hosts.hosts[i].remote_conn <= 0))
	    mod_cband_set_remote_current_speed(i, 0);

    }
    ap_rputs("</table>", r);
    ap_rputs("</td>", r);

    ap_rputs("<td valign=top>", r);
    if (handler_type == CBAND_HANDLER_ALL) {	
        ap_rputs("<div class=\"section\">", r);
        ap_rputs("<br><h2 style=\"display: inline;\">Server summary</h2>\n", r);
	ap_rprintf(r, "<p style=\"display: inline;\"><a href=\"?refresh=%d&amp;unit=%s\">[refresh]</a> &nbsp; <a href=\"?refresh=%d\">[human-readable]</a> <a href=\"?refresh=%d&amp;unit=G\">[GB]</a> <a href=\"?refresh=%d&amp;unit=M\">[MB]</a> <a href=\"?refresh=%d&amp;unit=K\">[KB]</a></p>\n", refresh, unit, refresh, refresh, refresh, refresh);
	ap_rputs("</div>", r);
	ap_rputs("<table width=400>", r);

        ap_rputs("<tr>", r);
	ap_rputs("<td>Server uptime</td>", r);
        ap_rprintf(r, "<td>%s</td>", mod_cband_create_time(r->pool, uptime));
	ap_rputs("</tr>", r);

        ap_rputs("<tr>", r);
	ap_rputs("<td>Total virtualhosts</td>", r);
        ap_rprintf(r, "<td>%d</td>", vhosts_number);
	ap_rputs("</tr>", r);

        ap_rputs("<tr>", r);
	ap_rputs("<td>Total users</td>", r);
        ap_rprintf(r, "<td>%d</td>", users_number);
	ap_rputs("</tr>", r);

        ap_rputs("<tr>", r);
	ap_rputs("<td>Total connections</td>", r);
        ap_rprintf(r, "<td>%lu</td>", total_connections);
	ap_rputs("</tr>", r);

        ap_rputs("<tr>", r);
	ap_rputs("<td>Total traffic</td>", r);
        ap_rprintf(r, "<td>%s</td>", mod_cband_create_traffic_size(r->pool, total_traffic / 1000, unit, 1000));
	ap_rputs("</tr>", r);

        ap_rputs("<tr>", r);
	ap_rputs("<td>Current speed</td>", r);
        ap_rprintf(r, "<td>%0.2f kbps / %0.2f rps</td>", current_speed_bps / 1024, current_speed_rps);
	ap_rputs("</tr>", r);
        ap_rputs("</table>", r);
    }

    ap_rputs("</td>", r);
    ap_rputs("</tr>", r);
    ap_rputs("</table>", r);

    ap_rputs(mod_cband_status_handler_foot, r);
    ap_rputs("</body>\n</html>\n", r);
    
    return OK;
}

/**
 * /cband-status handler output in XML
 */
static int mod_cband_status_handler_XML (request_rec *r, int handler_type)
{
    mod_cband_virtualhost_config_entry *entry, *entry_me = NULL;
    mod_cband_user_config_entry *entry_user, *entry_user_me = NULL;
    unsigned long sec, uptime;

    if (handler_type == CBAND_HANDLER_ME) {
    	if ((entry_me = mod_cband_get_virtualhost_entry(r->server, r->server->module_config, 0)) != NULL) {
	    if (entry_me->virtual_user != NULL)  
	       entry_user_me = mod_cband_get_user_entry(entry_me->virtual_user, r->server->module_config, 0);
	}
    }

    sec = (unsigned long)(apr_time_now() / 1e6);
    uptime = (unsigned long)(sec - config->start_time);

    ap_set_content_type(r, "text/xml");

    ap_rputs("<?xml version=\"1.0\" encoding=\"iso-8859-1\"?>\n", r);
    ap_rputs("<mod_cband>\n", r);
    ap_rputs("\t<Server>\n", r);
    ap_rprintf(r, "\t\t<uptime>%s</uptime>\n", mod_cband_create_time(r->pool, uptime));    
    ap_rputs("\t</Server>\n", r);
    
    ap_rputs("\t<Virtualhosts>\n", r);
    if (handler_type == CBAND_HANDLER_ALL) {	
        entry = config->next_virtualhost;
    
	while(entry != NULL) {
    	    mod_cband_check_virtualhost_refresh(entry, sec);
	    mod_cband_status_print_virtualhost_XML_row(r, entry, handler_type);
	    	    
	    if ((entry = entry->next) == NULL)
		break;
	}
    } else {
    	if ((entry_user_me != NULL) && (entry_user_me->user_name != NULL)) {
	    entry = config->next_virtualhost;
	    while(entry != NULL) {
		if ((entry->virtual_user != NULL) && !strcasecmp(entry->virtual_user, entry_user_me->user_name)) {
		    mod_cband_check_virtualhost_refresh(entry, sec);
		    mod_cband_status_print_virtualhost_XML_row(r, entry, handler_type);
		}
		    
		if ((entry = entry->next) == NULL)
		    break;
	    }
	} else	
	if (entry_me != NULL) {
	    mod_cband_check_virtualhost_refresh(entry_me, sec);
	    mod_cband_status_print_virtualhost_XML_row(r, entry_me, handler_type);
	}
    }
	
    ap_rputs("\t</Virtualhosts>\n", r);
    ap_rputs("\t<Users>\n", r);

    if (handler_type == CBAND_HANDLER_ALL) {	
        entry_user = config->next_user;
	
	while(entry_user != NULL) {
	    mod_cband_check_user_refresh(entry_user, sec);
	    mod_cband_status_print_user_XML_row(r, entry_user, handler_type);
	    
	    if ((entry_user = entry_user->next) == NULL)
		break;
	}
    } else {
	if (entry_user_me != NULL) {
	    mod_cband_check_user_refresh(entry_user_me, sec);
	    mod_cband_status_print_user_XML_row(r, entry_user_me, handler_type);
	}
    }
	
    ap_rputs("\t</Users>\n", r);
    ap_rputs("</mod_cband>", r);

    return OK;
}

int mod_cband_check_connections_speed(mod_cband_virtualhost_config_entry *entry, mod_cband_user_config_entry *entry_user, request_rec *r, int dst)
{
    float virtualhost_rps, virtualhost_curr_rps;
    float user_rps, user_curr_rps;
    float remote_rps;
    unsigned long max_remote_kbps, remote_curr_rps, remote_max_conn, remote_total_conn;
    int remote_idx;
    unsigned long time_now;
    int loops;
    int overlimit;

    remote_idx = mod_cband_get_remote_host(r->connection, 1, entry);
    mod_cband_get_dst_speed_lock(entry, entry_user, &max_remote_kbps, &remote_curr_rps, &remote_max_conn, dst);
    mod_cband_set_remote_max_connections(remote_idx, remote_max_conn);

    virtualhost_curr_rps   = 0;
    user_curr_rps          = 0;
    virtualhost_rps        = 0;
    user_rps               = 0;
    remote_rps             = 0;
    time_now               = apr_time_now();

    loops = 0;
    do {
        /* BEGIN CRITICAL SECTION */
	mod_cband_sem_down(config->sem_id);
    
        if (entry != NULL) {
	
	    mod_cband_update_speed(entry->shmem_data, 0, 0, remote_idx);            
	    if ((entry->shmem_data->curr_speed.max_conn > 0) && 
		(entry->shmem_data->total_conn >= entry->shmem_data->curr_speed.max_conn)) {
		    
		    mod_cband_sem_up(config->sem_id);
		    /* END CRITICAL SECTION */

		    return HTTP_SERVICE_UNAVAILABLE;
		}
	    
            mod_cband_get_real_speed(entry->shmem_data, NULL, &virtualhost_rps);
	    virtualhost_curr_rps = entry->shmem_data->curr_speed.rps;
	}
		
        if (entry_user != NULL) {

	    mod_cband_update_speed(entry_user->shmem_data, 0, 0, remote_idx);            
	    if ((entry_user->shmem_data->curr_speed.max_conn > 0) && 
		(entry_user->shmem_data->total_conn >= entry_user->shmem_data->curr_speed.max_conn)) {
		
		mod_cband_sem_up(config->sem_id);
		/* END CRITICAL SECTION */

		return HTTP_SERVICE_UNAVAILABLE;
	    }

	    mod_cband_get_real_speed(entry_user->shmem_data, NULL, &user_rps);
    	    user_curr_rps = entry_user->shmem_data->curr_speed.rps;
	}

	if (remote_idx >= 0) {
	    if (remote_max_conn > 0) {
		remote_total_conn = mod_cband_get_remote_total_connections(remote_idx);
	    
		if ((remote_total_conn > 0) && (remote_max_conn <= remote_total_conn)) {
		
		    mod_cband_sem_up(config->sem_id);
		    /* END CRITICAL SECTION */

		    return HTTP_SERVICE_UNAVAILABLE;
		}
	    } 
			
	    /* semafor na remote_hosts */
	    remote_rps = mod_cband_get_remote_connections_speed_lock(remote_idx);
	}

	overlimit = 0;
	if ((entry != NULL) && (virtualhost_curr_rps > 0) && (virtualhost_rps > virtualhost_curr_rps))
	    overlimit = 1;

	if ((entry_user != NULL) && (user_curr_rps > 0) && (user_rps > user_curr_rps))
	    overlimit = 1;

	if ((remote_idx >= 0) && (remote_curr_rps > 0) && (remote_rps > remote_curr_rps))
	    overlimit = 1;

	if (overlimit) {
	    mod_cband_sem_up(config->sem_id);
	    /* END CRITICAL SECTION */
	    
	    usleep(MAX_SLEEP_TIME + (rand() % MAX_SLEEP_TIME));
	}
        
	mod_cband_sem_up(config->sem_id);
	/* END CRITICAL SECTION */

	loops++;
    } while (overlimit && loops <= MAX_DELAY_LOOPS);

    if (loops > MAX_DELAY_LOOPS)
	return HTTP_SERVICE_UNAVAILABLE;
	
    return OK;
}

/*
 * /cband-status handler
 */
static int mod_cband_status_handler (request_rec *r)
{
    mod_cband_virtualhost_config_entry *entry = NULL;
    mod_cband_user_config_entry *entry_user = NULL;
    unsigned long remote_max_conn;
    int handler_type;
    int remote_idx, dst;

    if (strcmp(r->handler, "cband-status") && strcmp(r->handler, "cband-status-me"))
	return DECLINED;

    entry = mod_cband_get_virtualhost_entry(r->server, r->server->module_config, 0);

    if ((entry != NULL) && (entry->virtual_user != NULL)) 
	entry_user = mod_cband_get_user_entry(entry->virtual_user, r->server->module_config, 0);

    dst = mod_cband_get_dst(r);
    remote_idx = mod_cband_get_remote_host(r->connection, 1, entry);
    mod_cband_get_dst_speed_lock(entry, entry_user, NULL, NULL, &remote_max_conn, dst);
    mod_cband_set_remote_max_connections(remote_idx, remote_max_conn);

    ap_add_output_filter("mod_cband", NULL, r, r->connection);

    if (!strcmp(r->handler, "cband-status"))
	handler_type = CBAND_HANDLER_ALL;
    else
    	handler_type = CBAND_HANDLER_ME;
	
    if ((r->args != NULL) && (!strcasecmp(r->args, "xml")))
	return mod_cband_status_handler_XML(r, handler_type);
    else
	return mod_cband_status_handler_HTML(r, handler_type);
}

int mod_cband_check_limit(request_rec *r, mod_cband_shmem_data *shmem_data, unsigned long limit, unsigned long slice_limit, unsigned int mult, unsigned long long usage, char *limit_exceeded)
{
    /* Check if the bandwidth limit has been reached */
    if ((limit > 0) && ((((unsigned long long)limit * (unsigned long long)mult) < usage) || 
			(((unsigned long long)slice_limit * (unsigned long long)mult) < usage))) {
			
	if (limit_exceeded != NULL) {
	    apr_table_setn(r->headers_out, "Location", limit_exceeded);
	    return HTTP_MOVED_PERMANENTLY;
	}
	else
	if ((shmem_data->over_speed.kbps > (unsigned long)0) || (shmem_data->over_speed.rps > (unsigned long)0))
	    mod_cband_set_overlimit_speed_lock(shmem_data);
	else
	if (config->default_limit_exceeded != NULL) {
	    apr_table_setn(r->headers_out, "Location", config->default_limit_exceeded);
	    return HTTP_MOVED_PERMANENTLY;
	} else
	    return config->default_limit_exceeded_code;
    }
    
    return OK;
}

int mod_cband_get_virtualhost_limits(mod_cband_virtualhost_config_entry *entry, mod_cband_limits_usages *lu, int dst)
{
    if (entry == NULL || lu == NULL)
	return -1;

    lu->limit       = entry->virtual_limit;
    lu->limit_mult  = entry->virtual_limit_mult;
    lu->slice_limit = mod_cband_get_slice_limit(entry->shmem_data->total_usage.start_time, 
                      entry->refresh_time, entry->slice_len, entry->virtual_limit);
    lu->limit_exceeded = entry->virtual_limit_exceeded;
    lu->scoreboard  = entry->virtual_scoreboard;
    
    if (dst >= 0) {
	lu->class_limit        = entry->virtual_class_limit[dst];
	lu->class_limit_mult   = entry->virtual_class_limit_mult[dst];
	lu->class_slice_limit  = mod_cband_get_slice_limit(entry->shmem_data->total_usage.start_time, 
                                 entry->refresh_time, entry->slice_len, entry->virtual_class_limit[dst]);
    }

    return 0;
}

/*
 * semafor opuszczany przez funkcje mod_cband_request_handler
 */
int mod_cband_get_virtualhost_usages(request_rec *r, mod_cband_virtualhost_config_entry *entry, mod_cband_limits_usages *lu, int dst)
{
    if (entry == NULL || lu == NULL)
	return -1;

    mod_cband_get_score(r->server, entry->virtual_scoreboard, &lu->usage, -1, entry->shmem_data);
    
    if (dst >= 0) 
	mod_cband_get_score(r->server, lu->scoreboard, &lu->class_usage, dst, entry->shmem_data);

    return 0;
}

int mod_cband_get_user_limits(mod_cband_user_config_entry *entry_user, mod_cband_limits_usages *lu, int dst)
{
    if (entry_user == NULL || lu == NULL)
	return -1;

    lu->limit          = entry_user->user_limit;
    lu->limit_mult     = entry_user->user_limit_mult;
    lu->limit_exceeded = entry_user->user_limit_exceeded;
    lu->slice_limit    = mod_cband_get_slice_limit(entry_user->shmem_data->total_usage.start_time, 
                         entry_user->refresh_time, entry_user->slice_len, entry_user->user_limit);
    lu->scoreboard     = entry_user->user_scoreboard;
    
    if (dst >= 0) {
        lu->class_limit       = entry_user->user_class_limit[dst];
        lu->class_limit_mult  = entry_user->user_class_limit_mult[dst];
        lu->class_slice_limit = mod_cband_get_slice_limit(entry_user->shmem_data->total_usage.start_time, 
	                        entry_user->refresh_time, entry_user->slice_len, entry_user->user_class_limit[dst]);
    }
    
    return 0;
}

/*
 * semafor opuszczany przez funkcje mod_cband_request_handler
 */
int mod_cband_get_user_usages(request_rec *r, mod_cband_user_config_entry *entry_user, mod_cband_limits_usages *lu, int dst)
{
    if (entry_user == NULL || lu == NULL)
	return -1;

    mod_cband_get_score(r->server, lu->scoreboard, &lu->usage, -1, entry_user->shmem_data);

    if (dst >= 0)
	mod_cband_get_score(r->server, lu->scoreboard, &lu->class_usage, dst, entry_user->shmem_data);
	
    return 0;
}

int mod_cband_check_limits(request_rec *r, mod_cband_shmem_data *shmem_data, mod_cband_limits_usages *lu, int dst)
{
    int ret;

    if (shmem_data == NULL || lu == NULL)
	return OK;

    if ((lu->usage == 0) && (lu->class_usage == 0))
	return OK;

    if ((lu->limit == 0) && (lu->class_limit == 0))
        return OK;

    if ((ret = mod_cband_check_limit(r, shmem_data, lu->limit, lu->slice_limit, lu->limit_mult, lu->usage, lu->limit_exceeded)) != OK)
	return ret;

    if ((ret = mod_cband_check_limit(r, shmem_data, lu->class_limit, lu->class_slice_limit, lu->class_limit_mult, lu->class_usage, lu->limit_exceeded)) != OK)
	return ret;

    return OK;
}

/**
 * cband request handler
 * check bandwidth usage for virtualhosts. If it's exceeded, redirect to the specified URL
 */
static int mod_cband_request_handler (request_rec *r)
{
    mod_cband_virtualhost_config_entry *entry = NULL;
    mod_cband_user_config_entry *entry_user = NULL;
    mod_cband_limits_usages virtual_lu;
    mod_cband_limits_usages user_lu;
    unsigned long time_now;
    int dst = -1;
    mod_cband_shmem_data *shmem_data = NULL;
    int ret;

    if (r->main || (r->method_number != M_GET) || (r->status >= 300))
	return DECLINED;

    if ((entry = mod_cband_get_virtualhost_entry(r->server, r->server->module_config, 0)) == NULL)
	return DECLINED;

    memset(&virtual_lu, 0, sizeof(mod_cband_limits_usages));
    memset(&user_lu, 0, sizeof(mod_cband_limits_usages));

    shmem_data = entry->shmem_data;
    shmem_data->total_usage.was_request = 1;

    time_now = (unsigned long)(apr_time_now() / 1e6);
    dst = mod_cband_get_dst(r);

    mod_cband_get_virtualhost_limits(entry, &virtual_lu, dst);
    mod_cband_check_virtualhost_refresh(entry, time_now);
    
    if ((entry->virtual_user != NULL) && ((entry_user = mod_cband_get_user_entry(entry->virtual_user, r->server->module_config, 0)) != NULL)) {
        mod_cband_get_user_limits(entry_user, &user_lu, dst);
	mod_cband_check_user_refresh(entry_user, time_now);
    }

    if ((ret = mod_cband_check_connections_speed(entry, entry_user, r, dst)) != OK)
	return ret;

    ap_add_output_filter("mod_cband", NULL, r, r->connection);

    if (!strcmp(r->handler, "cband-status") || !strcmp(r->handler, "cband-status-me"))
	return DECLINED;

    /* BEGIN CRITICAL SECTION */
    mod_cband_sem_down(config->sem_id);
    mod_cband_get_virtualhost_usages(r, entry, &virtual_lu, dst);
    mod_cband_get_user_usages(r, entry_user, &user_lu, dst);
    mod_cband_sem_up(config->sem_id);
    /* END CRITICAL SECTION */

    if ((entry != NULL) && ((ret = mod_cband_check_limits(r, entry->shmem_data, &virtual_lu, dst)) != OK))
        return ret;

    if ((entry_user != NULL) && ((ret = mod_cband_check_limits(r, entry_user->shmem_data, &user_lu, dst)) != OK))
        return ret;
        
    return DECLINED;
}

float mod_cband_get_shared_speed_lock(mod_cband_virtualhost_config_entry *entry, mod_cband_user_config_entry *entry_user)
{
    float next_user_bps = 0, next_virtualhost_bps = 0;

    if (entry == NULL)
        return -1;

    if ((entry->shmem_data->curr_speed.kbps <= 0) && 
	((entry_user == NULL) || (entry_user->shmem_data->curr_speed.kbps <= 0)))
	return -1;

    /* BEGIN CRITICAL SECTION */
    mod_cband_sem_down(config->sem_id);

    next_user_bps = 0;
    next_virtualhost_bps = entry->shmem_data->shared_kbps * 1024;

    if (entry_user != NULL) {
	next_user_bps = entry_user->shmem_data->shared_kbps * 1024;
	if (entry_user->shmem_data->shared_connections > 0)
    	    next_user_bps /= (entry_user->shmem_data->shared_connections + 1);
    }

    if (entry->shmem_data->shared_connections > 0)
        next_virtualhost_bps /= (entry->shmem_data->shared_connections + 1);

    mod_cband_sem_up(config->sem_id);
    /* END CRITICAL SECTION */

    if ((next_user_bps > 0) && (next_virtualhost_bps > next_user_bps))
	return next_user_bps;
    else
    if (next_virtualhost_bps > 0)
	return next_virtualhost_bps;
    else
	return next_user_bps;
}

int mod_cband_log_bucket(request_rec *r, mod_cband_virtualhost_config_entry *entry, 
		    mod_cband_user_config_entry *entry_user, unsigned long bucket_bytes, int remote_idx)
{
    unsigned long long bytes;
    int dst = -1;
    
    bytes = (unsigned long long)bucket_bytes;

    if (r->method_number != M_GET)
	return 0;

    if (entry == NULL)
        return 0;

    dst = mod_cband_get_dst(r);

    /* BEGIN CRITICAL SECTION */
    mod_cband_sem_down(config->sem_id);
    mod_cband_update_speed(entry->shmem_data, bucket_bytes, 0, remote_idx);            
    mod_cband_update_score(entry->virtual_scoreboard, &bytes, dst, &(entry->shmem_data->total_usage));
    	
    if (entry_user != NULL) {
        mod_cband_update_speed(entry_user->shmem_data, bucket_bytes, 0, remote_idx);            
	mod_cband_update_score(entry_user->user_scoreboard, &bytes, dst, &(entry_user->shmem_data->total_usage));
    }
    
    mod_cband_sem_up(config->sem_id);
    /* END CRITICAL SECTION */
    
    return 0;
}

void mod_cband_change_total_connections_lock(mod_cband_virtualhost_config_entry *entry, mod_cband_user_config_entry *entry_user, int diff)
{
    /* BEGIN CRITICAL SECTION */
    mod_cband_sem_down(config->sem_id);

    if ((entry != NULL) && (entry->shmem_data != NULL))
        mod_cband_safe_change(&entry->shmem_data->total_conn, diff);

    if ((entry_user != NULL) && (entry_user->shmem_data != NULL))
        mod_cband_safe_change(&entry_user->shmem_data->total_conn, diff);
    
    mod_cband_sem_up(config->sem_id);
    /* END CRITICAL SECTION */
}

void mod_cband_change_shared_connections_lock(mod_cband_virtualhost_config_entry *entry, mod_cband_user_config_entry *entry_user, int diff)
{
    /* BEGIN CRITICAL SECTION */
    mod_cband_sem_down(config->sem_id);

    if (entry != NULL)
        mod_cband_safe_change(&entry->shmem_data->shared_connections, diff);

    if (entry_user != NULL)
        mod_cband_safe_change(&entry_user->shmem_data->shared_connections, diff);

    mod_cband_sem_up(config->sem_id);
    /* END CRITICAL SECTION */
}

void mod_cband_change_shared_speed_lock(mod_cband_virtualhost_config_entry *entry, mod_cband_user_config_entry *entry_user, int diff)
{
    /* BEGIN CRITICAL SECTION */
    mod_cband_sem_down(config->sem_id);

    if (entry != NULL) {
        mod_cband_safe_change(&entry->shmem_data->shared_kbps, diff);
	if (entry->shmem_data->overlimit && (entry->shmem_data->shared_kbps > entry->shmem_data->over_speed.kbps))
	    mod_cband_set_overlimit_speed(entry->shmem_data);
	else
	if (!entry->shmem_data->overlimit && (entry->shmem_data->shared_kbps > entry->shmem_data->max_speed.kbps))
	    mod_cband_set_normal_speed(entry->shmem_data);
    }

    if (entry_user != NULL) {
        mod_cband_safe_change(&entry_user->shmem_data->shared_kbps, diff);
	if (entry_user->shmem_data->overlimit && (entry_user->shmem_data->shared_kbps > entry_user->shmem_data->over_speed.kbps))
	    mod_cband_set_overlimit_speed(entry_user->shmem_data);
	else
	if (!entry_user->shmem_data->overlimit && (entry_user->shmem_data->shared_kbps > entry_user->shmem_data->max_speed.kbps))
	    mod_cband_set_normal_speed(entry_user->shmem_data);
    }

    mod_cband_sem_up(config->sem_id);
    /* END CRITICAL SECTION */
}

static int mod_cband_filter(ap_filter_t *f, apr_bucket_brigade *bb)
{
    mod_cband_virtualhost_config_entry *entry = NULL;
    mod_cband_user_config_entry *entry_user = NULL;
    apr_bucket *b = APR_BRIGADE_FIRST(bb);
    apr_bucket_brigade *bbOut;
    const char *buf;
    int bytes;
    int bytes_split;
    apr_size_t bytes_bucket;
    float next_bps, shared_bps, remote_bps, measured_bps, measured_bps_old;
    unsigned long max_remote_kbps, remote_rps, remote_connections;
    int remote_kbps;
    int shared_case = 0;
    int slow_remote = 0;
    int dst;
    int remote_idx = -1;
    unsigned long sleep_time, diff_time;
    int not_limit = 0;
    float div;
    unsigned long remote_bytes_in_second;
    unsigned long t1, t2, t1m, t2m;
    unsigned long remote_bytes_sum = 0;
    conn_rec *c = f->r->connection;

    if (f->r->main || (f->r->method_number != M_GET)) {
	ap_remove_output_filter(f);
	ap_pass_brigade(f->next, bb);
	return APR_SUCCESS;
    }
    
    bbOut = apr_brigade_create(f->r->pool, c->bucket_alloc);
    
    if ((entry = mod_cband_get_virtualhost_entry(f->r->server, f->r->server->module_config, 0)) != NULL) {
        mod_cband_flush_score_lock(entry->virtual_scoreboard, &(entry->shmem_data->total_usage));
	remote_idx = mod_cband_get_remote_host(f->r->connection, 1, entry);
        mod_cband_update_speed_lock(entry->shmem_data, 0, 1, remote_idx);            
    }

    dst = mod_cband_get_dst(f->r);

    if ((entry != NULL) && (entry->virtual_user != NULL) && ((entry_user = mod_cband_get_user_entry(entry->virtual_user, f->r->server->module_config, 0)) != NULL)) {
    	mod_cband_flush_score_lock(entry_user->user_scoreboard, &(entry_user->shmem_data->total_usage));
    	mod_cband_update_speed_lock(entry_user->shmem_data, 0, 1, remote_idx);            
    }

    mod_cband_get_dst_speed_lock(entry, entry_user, &max_remote_kbps, &remote_rps, NULL, dst);

    not_limit = 0;
    if ((mod_cband_get_shared_speed_lock(entry, entry_user) < 0) && (max_remote_kbps == 0))
	not_limit = 1;
	
    mod_cband_change_total_connections_lock(entry, entry_user, 1);
    mod_cband_change_remote_connections_lock(remote_idx, 1);

    /* 
     * Fairness Bandwidth Sharing algorithm 
     */
    while(b != APR_BRIGADE_SENTINEL(bb)) {
	if (f->r->connection->aborted) {
	    mod_cband_change_total_connections_lock(entry, entry_user, -1);
	    mod_cband_change_remote_connections_lock(remote_idx, -1);
	    return APR_SUCCESS;
	}
    
	if (APR_BUCKET_IS_EOS(b) || APR_BUCKET_IS_FLUSH(b)) {
	    APR_BUCKET_REMOVE(b);
	    APR_BRIGADE_INSERT_TAIL(bbOut, b);
	    ap_pass_brigade(f->next, bbOut);
	    mod_cband_change_total_connections_lock(entry, entry_user, -1);
	    mod_cband_change_remote_connections_lock(remote_idx, -1);
	    return APR_SUCCESS;
	}

	measured_bps = 0;	
	measured_bps_old = 0;
	t1 = t2 = apr_time_now();
	if (apr_bucket_read(b, &buf, &bytes_bucket, APR_NONBLOCK_READ) == APR_SUCCESS) {

	    bytes = (int)bytes_bucket;
	    while(bytes > 0) {
		mod_cband_set_remote_request_time(remote_idx, apr_time_now());
    		
		if (!not_limit) {
		    shared_bps = mod_cband_get_shared_speed_lock(entry, entry_user);
		    remote_bps = (float)(max_remote_kbps * 1024);
		    remote_connections = mod_cband_get_remote_connections(remote_idx);
		
		    if (remote_connections > 0)
			remote_bps /= remote_connections;

		    if (shared_bps < 0)
			shared_bps = 0;

		    if (config->random_pulse)
			sleep_time = ((MAX_PULSE_LEN / 2) + (rand() % (MAX_PULSE_LEN / 2))) * ((rand() % (MAX_PULSES)) + 1);
		    else
			sleep_time = MAX_PULSE_LEN * MAX_PULSES;

		    if ((measured_bps) > 0 && ((remote_bps > measured_bps) || (shared_bps > measured_bps))) {
			slow_remote = MAX_SLOW_REMOTE_LOOPS;
			measured_bps_old = measured_bps;
		    }
	
		    if (slow_remote > 0) {
			remote_bps = measured_bps_old;
			slow_remote--;
		    }
		    
		    remote_kbps = (remote_bps / 1024);
		    next_bps    = remote_bps;	    

		    shared_case = 0;
		    if (((shared_bps > 0) && (shared_bps < remote_bps)) || (remote_bps <= 0)) {
			next_bps    = shared_bps;
			shared_case = 1;
			mod_cband_change_shared_connections_lock(entry, entry_user, 1);
		    } else
			mod_cband_change_shared_speed_lock(entry, entry_user, -remote_kbps);

		    if (next_bps <= MIN_SPEED)
			next_bps = MIN_SPEED;

		    next_bps = (next_bps * sleep_time) / (CONST_PULSE_LEN);
		    bytes_split = (int)(next_bps / 8);
		} else {
		    next_bps = 0;
		    remote_kbps = 0;
		    sleep_time  = 0;
		    bytes_split = MAX_CHUNK_LEN;
		    
		    if (bytes_split > bytes)
			bytes_split = bytes;
		}

		/* 
		 * jezeli mamy mniej do przeslania niz bytes_split bajtow w sekundzie
		 * to wysylamy wszystko, ale czekamy tylko t = ile_bajtow/speed zeby male dokumenty
		 * albo ich koncowki tez byly transportowane z zadana predkoscia
		 */
		if (!not_limit && (bytes_split > bytes)) {
		    if (bytes_split > 0)
		        sleep_time = (unsigned long)((float)((float)bytes / bytes_split) * 1e6);
		    else
		        sleep_time = 0;
		
		    bytes_split = bytes;
		}

		if (bytes_split > MAX_CHUNK_LEN) {
		    div = (float)bytes_split / MAX_CHUNK_LEN;
		    if (div > 0)
			sleep_time /= div;
			
		    bytes_split = MAX_CHUNK_LEN;
		}
	    			
		apr_bucket_split(b, bytes_split);
		APR_BUCKET_REMOVE(b);
		APR_BRIGADE_INSERT_TAIL(bbOut, b);
		bytes -= bytes_split;
		
		t1m = apr_time_now();
		ap_pass_brigade(f->next, bbOut);
		t2m = apr_time_now();

	    	b = APR_BRIGADE_FIRST(bb);
		mod_cband_log_bucket(f->r, entry, entry_user, (unsigned long)bytes_split, remote_idx);

		remote_bytes_sum += bytes_split;		
		t2 = apr_time_now();

	        if (t2 > t1 + 1e6) {
		    div = (float)(t2 - t1) / 1e6;
		    
		    if (div > 0)
			remote_bytes_in_second = remote_bytes_sum / div; 
		    else
			remote_bytes_in_second = remote_bytes_sum;
			
		    mod_cband_set_remote_current_speed(remote_idx, (remote_bytes_in_second * 8) / 1024);
		    t1 = apr_time_now();
		    remote_bytes_sum = 0;
		}

		if (!not_limit) {
		    if ((diff_time = (t2m - t1m)) > 0)
			measured_bps = ((bytes_split * 8) / diff_time) * 1e6;
		    else
			measured_bps = next_bps;

		    usleep(sleep_time);

		    if (shared_case)
			mod_cband_change_shared_connections_lock(entry, entry_user, -1);
		    else
			mod_cband_change_shared_speed_lock(entry, entry_user, remote_kbps);
		}

		if (f->r->connection->aborted) {
	    	    mod_cband_change_total_connections_lock(entry, entry_user, -1);
	            mod_cband_change_remote_connections_lock(remote_idx, -1);
		    return APR_SUCCESS;
		}
	    }
	}
	
	APR_BUCKET_REMOVE(b);
	APR_BRIGADE_INSERT_TAIL(bbOut, b);
	b = APR_BRIGADE_FIRST(bb);
	ap_pass_brigade(f->next, bbOut);
    }

    mod_cband_change_total_connections_lock(entry, entry_user, -1);
    mod_cband_change_remote_connections_lock(remote_idx, -1);
    
    return APR_SUCCESS;
}

static apr_status_t mod_cband_cleanup1(void *s)
{
    int i;

    /* BEGIN CRITICAL SECTION */
    mod_cband_sem_down(config->sem_id);
    mod_cband_save_score_cache();
    mod_cband_sem_up(config->sem_id);
    /* END CRITICAL SECTION */

    for (i = 0; i <= config->shmem_seg_idx; i++)
	mod_cband_shmem_remove(config->shmem_seg[i].shmem_id);

    mod_cband_shmem_remove(config->remote_hosts.shmem_id);
    mod_cband_sem_remove(config->remote_hosts.sem_id);
    mod_cband_sem_remove(config->sem_id);
    
    return APR_SUCCESS;
}

static apr_status_t mod_cband_cleanup2(void *s)
{
    return APR_SUCCESS;
}

static apr_status_t mod_cband_post_config(apr_pool_t *p, apr_pool_t *plog, apr_pool_t *ptmp, 
					  server_rec *s)
{
    /* BEGIN CRITICAL SECTION */
    mod_cband_sem_down(config->sem_id);
    mod_cband_update_score_cache(s);
    mod_cband_sem_up(config->sem_id);
    /* END CRITICAL SECTION */

    return OK;
}

/**
 * register mod_cband hooks 
 */
static void mod_cband_register_hooks (apr_pool_t *p)
{
    ap_hook_handler(mod_cband_status_handler, NULL, NULL, APR_HOOK_FIRST);
    ap_hook_handler(mod_cband_request_handler, NULL, NULL, APR_HOOK_FIRST);
    apr_pool_cleanup_register(p, NULL, mod_cband_cleanup1, mod_cband_cleanup2);
    ap_hook_post_config(mod_cband_post_config, NULL, NULL, APR_HOOK_MIDDLE);
    ap_register_output_filter("mod_cband", mod_cband_filter, NULL, AP_FTYPE_TRANSCODE);
}

/**
 * allocate config_header for mod_cband - this will store module 
 * settings, as read from config file
 */
static void *mod_cband_create_config(apr_pool_t *p, server_rec *s)
{
    if (config == NULL) {
	config = (mod_cband_config_header *) apr_palloc(p, sizeof(mod_cband_config_header));
	config->next_virtualhost = NULL;
	config->next_user = NULL;
	config->next_class = NULL;
	config->default_limit_exceeded = NULL;
	config->p = p;
	config->tree = NULL;
	config->start_time = (unsigned long)(apr_time_now() / 1e6);
	config->score_flush_period = 0;
	config->sem_id = semget(IPC_PRIVATE, 1, IPC_CREAT | 0666);
	config->shmem_seg_idx = -1;
	config->default_limit_exceeded_code = HTTP_SERVICE_UNAVAILABLE;
	config->max_chunk_len = MAX_CHUNK_LEN;
	
	mod_cband_remote_hosts_init();
	mod_cband_sem_init(config->sem_id);
	mod_cband_shmem_init();
    } 
    
    return (void *)config;
}

module AP_MODULE_DECLARE_DATA cband_module =
{
	STANDARD20_MODULE_STUFF,
	NULL,
	NULL,
	mod_cband_create_config,
	NULL,
	mod_cband_cmds,
	mod_cband_register_hooks
};
