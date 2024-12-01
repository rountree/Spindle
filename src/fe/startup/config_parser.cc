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

#include "config_parser.h"
#include "config_mgr.h"
#include "spindle_debug.h"

#include <cstring>
#include <fstream>
#include <sstream>

using namespace std;

ConfigFile::ConfigFile(string filename_) :
   filename(filename_),
   open_error(false),
   parse_error(false),
   configmap(filename_)
   
{
   parse();
}

ConfigFile::~ConfigFile() {
}

bool ConfigFile::parseError() const {
   return parse_error;
}

bool ConfigFile::openError() const {
   return open_error;
}

string ConfigFile::errorString() const {
   return err_string;
}

const ConfigMap& ConfigFile::getConfigMap() const {
   return configmap;
}


static string trim(string s)
{
   const char *whitespace(" \t\n\r\f");
   s.erase(s.find_last_not_of(whitespace) + 1);
   s.erase(0, s.find_first_not_of(whitespace));
   return s;
}

bool ConfigFile::parseLine(string line, int lineno) {
   if (line.front() == '#')
      return true;
   if (line.empty())
      return true;
   
   string::size_type equals = line.find('=');
   if (equals == string::npos) {
      stringstream ss(ios_base::in | ios_base::out);
      parse_error = true;
      ss << "Config parse error in " << filename << ":" << lineno << ". No '=' character found in line. All lines must be of form 'name = value'";
      err_string = ss.str();
      err_printf("%s\n", err_string.c_str());
      return false;
   }
   string name = trim(line.substr(0, equals));
   string value = trim(line.substr(equals+1));

   if (name.empty()) {
      stringstream ss(ios_base::in | ios_base::out);
      parse_error = true;
      ss << "Config parse error in " << filename << ":" << lineno << ". Line is not in 'name = value' format.\n";
      err_string = ss.str();
      err_printf("%s\n", err_string.c_str());
      return false;
   }
   if (value.empty()) {
      debug_printf3("Ignoring empty value in config file %s:%d for '%s'\n", filename.c_str(), lineno, name.c_str());
      return true;
   }

   const map<string, const SpindleOption&> &optsbylong = getOptionsByLongName();
   const map<string, const SpindleOption&>::const_iterator i = optsbylong.find(name);
   if (i == optsbylong.end()) {
      stringstream ss(ios_base::in | ios_base::out);
      ss << "Parse error at " << filename << ":" << lineno << " - Unknown option " << name << "\n";
      err_string = ss.str();
      err_printf("%s\n", err_string.c_str());
      return false;
   }
   const SpindleOption &opt = i->second;

   string local_errstring;
   bool result = configmap.set(opt.config_id, value, local_errstring);
   if (!result) {
      stringstream ss(ios_base::in | ios_base::out);
      ss << "Parse error at " << filename << ":" << lineno << " - " << local_errstring;
      err_string = ss.str();
      err_printf("%s\n", err_string.c_str());
      return false;
   }
      
   return true;
}

void ConfigFile::parse() {
   ifstream file(filename);
   if (!file.is_open()) {
      open_error = true;
      err_string = "File " + filename + " not found or not readable";
      debug_printf2("Config file %s not found\n", filename.c_str());
      return;
   }

   string prev_incomplete_line;
   int lineno = 0;
   while (!file.eof()) {
      lineno++;
      
      string line;
      getline(file, line);
      if (line.empty() && file.eof())
         break;
      line = trim(line);

      if (!prev_incomplete_line.empty()) {
         line = prev_incomplete_line + line;
         prev_incomplete_line = "";
      }

      if (line.back() == '\\') {
         line.pop_back();
         prev_incomplete_line = line;
         continue;
      }

      bool result = parseLine(line, lineno);
      if (!result) {
         debug_printf("Aborting config parsing of %s due to parse error\n", filename.c_str());
         return;
      }
   }

   if (!prev_incomplete_line.empty()) {
      stringstream ss(ios_base::in | ios_base::out);
      parse_error = true;
      ss << "Config parse error in " << filename << ":" << lineno << ". Unexpected End-of-File.";
      err_string = ss.str();
      err_printf("%s\n", err_string.c_str());
      return;
   }
}
