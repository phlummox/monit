/*
 * Copyright (C) Tildeslash Ltd. All rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * In addition, as a special exception, the copyright holders give
 * permission to link the code of portions of this program with the
 * OpenSSL library under certain conditions as described in each
 * individual source file, and distribute linked combinations
 * including the two.
 *
 * You must obey the GNU Affero General Public License in all respects
 * for all of the code used other than OpenSSL.
 */

#include "config.h"

#ifdef HAVE_STDIO_H
#include <stdio.h>
#endif

#ifdef HAVE_STDARG_H
#include <stdarg.h>
#endif

#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif

#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif

#ifdef HAVE_SIGNAL_H
#include <signal.h>
#endif

#ifdef HAVE_SETJMP_H
#include <setjmp.h>
#endif

#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif

#ifdef HAVE_STRING_H
#include <string.h>
#endif

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif

#ifdef HAVE_TIME_H
#include <time.h>
#endif

#ifndef HAVE_SOL_IP
#include <netinet/in_systm.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#endif

#ifdef HAVE_NETINET_IP_ICMP_H
#include <netinet/ip_icmp.h>
#endif

#include "monit.h"
#include "alert.h"
#include "event.h"
#include "socket.h"
#include "net.h"
#include "device.h"
#include "process.h"
#include "protocol.h"

// libmonit
#include "system/Time.h"
#include "io/File.h"

/**
 *  Implementation of validation engine
 *
 *  @file
 */


/* ------------------------------------------------------------- Definitions */


#define MATCH_LINE_LENGTH 512


/* -------------------------------------------------------------- Prototypes */


static void check_uid(Service_T);
static void check_gid(Service_T);
static void check_size(Service_T);
static void check_uptime(Service_T);
static void check_perm(Service_T);
static void check_match(Service_T);
static int  check_skip(Service_T, time_t);
static void check_timeout(Service_T);
static void check_checksum(Service_T);
static void check_timestamp(Service_T);
static void check_process_state(Service_T);
static void check_process_pid(Service_T);
static void check_process_ppid(Service_T);
static void check_connection(Service_T, Port_T);
static void check_filesystem_flags(Service_T);
static void check_filesystem_resources(Service_T, Filesystem_T);
static void check_process_resources(Service_T, Resource_T);
static int  do_scheduled_action(Service_T);

/* ---------------------------------------------------------------- Public */


/**
 *  This function contains the main check machinery for  monit. The
 *  validate function check services in the service list to see if
 *  they will pass all defined tests.
 */
int validate() {
        int errors = 0;
        Service_T s;

        Run.handler_flag = HANDLER_SUCCEEDED;
        Event_queue_process();

        update_system_load();
        initprocesstree(&ptree, &ptreesize, &oldptree, &oldptreesize);
        gettimeofday(&systeminfo.collected, NULL);

        /* In the case that at least one action is pending, perform quick
         * loop to handle the actions ASAP */
        if (Run.doaction) {
                Run.doaction = 0;
                for (s = servicelist; s; s = s->next)
                        do_scheduled_action(s);
        }

        /* Check the services */
        time_t now = Time_now();
        for (s = servicelist; s && !Run.stopped; s = s->next) {
                if (! do_scheduled_action(s) && s->monitor && ! check_skip(s, now)) {
                        check_timeout(s); // Can disable monitoring => need to check s->monitor again
                        if (s->monitor) {
                                if (! s->check(s))
                                        errors++;
                                /* The monitoring may be disabled by some matching rule in s->check
                                 * so we have to check again before setting to MONITOR_YES */
                                if (s->monitor != MONITOR_NOT)
                                        s->monitor = MONITOR_YES;
                        }
                        gettimeofday(&s->collected, NULL);
                }
        }

        reset_depend();

        return errors;
}


/**
 * Validate a given process service s. Events are posted according to
 * its configuration. In case of a fatal event FALSE is returned.
 */
int check_process(Service_T s) {

        pid_t  pid = -1;
        Port_T pp = NULL;
        Resource_T pr = NULL;

        ASSERT(s);

        /* Test for running process */
        if (!(pid = Util_isProcessRunning(s, FALSE))) {
                Event_post(s, Event_Nonexist, STATE_FAILED, s->action_NONEXIST, "process is not running");
                return FALSE;
        } else
                Event_post(s, Event_Nonexist, STATE_SUCCEEDED, s->action_NONEXIST, "process is running with pid %d", (int)pid);

        /* Reset the exec and timeout errors if active ... the process is running (most probably after manual intervention) */
        if (IS_EVENT_SET(s->error, Event_Exec))
                Event_post(s, Event_Exec, STATE_SUCCEEDED, s->action_EXEC, "process is running after previous exec error (slow starting or manually recovered?)");
        if (IS_EVENT_SET(s->error, Event_Timeout))
                for (ActionRate_T ar = s->actionratelist; ar; ar = ar->next)
                        Event_post(s, Event_Timeout, STATE_SUCCEEDED, ar->action, "process is running after previous restart timeout (manually recovered?)");

        if (Run.doprocess) {
                if (update_process_data(s, ptree, ptreesize, pid)) {
                        check_process_state(s);
                        check_process_pid(s);
                        check_process_ppid(s);
                        if (s->uptimelist)
                                check_uptime(s);
                        for (pr = s->resourcelist; pr; pr = pr->next)
                                check_process_resources(s, pr);
                } else
                        LogError("'%s' failed to get service data\n", s->name);
        }

        /* Test each host:port and protocol in the service's portlist */
        if (s->portlist)
                for (pp = s->portlist; pp; pp = pp->next)
                        check_connection(s, pp);

        return TRUE;

}


/**
 * Validate a given filesystem service s. Events are posted according to
 * its configuration. In case of a fatal event FALSE is returned.
 */
