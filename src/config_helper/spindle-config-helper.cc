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
#include <fstream>
#include <sstream>
#include <vector>
#include <set>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <limits.h>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wordexp.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <time.h>
#include <dirent.h>
#include <errno.h>
#include <mpi.h>

extern "C" {
#include "keyfile.h"
}

struct MountPoint {
   std::string path;
   std::string fstype;
   bool is_remote;
   uint64_t avail_gb;

   MountPoint(const std::string& p, const std::string& fs)
      : path(p), fstype(fs), is_remote(false), avail_gb(0) {}
};

static bool is_remote_filesystem(const std::string& fstype)
{
   static const char* remote_types[] = {
      "nfs", "nfs4", "lustre", "gpfs", "panfs",
      "ceph", "glusterfs", "afs", "smbfs", "cifs",
      nullptr
   };

   for (int i = 0; remote_types[i]; i++) {
      if (fstype == remote_types[i] ||
          fstype.find(std::string("fuse.") + remote_types[i]) == 0) {
         return true;
      }
   }
   return false;
}

static bool is_kernel_filesystem(const std::string& path)
{
   // Hardcode known kernel filesystem paths
   // Note: /dev/shm is NOT a kernel filesystem, it's tmpfs
   return path == "/proc" || path == "/sys" ||
          (path.find("/proc/") == 0) ||
          (path.find("/sys/") == 0) ||
          (path == "/dev" || (path.find("/dev/") == 0 && path != "/dev/shm"));
}

static bool should_ignore_mount(const std::string& path, const std::string& fstype)
{
   // Ignore virtual/system filesystems
   if (fstype == "proc" || fstype == "sysfs" || fstype == "devtmpfs" ||
       fstype == "devpts" || fstype == "securityfs" || fstype == "pstore" ||
       fstype == "autofs" || fstype == "mqueue" || fstype == "hugetlbfs" ||
       fstype.find("cgroup") == 0 || fstype.find("bpf") == 0) {
      return true;
   }

   // Ignore system tmpfs mounts (but NOT /dev/shm which is useful)
   if (fstype == "tmpfs" && (path == "/run" || path == "/sys/fs/cgroup" ||
                              path.find("/run/") == 0 || path.find("/sys/") == 0 ||
                              (path.find("/dev/") == 0 && path != "/dev/shm"))) {
      return true;
   }

   return false;
}

static uint64_t get_available_gb(const std::string& path)
{
   struct statvfs stat;
   if (statvfs(path.c_str(), &stat) != 0) {
      return 0;
   }

   uint64_t avail_bytes = (uint64_t)stat.f_bavail * stat.f_frsize;
   return avail_bytes / (1024ULL * 1024 * 1024);
}

static std::vector<MountPoint> parse_mounts()
{
   std::vector<MountPoint> mounts;
   std::ifstream mounts_file("/proc/mounts");
   std::string line;
   std::set<std::string> seen_paths;

   while (std::getline(mounts_file, line)) {
      std::istringstream iss(line);
      std::string device, path, fstype, options;

      if (!(iss >> device >> path >> fstype >> options)) {
         continue;
      }

      if (should_ignore_mount(path, fstype)) {
         continue;
      }

      // Skip duplicate paths
      if (seen_paths.count(path)) {
         continue;
      }
      seen_paths.insert(path);

      MountPoint mp(path, fstype);

      // Overlay filesystems are typically local (container backing storage)
      if (fstype == "overlay") {
         mp.is_remote = false;
      } else {
         mp.is_remote = is_remote_filesystem(fstype);
      }

      if (!mp.is_remote) {
         mp.avail_gb = get_available_gb(path);

         // Skip local mounts with < 1GB available
         if (mp.avail_gb < 1) {
            continue;
         }

         // Check if writable
         if (access(path.c_str(), W_OK) != 0) {
            continue;
         }
      }

      mounts.push_back(mp);
   }

   // Sort by path length (longest first) for prefix matching
   std::sort(mounts.begin(), mounts.end(),
             [](const MountPoint& a, const MountPoint& b) {
                return a.path.length() > b.path.length();
             });

   return mounts;
}

enum ExpandResult {
   EXPAND_OK,
   EXPAND_UNDEFINED_VAR,
   EXPAND_EMPTY,
   EXPAND_ERROR
};

struct ExpandedPath {
   std::string expanded;
   ExpandResult result;
};

static ExpandedPath expand_path(const std::string& path)
{
   ExpandedPath ep;

   if (path.empty()) {
      ep.expanded = path;
      ep.result = EXPAND_EMPTY;
      return ep;
   }

   // Use wordexp for shell expansion (handles $VAR, ~, etc.)
   wordexp_t exp_result;
   int ret = wordexp(path.c_str(), &exp_result, WRDE_NOCMD | WRDE_UNDEF);

   if (ret == WRDE_BADVAL) {
      // Undefined variable
      ep.expanded = "";
      ep.result = EXPAND_UNDEFINED_VAR;
      return ep;
   } else if (ret != 0) {
      // Other expansion error
      ep.expanded = "";
      ep.result = EXPAND_ERROR;
      return ep;
   }

   if (exp_result.we_wordc > 0 && exp_result.we_wordv[0] && exp_result.we_wordv[0][0] != '\0') {
      ep.expanded = exp_result.we_wordv[0];
      ep.result = EXPAND_OK;
   } else {
      // Empty expansion
      ep.expanded = "";
      ep.result = EXPAND_EMPTY;
   }

   wordfree(&exp_result);
   return ep;
}

static std::string resolve_real_path(const std::string& path)
{
   if (path.empty()) {
      return path;
   }

   char resolved[PATH_MAX];
   if (realpath(path.c_str(), resolved) != nullptr) {
      return std::string(resolved);
   }

   // If realpath fails, try to handle relative paths manually
   if (path[0] != '/') {
      char cwd[PATH_MAX];
      if (getcwd(cwd, sizeof(cwd))) {
         std::string abs = std::string(cwd) + "/" + path;
         if (realpath(abs.c_str(), resolved) != nullptr) {
            return std::string(resolved);
         }
         return abs;  // Return constructed absolute path even if realpath fails
      }
   }

   return path;  // Return original if we can't resolve
}

static const MountPoint* find_mount_for_path(const std::string& resolved_path,
                                               const std::vector<MountPoint>& mounts)
{
   if (resolved_path.empty()) {
      return nullptr;
   }

   // Find longest matching mount point
   for (const auto& mp : mounts) {
      if (resolved_path == mp.path ||
          (resolved_path.find(mp.path + "/") == 0)) {
         return &mp;
      }
   }

   return nullptr;
}

struct PathAnalysis {
   std::string original;
   std::string expanded;
   std::string resolved;
   bool is_ok;
   std::vector<std::string> warnings;
   const MountPoint* mount;

   PathAnalysis(const std::string& orig) : original(orig), is_ok(false), mount(nullptr) {}
};

static std::string find_writable_parent(const std::string& path)
{
   // Find the first existing parent directory
   std::string test_path = path;

   while (!test_path.empty() && test_path != "/") {
      if (access(test_path.c_str(), F_OK) == 0) {
         // Path exists, check if writable
         return test_path;
      }

      // Move to parent
      size_t last_slash = test_path.rfind('/');
      if (last_slash == std::string::npos) {
         break;
      }
      test_path = test_path.substr(0, last_slash);
      if (test_path.empty()) {
         test_path = "/";
      }
   }

   return "/";
}

