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

#include <string>
#include <vector>
#include <set>

using namespace std;

#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <assert.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "spindle_launch.h"
#include "spindle_debug.h"
#include "../startup/launcher.h"

#if !defined(LIBEXEC)
#error Expected libexec to be defined
#endif
static const char libexec_dir[] = LIBEXEC "/";

class HostbinLauncher : public ForkLauncher {
   friend Launcher *createHostbinLauncher(spindle_args_t *params, ConfigMap &config);
   friend void hostbin_on_child(int);
private:
   static HostbinLauncher *hlauncher;
   char **host_array;
   bool initError;
   bool useRSH;
   pid_t launcher_pid;
protected:
   virtual bool spawnDaemon();
   HostbinLauncher(spindle_args_t *args, ConfigMap &config_);
public:
   virtual bool spawnJob(app_id_t id, int app_argc, char **app_argv);
   virtual const char **getProcessTable();
   virtual const char *getDaemonArg();
   virtual void getSecondaryDaemonArgs(std::vector<const char *> &secondary_args);
   virtual bool getReturnCodes(bool &daemon_done, int &daemon_ret,
                               std::vector<std::pair<app_id_t, int> > &app_rets);
   virtual ~HostbinLauncher();   
};
HostbinLauncher *HostbinLauncher::hlauncher = NULL;

Launcher *createHostbinLauncher(spindle_args_t *params, ConfigMap &config) {
   HostbinLauncher::hlauncher = new HostbinLauncher(params, config);
   if (HostbinLauncher::hlauncher->initError) {
      delete HostbinLauncher::hlauncher;
      return NULL;
   }
   return HostbinLauncher::hlauncher;
}

HostbinLauncher::HostbinLauncher(spindle_args_t *params_, ConfigMap &config_) :
   ForkLauncher(params_, config_),
   host_array(NULL),
   initError(false),
   launcher_pid(0)
{
   useRSH = params_->opts & OPT_RSHLAUNCH;   
}

HostbinLauncher::~HostbinLauncher()
{
   HostbinLauncher::hlauncher = NULL;
}

bool HostbinLauncher::spawnDaemon()
{
   if (useRSH)
      return true;
   fprintf(stderr, "ERROR: Hostbin launcher must be built/run with RSH launching enabled\n");
   return false;
}

static bool fileExists(string f)
{
   struct stat buf;
   int result = stat(f.c_str(), &buf);
   return result != -1;
}

bool HostbinLauncher::spawnJob(app_id_t id, int /*app_argc*/, char **app_argv)
{
   launcher_pid = fork();
   if (launcher_pid == -1) {
      fprintf(stderr, "Spindle error: Failed to fork process for job launcher: %s\n", strerror(errno));
      exit(-1);
   }
   if (launcher_pid == 0) {
      execvp(app_argv[0], app_argv);
      char *errstr = strerror(errno);
      fprintf(stderr, "Spindle error: Could not invoke job launcher %s: %s\n", app_argv[0], errstr);
      exit(-1);
   }
   app_pids[launcher_pid] = id;
   return true;
}

const char **HostbinLauncher::getProcessTable()
{
   if (host_array) {
      return const_cast<const char**>(host_array);
   }

   pair<bool, string> gethostbin = config.getValueString(confHostbin);
   if (!gethostbin.first) {
      fprintf(stderr, "Spindle Error: Hostbin requested, but hostbin path unset\n");
      err_printf("Hostbin path unset\n");
   }
   string hostbin_exe = gethostbin.second;
   
   if (hostbin_exe.find('/') == string::npos) {
      string fullpath = string(libexec_dir) + hostbin_exe;
      if (fileExists(fullpath))
         hostbin_exe = fullpath;
   }

   FILE *hostbin_file = popen(hostbin_exe.c_str(), "r");
   if (!hostbin_file) {
      int error = errno;
      err_printf("Failed to run hostbin %s: %s\n", hostbin_exe.c_str(), strerror(error));
      fprintf(stderr, "ERROR: Hostbin launch of %s failed: %s\n", hostbin_exe.c_str(), strerror(error));
      return NULL;
   }

   list<string> hosts;
   int result = 0;
   while (!feof(hostbin_file) && result != -1) {
      char hostname[256];
      
      result = fscanf(hostbin_file, "%255s", hostname);
      if (result > 0) {
         hostname[255] = '\0';
         hosts.push_back(hostname);
      }
   }

   pclose(hostbin_file);

   host_array = (char **) malloc(sizeof(char *) * (hosts.size()+1));
   int j = 0;
   for (list<string>::iterator i = hosts.begin(); i != hosts.end(); i++, j++) {
      debug_printf2("Hostbin host %d = %s\n", j, i->c_str());
      host_array[j] = strdup(i->c_str());
   }
   host_array[j] = NULL;
   
   return const_cast<const char**>(host_array);      
}

const char *HostbinLauncher::getDaemonArg()
{
   return "--spindle_hostbin";
}

void HostbinLauncher::getSecondaryDaemonArgs(vector<const char *> &secondary_args)
{
   char port_str[32], ss_str[32], port_num_str[32];
   snprintf(port_str, 32, "%d", params->port);
   snprintf(port_num_str, 32, "%d", params->num_ports);
   snprintf(ss_str, 32, "%lu", params->unique_id);
   secondary_args.push_back(strdup(port_str));
   secondary_args.push_back(strdup(port_num_str));
   secondary_args.push_back(strdup(ss_str));
}

bool HostbinLauncher::getReturnCodes(bool &daemon_done, int &daemon_ret,
                                     std::vector<std::pair<app_id_t, int> > &app_rets)
{   
   if (!daemon_pid && useRSH) {
      daemon_pid = getRSHPidFE();
      debug_printf3("Marked daemon pid as %d\n", (int) daemon_pid);
      markRSHPidReapedFE();
   }
   return ForkLauncher::getReturnCodes(daemon_done, daemon_ret, app_rets);
}