int check_filesystem(Service_T s) {
        char *p;
        char path_buf[PATH_MAX+1];
        Filesystem_T td;
        struct stat stat_buf;

        ASSERT(s);

        p = s->path;

        /* We need to resolve symbolic link so if it points to device, we'll be able to find it in mnttab */
        if (lstat(s->path, &stat_buf) != 0) {
                Event_post(s, Event_Nonexist, STATE_FAILED, s->action_NONEXIST, "filesystem doesn't exist");
                return FALSE;
        }
        if (S_ISLNK(stat_buf.st_mode)) {
                if (! realpath(s->path, path_buf)) {
                        Event_post(s, Event_Nonexist, STATE_FAILED, s->action_NONEXIST, "filesystem symbolic link error -- %s", STRERROR);
                        return FALSE;
                }
                p = path_buf;
                Event_post(s, Event_Nonexist, STATE_SUCCEEDED, s->action_NONEXIST, "filesystem symbolic link %s -> %s", s->path, p);
                if (stat(p, &stat_buf) != 0) {
                        Event_post(s, Event_Nonexist, STATE_FAILED, s->action_NONEXIST, "filesystem doesn't exist");
                        return FALSE;
                }
        }
        Event_post(s, Event_Nonexist, STATE_SUCCEEDED, s->action_NONEXIST, "filesystem exists");

        s->inf->st_mode = stat_buf.st_mode;
        s->inf->st_uid  = stat_buf.st_uid;
        s->inf->st_gid  = stat_buf.st_gid;

        if (!filesystem_usage(s->inf, p)) {
                Event_post(s, Event_Data, STATE_FAILED, s->action_DATA, "unable to read filesystem %s state", p);
                return FALSE;
        }
        s->inf->priv.filesystem.inode_percent = s->inf->priv.filesystem.f_files > 0 ? (int)((1000.0 * (s->inf->priv.filesystem.f_files - s->inf->priv.filesystem.f_filesfree)) / (float)s->inf->priv.filesystem.f_files) : 0;
        s->inf->priv.filesystem.space_percent = s->inf->priv.filesystem.f_blocks > 0 ? (int)((1000.0 * (s->inf->priv.filesystem.f_blocks - s->inf->priv.filesystem.f_blocksfree)) / (float)s->inf->priv.filesystem.f_blocks) : 0;
        s->inf->priv.filesystem.inode_total   = s->inf->priv.filesystem.f_files - s->inf->priv.filesystem.f_filesfree;
        s->inf->priv.filesystem.space_total   = s->inf->priv.filesystem.f_blocks - s->inf->priv.filesystem.f_blocksfreetotal;
        Event_post(s, Event_Data, STATE_SUCCEEDED, s->action_DATA, "succeeded getting filesystem statistic for %s", p);

        if (s->perm)
                check_perm(s);

        if (s->uid)
                check_uid(s);

        if (s->gid)
                check_gid(s);

        check_filesystem_flags(s);

        if (s->filesystemlist)
                for (td = s->filesystemlist; td; td = td->next)
                        check_filesystem_resources(s, td);

        return TRUE;
}


/**
 * Validate a given file service s. Events are posted according to
 * its configuration. In case of a fatal event FALSE is returned.
 */
int check_file(Service_T s) {
        struct stat stat_buf;

        ASSERT(s);

        if (stat(s->path, &stat_buf) != 0) {
                Event_post(s, Event_Nonexist, STATE_FAILED, s->action_NONEXIST, "file doesn't exist");
                return FALSE;
        } else {
                s->inf->st_mode = stat_buf.st_mode;
                if (s->inf->priv.file.st_ino)
                        s->inf->priv.file.st_ino_prev = s->inf->priv.file.st_ino;
                s->inf->priv.file.st_ino  = stat_buf.st_ino;
                s->inf->st_uid            = stat_buf.st_uid;
                s->inf->st_gid            = stat_buf.st_gid;
                s->inf->priv.file.st_size = stat_buf.st_size;
                s->inf->timestamp         = MAX(stat_buf.st_mtime, stat_buf.st_ctime);
                DEBUG("'%s' file exists check succeeded\n", s->name);
                Event_post(s, Event_Nonexist, STATE_SUCCEEDED, s->action_NONEXIST, "file exist");
        }

        if (!S_ISREG(s->inf->st_mode)) {
                Event_post(s, Event_Invalid, STATE_FAILED, s->action_INVALID, "is not a regular file");
                return FALSE;
        } else {
                DEBUG("'%s' is a regular file\n", s->name);
                Event_post(s, Event_Invalid, STATE_SUCCEEDED, s->action_INVALID, "is a regular file");
        }

        if (s->checksum)
                check_checksum(s);

        if (s->perm)
                check_perm(s);

        if (s->uid)
                check_uid(s);

        if (s->gid)
                check_gid(s);

        if (s->sizelist)
                check_size(s);

        if (s->timestamplist)
                check_timestamp(s);

        if (s->matchlist)
                check_match(s);

        return TRUE;

}


/**
 * Validate a given directory service s. Events are posted according to
 * its configuration. In case of a fatal event FALSE is returned.
 */
int check_directory(Service_T s) {

        struct stat stat_buf;

        ASSERT(s);

        if (stat(s->path, &stat_buf) != 0) {
                Event_post(s, Event_Nonexist, STATE_FAILED, s->action_NONEXIST, "directory doesn't exist");
                return FALSE;
        } else {
                s->inf->st_mode   = stat_buf.st_mode;
                s->inf->st_uid    = stat_buf.st_uid;
                s->inf->st_gid    = stat_buf.st_gid;
                s->inf->timestamp = MAX(stat_buf.st_mtime, stat_buf.st_ctime);
                DEBUG("'%s' directory exists check succeeded\n", s->name);
                Event_post(s, Event_Nonexist, STATE_SUCCEEDED, s->action_NONEXIST, "directory exist");
        }

        if (!S_ISDIR(s->inf->st_mode)) {
                Event_post(s, Event_Invalid, STATE_FAILED, s->action_INVALID, "is not directory");
                return FALSE;
        } else {
                DEBUG("'%s' is directory\n", s->name);
                Event_post(s, Event_Invalid, STATE_SUCCEEDED, s->action_INVALID, "is directory");
        }

        if (s->perm)
                check_perm(s);

        if (s->uid)
                check_uid(s);

        if (s->gid)
                check_gid(s);

        if (s->timestamplist)
                check_timestamp(s);

        return TRUE;

}


/**
 * Validate a given fifo service s. Events are posted according to
 * its configuration. In case of a fatal event FALSE is returned.
 */
int check_fifo(Service_T s) {

        struct stat stat_buf;

        ASSERT(s);

        if (stat(s->path, &stat_buf) != 0) {
                Event_post(s, Event_Nonexist, STATE_FAILED, s->action_NONEXIST, "fifo doesn't exist");
                return FALSE;
        } else {
                s->inf->st_mode   = stat_buf.st_mode;
                s->inf->st_uid    = stat_buf.st_uid;
                s->inf->st_gid    = stat_buf.st_gid;
                s->inf->timestamp = MAX(stat_buf.st_mtime, stat_buf.st_ctime);
                DEBUG("'%s' fifo exists check succeeded\n", s->name);
                Event_post(s, Event_Nonexist, STATE_SUCCEEDED, s->action_NONEXIST, "fifo exist");
        }

        if (!S_ISFIFO(s->inf->st_mode)) {
                Event_post(s, Event_Invalid, STATE_FAILED, s->action_INVALID, "is not fifo");
                return FALSE;
        } else {
                DEBUG("'%s' is fifo\n", s->name);
                Event_post(s, Event_Invalid, STATE_SUCCEEDED, s->action_INVALID, "is fifo");
        }

        if (s->perm)
                check_perm(s);

        if (s->uid)
                check_uid(s);

        if (s->gid)
                check_gid(s);

        if (s->timestamplist)
                check_timestamp(s);

        return TRUE;

}


/**
 * Validate a program status. Events are posted according to
 * its configuration. In case of a fatal event FALSE is returned.
 */
