/*
This file is part of Spindle.  For copyright information see the COPYRIGHT 
file in the top level directory, or at 
https://github.com/hpc/Spindle/blob/master/COPYRIGHT

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU Lesser General Public License (as published by the Free Software
Foundation) version 2.1 dated February 1999.  This program is distributed in the
hope that it will be useful, but WITHOUT ANY WARRANTY; without even the IMPLIED
WARRANTY OF MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the terms 
and conditions of the GNU Lesser General Public License for more details.  You should 
have received a copy of the GNU Lesser General Public License along with this 
program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "spindle_debug.h"
#include "ldcs_api.h"
#include "spindle_launch.h"
#include "client.h"
#include "client_api.h"
#include "exec_util.h"
#include "shmcache.h"

#include "config.h"

#if !defined(LIBEXECDIR)
#error Expected to be built with libdir defined
#endif
#if !defined(PROGLIBDIR)
#error Expected to be built with proglib defined
#endif

char spindle_daemon[] = LIBEXECDIR "/spindle_be";
char spindle_interceptlib[] = PROGLIBDIR "/libspindleint.so";

int ldcsid;
unsigned int shm_cachesize;

static int rankinfo[4]={-1,-1,-1,-1};
static int use_cache;

static char *executable;
static char *client_lib;
static char **daemon_args;

// Initialized via parse_cmdline()
static *symbolic_location;
static char *cache_path, *fifo_path, *daemon_path; // potentially multiple colon-separated paths
static char *number_s;
static int number;
static char *opts_s;
opt_t opts;
static char *cachesize_s;
static unsigned int cachesize;
static char **cmdline;

// Initialized via main()
static char *instantiated_cache_path;
static char *instantiated_fifo_path;
static char *instantiated_daemon_path;

char libstr_socket_subaudit[] = PROGLIBDIR "/libspindle_subaudit_socket.so";
char libstr_pipe_subaudit[] = PROGLIBDIR "/libspindle_subaudit_pipe.so";
char libstr_biter_subaudit[] = PROGLIBDIR "/libspindle_subaudit_biter.so";

char libstr_socket_audit[] = PROGLIBDIR "/libspindle_audit_socket.so";
char libstr_pipe_audit[] = PROGLIBDIR "/libspindle_audit_pipe.so";
char libstr_biter_audit[] = PROGLIBDIR "/libspindle_audit_biter.so";

#if defined(COMM_SOCKET)
static char *default_audit_libstr = libstr_socket_audit;
static char *default_subaudit_libstr = libstr_socket_subaudit;
#elif defined(COMM_PIPES)
static char *default_audit_libstr = libstr_pipe_audit;
static char *default_subaudit_libstr = libstr_pipe_subaudit;
#elif defined(COMM_BITER)
static char *default_audit_libstr = libstr_biter_audit;
static char *default_subaudit_libstr = libstr_biter_subaudit;
#else
#error Unknown connection type
#endif

extern int spindle_mkdir(char *path);
extern char *parse_location(char *loc, int number);
extern char *realize(char *path);

static int establish_connection()
{
   debug_printf2("Opening connection to server\n");
   ldcsid = client_open_connection(instantiated_fifo_path, number);
   if (ldcsid == -1) 
      return -1;

   send_pid(ldcsid);
   send_rankinfo_query(ldcsid, &rankinfo[0], &rankinfo[1], &rankinfo[2], &rankinfo[3]);      
   if (opts & OPT_NUMA)
      send_cpu(ldcsid, get_cur_cpu());

   return 0;
}

static void setup_environment()
{
   char rankinfo_str[256];
   snprintf(rankinfo_str, 256, "%d %d %d %d %d", ldcsid, rankinfo[0], rankinfo[1], rankinfo[2], rankinfo[3]);
   
   char *connection_str = NULL;
   if (opts & OPT_RELOCAOUT) 
      connection_str = client_get_connection_string(ldcsid);

   setenv("LD_AUDIT", client_lib, 1);
   setenv("LDCS_INSTANTIATED_CACHE_PATH", instantiated_cache_path, 1);
   setenv("LDCS_INSTANTIATED_FIFO_PATH", instantiated_fifo_path, 1);
   setenv("LDCS_INSTANTIATED_DAEMON_PATH", instantiated_daemon_path, 1);
   setenv("LDCS_NUMBER", number_s, 1);
   setenv("LDCS_RANKINFO", rankinfo_str, 1);
   if (connection_str)
      setenv("LDCS_CONNECTION", connection_str, 1);
   setenv("LDCS_OPTIONS", opts_s, 1);
   setenv("LDCS_CACHESIZE", cachesize_s, 1);
   setenv("LDCS_BOOTSTRAPPED", "1", 1);
   setenv("SPINDLE", "true", 1);
   if (opts & OPT_SUBAUDIT) {
      char *preload_str = spindle_interceptlib;
      char *preload_env = getenv("LD_PRELOAD");
      char *preload;
      if (preload_env) {
         size_t len = strlen(preload_env) + strlen(preload_str) + 2;
         preload = malloc(len);
         snprintf(preload, len, "%s %s", preload_str, preload_env);
      }
      else {
         preload = preload_str;
      }
      setenv("LD_PRELOAD", preload, 1);
      
      if (preload_env) {
         free(preload);
      }
   }
}

static int parse_cmdline(int argc, char *argv[])
{
   int i, daemon_arg_count;
   if (argc < 5)
      return -1;
   
   i = 1;
   if (strcmp(argv[1], "-daemon_args") == 0) {
      debug_printf("Parsing daemon args out of bootstrap launch line\n");
      daemon_arg_count = atoi(argv[2]);
      daemon_args = malloc(sizeof(char *) * (daemon_arg_count + 2));
      daemon_args[0] = spindle_daemon;
      for (i = 4; i < 3 + daemon_arg_count; i++)
         daemon_args[i - 3] = argv[i];
      daemon_args[i - 3] = NULL;
   }

   symbolic_location = argv[i++];
   cache_path = argv[i++];
   fifo_path = argv[i++];
   daemon_path = argv[i++];
   number_s = argv[i++];
   number = atoi(number_s);
   opts_s = argv[i++];
   opts = atol(opts_s);
   cachesize_s = argv[i++];
   cachesize = atoi(cachesize_s);
   cmdline = argv + i;
   assert(i < argc);

   return 0;
}

static void launch_daemon()
{
   /*grand-child fork, then execv daemon.  By grand-child forking we ensure that
     the app won't get confused by seeing an unknown process as a child. */
   pid_t child, gchild;
   int status, result;
   int fd;
   char unique_file[MAX_PATH_LEN+1];
   char buffer[32];

   snprintf(unique_file, MAX_PATH_LEN, "%s/spindle_daemon_pid", instantiated_daemon_path);
   unique_file[MAX_PATH_LEN] = '\0';
   fd = open(unique_file, O_CREAT | O_EXCL | O_WRONLY, 0600);
   if (fd == -1) {
      debug_printf("Not starting daemon -- %s already exists\n", unique_file);
      return;
   }
   debug_printf("Client is spawning daemon\n");
      
   child = fork();
   if (child == 0) {
      gchild = fork();
      if (gchild != 0) {
         snprintf(buffer, sizeof(buffer), "%d\n", getpid());
         (void)! write(fd, buffer, strlen(buffer));
         close(fd);
         exit(0);
      }
      close(fd);
      result = setpgid(0, 0);
      if (result == -1) {
         err_printf("Failed to setpgid: %s\n", strerror(errno));
      }
      execv(spindle_daemon, daemon_args);
      fprintf(stderr, "Spindle error: Could not execv daemon %s\n", daemon_args[0]);
      exit(-1);
   }
   else if (child > 0) {
      close(fd);
      waitpid(child, &status, 0);
   }
}