static PathAnalysis analyze_path(const std::string& path, const std::vector<MountPoint>& mounts,
                                   bool check_for_python = false)
{
   PathAnalysis analysis(path);

   if (path.empty()) {
      analysis.warnings.push_back("Path is empty");
      return analysis;
   }

   // Expand variables
   ExpandedPath ep = expand_path(path);
   analysis.expanded = ep.expanded;

   if (ep.result == EXPAND_UNDEFINED_VAR) {
      analysis.warnings.push_back("Variable is not set");
      return analysis;
   } else if (ep.result == EXPAND_EMPTY) {
      analysis.warnings.push_back("Variable expands to empty string");
      return analysis;
   } else if (ep.result == EXPAND_ERROR) {
      analysis.warnings.push_back("Cannot expand path");
      return analysis;
   }

   // Check for empty segments (e.g., /var// from /var/$EMPTY/)
   if (analysis.expanded.find("//") != std::string::npos) {
      analysis.warnings.push_back("Path contains empty segment (variable may be empty)");
   }

   // Resolve to real path
   analysis.resolved = resolve_real_path(ep.expanded);

   // Check for kernel filesystems first (these are always OK)
   if (is_kernel_filesystem(analysis.resolved)) {
      analysis.is_ok = true;
      return analysis;
   }

   // Check if path exists
   bool path_exists = false;
   struct stat st;
   if (stat(analysis.resolved.c_str(), &st) == 0) {
      path_exists = true;
   } else {
      // For non-python paths, check if parent is writable
      if (!check_for_python) {
         std::string parent = find_writable_parent(analysis.resolved);
         bool parent_writable = (access(parent.c_str(), W_OK) == 0);

         if (parent_writable) {
            analysis.warnings.push_back("Path does not exist (will be created by Spindle)");
         } else {
            analysis.warnings.push_back("Path does not exist and parent directory is not writable");
            analysis.is_ok = false;
            return analysis;
         }
      } else {
         // Python prefix must exist
         analysis.warnings.push_back("Path does not exist");
         return analysis;
      }
   }

   // Special validation for Python prefix
   if (check_for_python && path_exists) {
      // Check for bin/python or bin/python3
      std::string python_path = analysis.resolved + "/bin/python";
      std::string python3_path = analysis.resolved + "/bin/python3";

      bool has_python = (access(python_path.c_str(), X_OK) == 0);
      bool has_python3 = (access(python3_path.c_str(), X_OK) == 0);

      if (!has_python && !has_python3) {
         analysis.warnings.push_back("No bin/python or bin/python3 found (not a Python installation?)");
      }
   }

   // Find mount point (only for cache/comm paths, not python prefix)
   if (!check_for_python) {
      analysis.mount = find_mount_for_path(analysis.resolved, mounts);

      if (!analysis.mount) {
         analysis.warnings.push_back("Cannot determine filesystem type");
         return analysis;
      }

      if (analysis.mount->is_remote) {
         analysis.warnings.push_back("Path is on REMOTE filesystem (" + analysis.mount->fstype + ")");
         return analysis;
      }
   }

   // All checks passed (or acceptable warnings)
   if (analysis.warnings.empty()) {
      analysis.is_ok = true;
   }

   return analysis;
}

static void print_filesystem_recommendations(const std::vector<MountPoint>& mounts,
                                              const ConfigMap& confmap)
{
   std::cout << "# Spindle filesystem recommendations:" << std::endl;

   // Show local writable mounts
   std::vector<const MountPoint*> local_mounts;
   for (const auto& mp : mounts) {
      if (!mp.is_remote) {
         local_mounts.push_back(&mp);
      }
   }

   if (!local_mounts.empty()) {
      std::cout << "#   Local: ";
      bool first = true;
      for (const auto* mp : local_mounts) {
         if (!first) std::cout << ", ";
         std::cout << mp->path << " (" << mp->fstype << ", "
                   << mp->avail_gb << "GB)";
         first = false;
      }
      std::cout << std::endl;
   } else {
      std::cout << "#   WARNING: No suitable local filesystems found!" << std::endl;
   }

   std::cout << "#" << std::endl;

   // Analyze each configured option
   auto analyze_option = [&](SpindleConfigID config_id, const char* name, bool check_python = false) {
      if (!confmap.isSet(config_id)) {
         return;
      }

      auto result = confmap.getValueString(config_id);
      if (!result.first || result.second.empty()) {
         return;
      }

      std::string paths = result.second;
      std::vector<PathAnalysis> analyses;

      // Split on colons for multi-path options
      size_t pos = 0;
      while (pos < paths.length()) {
         size_t end = paths.find(':', pos);
         if (end == std::string::npos) {
            end = paths.length();
         }

         std::string path = paths.substr(pos, end - pos);
         if (!path.empty()) {
            analyses.push_back(analyze_path(path, mounts, check_python));
         }

         pos = end + 1;
      }

      if (analyses.empty()) {
         return;
      }

      std::cout << "#   " << name << " analysis:" << std::endl;

      for (const auto& analysis : analyses) {
         const char* status = analysis.is_ok ? "OK" : "WARNING";
         std::cout << "#     " << status << ": " << analysis.original;

         if (!analysis.resolved.empty()) {
            std::cout << " -> " << analysis.resolved;
            if (analysis.mount && !analysis.mount->is_remote) {
               std::cout << " (" << analysis.mount->fstype << ", "
                        << analysis.mount->avail_gb << "GB)";
            } else if (analysis.mount && analysis.mount->is_remote) {
               std::cout << " (" << analysis.mount->fstype << ")";
            }
         } else if (!analysis.expanded.empty()) {
            std::cout << " -> " << analysis.expanded;
         } else {
            std::cout << " -> (empty)";
         }

         std::cout << std::endl;

         for (const auto& warning : analysis.warnings) {
            std::cout << "#       " << warning << std::endl;
         }
      }

      std::cout << "#" << std::endl;
   };

   analyze_option(confCommPaths, "commpaths");
   analyze_option(confCachePaths, "cachepaths");
   analyze_option(confPythonPrefix, "pythonprefix", true);
   analyze_option(confLocalPrefix, "localprefix");
}

static std::string get_timestamp()
{
   time_t now = time(nullptr);
   char buf[80];
   strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", localtime(&now));
   return std::string(buf);
}

static bool test_directory_writable(const std::string& path)
{
   // Try to create a test file in the directory
   std::string testfile = path + "/.spindle_config_test_" + std::to_string(getpid());

   int fd = open(testfile.c_str(), O_CREAT | O_WRONLY | O_EXCL, 0600);
   if (fd < 0) {
      return false;
   }

   close(fd);
   unlink(testfile.c_str());
   return true;
}

static bool validate_tmpdir_path(const std::string& path,
                                  const std::vector<MountPoint>& mounts,
                                  std::string& error_msg)
{
   // Resolve the path (in case it's a symlink)
   std::string resolved = resolve_real_path(path);

   // Check if path exists
   struct stat st;
   if (stat(resolved.c_str(), &st) != 0) {
      error_msg = "Path does not exist: " + path;
      if (resolved != path) {
         error_msg += " (resolves to " + resolved + ")";
      }
      return false;
   }

   // Check if it's a directory
   if (!S_ISDIR(st.st_mode)) {
      error_msg = "Not a directory: " + path;
      return false;
   }

   // Check if writable
   if (!test_directory_writable(resolved)) {
      error_msg = "Directory is not writable: " + path;
      return false;
   }

   // Check if on local filesystem
   const MountPoint* mp = find_mount_for_path(resolved, mounts);
   if (!mp) {
      error_msg = "Cannot determine filesystem type for: " + path;
      return false;
   }

   if (mp->is_remote) {
      error_msg = "Path is on REMOTE filesystem (" + mp->fstype + "): " + path;
      return false;
   }

   return true;
}

static std::string determine_best_path(const std::vector<MountPoint>& mounts,
                                        std::vector<std::string>& warnings)
{
   // Priority order: TMPDIR, TEMPDIR, TMP, then /tmp
   const char* env_vars[] = {"TMPDIR", "TEMPDIR", "TMP", nullptr};

   for (int i = 0; env_vars[i] != nullptr; i++) {
      const char* env_value = getenv(env_vars[i]);

      if (env_value && env_value[0] != '\0') {
         std::string tmpdir = env_value;
         std::string error_msg;

         if (validate_tmpdir_path(tmpdir, mounts, error_msg)) {
            return tmpdir;
         } else {
            warnings.push_back(std::string("$") + env_vars[i] + " is set to '" +
                             tmpdir + "' but not usable: " + error_msg);
         }
      }
   }

   // None of the environment variables are set or usable
   warnings.push_back("$TMPDIR, $TEMPDIR, and $TMP are not set or not usable");

   // Fall back to /tmp
   std::string error_msg;
   if (validate_tmpdir_path("/tmp", mounts, error_msg)) {
      return "/tmp";
   } else {
      warnings.push_back("/tmp is not usable: " + error_msg);
   }

   // No suitable path found
   return "";
}