int check_program(Service_T s) {
        ASSERT(s);
        ASSERT(s->program);
        time_t now = Time_now();
        Process_T P = s->program->P;
        if (P) {
                if (Process_exitStatus(P) < 0) { // Program is still running
                        time_t execution_time = (now - s->program->started);
                        if (execution_time > s->program->timeout) { // Program timed out
                                LogError("'%s' program timed out after %ld seconds. Killing program with pid %ld\n", s->name, (long)execution_time, (long)Process_getPid(P));
                                Process_kill(P);
                                Process_waitFor(P); // Wait for child to exit to get correct exit value
                                // Fall-through with P and evaluate exit value below.
                        } else {
                                // Defer test of exit value until program exit or timeout
                                DEBUG("'%s' status check defered - waiting on program to exit\n", s->name);
                                return TRUE;
                        }
                }
                s->program->exitStatus = Process_exitStatus(P); // Save exit status for web-view display
                int n = 0;
                char buf[STRLEN + 1];
                // Evaluate program's exit status against our status checks.
                /* TODO: Multiple checks we have now should be deprecated and removed - not useful because it
                 will alert on everything if != is used other than the match or if = is used, might report nothing on error. */
                for (Status_T status = s->statuslist; status; status = status->next) {
                        if (Util_evalQExpression(status->operator, s->program->exitStatus, status->return_value)) {
                                // Read message from script
                                if ((n = InputStream_readBytes(Process_getErrorStream(P), buf, STRLEN)) <= 0)
                                        n = InputStream_readBytes(Process_getInputStream(P), buf, STRLEN);
                                if (n > 0) {
                                        buf[n] = 0;
                                        Event_post(s, Event_Status, STATE_FAILED, status->action, "%s", buf);
                                } else {
                                        Event_post(s, Event_Status, STATE_FAILED, status->action, "'%s' failed with exit status (%d) -- no output from program", s->path, s->program->exitStatus);
                                }
                        } else {
                                DEBUG("'%s' status check succeeded\n", s->name);
                                Event_post(s, Event_Status, STATE_SUCCEEDED, status->action, "status succeeded");
                        }
                }
                Process_free(&s->program->P);
        }
        // Start program
        s->program->P = Command_execute(s->program->C);
        if (! s->program->P) {
                Event_post(s, Event_Status, STATE_FAILED, s->action_EXEC, "failed to execute '%s' -- %s", s->path, STRERROR);
        } else {
                Event_post(s, Event_Status, STATE_SUCCEEDED, s->action_EXEC, "'%s' program started", s->name);
                s->program->started = now;
        }
        return TRUE;
}


/**
 * Validate a remote service.
 * @param s The remote service to validate
 * @return FALSE if there was an error otherwise TRUE
 */
int check_remote_host(Service_T s) {

        Port_T p = NULL;
        Icmp_T icmp = NULL;
        Icmp_T last_ping = NULL;

        ASSERT(s);

        /* Test each icmp type in the service's icmplist */
        if (s->icmplist) {
                for (icmp = s->icmplist; icmp; icmp = icmp->next) {

                        switch(icmp->type) {
                                case ICMP_ECHO:

                                        icmp->response = icmp_echo(s->path, icmp->timeout, icmp->count);

                                        if (icmp->response == -2) {
                                                icmp->is_available = TRUE;
                                                DEBUG("'%s' icmp ping skipped -- the monit user has no permission to create raw socket, please run monit as root or add privilege for net_icmpaccess\n", s->name);
                                        } else if (icmp->response == -1) {
                                                icmp->is_available = FALSE;
                                                DEBUG("'%s' icmp ping failed\n", s->name);
                                                Event_post(s, Event_Icmp, STATE_FAILED, icmp->action, "failed ICMP test [%s]", icmpnames[icmp->type]);
                                        } else {
                                                icmp->is_available = TRUE;
                                                DEBUG("'%s' icmp ping succeeded [response time %.3fs]\n", s->name, icmp->response);
                                                Event_post(s, Event_Icmp, STATE_SUCCEEDED, icmp->action, "succeeded ICMP test [%s]", icmpnames[icmp->type]);
                                        }
                                        last_ping = icmp;
                                        break;

                                default:
                                        LogError("'%s' error -- unknown ICMP type: [%d]\n", s->name, icmp->type);
                                        return FALSE;

                        }
                }
        }

        /* If we could not ping the host we assume it's down and do not
         * continue to check any port connections  */
        if (last_ping && !last_ping->is_available) {
                DEBUG("'%s' icmp ping failed, skipping any port connection tests\n", s->name);
                return FALSE;
        }

        /* Test each host:port and protocol in the service's portlist */
        if (s->portlist)
                for (p = s->portlist; p; p = p->next)
                        check_connection(s, p);

        return TRUE;

}


/**
 * Validate the general system indicators. In case of a fatal event
 * FALSE is returned.
 */
int check_system(Service_T s) {
        Resource_T r = NULL;

        ASSERT(s);

        for (r = s->resourcelist; r; r = r->next) {
                check_process_resources(s, r);
        }

        return TRUE;
}


/* --------------------------------------------------------------- Private */


/**
 * Test the connection and protocol
 */
static void check_connection(Service_T s, Port_T p) {
        Socket_T socket;
        volatile int retry_count = p->retry;
        volatile int rv = TRUE;
        char buf[STRLEN];
        char report[STRLEN] = {0};
        struct timeval t1;
        struct timeval t2;

        ASSERT(s && p);
retry:
        /* Get time of connection attempt beginning */
        gettimeofday(&t1, NULL);

        /* Open a socket to the destination INET[hostname:port] or UNIX[pathname] */
        socket = socket_create(p);
        if (!socket) {
                snprintf(report, STRLEN, "failed, cannot open a connection to %s", Util_portDescription(p, buf, sizeof(buf)));
                rv = FALSE;
                goto error;
        } else
                DEBUG("'%s' succeeded connecting to %s\n", s->name, Util_portDescription(p, buf, sizeof(buf)));

        /* Verify that the socket is ready for i|o. TCP sockets are checked anytime, UDP
         * sockets just when there is no specific protocol test used since the socket_is_ready()
         * adds 2s delay when used with UDP socket. When there is specific protocol used, we
         * don't need it for UDP, since the protocol test is sufficient */
        if ((socket_get_type(socket) != SOCK_DGRAM || p->protocol->check == check_default) && !socket_is_ready(socket)) {
                snprintf(report, STRLEN, "connection failed, %s is not ready for i|o -- %s", Util_portDescription(p, buf, sizeof(buf)), STRERROR);
                rv = FALSE;
                goto error;
        }

        /* Run the protocol verification routine through the socket */
        if (! p->protocol->check(socket)) {
                snprintf(report, STRLEN, "failed protocol test [%s] at %s -- %s", p->protocol->name, Util_portDescription(p, buf, sizeof(buf)), socket_getError(socket));
                rv = FALSE;
                goto error;
        } else
                DEBUG("'%s' succeeded testing protocol [%s] at %s\n", s->name, p->protocol->name, Util_portDescription(p, buf, sizeof(buf)));

        /* Get time of connection attempt finish */
        gettimeofday(&t2, NULL);

        /* Get the response time */
        p->response = (double)(t2.tv_sec - t1.tv_sec) + (double)(t2.tv_usec - t1.tv_usec)/1000000;

error:
        if (socket)
                socket_free(&socket);
        if (!rv) {
                if (retry_count-- > 1) {
                        DEBUG("'%s' %s (attempt %d/%d)\n", s->name, report, p->retry - retry_count, p->retry);
                        goto retry;
                }
                p->response = -1;
                p->is_available = FALSE;
                Event_post(s, Event_Connection, STATE_FAILED, p->action, report);
        } else {
                p->is_available = TRUE;
                Event_post(s, Event_Connection, STATE_SUCCEEDED, p->action, "connection succeeded to %s", Util_portDescription(p, buf, sizeof(buf)));
        }

}


