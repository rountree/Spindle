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

#if !defined(LAUNCHER_H_)
#define LAUNCHER_H_

#include <set>
#include <vector>
#include <utility>
#include <map>

#include "spindle_launch.h"
#include "config_mgr.h"
#include <unistd.h>

typedef unsigned long app_id_t;

class Launcher 
{
protected:
   spindle_args_t *params;
   ConfigMap &config;
   int jobfinish_write_fd;
   int jobfinish_read_fd;
   int daemon_argc;
   char **daemon_argv;

   void markFinished();
   virtual bool spawnDaemon() = 0;
public:
   Launcher(spindle_args_t *params_, ConfigMap &config_);
   virtual ~Launcher();
   bool setupDaemons();
   virtual bool setupJob(app_id_t id, int &app_argc, char** &app_argv);
   virtual const char **getProcessTable() = 0;
   virtual const char *getDaemonArg() = 0;
   virtual void getSecondaryDaemonArgs(std::vector<const char *> &secondary_args);
   virtual bool getReturnCodes(bool &daemon_done, int &daemon_ret,
                               std::vector<std::pair<app_id_t, int> > &app_rets) = 0;
   virtual bool spawnJob(app_id_t id, int app_argc, char **app_argv) = 0;
   int getJobFinishFD();
   bool runSpindleFE();
}; 

class ForkLauncher : public Launcher
{
   friend void on_child_forklauncher(int sig);
  protected:
   pid_t daemon_pid;
   std::map<pid_t, app_id_t> app_pids;
   static ForkLauncher *flauncher;
  public:
   ForkLauncher(spindle_args_t *params_, ConfigMap &config_);
   virtual ~ForkLauncher();   virtual bool getReturnCodes(bool &daemon_done, int &daemon_ret,
                               std::vector<std::pair<app_id_t, int> > &app_rets);
};

class PassthroughLauncher : public ForkLauncher
{
protected:
   PassthroughLauncher(spindle_args_t *params_, ConfigMap &config_);
   virtual bool spawnDaemon();
public:
   friend Launcher *createPassthroughLauncher(spindle_args_t *params, ConfigMap &config);
   virtual const char **getProcessTable();
   virtual const char *getDaemonArg();
   virtual void getSecondaryDaemonArgs(std::vector<const char *> &secondary_args);
   virtual bool getReturnCodes(bool &daemon_done, int &daemon_ret,
                               std::vector<std::pair<app_id_t, int> > &app_rets);
   virtual bool spawnJob(app_id_t id, int app_argc, char **app_argv);
   virtual bool setupJob(app_id_t id, int &app_argc, char** &app_argv);   
   virtual ~PassthroughLauncher();
};

#endif
