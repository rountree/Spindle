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

#include "config.h"
#include "config_mgr.h"
#include <iostream>
#include <cstdlib>
#include <cstring>
#include <algorithm>

static void print_version()
{
   std::cout << "spindle-config-helper version " << PACKAGE_VERSION << std::endl;
}

static void print_usage(const char *progname)
{
   std::cout << "Usage: " << progname << " [OPTIONS]\n\n"
             << "Parse Spindle configuration from command line, environment variables, and\n"
             << "configuration files, then output the resulting configuration in spindle.conf format.\n\n"
             << "Options:\n"
             << "  -h, --help              Display this help message and exit\n"
             << "  -V, --version           Display version information and exit\n"
             << "  --usage                 Display brief usage summary and exit\n\n"
             << "All Spindle configuration options are supported. See spindle-config-helper(1)\n"
             << "for details, or use --usage for a complete list of options.\n\n"
             << "Examples:\n"
             << "  " << progname << " --commpaths=/dev/shm --cachepaths=/tmp\n"
             << "  " << progname << " --config=/etc/spindle.conf\n"
             << "  " << progname << " --launcher=slurm --security=none\n"
             << std::endl;
}

static void format_config_value(const ConfigMap &confmap, SpindleConfigID id,
                                 const SpindleOption &opt)
{
   if (!confmap.isSet(id)) {
      return;
   }

   std::string value;
   bool has_value = false;

   switch (opt.conftype) {
      case cvBool: {
         auto result = confmap.getValueBool(id);
         if (result.first) {
            value = result.second ? "yes" : "no";
            has_value = true;
         }
         break;
      }
      case cvInteger: {
         auto result = confmap.getValueIntegral(id);
         if (result.first) {
            value = std::to_string(result.second);
            has_value = true;
         }
         break;
      }
      case cvString:
      case cvStringOptional:
      case cvList:
      case cvEnum: {
         auto result = confmap.getValueString(id);
         if (result.first) {
            value = result.second;
            has_value = true;
         }
         break;
      }
   }

   if (has_value && opt.cmdline_long) {
      // Convert cmdline option name to config file format
      // e.g., "comm-paths" -> "commpaths"
      std::string key = opt.cmdline_long;
      // Remove dashes using erase-remove idiom
      key.erase(std::remove_if(key.begin(), key.end(),
                               [](char c) { return c == '-'; }),
                key.end());

      std::cout << key << " = " << value << std::endl;
   }
}

int main(int argc, char *argv[])
{
   // Handle -h/--help and -V/--version before config parsing
   for (int i = 1; i < argc; i++) {
      if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
         print_usage(argv[0]);
         return 0;
      }
      if (strcmp(argv[i], "-V") == 0 || strcmp(argv[i], "--version") == 0) {
         print_version();
         return 0;
      }
   }

   // Check if application command was provided (after '--')
   bool has_app = false;
   for (int i = 1; i < argc; i++) {
      if (strcmp(argv[i], "--") == 0 && i + 1 < argc) {
         has_app = true;
         break;
      }
   }

   // If no application provided, add a dummy one for the parser
   char **modified_argv = argv;
   int modified_argc = argc;
   char *dummy_app = nullptr;

   if (!has_app) {
      dummy_app = strdup("/bin/true");
      modified_argv = new char*[argc + 2];
      for (int i = 0; i < argc; i++) {
         modified_argv[i] = argv[i];
      }
      modified_argv[argc] = strdup("--");
      modified_argv[argc + 1] = dummy_app;
      modified_argc = argc + 2;
   }

   ConfigMap confmap;
   std::string errmsg;

   // Reuse Spindle's existing parsing: cmdline, env vars, config files
   // Pass false for inExecutable since we're not the spindle launcher
   if (!gatherAllConfigInfo(modified_argc, modified_argv, false, confmap, errmsg)) {
      std::cerr << "Error: " << errmsg << std::endl;
      if (!has_app) {
         free(dummy_app);
         free(modified_argv[argc]);
         delete[] modified_argv;
      }
      return 1;
   }

   // Clean up temporary argv if we created one
   if (!has_app) {
      free(dummy_app);
      free(modified_argv[argc]);
      delete[] modified_argv;
   }

   // Output configuration in spindle.conf format
   const std::map<SpindleConfigID, const SpindleOption&> &options = getOptionsByID();

   for (const auto &pair : options) {
      format_config_value(confmap, pair.first, pair.second);
   }

   return 0;
}