/**
 * Test process state (e.g. Zombie)
 */
static void check_process_state(Service_T s) {

        ASSERT(s);

        if (s->inf->priv.process.status_flag & PROCESS_ZOMBIE)
                Event_post(s, Event_Data, STATE_FAILED, s->action_DATA, "process with pid %d is a zombie", s->inf->priv.process.pid);
        else {
                DEBUG("'%s' zombie check succeeded [status_flag=%04x]\n", s->name,  s->inf->priv.process.status_flag);
                Event_post(s, Event_Data, STATE_SUCCEEDED, s->action_DATA, "check process state succeeded");
        }

}


/**
 * Test process pid for possible change since last cycle
 */
static void check_process_pid(Service_T s) {

        ASSERT(s && s->inf);

        /* process pid was not initialized yet */
        if (s->inf->priv.process._pid == -1)
                return;

        if (s->inf->priv.process._pid != s->inf->priv.process.pid)
                Event_post(s, Event_Pid, STATE_CHANGED, s->action_PID, "process PID changed from %d to %d", s->inf->priv.process._pid, s->inf->priv.process.pid);
        else
                Event_post(s, Event_Pid, STATE_CHANGEDNOT, s->action_PID, "process PID has not changed since last cycle");
}


/**
 * Test process ppid for possible change since last cycle
 */
static void check_process_ppid(Service_T s) {

        ASSERT(s && s->inf);

        /* process ppid was not initialized yet */
        if (s->inf->priv.process._ppid == -1)
                return;

        if (s->inf->priv.process._ppid != s->inf->priv.process.ppid)
                Event_post(s, Event_PPid, STATE_CHANGED, s->action_PPID, "process PPID changed from %d to %d", s->inf->priv.process._ppid, s->inf->priv.process.ppid);
        else
                Event_post(s, Event_PPid, STATE_CHANGEDNOT, s->action_PPID, "process PPID has not changed since last cycle");
}


/**
 * Check process resources
 */