static std::string get_python_prefix_from_which(const char* python_cmd)
{
   // Run 'which python' or 'which python3'
   std::string cmd = std::string("which ") + python_cmd + " 2>/dev/null";
   FILE* fp = popen(cmd.c_str(), "r");
   if (!fp) {
      return "";
   }

   char path[PATH_MAX];
   std::string result;
   if (fgets(path, sizeof(path), fp) != nullptr) {
      // Remove trailing newline
      size_t len = strlen(path);
      if (len > 0 && path[len-1] == '\n') {
         path[len-1] = '\0';
      }

      // Resolve symlinks
      char resolved[PATH_MAX];
      if (realpath(path, resolved) != nullptr) {
         // Get parent directory (strip /bin/python*)
         std::string full_path = resolved;
         size_t bin_pos = full_path.rfind("/bin/");
         if (bin_pos != std::string::npos) {
            result = full_path.substr(0, bin_pos);
         }
      }
   }
   pclose(fp);
   return result;
}

// Get Python paths from sys.prefix and sys.path
static std::vector<std::string> get_python_sys_paths()
{
   std::set<std::string> paths;

   // Run Python to get sys.prefix and sys.path
   const char* cmd = "python3 -c \"import sys; print(sys.prefix); print('\\n'.join(sys.path))\" 2>/dev/null";
   FILE* fp = popen(cmd, "r");
   if (!fp) {
      return {};
   }

   char line[PATH_MAX];
   while (fgets(line, sizeof(line), fp) != nullptr) {
      // Remove trailing newline
      size_t len = strlen(line);
      if (len > 0 && line[len-1] == '\n') {
         line[len-1] = '\0';
      }

      if (strlen(line) > 0) {
         // Check if this looks like a valid path
         if (line[0] == '/' && access(line, F_OK) == 0) {
            paths.insert(line);
         }
      }
   }
   pclose(fp);

   return std::vector<std::string>(paths.begin(), paths.end());
}

static std::vector<std::string> discover_python_prefixes()
{
   std::set<std::string> prefixes;

   // Priority 1: Check PYTHONPREFIX environment variable
   const char* pythonprefix_env = getenv("PYTHONPREFIX");
   if (pythonprefix_env && pythonprefix_env[0] != '\0') {
      // Parse colon-separated list
      std::string paths_str(pythonprefix_env);
      size_t start = 0;
      size_t end = paths_str.find(':');

      while (end != std::string::npos) {
         std::string path = paths_str.substr(start, end - start);
         if (!path.empty() && access(path.c_str(), F_OK) == 0) {
            prefixes.insert(path);
         }
         start = end + 1;
         end = paths_str.find(':', start);
      }

      // Add last path
      std::string path = paths_str.substr(start);
      if (!path.empty() && access(path.c_str(), F_OK) == 0) {
         prefixes.insert(path);
      }

      // If PYTHONPREFIX provided valid paths, use only those
      if (!prefixes.empty()) {
         return std::vector<std::string>(prefixes.begin(), prefixes.end());
      }
   }

   // Priority 2: Try Python sys.prefix and sys.path
   std::vector<std::string> sys_paths = get_python_sys_paths();
   if (!sys_paths.empty()) {
      for (const auto& path : sys_paths) {
         prefixes.insert(path);
      }
      if (!prefixes.empty()) {
         return std::vector<std::string>(prefixes.begin(), prefixes.end());
      }
   }

   // Priority 3: Fall back to 'which python' discovery
   std::string py2 = get_python_prefix_from_which("python");
   if (!py2.empty()) {
      prefixes.insert(py2);
   }

   std::string py3 = get_python_prefix_from_which("python3");
   if (!py3.empty()) {
      prefixes.insert(py3);
   }

   return std::vector<std::string>(prefixes.begin(), prefixes.end());
}

static bool has_remote_mount_under(const std::string& prefix, const std::vector<MountPoint>& mounts)
{
   // Check if any remote mount exists under this prefix
   std::string prefix_slash = prefix;
   if (prefix != "/" && prefix_slash.back() != '/') {
      prefix_slash += "/";
   }

   for (const auto& mp : mounts) {
      if (!mp.is_remote) continue;

      // Check if this remote mount is under prefix
      if (mp.path == prefix || mp.path.find(prefix_slash) == 0) {
         return true;
      }
   }
   return false;
}