static void get_executable()
{
   int errcode = 0;
   if (!(opts & OPT_RELOCAOUT) || (opts & OPT_REMAPEXEC)) {
      debug_printf3("Using default executable %s\n", *cmdline);
      executable = *cmdline;
      return;
   }

   debug_printf2("Sending request for executable %s\n", *cmdline);
   exec_pathsearch(ldcsid, *cmdline, &executable, &errcode);

   if (executable == NULL) {
      executable = *cmdline;
      err_printf("Failed to relocate executable %s\n", executable);
   }
   else {
      debug_printf("Relocated executable %s to %s\n", *cmdline, executable);
      chmod(executable, 0700);
   }
}

static void adjust_script()
{
   int result;
   char **new_cmdline;
   char *new_executable;

   if (!(opts & OPT_RELOCAOUT)) {
      return;
   }

   if (!executable)
      return;

   result = adjust_if_script(*cmdline, executable, cmdline, &new_executable, &new_cmdline);
   if (result != 0)
      return;

   cmdline = new_cmdline;
   executable = new_executable;
}

static void get_clientlib()
{
   char *default_libstr = (opts & OPT_SUBAUDIT) ? default_subaudit_libstr : default_audit_libstr;
   int errorcode;
   
   if (!(opts & OPT_RELOCAOUT)) {
      debug_printf3("Using default client_lib %s\n", default_libstr);
      client_lib = default_libstr;
      return;
   }

   get_relocated_file(ldcsid, default_libstr, &client_lib, &errorcode);
   if (client_lib == NULL) {
      client_lib = default_libstr;
      err_printf("Failed to relocate client library %s\n", default_libstr);
   }
   else {
      debug_printf2("Relocated client library %s to %s\n", default_libstr, client_lib);
      chmod(client_lib, 0600);
   }
}