static void check_process_resources(Service_T s, Resource_T r) {

        int okay = TRUE;
        char report[STRLEN]={0};

        ASSERT(s && r);

        switch(r->resource_id) {

                case RESOURCE_ID_CPU_PERCENT:
                        if (s->monitor & MONITOR_INIT || s->inf->priv.process.cpu_percent < 0) {
                                DEBUG("'%s' cpu usage check skipped (initializing)\n", s->name);
                        } else if (Util_evalQExpression(r->operator, s->inf->priv.process.cpu_percent, r->limit)) {
                                snprintf(report, STRLEN, "cpu usage of %.1f%% matches resource limit [cpu usage%s%.1f%%]", s->inf->priv.process.cpu_percent/10.0, operatorshortnames[r->operator], r->limit/10.0);
                                okay = FALSE;
                        } else
                                snprintf(report, STRLEN, "'%s' cpu usage check succeeded [current cpu usage=%.1f%%]", s->name, s->inf->priv.process.cpu_percent/10.0);
                        break;

                case RESOURCE_ID_TOTAL_CPU_PERCENT:
                        if (s->monitor & MONITOR_INIT || s->inf->priv.process.total_cpu_percent < 0) {
                                DEBUG("'%s' total cpu usage check skipped (initializing)\n", s->name);
                        } else if (Util_evalQExpression(r->operator, s->inf->priv.process.total_cpu_percent, r->limit)) {
                                snprintf(report, STRLEN, "total cpu usage of %.1f%% matches resource limit [cpu usage%s%.1f%%]", s->inf->priv.process.total_cpu_percent/10.0, operatorshortnames[r->operator], r->limit/10.0);
                                okay = FALSE;
                        } else
                                snprintf(report, STRLEN, "'%s' total cpu usage check succeeded [current cpu usage=%.1f%%]", s->name, s->inf->priv.process.total_cpu_percent/10.0);
                        break;

                case RESOURCE_ID_CPUUSER:
                        if (s->monitor & MONITOR_INIT || systeminfo.total_cpu_user_percent < 0) {
                                DEBUG("'%s' cpu user usage check skipped (initializing)\n", s->name);
                        } else if (Util_evalQExpression(r->operator, systeminfo.total_cpu_user_percent, r->limit)) {
                                snprintf(report, STRLEN, "cpu user usage of %.1f%% matches resource limit [cpu user usage%s%.1f%%]", systeminfo.total_cpu_user_percent/10.0, operatorshortnames[r->operator], r->limit/10.0);
                                okay = FALSE;
                        } else
                                snprintf(report, STRLEN, "'%s' cpu user usage check succeeded [current cpu user usage=%.1f%%]", s->name, systeminfo.total_cpu_user_percent/10.0);
                        break;

                case RESOURCE_ID_CPUSYSTEM:
                        if (s->monitor & MONITOR_INIT || systeminfo.total_cpu_syst_percent < 0) {
                                DEBUG("'%s' cpu system usage check skipped (initializing)\n", s->name);
                        } else if (Util_evalQExpression(r->operator, systeminfo.total_cpu_syst_percent, r->limit)) {
                                snprintf(report, STRLEN, "cpu system usage of %.1f%% matches resource limit [cpu system usage%s%.1f%%]", systeminfo.total_cpu_syst_percent/10.0, operatorshortnames[r->operator], r->limit/10.0);
                                okay = FALSE;
                        } else
                                snprintf(report, STRLEN, "'%s' cpu system usage check succeeded [current cpu system usage=%.1f%%]", s->name, systeminfo.total_cpu_syst_percent/10.0);
                        break;

                case RESOURCE_ID_CPUWAIT:
                        if (s->monitor & MONITOR_INIT || systeminfo.total_cpu_wait_percent < 0) {
                                DEBUG("'%s' cpu wait usage check skipped (initializing)\n", s->name);
                        } else if (Util_evalQExpression(r->operator, systeminfo.total_cpu_wait_percent, r->limit)) {
                                snprintf(report, STRLEN, "cpu wait usage of %.1f%% matches resource limit [cpu wait usage%s%.1f%%]", systeminfo.total_cpu_wait_percent/10.0, operatorshortnames[r->operator], r->limit/10.0);
                                okay = FALSE;
                        } else
                                snprintf(report, STRLEN, "'%s' cpu wait usage check succeeded [current cpu wait usage=%.1f%%]", s->name, systeminfo.total_cpu_wait_percent/10.0);
                        break;

                case RESOURCE_ID_MEM_PERCENT:
                        if (s->type == TYPE_SYSTEM) {
                                if (Util_evalQExpression(r->operator, systeminfo.total_mem_percent, r->limit)) {
                                        snprintf(report, STRLEN, "mem usage of %.1f%% matches resource limit [mem usage%s%.1f%%]", systeminfo.total_mem_percent/10.0, operatorshortnames[r->operator], r->limit/10.0);
                                        okay = FALSE;
                                } else
                                        snprintf(report, STRLEN, "'%s' mem usage check succeeded [current mem usage=%.1f%%]", s->name, systeminfo.total_mem_percent/10.0);
                        } else {
                                if (Util_evalQExpression(r->operator, s->inf->priv.process.mem_percent, r->limit)) {
                                        snprintf(report, STRLEN, "mem usage of %.1f%% matches resource limit [mem usage%s%.1f%%]", s->inf->priv.process.mem_percent/10.0, operatorshortnames[r->operator], r->limit/10.0);
                                        okay = FALSE;
                                } else
                                        snprintf(report, STRLEN, "'%s' mem usage check succeeded [current mem usage=%.1f%%]", s->name, s->inf->priv.process.mem_percent/10.0);
                        }
                        break;

                case RESOURCE_ID_MEM_KBYTE:
                        if (s->type == TYPE_SYSTEM) {
                                if (Util_evalQExpression(r->operator, systeminfo.total_mem_kbyte, r->limit)) {
                                        snprintf(report, STRLEN, "mem amount of %ldkB matches resource limit [mem amount%s%ldkB]", systeminfo.total_mem_kbyte, operatorshortnames[r->operator], r->limit);
                                        okay = FALSE;
                                } else
                                        snprintf(report, STRLEN, "'%s' mem amount check succeeded [current mem amount=%ldkB]", s->name, systeminfo.total_mem_kbyte);
                        } else {
                                if (Util_evalQExpression(r->operator, s->inf->priv.process.mem_kbyte, r->limit)) {
                                        snprintf(report, STRLEN, "mem amount of %ldkB matches resource limit [mem amount%s%ldkB]", s->inf->priv.process.mem_kbyte, operatorshortnames[r->operator], r->limit);
                                        okay = FALSE;
                                } else
                                        snprintf(report, STRLEN, "'%s' mem amount check succeeded [current mem amount=%ldkB]", s->name, s->inf->priv.process.mem_kbyte);
                        }
                        break;

                case RESOURCE_ID_SWAP_PERCENT:
                        if (s->type == TYPE_SYSTEM) {
                                if (Util_evalQExpression(r->operator, systeminfo.total_swap_percent, r->limit)) {
                                        snprintf(report, STRLEN, "swap usage of %.1f%% matches resource limit [swap usage%s%.1f%%]", systeminfo.total_swap_percent/10.0, operatorshortnames[r->operator], r->limit/10.0);
                                        okay = FALSE;
                                } else
                                        snprintf(report, STRLEN, "'%s' swap usage check succeeded [current swap usage=%.1f%%]", s->name, systeminfo.total_swap_percent/10.0);
                        }
                        break;

                case RESOURCE_ID_SWAP_KBYTE:
                        if (s->type == TYPE_SYSTEM) {
                                if (Util_evalQExpression(r->operator, systeminfo.total_swap_kbyte, r->limit)) {
                                        snprintf(report, STRLEN, "swap amount of %ldkB matches resource limit [swap amount%s%ldkB]", systeminfo.total_swap_kbyte, operatorshortnames[r->operator], r->limit);
                                        okay = FALSE;
                                } else
                                        snprintf(report, STRLEN, "'%s' swap amount check succeeded [current swap amount=%ldkB]", s->name, systeminfo.total_swap_kbyte);
                        }
                        break;

                case RESOURCE_ID_LOAD1:
                        if (Util_evalQExpression(r->operator, (int)(systeminfo.loadavg[0]*10.0), r->limit)) {
                                snprintf(report, STRLEN, "loadavg(1min) of %.1f matches resource limit [loadavg(1min)%s%.1f]", systeminfo.loadavg[0], operatorshortnames[r->operator], r->limit/10.0);
                                okay = FALSE;
                        } else
                                snprintf(report, STRLEN, "'%s' loadavg(1min) check succeeded [current loadavg(1min)=%.1f]", s->name, systeminfo.loadavg[0]);
                        break;

                case RESOURCE_ID_LOAD5:
                        if (Util_evalQExpression(r->operator, (int)(systeminfo.loadavg[1]*10.0), r->limit)) {
                                snprintf(report, STRLEN, "loadavg(5min) of %.1f matches resource limit [loadavg(5min)%s%.1f]", systeminfo.loadavg[1], operatorshortnames[r->operator], r->limit/10.0);
                                okay = FALSE;
                        } else
                                snprintf(report, STRLEN, "'%s' loadavg(5min) check succeeded [current loadavg(5min)=%.1f]", s->name, systeminfo.loadavg[1]);
                        break;

                case RESOURCE_ID_LOAD15:
                        if (Util_evalQExpression(r->operator, (int)(systeminfo.loadavg[2]*10.0), r->limit)) {
                                snprintf(report, STRLEN, "loadavg(15min) of %.1f matches resource limit [loadavg(15min)%s%.1f]", systeminfo.loadavg[2], operatorshortnames[r->operator], r->limit/10.0);
                                okay = FALSE;
                        } else
                                snprintf(report, STRLEN, "'%s' loadavg(15min) check succeeded [current loadavg(15min)=%.1f]", s->name, systeminfo.loadavg[2]);
                        break;

                case RESOURCE_ID_CHILDREN:
                        if (Util_evalQExpression(r->operator, s->inf->priv.process.children, r->limit)) {
                                snprintf(report, STRLEN, "children of %i matches resource limit [children%s%ld]", s->inf->priv.process.children, operatorshortnames[r->operator], r->limit);
                                okay = FALSE;
                        } else
                                snprintf(report, STRLEN, "'%s' children check succeeded [current children=%i]", s->name, s->inf->priv.process.children);
                        break;

                case RESOURCE_ID_TOTAL_MEM_KBYTE:
                        if (Util_evalQExpression(r->operator, s->inf->priv.process.total_mem_kbyte, r->limit)) {
                                snprintf(report, STRLEN, "total mem amount of %ldkB matches resource limit [total mem amount%s%ldkB]", s->inf->priv.process.total_mem_kbyte, operatorshortnames[r->operator], r->limit);
                                okay = FALSE;
                        } else
                                snprintf(report, STRLEN, "'%s' total mem amount check succeeded [current total mem amount=%ldkB]", s->name, s->inf->priv.process.total_mem_kbyte);
                        break;

                case RESOURCE_ID_TOTAL_MEM_PERCENT:
                        if (Util_evalQExpression(r->operator, s->inf->priv.process.total_mem_percent, r->limit)) {
                                snprintf(report, STRLEN, "total mem amount of %.1f%% matches resource limit [total mem amount%s%.1f%%]", (float)s->inf->priv.process.total_mem_percent/10.0, operatorshortnames[r->operator], (float)r->limit/10.0);
                                okay = FALSE;
                        } else
                                snprintf(report, STRLEN, "'%s' total mem amount check succeeded [current total mem amount=%.1f%%]", s->name, s->inf->priv.process.total_mem_percent/10.0);
                        break;

                default:
                        LogError("'%s' error -- unknown resource ID: [%d]\n", s->name, r->resource_id);
                        return;
        }

        if (! okay)
                Event_post(s, Event_Resource, STATE_FAILED, r->action, "%s", report);
        else {
                Event_post(s, Event_Resource, STATE_SUCCEEDED, r->action, "%s", report);
                if (*report)
                        DEBUG("%s\n", report);
        }
}