static std::vector<std::string> generate_localprefix(const std::vector<MountPoint>& mounts)
{
   std::vector<std::string> local_dirs;

   // Always include kernel filesystems
   local_dirs.push_back("/proc");
   local_dirs.push_back("/sys");
   local_dirs.push_back("/dev");

   // Get top-level directories
   DIR* dir = opendir("/");
   if (!dir) {
      return local_dirs;
   }

   struct dirent* entry;
   while ((entry = readdir(dir)) != nullptr) {
      if (entry->d_name[0] == '.') continue;

      std::string top_level = std::string("/") + entry->d_name;

      // Skip if already added (kernel filesystems)
      if (top_level == "/proc" || top_level == "/sys" || top_level == "/dev") {
         continue;
      }

      // Check if directory
      struct stat st;
      if (stat(top_level.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
         continue;
      }

      // Skip if has remote mounts underneath
      if (has_remote_mount_under(top_level, mounts)) {
         continue;
      }

      local_dirs.push_back(top_level);
   }
   closedir(dir);

   // Sort for consistent output (kernel filesystems first)
   std::sort(local_dirs.begin() + 3, local_dirs.end());

   return local_dirs;
}

static std::string get_hostname_for_ssh()
{
   char hostname[256];
   if (gethostname(hostname, sizeof(hostname)) != 0) {
      return "";
   }
   return std::string(hostname);
}

// Forward declaration
static bool test_ssh_connectivity(const std::string& hostname, std::string& error_msg);

// Structure to hold SSH test results for MPI communication
struct SSHTestResult {
   int rank;
   int target_rank;
   bool success;
   char error_msg[256];
   char source_host[256];
   char target_host[256];
};

// Ring topology SSH test: each rank tests SSH to next rank
static void test_ssh_ring(int mpi_rank, int mpi_size, std::vector<std::string>& warnings)
{
   // Get local hostname
   std::string local_hostname = get_hostname_for_ssh();

   // Exchange hostnames among all ranks using MPI_Allgather
   const int MAX_HOSTNAME_LEN = 256;
   char local_host[MAX_HOSTNAME_LEN];
   char all_hosts[MAX_HOSTNAME_LEN * mpi_size];

   strncpy(local_host, local_hostname.c_str(), MAX_HOSTNAME_LEN - 1);
   local_host[MAX_HOSTNAME_LEN - 1] = '\0';

   MPI_Allgather(local_host, MAX_HOSTNAME_LEN, MPI_CHAR,
                 all_hosts, MAX_HOSTNAME_LEN, MPI_CHAR,
                 MPI_COMM_WORLD);

   // Determine target: (rank + 1) % size
   int target_rank = (mpi_rank + 1) % mpi_size;
   std::string target_hostname(&all_hosts[target_rank * MAX_HOSTNAME_LEN]);

   // Test SSH to target
   SSHTestResult result;
   result.rank = mpi_rank;
   result.target_rank = target_rank;
   strncpy(result.source_host, local_hostname.c_str(), 255);
   result.source_host[255] = '\0';
   strncpy(result.target_host, target_hostname.c_str(), 255);
   result.target_host[255] = '\0';

   std::string ssh_error;
   result.success = test_ssh_connectivity(target_hostname, ssh_error);
   strncpy(result.error_msg, ssh_error.c_str(), 255);
   result.error_msg[255] = '\0';

   // Gather all results to rank 0
   SSHTestResult all_results[mpi_size];
   MPI_Gather(&result, sizeof(SSHTestResult), MPI_BYTE,
              all_results, sizeof(SSHTestResult), MPI_BYTE,
              0, MPI_COMM_WORLD);

   // Rank 0 processes and reports results
   if (mpi_rank == 0) {
      int successful_tests = 0;
      bool all_passed = true;

      for (int i = 0; i < mpi_size; i++) {
         if (all_results[i].success) {
            successful_tests++;
         } else {
            all_passed = false;
         }
      }

      if (all_passed) {
         char msg[512];
         snprintf(msg, sizeof(msg), "SSH ring test passed: All %d ranks successfully tested SSH connectivity", mpi_size);
         warnings.push_back(std::string(msg));
      } else {
         char msg[512];
         snprintf(msg, sizeof(msg), "SSH ring test: %d/%d successful", successful_tests, mpi_size);
         warnings.push_back(std::string(msg));

         // Report failures
         for (int i = 0; i < mpi_size; i++) {
            if (!all_results[i].success) {
               snprintf(msg, sizeof(msg), "  Rank %d (%s) -> Rank %d (%s): FAILED - %s",
                       all_results[i].rank, all_results[i].source_host,
                       all_results[i].target_rank, all_results[i].target_host,
                       all_results[i].error_msg);
               warnings.push_back(std::string(msg));
            }
         }
      }
   }
}

static bool test_ssh_connectivity(const std::string& hostname, std::string& error_msg)
{
   if (hostname.empty()) {
      error_msg = "Could not determine hostname";
      return false;
   }

   // Build SSH command: ssh -o BatchMode=yes -o ConnectTimeout=10 <hostname> /bin/true
   std::string cmd = "ssh -o BatchMode=yes -o ConnectTimeout=10 -o StrictHostKeyChecking=no " +
                     hostname + " /bin/true 2>&1";

   FILE* fp = popen(cmd.c_str(), "r");
   if (!fp) {
      error_msg = "Failed to execute ssh command";
      return false;
   }

   // Read any error output
   char buffer[256];
   std::string output;
   while (fgets(buffer, sizeof(buffer), fp) != nullptr) {
      output += buffer;
   }

   int status = pclose(fp);
   int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

   if (exit_code == 0) {
      return true;
   }

   // SSH failed - extract useful error info
   if (output.find("Permission denied") != std::string::npos) {
      error_msg = "SSH authentication failed (check SSH keys)";
   } else if (output.find("Connection refused") != std::string::npos) {
      error_msg = "SSH connection refused";
   } else if (output.find("Connection timed out") != std::string::npos) {
      error_msg = "SSH connection timed out";
   } else if (output.find("No route to host") != std::string::npos) {
      error_msg = "No route to host";
   } else if (!output.empty()) {
      // Truncate long error messages
      error_msg = output.substr(0, 100);
      if (output.length() > 100) error_msg += "...";
   } else {
      error_msg = "SSH test failed (exit code " + std::to_string(exit_code) + ")";
   }

   return false;
}

// Forward declarations
static void print_config_header(std::vector<std::string>& warnings);

// Parse a spindle.conf file and return key-value pairs
// Returns: map of key -> value pairs
// Ignores comments and blank lines
static std::map<std::string, std::string> parse_config_file(const std::string& filepath,
                                                              std::vector<std::string>& warnings)
{
   std::map<std::string, std::string> config;
   std::ifstream file(filepath);

   if (!file.is_open()) {
      warnings.push_back("WARNING: Could not open config file: " + filepath);
      return config;
   }

   std::string line;
   int line_num = 0;
   while (std::getline(file, line)) {
      line_num++;

      // Trim leading/trailing whitespace
      size_t start = line.find_first_not_of(" \t\r\n");
      if (start == std::string::npos) continue;  // Empty line

      size_t end = line.find_last_not_of(" \t\r\n");
      line = line.substr(start, end - start + 1);

      // Skip comments and empty lines
      if (line.empty() || line[0] == '#') continue;

      // Find '=' separator
      size_t eq_pos = line.find('=');
      if (eq_pos == std::string::npos) {
         warnings.push_back("WARNING: Malformed line " + std::to_string(line_num) +
                           " in " + filepath + ": " + line);
         continue;
      }

      // Extract key and value
      std::string key = line.substr(0, eq_pos);
      std::string value = line.substr(eq_pos + 1);

      // Trim whitespace around key and value
      start = key.find_first_not_of(" \t");
      end = key.find_last_not_of(" \t");
      if (start != std::string::npos) {
         key = key.substr(start, end - start + 1);
      }

      start = value.find_first_not_of(" \t");
      end = value.find_last_not_of(" \t");
      if (start != std::string::npos) {
         value = value.substr(start, end - start + 1);
      }

      if (!key.empty()) {
         config[key] = value;
      }
   }

   file.close();
   return config;
}

// Parse command-line options from argv (options after --)
// Returns: map of key -> value pairs
static std::map<std::string, std::string> parse_cmdline_options(int argc, char* argv[],
                                                                  int separator_idx,
                                                                  std::vector<std::string>& warnings)
{
   std::map<std::string, std::string> options;

   if (separator_idx == -1 || separator_idx + 1 >= argc) {
      return options;  // No options after --
   }

   // Parse options after --
   for (int i = separator_idx + 1; i < argc; i++) {
      std::string arg = argv[i];

      // Skip --usage
      if (arg == "--usage") continue;

      // Parse --key=value or --key value
      if (arg.substr(0, 2) == "--") {
         std::string key, value;
         size_t eq_pos = arg.find('=');

         if (eq_pos != std::string::npos) {
            // --key=value
            key = arg.substr(2, eq_pos - 2);
            value = arg.substr(eq_pos + 1);
         } else {
            // --key (boolean or next arg is value)
            key = arg.substr(2);

            // Check if next arg exists and doesn't start with --
            if (i + 1 < argc && argv[i + 1][0] != '-') {
               value = argv[i + 1];
               i++;  // Skip next arg
            } else {
               // Boolean flag
               value = "true";
            }
         }

         // Remove dashes from key to match config file format
         key.erase(std::remove(key.begin(), key.end(), '-'), key.end());

         if (!key.empty()) {
            options[key] = value;
         }
      } else {
         warnings.push_back("WARNING: Ignoring unrecognized argument: " + arg);
      }
   }

   return options;
}

// Output a checked/validated config (for check mode)
static void output_checked_config(const std::map<std::string, std::string>& config,
                                   std::vector<std::string>& warnings)
{
   // Print header
   print_config_header(warnings);

   // Output all options in sorted order
   std::vector<std::string> keys;
   for (const auto& kv : config) {
      keys.push_back(kv.first);
   }
   std::sort(keys.begin(), keys.end());

   for (const auto& key : keys) {
      std::cout << key << " = " << config.at(key) << std::endl;
   }
}

static void print_config_header(std::vector<std::string>& warnings)
{
   std::cout << "# spindle.conf generated by spindle-config-helper version "
             << PACKAGE_VERSION << std::endl;
   std::cout << "# Generated: " << get_timestamp() << std::endl;
   std::cout << "#" << std::endl;

   if (!warnings.empty()) {
      // Check if we have any actual warnings vs just informational notes
      bool has_warnings = false;
      for (const auto& msg : warnings) {
         if (msg.find("WARNING") != std::string::npos ||
             msg.find("CRITICAL") != std::string::npos ||
             msg.find("failed") != std::string::npos ||
             msg.find("not set") != std::string::npos ||
             msg.find("not usable") != std::string::npos) {
            has_warnings = true;
            break;
         }
      }

      std::cout << "# " << (has_warnings ? "WARNINGS:" : "NOTES:") << std::endl;
      for (const auto& warning : warnings) {
         std::cout << "#   " << warning << std::endl;
      }
      std::cout << "#" << std::endl;
   }
}

static void print_path_variable(const std::string& name, const std::string& value,
                                 const std::vector<std::string>& comments)
{
   // Print comments above the variable
   for (const auto& comment : comments) {
      std::cout << "# " << comment << std::endl;
   }

   std::cout << name << " = " << value << std::endl;

   if (!comments.empty()) {
      std::cout << std::endl;  // Blank line after commented variables
   }
}

static void generate_default_config(const ConfigMap& confmap,
                                     const std::vector<MountPoint>& mounts,
                                     std::vector<std::string>& header_warnings,
                                     int mpi_rank, int mpi_size)
{
   // Determine best path for commpaths/cachepaths if using defaults
   std::string recommended_path;
   bool need_path_recommendation = false;

   // Check if commpaths or cachepaths are using the default $TMPDIR
   auto commpaths_result = confmap.getValueString(confCommPaths);
   auto cachepaths_result = confmap.getValueString(confCachePaths);

   bool commpaths_is_default = (commpaths_result.first && commpaths_result.second == "$TMPDIR");
   bool cachepaths_is_default = (cachepaths_result.first && cachepaths_result.second == "$TMPDIR");

   if (commpaths_is_default || cachepaths_is_default) {
      need_path_recommendation = true;
      std::vector<std::string> path_warnings;
      recommended_path = determine_best_path(mounts, path_warnings);

      // Add path-related warnings to header
      for (const auto& warning : path_warnings) {
         header_warnings.push_back(warning);
      }

      if (recommended_path.empty()) {
         header_warnings.push_back("CRITICAL: No suitable local filesystem found for commpaths/cachepaths");
         header_warnings.push_back("          Cannot generate valid configuration");
         // Print header with errors and exit
         print_config_header(header_warnings);
         return;
      }
   }

   // SSH test results already collected in header_warnings (done in main before calling this function)

   // Header
   print_config_header(header_warnings);

   // Generate the 4 path-based variables with analysis
   auto output_path_option = [&](SpindleConfigID config_id, const char* name,
                                  bool check_python = false) {
      // Check if this option is set (either default or user-specified)
      if (!confmap.isSet(config_id)) {
         return;
      }

      auto result = confmap.getValueString(config_id);
      if (!result.first || result.second.empty()) {
         return;
      }

      std::string paths = result.second;
      std::vector<std::string> comments;

      // If this is commpaths or cachepaths with $TMPDIR default, use recommended path
      if (need_path_recommendation &&
          (config_id == confCommPaths || config_id == confCachePaths) &&
          paths == "$TMPDIR" && !recommended_path.empty()) {
         paths = recommended_path;
      }

      // For pythonprefix, discover additional Python installations
      std::vector<std::string> discovered_pythons;
      if (config_id == confPythonPrefix) {
         discovered_pythons = discover_python_prefixes();
      }

      // Analyze each path in the colon-separated list
      std::vector<std::string> path_list;
      std::vector<PathAnalysis> analyses;
      size_t pos = 0;
      while (pos < paths.length()) {
         size_t end = paths.find(':', pos);
         if (end == std::string::npos) {
            end = paths.length();
         }
         std::string path = paths.substr(pos, end - pos);
         if (!path.empty()) {
            path_list.push_back(path);
            analyses.push_back(analyze_path(path, mounts, check_python));
         }
         pos = end + 1;
      }

      // Add discovered Python paths to the list
      if (config_id == confPythonPrefix) {
         for (const auto& pypath : discovered_pythons) {
            // Check if not already in list
            bool already_present = false;
            for (const auto& existing : path_list) {
               PathAnalysis ex = analyze_path(existing, mounts, check_python);
               if (ex.resolved == resolve_real_path(pypath)) {
                  already_present = true;
                  break;
               }
            }
            if (!already_present) {
               path_list.push_back(pypath);
               analyses.push_back(analyze_path(pypath, mounts, check_python));
               comments.push_back("NOTE: Added " + pypath + " from 'which python/python3'");
            }
         }
      }

      // Separate usable from unusable paths
      std::vector<std::string> good_paths, bad_paths;

      for (size_t i = 0; i < path_list.size(); i++) {
         bool has_critical_issues = false;

         // For pythonprefix, check for Python installation issues
         if (check_python) {
            for (const auto& warning : analyses[i].warnings) {
               if (warning.find("No bin/python") != std::string::npos ||
                   warning.find("Path does not exist") != std::string::npos ||
                   warning.find("parent directory is not writable") != std::string::npos) {
                  has_critical_issues = true;
                  break;
               }
            }
         } else {
            // For commpaths/cachepaths/localprefix
            for (const auto& warning : analyses[i].warnings) {
               if (warning.find("parent directory is not writable") != std::string::npos ||
                   warning.find("REMOTE filesystem") != std::string::npos) {
                  has_critical_issues = true;
                  break;
               }
            }
         }

         if (has_critical_issues) {
            bad_paths.push_back(path_list[i]);
         } else {
            good_paths.push_back(path_list[i]);
         }
      }

      // If no good paths, add fallback
      if (good_paths.empty() && (config_id == confCommPaths || config_id == confCachePaths)) {
         if (!recommended_path.empty()) {
            good_paths.push_back(recommended_path);
            comments.push_back("NOTE: No usable paths found, using " + recommended_path);
         }
      }

      // Build comments for bad paths (these came from configure/defaults, not user choice)
      if (!bad_paths.empty()) {
         if (check_python) {
            comments.push_back("NOTE: The following paths are not valid Python installations:");
         } else {
            comments.push_back("NOTE: The following paths from configure/defaults are not usable:");
         }
         for (size_t i = 0; i < path_list.size(); i++) {
            bool is_bad = (std::find(bad_paths.begin(), bad_paths.end(), path_list[i]) != bad_paths.end());
            if (is_bad) {
               comments.push_back("  " + path_list[i] + ":");
               for (const auto& warning : analyses[i].warnings) {
                  comments.push_back("    " + warning);
               }
            }
         }
         if (!good_paths.empty()) {
            comments.push_back("  (These paths have been filtered out)");
         }
      }

      // Use only good paths in output
      if (!good_paths.empty()) {
         std::string filtered;
         for (const auto& p : good_paths) {
            if (!filtered.empty()) filtered += ":";
            filtered += p;
         }
         paths = filtered;
      } else {
         // No good paths at all
         if (check_python) {
            // For pythonprefix, skip output if no valid Python found
            comments.push_back("WARNING: No valid Python installations found");
            return;
         }
         paths = "";
      }

      // Add comment for usable paths if no issues
      bool has_issues = !bad_paths.empty();
      if (!has_issues && !good_paths.empty()) {
         PathAnalysis first = analyze_path(good_paths[0], mounts, check_python);
         if (first.mount && !first.mount->is_remote) {
            comments.push_back(std::string(name) + ": Local " + first.mount->fstype +
                             " filesystem, " + std::to_string(first.mount->avail_gb) + "GB available");
         } else if (is_kernel_filesystem(first.resolved)) {
            comments.push_back(std::string(name) + ": Kernel filesystem");
         }
      }

      print_path_variable(name, paths, comments);
   };

   output_path_option(confCommPaths, "commpaths");
   output_path_option(confCachePaths, "cachepaths");
   output_path_option(confPythonPrefix, "pythonprefix", true);

   // Special handling for localprefix - generate from top-level dirs without remote mounts
   auto localprefix_result = confmap.getValueString(confLocalPrefix);
   if (localprefix_result.first) {
      std::vector<std::string> generated_locals = generate_localprefix(mounts);
      std::vector<std::string> comments;

      // Check if user specified additional paths (beyond defaults)
      std::string user_value = localprefix_result.second;
      std::set<std::string> user_paths;

      // Parse user's colon-separated list
      size_t pos = 0;
      while (pos < user_value.length()) {
         size_t end = user_value.find(':', pos);
         if (end == std::string::npos) end = user_value.length();

         std::string path = user_value.substr(pos, end - pos);
         // Strip trailing slash for comparison
         if (!path.empty() && path.back() == '/') path.pop_back();
         if (!path.empty()) user_paths.insert(path);

         pos = end + 1;
      }

      // Add generated paths to user paths
      std::set<std::string> all_paths(user_paths);
      for (const auto& gpath : generated_locals) {
         all_paths.insert(gpath);
      }

      // Validate user additions
      for (const auto& upath : user_paths) {
         // Check if this was NOT auto-generated (meaning user added it)
         bool was_generated = (std::find(generated_locals.begin(), generated_locals.end(), upath) != generated_locals.end());
         if (!was_generated) {
            // User added this - validate it
            if (has_remote_mount_under(upath, mounts)) {
               comments.push_back("WARNING: User-specified path has remote mounts underneath: " + upath);
            } else {
               comments.push_back("NOTE: User added: " + upath);
            }
         }
      }

      // Build output with trailing slashes
      std::string output_value;
      for (const auto& dir : all_paths) {
         if (!output_value.empty()) output_value += ":";
         output_value += (dir == "/" ? "/" : dir + "/");
      }

      if (comments.empty()) {
         comments.push_back("localprefix: Top-level directories without remote mounts");
      }

      print_path_variable("localprefix", output_value, comments);
   }
}

static void generate_all_config(const ConfigMap& confmap,
                                 const std::vector<MountPoint>& mounts,
                                 std::vector<std::string>& header_warnings)
{
   // Print header
   print_config_header(header_warnings);

   // Get all Spindle options
   const std::map<SpindleConfigID, const SpindleOption&>& all_opts = getOptionsByID();

   // Iterate through all options and output them
   for (const auto& pair : all_opts) {
      SpindleConfigID id = pair.first;
      const SpindleOption& opt = pair.second;

      // Skip options without command-line names (internal options)
      if (!opt.cmdline_long) continue;

      // Get the config file key name (cmdline_long with dashes removed)
      std::string key = opt.cmdline_long;
      key.erase(std::remove(key.begin(), key.end(), '-'), key.end());

      // Check if this option is set (either by user or default)
      bool is_set = confmap.isSet(id);
      std::string value;

      if (is_set) {
         // Get the actual value
         auto result = confmap.getValueString(id);
         if (result.first) {
            value = result.second;
         }
      }

      // For the 4 path variables, output with full analysis
      if (id == confCommPaths || id == confCachePaths ||
          id == confPythonPrefix || id == confLocalPrefix) {
         // Use the existing detailed output from generate_default_config
         continue;  // We'll call those separately
      }

      // For other options, output as comment if not explicitly set by user
      // or uncommented if set by command line
      std::string comment_prefix = is_set ? "" : "# ";

      // Output the option
      std::cout << comment_prefix << key << " = " << value << std::endl;
   }

   // Now output the 4 path variables with full analysis
   // These are always uncommented and analyzed
   auto output_path_option = [&](SpindleConfigID config_id, const char* name,
                                  bool check_python = false) {
      if (!confmap.isSet(config_id)) {
         return;
      }

      auto result = confmap.getValueString(config_id);
      if (!result.first || result.second.empty()) {
         return;
      }

      std::string paths = result.second;
      std::vector<std::string> comments;

      // For pythonprefix, discover additional Python installations
      std::vector<std::string> discovered_pythons;
      if (config_id == confPythonPrefix) {
         discovered_pythons = discover_python_prefixes();
      }

      // Analyze paths (simplified for all mode)
      std::vector<std::string> path_list;
      size_t pos = 0;
      while (pos < paths.length()) {
         size_t end = paths.find(':', pos);
         if (end == std::string::npos) {
            end = paths.length();
         }
         std::string path = paths.substr(pos, end - pos);
         if (!path.empty()) {
            path_list.push_back(path);
         }
         pos = end + 1;
      }

      // Add discovered Python paths
      if (config_id == confPythonPrefix) {
         for (const auto& pypath : discovered_pythons) {
            if (std::find(path_list.begin(), path_list.end(), pypath) == path_list.end()) {
               path_list.push_back(pypath);
            }
         }
      }

      // Output
      std::string output_value;
      for (size_t i = 0; i < path_list.size(); i++) {
         if (i > 0) output_value += ":";
         output_value += path_list[i];
      }

      if (!output_value.empty()) {
         print_path_variable(name, output_value, comments);
      }
   };

   output_path_option(confCommPaths, "commpaths");
   output_path_option(confCachePaths, "cachepaths");
   output_path_option(confPythonPrefix, "pythonprefix", true);
   output_path_option(confLocalPrefix, "localprefix");
}

static void print_version()
{
   std::cout << "spindle-config-helper version " << PACKAGE_VERSION << std::endl;
}

static bool check_munge_available(bool in_sandbox)
{
   // Check if munge binary exists
   if (access("/usr/bin/munge", X_OK) != 0) {
      return false;
   }

   // If in sandbox, we can't test the daemon
   if (in_sandbox) {
      return true;  // Binary exists, can't test further
   }

   // Try to actually use munge to verify daemon is running
   pid_t pid = fork();
   if (pid == 0) {
      // Child: run munge -n with stdout/stderr redirected to /dev/null
      int devnull = open("/dev/null", O_WRONLY);
      dup2(devnull, STDOUT_FILENO);
      dup2(devnull, STDERR_FILENO);
      close(devnull);
      execl("/usr/bin/munge", "munge", "-n", (char*)NULL);
      _exit(127);
   } else if (pid > 0) {
      // Parent: wait for child
      int status;
      waitpid(pid, &status, 0);
      return WIFEXITED(status) && WEXITSTATUS(status) == 0;
   }

   return false;  // fork failed
}

static void print_security_analysis(const ConfigMap& confmap, const std::vector<MountPoint>& mounts)
{
   std::cout << "# Security configuration:" << std::endl;

   // Detect if we're in a sandbox
   const char* sandbox_engine = getenv("SANDBOX_ENGINE");
   bool in_sandbox = (sandbox_engine != NULL);

   // Get the current security setting
   auto sec_result = confmap.getValueString(confSecurity);
   std::string current_mode = "unknown";
   if (sec_result.first) {
      current_mode = sec_result.second;
   }

   std::cout << "#   Current mode: " << current_mode << std::endl;

   // Validate based on mode
   if (current_mode == "none") {
      std::cout << "#     WARNING: No authentication enabled (connections are insecure)" << std::endl;
   } else if (current_mode == "keyfile") {
      const char* keydir = get_sec_keydir_path();
      if (keydir) {
         std::cout << "#     Keyfile directory: " << keydir << std::endl;

         // Check if keydir is on a shared (remote) filesystem
         std::string keydir_resolved = resolve_real_path(keydir);
         const MountPoint* mp = find_mount_for_path(keydir_resolved, mounts);

         if (mp) {
            if (mp->is_remote) {
               std::cout << "#     OK: Keyfile directory is on shared filesystem ("
                        << mp->fstype << ")" << std::endl;
            } else {
               std::cout << "#     WARNING: Keyfile directory is on LOCAL filesystem ("
                        << mp->fstype << ")" << std::endl;
               std::cout << "#       Keyfile must be on a filesystem shared across all nodes" << std::endl;
            }
         } else {
            std::cout << "#     WARNING: Cannot determine filesystem type for keyfile directory" << std::endl;
         }
      } else {
         std::cout << "#     Note: Keyfile mode not compiled in" << std::endl;
      }
   } else if (current_mode == "launchmon") {
      std::cout << "#     WARNING: LaunchMON security mode (deprecated/untested)" << std::endl;
   } else if (current_mode == "munge") {
      std::cout << "#     Munge authentication enabled" << std::endl;
   }

   std::cout << "#" << std::endl;
   std::cout << "#   Munge availability:" << std::endl;

   // Always check for munge as a recommendation
   bool munge_binary_exists = (access("/usr/bin/munge", X_OK) == 0);

   if (munge_binary_exists) {
      if (in_sandbox) {
         std::cout << "#     Munge binary found at /usr/bin/munge" << std::endl;
         std::cout << "#     Note: Cannot test munge daemon (running in sandbox)" << std::endl;
      } else {
         bool munge_works = check_munge_available(false);
         if (munge_works) {
            std::cout << "#     Munge is available and working (recommended security option)" << std::endl;
         } else {
            std::cout << "#     Munge binary found but daemon is not running" << std::endl;
            std::cout << "#     Munge provides secure authentication (recommended if available)" << std::endl;
         }
      }
   } else {
      std::cout << "#     Munge not found" << std::endl;
      std::cout << "#     Munge provides secure authentication (recommended if available)" << std::endl;
   }

   std::cout << "#" << std::endl;
}

static void print_usage(const char *progname)
{
   std::cout << "Usage: " << progname << " [OPTIONS] [-- SPINDLE_OPTIONS]\n\n"
             << "Generate and validate Spindle configuration files for HPC environments.\n\n"
             << "DESCRIPTION:\n"
             << "  spindle-config-helper analyzes the local system and generates appropriate\n"
             << "  spindle.conf files. It detects local vs. remote filesystems, discovers\n"
             << "  Python installations, validates paths, and tests SSH connectivity.\n\n"
             << "OPTIONS:\n"
             << "  -h, --help              Display this help message and exit\n"
             << "  -V, --version           Display version information and exit\n"
             << "  -o, --output=FILE       Write output to FILE instead of stdout\n"
             << "  --overwrite             Allow overwriting existing output file\n"
             << "  -c, --check[=FILE]      Validate existing config FILE (optional)\n"
             << "  -a, --all               Generate complete config with all options\n"
             << "  --usage                 Display Spindle's usage summary\n\n"
             << "MODES:\n"
             << "  Default mode (no options):\n"
             << "    - Analyzes filesystem mounts (local vs. remote)\n"
             << "    - Checks $TMPDIR availability and recommends paths\n"
             << "    - Tests SSH connectivity to verify compute node access\n"
             << "    - Discovers Python installations via 'which python/python3'\n"
             << "    - Generates localprefix from top-level directories\n"
             << "    - Outputs minimal spindle.conf with 4 path variables:\n"
             << "      * commpaths:     Local directories for inter-process communication\n"
             << "      * cachepaths:    Local directories for file caching\n"
             << "      * pythonprefix:  Python installation directories\n"
             << "      * localprefix:   Directories excluded from caching\n"
             << "    - Includes warnings and recommendations as comments\n\n"
             << "  Check mode (-c [FILE]):\n"
             << "    - FILE is optional: validates and outputs options from:\n"
             << "      * Config file (if FILE provided)\n"
             << "      * Command-line options after --\n"
             << "    - Command-line options override config file values\n"
             << "    - Validates option names and values\n"
             << "    - Use cases:\n"
             << "      * -c FILE           : Validate file\n"
             << "      * -c -- --opt=val   : Validate single option\n"
             << "      * -c FILE -- --opt  : Validate file + override\n\n"
             << "  All mode (-a):\n"
             << "    - Enumerates all Spindle configuration options\n"
             << "    - Comments out options with default values (prefix '#')\n"
             << "    - Uncomments options specified after --\n"
             << "    - Useful for discovering available options\n\n"
             << "ENVIRONMENT:\n"
             << "  $TMPDIR                 Preferred directory for commpaths/cachepaths\n"
             << "                          Falls back to /tmp if not set\n\n"
             << "SPINDLE_OPTIONS:\n"
             << "  Any valid Spindle option can be passed after --\n"
             << "  Use '" << progname << " -- --usage' to see all available options\n"
             << "  Common options:\n"
             << "    --commpaths=PATHS     Colon-separated list of comm directories\n"
             << "    --cachepaths=PATHS    Colon-separated list of cache directories\n"
             << "    --level=LEVEL         Aggressiveness: high, medium, low, off\n"
             << "    --security=MODE       Security: none, munge, keyfile\n"
             << "    --launcher=TYPE       Launcher: serial, slurm, flux, etc.\n\n"
             << "EXAMPLES:\n"
             << "  # Generate new config to stdout\n"
             << "  " << progname << "\n\n"
             << "  # Generate config for review before saving\n"
             << "  " << progname << " | less\n\n"
             << "  # Save config to file\n"
             << "  " << progname << " -o ~/.spindle/spindle.conf\n\n"
             << "  # Generate with custom paths\n"
             << "  " << progname << " -- --commpaths=/dev/shm --cachepaths=/tmp\n\n"
             << "  # Check existing config for warnings\n"
             << "  " << progname << " -c ~/.spindle/spindle.conf | grep WARNING\n\n"
             << "  # Update config with new security setting\n"
             << "  " << progname << " -c ~/.spindle/spindle.conf -o ~/.spindle/spindle.conf \\\n"
             << "    --overwrite -- --security=munge\n\n"
             << "  # Generate complete config with all options\n"
             << "  " << progname << " -a -o spindle.conf.full\n\n"
             << "  # See all available Spindle options\n"
             << "  " << progname << " -- --usage\n\n"
             << "EXIT STATUS:\n"
             << "  0   Success\n"
             << "  1   Error (invalid arguments, file I/O error, etc.)\n\n"
             << "SEE ALSO:\n"
             << "  spindle(1), spindle-config-helper(1)\n\n"
             << "VERSION:\n"
             << "  spindle-config-helper " << PACKAGE_VERSION << "\n"
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
   // Initialize MPI
   int mpi_rank = 0, mpi_size = 1;
   int mpi_initialized = 0;
   MPI_Initialized(&mpi_initialized);

   if (!mpi_initialized) {
      MPI_Init(&argc, &argv);
   }
   MPI_Comm_rank(MPI_COMM_WORLD, &mpi_rank);
   MPI_Comm_size(MPI_COMM_WORLD, &mpi_size);

   // Handle -h/--help and -V/--version before config parsing
   // Also check for -- --usage which should show Spindle's usage
   bool show_spindle_usage = false;
   int separator_idx = -1;
   std::string output_file;
   bool overwrite = false;
   std::string check_file;
   bool check_mode = false;
   bool all_mode = false;

   // First pass: check for help/version and find separator, parse output options
   for (int i = 1; i < argc; i++) {
      if (strcmp(argv[i], "--") == 0) {
         separator_idx = i;
         continue;
      }

      if (separator_idx == -1) {
         // Before --: check for config-helper options
         if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            if (mpi_rank == 0) {
               print_usage(argv[0]);
            }
            MPI_Finalize();
            return 0;
         }
         if (strcmp(argv[i], "-V") == 0 || strcmp(argv[i], "--version") == 0) {
            if (mpi_rank == 0) {
               print_version();
            }
            MPI_Finalize();
            return 0;
         }
         if (strcmp(argv[i], "--usage") == 0) {
            show_spindle_usage = true;
         }
         if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_file = argv[i + 1];
            i++; // Skip next argument
         }
         if (strncmp(argv[i], "--output=", 9) == 0) {
            output_file = argv[i] + 9;
         }
         if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            output_file = argv[i + 1];
            i++; // Skip next argument
         }
         if (strcmp(argv[i], "--overwrite") == 0) {
            overwrite = true;
         }
         if (strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "--all") == 0) {
            all_mode = true;
         }
         if (strcmp(argv[i], "-c") == 0) {
            check_mode = true;
            // Check if next arg is a file (not starting with -)
            if (i + 1 < argc && argv[i + 1][0] != '-') {
               check_file = argv[i + 1];
               i++; // Skip next argument
            }
         }
         if (strncmp(argv[i], "--check=", 8) == 0) {
            check_mode = true;
            check_file = argv[i] + 8;
         }
         if (strcmp(argv[i], "--check") == 0) {
            check_mode = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
               check_file = argv[i + 1];
               i++; // Skip next argument
            }
         }
      } else {
         // After --: check for --usage
         if (strcmp(argv[i], "--usage") == 0) {
            show_spindle_usage = true;
         }
      }
   }

   // Handle -- --usage early (don't generate config)
   if (show_spindle_usage) {
      // Pass through to Spindle's option system by continuing with normal flow
      // but we'll build argv with --usage and let it print
      // Actually, just build minimal argv and call gatherAllConfigInfo
      // which will handle --usage and exit

      std::vector<char*> usage_argv;
      usage_argv.push_back(argv[0]);

      // Add --usage flag
      char *usage_flag = strdup("--usage");
      usage_argv.push_back(usage_flag);

      // Add options after -- (if any)
      if (separator_idx != -1) {
         for (int i = separator_idx + 1; i < argc; i++) {
            // Skip --usage if it appears here too
            if (strcmp(argv[i], "--usage") != 0) {
               usage_argv.push_back(argv[i]);
            }
         }
      }

      // Add dummy app
      char *dummy_sep = strdup("--");
      char *dummy_app = strdup("/bin/true");
      usage_argv.push_back(dummy_sep);
      usage_argv.push_back(dummy_app);

      char **usage_argv_ptr = usage_argv.data();
      int usage_argc = usage_argv.size();

      ConfigMap confmap;
      std::string errmsg;
      gatherAllConfigInfo(usage_argc, usage_argv_ptr, false, confmap, errmsg);

      free(usage_flag);
      free(dummy_sep);
      free(dummy_app);
      MPI_Finalize();
      return 0;
   }

   // Handle check mode with separate code path
   if (check_mode) {
      std::map<std::string, std::string> config_values;
      std::vector<std::string> warnings;

      // Parse config file if provided
      if (!check_file.empty()) {
         config_values = parse_config_file(check_file, warnings);
      }

      // Parse command-line options after --
      std::map<std::string, std::string> cmdline_options =
          parse_cmdline_options(argc, argv, separator_idx, warnings);

      // Merge: command-line overrides file
      for (const auto& kv : cmdline_options) {
         config_values[kv.first] = kv.second;
      }

      // Validate all options by building argv and using gatherAllConfigInfo
      // This reuses Spindle's validation without needing getOptionsByID

      std::vector<char*> val_argv;
      std::vector<std::string> val_strings;  // Keep strings alive

      val_argv.push_back(argv[0]);  // Program name

      // Build command-line arguments for validation
      // Map config file keys (no dashes) to command-line format (with dashes)
      // Use a static mapping to avoid iterating through option lists
      std::map<std::string, std::string> key_mapping = {
         {"localprefix", "local-prefix"},
         {"pythonprefix", "python-prefix"},
         // Note: commpaths and cachepaths don't need mapping - they're single words
         {"relocaout", "reloc-aout"},
         {"reloclibs", "reloc-libs"},
         {"relocpython", "reloc-python"},
         {"relocexec", "reloc-exec"},
         {"followfork", "follow-fork"},
         {"stopreloc", "stop-reloc"},
         {"pushpull", "push"},
         {"shmcachesize", "shmcache-size"},
         {"msgcachebuffer", "msgcache-buffer"},
         {"msgcachetimeout", "msgcache-timeout"},
         {"cleanupproc", "cleanup-proc"},
         {"cacheprefix", "cache-prefix"},
         {"hostbin", "host-bin"},
         {"startmultisession", "start-multisession"},
         {"startsession", "start-session"},
         {"endsession", "end-session"},
         {"runinsession", "run-in-session"},
         {"patchldso", "patch-ldso"},
         {"crashdedup", "crash-dedup"},
         {"crashaltstack", "crash-altstack"},
         {"crashlog", "crash-log"},
         {"rshcommand", "rsh-command"},
         {"enablersh", "enable-rsh"},
         {"execexcludes", "exec-excludes"},
         {"numaincludes", "numa-includes"},
         {"numaexcludes", "numa-excludes"},
         {"audittype", "audit-type"},
         {"disablelogging", "disable-logging"},
         {"noclean", "no-clean"},
         {"nohide", "no-hide"}
         // Add more as needed - most single-word keys don't need mapping
      };

      for (const auto& kv : config_values) {
         std::string cmdline_key = kv.first;

         // Check if we have a mapping for this key
         auto it = key_mapping.find(kv.first);
         if (it != key_mapping.end()) {
            cmdline_key = it->second;
         }

         // Add as two separate args: --key value
         std::string opt = "--" + cmdline_key;
         val_strings.push_back(opt);
         val_strings.push_back(kv.second);
      }

      for (auto& s : val_strings) {
         val_argv.push_back(const_cast<char*>(s.c_str()));
      }

      // Add dummy app
      char *val_sep = strdup("--");
      char *val_app = strdup("/bin/true");
      val_argv.push_back(val_sep);
      val_argv.push_back(val_app);

      char **val_argv_ptr = val_argv.data();
      int val_argc = val_argv.size();

      ConfigMap val_confmap;
      std::string val_errmsg;
      if (!gatherAllConfigInfo(val_argc, val_argv_ptr, false, val_confmap, val_errmsg)) {
         // Validation failed - gatherAllConfigInfo already printed error to stderr
         // Do not generate config output
         free(val_sep);
         free(val_app);
         return 1;
      }

      free(val_sep);
      free(val_app);

      // Handle file output if specified
      FILE* output_fp = nullptr;
      int saved_stdout = -1;

      if (!output_file.empty()) {
         struct stat st;
         if (stat(output_file.c_str(), &st) == 0 && !overwrite) {
            std::cerr << "Error: Output file '" << output_file << "' already exists." << std::endl;
            std::cerr << "Use --overwrite to overwrite the existing file." << std::endl;
            return 1;
         }

         output_fp = fopen(output_file.c_str(), "w");
         if (!output_fp) {
            std::cerr << "Error: Could not open output file '" << output_file
                      << "': " << strerror(errno) << std::endl;
            return 1;
         }

         saved_stdout = dup(STDOUT_FILENO);
         dup2(fileno(output_fp), STDOUT_FILENO);
      }

      // Output the validated config
      output_checked_config(config_values, warnings);

      // Restore stdout and close file if we redirected
      if (output_fp) {
         fflush(stdout);
         dup2(saved_stdout, STDOUT_FILENO);
         close(saved_stdout);
         fclose(output_fp);
      }

      MPI_Finalize();
      return 0;
   }

   // Reconstruct argv to move Spindle options before -- and add dummy app
   // Input:  spindle-config-helper [config-helper-opts] -- [spindle-opts]
   // Output: spindle-config-helper [spindle-opts] -- /bin/true (filtering out config-helper opts)

   std::vector<char*> new_argv;
   new_argv.push_back(argv[0]);  // Program name

   auto is_config_helper_opt = [](const char* arg) {
      return strcmp(arg, "-o") == 0 ||
             strcmp(arg, "-c") == 0 ||
             strcmp(arg, "-a") == 0 ||
             strncmp(arg, "--output", 8) == 0 ||
             strncmp(arg, "--check", 7) == 0 ||
             strcmp(arg, "--all") == 0 ||
             strcmp(arg, "--overwrite") == 0;
   };

   if (separator_idx != -1) {
      // Add config-helper options (before --), filtering out our options
      for (int i = 1; i < separator_idx; i++) {
         if (is_config_helper_opt(argv[i])) {
            // Skip this arg, and if it takes an argument, skip that too
            if ((strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0 ||
                 strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--check") == 0) &&
                i + 1 < argc && argv[i + 1][0] != '-') {
               i++; // Skip the file argument too
            }
            continue;
         }
         new_argv.push_back(argv[i]);
      }

      // Add spindle options (after --)
      for (int i = separator_idx + 1; i < argc; i++) {
         new_argv.push_back(argv[i]);
      }
   } else {
      // No -- found, copy args but filter out config-helper options
      for (int i = 1; i < argc; i++) {
         if (is_config_helper_opt(argv[i])) {
            if ((strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0 ||
                 strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--check") == 0) &&
                i + 1 < argc && argv[i + 1][0] != '-') {
               i++; // Skip the file argument too
            }
            continue;
         }
         new_argv.push_back(argv[i]);
      }
   }

   // Add dummy app
   char *dummy_sep = strdup("--");
   char *dummy_app = strdup("/bin/true");
   new_argv.push_back(dummy_sep);
   new_argv.push_back(dummy_app);

   char **modified_argv = new_argv.data();
   int modified_argc = new_argv.size();

   ConfigMap confmap;
   std::string errmsg;

   // Reuse Spindle's existing parsing: cmdline, env vars, config files
   // Pass false for inExecutable since we're not the spindle launcher
   if (!gatherAllConfigInfo(modified_argc, modified_argv, false, confmap, errmsg)) {
      std::cerr << "Error: " << errmsg << std::endl;
      free(dummy_app);
      free(dummy_sep);
      return 1;
   }

   // Clean up temporary argv
   free(dummy_app);
   free(dummy_sep);

   // Parse and analyze mount points (all ranks need this for SSH test)
   std::vector<MountPoint> mounts = parse_mounts();

   // Collect header warnings (will add $TMPDIR and SSH checks later)
   std::vector<std::string> header_warnings;

   // ALL RANKS: Test SSH connectivity using ring topology (uses collective MPI operations)
   if (mpi_size > 1) {
      test_ssh_ring(mpi_rank, mpi_size, header_warnings);
   } else if (mpi_rank == 0) {
      // Single node - test local SSH only
      std::string hostname = get_hostname_for_ssh();
      std::string ssh_error;
      if (!test_ssh_connectivity(hostname, ssh_error)) {
         header_warnings.push_back("SSH test failed for " + hostname + ": " + ssh_error);
         header_warnings.push_back("  (SSH keys may not be configured for compute node access)");
      } else {
         header_warnings.push_back("SSH test passed for " + hostname);
      }
   }

   // Only rank 0 generates and prints output
   if (mpi_rank == 0) {
      // Handle output file if specified
      FILE* output_fp = nullptr;
      int saved_stdout = -1;

      if (!output_file.empty()) {
         // Check if file exists
         struct stat st;
         if (stat(output_file.c_str(), &st) == 0 && !overwrite) {
            std::cerr << "Error: Output file '" << output_file << "' already exists." << std::endl;
            std::cerr << "Use --overwrite to overwrite the existing file." << std::endl;
            MPI_Finalize();
            return 1;
         }

         // Open output file
         output_fp = fopen(output_file.c_str(), "w");
         if (!output_fp) {
            std::cerr << "Error: Could not open output file '" << output_file
                      << "': " << strerror(errno) << std::endl;
            MPI_Finalize();
            return 1;
         }

         // Redirect stdout to file
         saved_stdout = dup(STDOUT_FILENO);
         dup2(fileno(output_fp), STDOUT_FILENO);
      }

      // Generate config based on mode
      if (all_mode) {
         generate_all_config(confmap, mounts, header_warnings);
      } else {
         generate_default_config(confmap, mounts, header_warnings, mpi_rank, mpi_size);
      }

      // Restore stdout and close file if we redirected
      if (output_fp) {
         fflush(stdout);
         dup2(saved_stdout, STDOUT_FILENO);
         close(saved_stdout);
         fclose(output_fp);
      }
   }

   MPI_Finalize();
   return 0;
}