void test_log(const char *name)
{
}

/**
 * Are you staring at this strange code after running TotalView (or another debugger)?
 *
 * If so, you're currently stopped at spindle's bootstrapper, which is providing scalable program launch on this system. 
 * Go ahead and hit 'go'. Your regularly scheduled program will resume shortly. 
 **/
int main(int argc, char *argv[])
{
   int error, result;
   char **j, *spindle_env;

   LOGGING_INIT_PREEXEC("Client");

   spindle_env = getenv("SPINDLE");
   if (spindle_env) {
      if (strcasecmp(spindle_env, "false") == 0 || strcmp(spindle_env, "0") == 0) {
         debug_printf("Turning off spindle from bootstrapper because SPINDLE is %s\n", spindle_env);
         execvp(cmdline[0], cmdline);
         fprintf(stderr, "%s: Command not found.\n", cmdline[0]);
         return -1;
      }
   }

   debug_printf("Launched Spindle Bootstrapper\n");

   result = parse_cmdline(argc, argv);
   if (result == -1) {
      fprintf(stderr, "spindle_boostrap cannot be invoked directly\n");
      return -1;
   }

   debug_printf("QQQ Candidate cache paths:  %s:%s\n", cache_path, symbolic_location );
   instantiated_cache_path  = instatiate_directory( cache_path, location );
   if( NULL == cache_path ){
        fprintf( stderr, "None of the following cache path directory candidates could be instantiated.\n");
        fprintf( stderr, "%s:%s\n", cache_path, symbolic_location );
        exit(-1);
   }else{
       debug_printf("QQQ Instantiated cache path:  %s\n", instantiated_cache_path);
   }

   debug_printf("QQQ Candidate fifo paths:   %s:%s\n", fifo_path, symbolic_location );
   instantiated_fifo_path  = instatiate_directory( fifo_path, location );
   if( NULL == fifo_path ){
        fprintf( stderr, "None of the following fifo path directory candidates could be instantiated.\n");
        fprintf( stderr, "%s:%s\n", fifo_path, symbolic_location );
        exit(-1);
   }else{
       debug_printf("QQQ Instantiated fifo path:  %s\n", instantiated_cache_path);
   }

   debug_printf("QQQ Candidate daemon paths: %s:%s\n", daemon_path, symbolic_location );
   instantiated_daemon_path  = instatiate_directory( daemon_path, location );
   if( NULL == daemon_path ){
        fprintf( stderr, "None of the following daemon path directory candidates could be instantiated.\n");
        fprintf( stderr, "%s:%s", daemon_path, symbolic_location );
        exit(-1);
   }else{
       debug_printf("QQQ Instantiated cache path:  %s\n", instantiated_cache_path);
   }

   if (daemon_args) {
      launch_daemon();
   }

   if (opts & OPT_RELOCAOUT) {
      result = establish_connection();
      if (result == -1) {
         err_printf("spindle_bootstrap failed to connect to daemons\n");
         return -1;
      }
   }

   if ((opts & OPT_SHMCACHE) && cachesize) {
      unsigned int shm_cache_limit;
      cachesize *= 1024;
#if defined(COMM_BITER)
      shm_cache_limit = cachesize > 512*1024 ? cachesize - 512*1024 : 0;
#else
      shm_cache_limit = cachesize;
#endif
      shmcache_init(instantiated_cache_path, number, cachesize, shm_cache_limit);
      use_cache = 1;
   }      
   
   get_executable();
   get_clientlib();
   adjust_script();
   
   /**
    * Exec setup
    **/
   debug_printf("Spindle bootstrap launching: ");
   if (!executable) {
      bare_printf("<no executable given>");
   }
   else {
      bare_printf("%s.  Args:  ", executable);
      for (j = cmdline; *j; j++) {
         bare_printf("%s ", *j);
      }
   }
   bare_printf("\n");

   /**
    * Exec the user's application.
    **/
   setup_environment();
   execvp(executable, cmdline);

   /**
    * Exec error handling
    **/
   error = errno;
   err_printf("Error execing app: %s\n", strerror(error));

   return -1;
}