/**
 * Test for associated path checksum change
 */
static void check_checksum(Service_T s) {
        int         changed;
        Checksum_T  cs;

        ASSERT(s && s->path && s->checksum);

        cs = s->checksum;

        if (Util_getChecksum(s->path, cs->type, s->inf->priv.file.cs_sum, sizeof(s->inf->priv.file.cs_sum))) {

                Event_post(s, Event_Data, STATE_SUCCEEDED, s->action_DATA, "checksum computed for %s", s->path);

                if (! cs->initialized) {
                        cs->initialized = TRUE;
                        snprintf(cs->hash, sizeof(cs->hash), "%s", s->inf->priv.file.cs_sum);
                }

                switch(cs->type) {
                        case HASH_MD5:
                                changed = strncmp(cs->hash, s->inf->priv.file.cs_sum, 32);
                                break;
                        case HASH_SHA1:
                                changed = strncmp(cs->hash, s->inf->priv.file.cs_sum, 40);
                                break;
                        default:
                                LogError("'%s' unknown hash type\n", s->name);
                                *s->inf->priv.file.cs_sum = 0;
                                return;
                }

                if (changed) {

                        if (cs->test_changes) {
                                /* if we are testing for changes only, the value is variable */
                                Event_post(s, Event_Checksum, STATE_CHANGED, cs->action, "checksum was changed for %s", s->path);
                                /* reset expected value for next cycle */
                                snprintf(cs->hash, sizeof(cs->hash), "%s", s->inf->priv.file.cs_sum);
                        } else {
                                /* we are testing constant value for failed or succeeded state */
                                Event_post(s, Event_Checksum, STATE_FAILED, cs->action, "checksum test failed for %s", s->path);
                        }

                } else if (cs->test_changes) {

                        DEBUG("'%s' checksum has not changed\n", s->name);
                        Event_post(s, Event_Checksum, STATE_CHANGEDNOT, cs->action, "checksum has not changed");

                } else {

                        DEBUG("'%s' has valid checksums\n", s->name);
                        Event_post(s, Event_Checksum, STATE_SUCCEEDED, cs->action, "checksum succeeded");

                }
                return;
        }

        Event_post(s, Event_Data, STATE_FAILED, s->action_DATA, "cannot compute checksum for %s", s->path);

}


/**
 * Test for associated path permission change
 */
static void check_perm(Service_T s) {
        ASSERT(s && s->perm);

        if ((s->inf->st_mode & 07777) != s->perm->perm)
                Event_post(s, Event_Permission, STATE_FAILED, s->perm->action, "permission test failed for %s -- current permission is %04o", s->path, s->inf->st_mode&07777);
        else {
                DEBUG("'%s' permission check succeeded [current permission=%04o]\n", s->name, s->inf->st_mode&07777);
                Event_post(s, Event_Permission, STATE_SUCCEEDED, s->perm->action, "permission succeeded");
        }
}


/**
 * Test for associated path uid change
 */
static void check_uid(Service_T s) {
        ASSERT(s && s->uid);

        if (s->inf->st_uid != s->uid->uid)
                Event_post(s, Event_Uid, STATE_FAILED, s->uid->action, "uid test failed for %s -- current uid is %d", s->path, (int)s->inf->st_uid);
        else {
                DEBUG("'%s' uid check succeeded [current uid=%d]\n", s->name, (int)s->inf->st_uid);
                Event_post(s, Event_Uid, STATE_SUCCEEDED, s->uid->action, "uid succeeded");
        }
}


/**
 * Test for associated path gid change
 */
static void check_gid(Service_T s) {
        ASSERT(s && s->gid);

        if (s->inf->st_gid != s->gid->gid )
                Event_post(s, Event_Gid, STATE_FAILED, s->gid->action, "gid test failed for %s -- current gid is %d", s->path, (int)s->inf->st_gid);
        else {
                DEBUG("'%s' gid check succeeded [current gid=%d]\n", s->name, (int)s->inf->st_gid);
                Event_post(s, Event_Gid, STATE_SUCCEEDED, s->gid->action, "gid succeeded");
        }
}


/**
 * Validate timestamps of a service s
 */
static void check_timestamp(Service_T s) {
        Timestamp_T t;
        time_t      now;

        ASSERT(s && s->timestamplist);

        if ((int)time(&now) == -1) {
                Event_post(s, Event_Data, STATE_FAILED, s->action_DATA, "can't obtain actual system time");
                return;
        } else
                Event_post(s, Event_Data, STATE_SUCCEEDED, s->action_DATA, "actual system time obtained");

        for (t = s->timestamplist; t; t = t->next) {
                if (t->test_changes) {

                        /* if we are testing for changes only, the value is variable */

                        if (t->timestamp != s->inf->timestamp) {
                                /* reset expected value for next cycle */
                                t->timestamp = s->inf->timestamp;
                                Event_post(s, Event_Timestamp, STATE_CHANGED, t->action, "timestamp was changed for %s", s->path);
                        } else {
                                DEBUG("'%s' timestamp was not changed for %s\n", s->name, s->path);
                                Event_post(s, Event_Timestamp, STATE_CHANGEDNOT, t->action, "timestamp was not changed for %s", s->path);
                        }
                        break;
                } else {

                        /* we are testing constant value for failed or succeeded state */

                        if (Util_evalQExpression(t->operator, (int)(now - s->inf->timestamp), t->time))
                                Event_post(s, Event_Timestamp, STATE_FAILED, t->action, "timestamp test failed for %s", s->path);
                        else {
                                DEBUG("'%s' timestamp test succeeded for %s\n", s->name, s->path);
                                Event_post(s, Event_Timestamp, STATE_SUCCEEDED, t->action, "timestamp succeeded");
                        }
                }
        }
}


/**
 * Test size
 */
static void check_size(Service_T s) {
        Size_T sl;

        ASSERT(s && s->sizelist);

        for (sl = s->sizelist; sl; sl = sl->next) {

                /* if we are testing for changes only, the value is variable */
                if (sl->test_changes) {
                        if (!sl->initialized) {
                                /* the size was not initialized during monit start, so set the size now
                                 * and allow further size change testing */
                                sl->initialized = TRUE;
                                sl->size = s->inf->priv.file.st_size;
                        } else {
                                if (sl->size != s->inf->priv.file.st_size) {
                                        Event_post(s, Event_Size, STATE_CHANGED, sl->action, "size was changed for %s", s->path);
                                        /* reset expected value for next cycle */
                                        sl->size = s->inf->priv.file.st_size;
                                } else {
                                        DEBUG("'%s' size has not changed [current size=%llu B]\n", s->name, s->inf->priv.file.st_size);
                                        Event_post(s, Event_Size, STATE_CHANGEDNOT, sl->action, "size was not changed", s->path);
                                }
                        }
                        break;
                }

                /* we are testing constant value for failed or succeeded state */
                if (Util_evalQExpression(sl->operator, s->inf->priv.file.st_size, sl->size))
                        Event_post(s, Event_Size, STATE_FAILED, sl->action, "size test failed for %s -- current size is %llu B", s->path, s->inf->priv.file.st_size);
                else {
                        DEBUG("'%s' size check succeeded [current size=%llu B]\n", s->name, s->inf->priv.file.st_size);
                        Event_post(s, Event_Size, STATE_SUCCEEDED, sl->action, "size succeeded");
                }
        }
}


/**
 * Test uptime
 */
static void check_uptime(Service_T s) {
        Uptime_T ul;

        ASSERT(s && s->uptimelist);

        for (ul = s->uptimelist; ul; ul = ul->next) {
                if (Util_evalQExpression(ul->operator, s->inf->priv.process.uptime, ul->uptime))
                        Event_post(s, Event_Uptime, STATE_FAILED, ul->action, "uptime test failed for %s -- current uptime is %llu seconds", s->path, s->inf->priv.process.uptime);
                else {
                        DEBUG("'%s' uptime check succeeded [current uptime=%llu seconds]\n", s->name, s->inf->priv.process.uptime);
                        Event_post(s, Event_Uptime, STATE_SUCCEEDED, ul->action, "uptime succeeded");
                }
        }
}


static int check_pattern(Match_T pattern, const char *line) {
#ifdef HAVE_REGEX_H
        return regexec(pattern->regex_comp, line, 0, NULL, 0);
#else
        if (strstr(line, pattern->match_string) == NULL)
                return -1;
        else
                return 0;
#endif
}


/**
 * Match content.
 *
 * The test compares only the lines terminated with \n.
 *
 * In the case that line with missing \n is read, the test stops, as we suppose that the file contains only partial line and the rest of it is yet stored in the buffer of the application which writes to the file.
 * The test will resume at the beginning of the incomplete line during the next cycle, allowing the writer to finish the write.
 *
 * We test only MATCH_LINE_LENGTH at maximum (512 bytes) - in the case that the line is bigger, we read the rest of the line (till '\n') but ignore the characters past the maximum (512+).
 */
static void check_match(Service_T s) {
        Match_T ml;
        FILE *file;
        char line[MATCH_LINE_LENGTH];

        ASSERT(s && s->matchlist);

        /* Open the file */
        if (! (file = fopen(s->path, "r"))) {
                LogError("'%s' cannot open file %s: %s\n", s->name, s->path, STRERROR);
                return;
        }

        /* FIXME: Refactor: Initialize the filesystems table ahead of file and filesystems test and index it by device id + replace the Str_startsWith() with lookup to the table by device id (obtained via file's stat()).
                            The central filesystems initialization will allow to reduce the statfs() calls in the case that there will be multiple file and/or filesystems tests for the same fs. Temporarily we go with
                            dummy Str_startsWith() as quick fix which will cover 99.9% of use cases without rising the statfs overhead if statfs call would be inlined here.
         */
        if (Str_startsWith(s->path, "/proc")) {
                s->inf->priv.file.readpos = 0;
        } else {
                /* If inode changed or size shrinked -> set read position = 0 */
                if (s->inf->priv.file.st_ino != s->inf->priv.file.st_ino_prev || s->inf->priv.file.readpos > s->inf->priv.file.st_size)
                        s->inf->priv.file.readpos = 0;

                /* Do we need to match? Even if not, go to final, so we can reset the content match error flags in this cycle */
                if (s->inf->priv.file.readpos == s->inf->priv.file.st_size) {
                        DEBUG("'%s' content match skipped - file size nor inode has not changed since last test\n", s->name);
                        goto final;
                }
        }

        while (TRUE) {
next:
                /* Seek to the read position */
                if (fseek(file, (long)s->inf->priv.file.readpos, SEEK_SET)) {
                        LogError("'%s' cannot seek file %s: %s\n", s->name, s->path, STRERROR);
                        goto final;
                }

                if (! fgets(line, MATCH_LINE_LENGTH, file)) {
                        if (! feof(file))
                                LogError("'%s' cannot read file %s: %s\n", s->name, s->path, STRERROR);
                        goto final;
                }

                size_t length = strlen(line);
                if (length == 0) {
                        /* No content: shouldn't happen - empty line will contain at least '\n' */
                        goto final;
                } else if (line[length-1] != '\n') {
                        if (length < MATCH_LINE_LENGTH-1) {
                                /* Incomplete line: we gonna read it next time again, allowing the writer to complete the write */
                                DEBUG("'%s' content match: incomplete line read - no new line at end. (retrying next cycle)\n", s->name);
                                goto final;
                        } else if (length == MATCH_LINE_LENGTH-1) {
                                /* Our read buffer is full: ignore the content past the MATCH_LINE_LENGTH */
                                int rv;
                                do {
                                        if ((rv = fgetc(file)) == EOF)
                                                goto final;
                                        length++;
                                } while (rv != '\n');
                        }
                } else {
                        /* Remove appending newline */
                        line[length-1] = 0;
                }
                /* Set read position to the end of last read */
                s->inf->priv.file.readpos += length;

                /* Check ignores */
                for (ml = s->matchignorelist; ml; ml = ml->next) {
                        if ((check_pattern(ml, line) == 0)  ^ (ml->not)) {
                                /* We match! -> line is ignored! */
                                DEBUG("'%s' Ignore pattern %s'%s' match on content line\n", s->name, ml->not ? "not " : "", ml->match_string);
                                goto next;
                        }
                }

                /* Check non ignores */
                for (ml = s->matchlist; ml; ml = ml->next) {
                        if ((check_pattern(ml, line) == 0) ^ (ml->not)) {
                                DEBUG("'%s' Pattern %s'%s' match on content line [%s]\n", s->name, ml->not ? "not " : "", ml->match_string, line);
                                /* Save the line: we limit the content showed in the event roughly to MATCH_LINE_LENGTH (we allow exceed to not break the line) */
                                if (! ml->log)
                                        ml->log = StringBuffer_create(MATCH_LINE_LENGTH);
                                if (StringBuffer_length(ml->log) < MATCH_LINE_LENGTH) {
                                        StringBuffer_append(ml->log, "%s\n", line);
                                        if (StringBuffer_length(ml->log) >= MATCH_LINE_LENGTH)
                                                StringBuffer_append(ml->log, "...\n");
                                }
                        } else {
                                DEBUG("'%s' Pattern %s'%s' doesn't match on content line [%s]\n", s->name, ml->not ? "not " : "", ml->match_string, line);
                        }
                }
        }
final:
        if (fclose(file))
                LogError("'%s' cannot close file %s: %s\n", s->name, s->path, STRERROR);

        /* Post process the matches: generate events for particular patterns */
        for (ml = s->matchlist; ml; ml = ml->next) {
                if (ml->log) {
                        Event_post(s, Event_Content, STATE_CHANGED, ml->action, "content match:\n%s", StringBuffer_toString(ml->log));
                        StringBuffer_free(&ml->log);
                } else {
                        Event_post(s, Event_Content, STATE_CHANGEDNOT, ml->action, "content doesn't match");
                }
        }
}


/**
 * Test filesystem flags for possible change since last cycle
 */
static void check_filesystem_flags(Service_T s) {
        ASSERT(s && s->inf);

        /* filesystem flags were not initialized yet */
        if (s->inf->priv.filesystem._flags == -1)
                return;

        if (s->inf->priv.filesystem._flags != s->inf->priv.filesystem.flags)
                Event_post(s, Event_Fsflag, STATE_CHANGED, s->action_FSFLAG, "filesytem flags changed to %#lx", s->inf->priv.filesystem.flags);
}

/**
 * Filesystem test
 */
static void check_filesystem_resources(Service_T s, Filesystem_T td) {
        ASSERT(s && td);

        if ( (td->limit_percent < 0) && (td->limit_absolute < 0) ) {
                LogError("'%s' error: filesystem limit not set\n", s->name);
                return;
        }

        switch(td->resource) {

                case RESOURCE_ID_INODE:
                        if (s->inf->priv.filesystem.f_files <= 0) {
                                DEBUG("'%s' filesystem doesn't support inodes\n", s->name);
                                return;
                        }

                        if (td->limit_percent >= 0) {
                                if (Util_evalQExpression( td->operator, s->inf->priv.filesystem.inode_percent, td->limit_percent)) {
                                        Event_post(s, Event_Resource, STATE_FAILED, td->action, "inode usage %.1f%% matches resource limit [inode usage%s%.1f%%]", s->inf->priv.filesystem.inode_percent/10., operatorshortnames[td->operator], td->limit_percent/10.);
                                        return;
                                }
                        } else {
                                if (Util_evalQExpression(td->operator, s->inf->priv.filesystem.inode_total, td->limit_absolute)) {
                                        Event_post(s, Event_Resource, STATE_FAILED, td->action, "inode usage %ld matches resource limit [inode usage%s%ld]", s->inf->priv.filesystem.inode_total, operatorshortnames[td->operator], td->limit_absolute);
                                        return;
                                }
                        }
                        DEBUG("'%s' inode usage check succeeded [current inode usage=%.1f%%]\n", s->name, s->inf->priv.filesystem.inode_percent/10.);
                        Event_post(s, Event_Resource, STATE_SUCCEEDED, td->action, "filesystem resources succeeded");
                        return;

                case RESOURCE_ID_SPACE:
                        if (td->limit_percent >= 0) {
                                if (Util_evalQExpression( td->operator, s->inf->priv.filesystem.space_percent, td->limit_percent)) {
                                        Event_post(s, Event_Resource, STATE_FAILED, td->action, "space usage %.1f%% matches resource limit [space usage%s%.1f%%]", s->inf->priv.filesystem.space_percent/10., operatorshortnames[td->operator], td->limit_percent/10.);
                                        return;
                                }
                        } else {
                                if (Util_evalQExpression(td->operator, s->inf->priv.filesystem.space_total, td->limit_absolute)) {
                                        Event_post(s, Event_Resource, STATE_FAILED, td->action, "space usage %ld blocks matches resource limit [space usage%s%ld blocks]", s->inf->priv.filesystem.space_total, operatorshortnames[td->operator], td->limit_absolute);
                                        return;
                                }
                        }
                        DEBUG("'%s' space usage check succeeded [current space usage=%.1f%%]\n", s->name, s->inf->priv.filesystem.space_percent/10.);
                        Event_post(s, Event_Resource, STATE_SUCCEEDED, td->action, "filesystem resources succeeded");
                        return;

                default:
                        LogError("'%s' error -- unknown resource type: [%d]\n", s->name, td->resource);
                        return;
        }

}


static void check_timeout(Service_T s) {
        ActionRate_T ar;
        int max = 0;

        ASSERT(s);

        if (! s->actionratelist)
                return;

        /* Start counting cycles */
        if (s->nstart > 0)
                s->ncycle++;

        for (ar = s->actionratelist; ar; ar = ar->next) {
                if (max < ar->cycle)
                        max = ar->cycle;
                if (s->nstart >= ar->count && s->ncycle <= ar->cycle)
                        Event_post(s, Event_Timeout, STATE_FAILED, ar->action, "service restarted %d times within %d cycles(s) - %s", s->nstart, s->ncycle, actionnames[ar->action->failed->id]);
        }

        /* Stop counting and reset if the cycle interval is succeeded */
        if (s->ncycle > max) {
                s->ncycle = 0;
                s->nstart = 0;
        }
}


/**
 * Returns TRUE if validation should be skiped for
 * this service in this cycle, otherwise FALSE. Handle
 * every statement
 */
static int check_skip(Service_T s, time_t time) {
        ASSERT(s);
        if (s->visited) {
                DEBUG("'%s' check skipped -- service already handled in a dependency chain\n", s->name);
                return TRUE;
        }
        if (s->every.type == EVERY_SKIPCYCLES) {
                s->every.spec.cycle.counter++;
                if (s->every.spec.cycle.counter < s->every.spec.cycle.number) {
                        s->monitor |= MONITOR_WAITING;
                        DEBUG("'%s' test skipped as current cycle (%d) < every cycle (%d) \n", s->name, s->every.spec.cycle.counter, s->every.spec.cycle.number);
                        return TRUE;
                }
                s->every.spec.cycle.counter = 0;
        } else if (s->every.type == EVERY_CRON && ! Time_incron(s->every.spec.cron, time)) {
                s->monitor |= MONITOR_WAITING;
                DEBUG("'%s' test skipped as current time (%ld) does not match every's cron spec \"%s\"\n", s->name, (long)time, s->every.spec.cron);
                return TRUE;
        } else if (s->every.type == EVERY_NOTINCRON && Time_incron(s->every.spec.cron, time)) {
                s->monitor |= MONITOR_WAITING;
                DEBUG("'%s' test skipped as current time (%ld) matches every's cron spec \"not %s\"\n", s->name, (long)time, s->every.spec.cron);
                return TRUE;
        }
        s->monitor &= ~MONITOR_WAITING;
        return FALSE;
}


/**
 * Returns TRUE if scheduled action was performed
 */
static int do_scheduled_action(Service_T s) {
        int rv = FALSE;
        if (s->doaction != ACTION_IGNORE) {
                // FIXME: let the event engine do the action directly? (just replace s->action_ACTION with s->doaction and drop control_service call)
                rv = control_service(s->name, s->doaction);
                Event_post(s, Event_Action, STATE_CHANGED, s->action_ACTION, "%s action done", actionnames[s->doaction]);
                s->doaction = ACTION_IGNORE;
                FREE(s->token);
        }
        return rv;
}

