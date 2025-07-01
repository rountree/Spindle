#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#if defined(HAVE_MPI)
#include <mpi.h>
#endif
#include <set>
#include <string>
#include <iostream>

#include <cstring>
#include <cstdlib>
#include <cassert>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <dlfcn.h>
#include <fcntl.h>

#if !defined(HOST_NAME_MAX)
#define HOST_NAME_MAX 64
#endif

using namespace std;

set<string> all_hosts;
string host;

static void init(int *pargc, char ***pargv)
{
#if defined(HAVE_MPI)
   MPI_Init(pargc, pargv);
#endif
}

static void fini()
{
#if defined(HAVE_MPI)
   MPI_Finalize();
#endif
}

static bool setupHostSet()
{
#if defined(HAVE_MPI)
   int rank, size;
   char *to_send;
   char *recvd;
   MPI_Datatype strtype;
   
   MPI_Comm_rank(MPI_COMM_WORLD, &rank);
   MPI_Comm_size(MPI_COMM_WORLD, &size);

   to_send = (char *) calloc(HOST_NAME_MAX, 1);
   recvd = (char *) calloc(HOST_NAME_MAX, size);

   memset(to_send, 0, HOST_NAME_MAX);
   strncpy(to_send, host.c_str(), HOST_NAME_MAX);

   MPI_Gather(to_send, HOST_NAME_MAX, MPI_CHAR,
              recvd, HOST_NAME_MAX, MPI_CHAR,
              0, MPI_COMM_WORLD);

   if (rank == 0) {
      char *hostbuf = (char *) calloc(HOST_NAME_MAX, 1);
      for (int i = 0; i < size; i++) {
         memset(hostbuf, 0, HOST_NAME_MAX);
         strncpy(hostbuf, recvd + (i * HOST_NAME_MAX), HOST_NAME_MAX);
         hostbuf[HOST_NAME_MAX-1] = '\0';
         all_hosts.insert(hostbuf);
      }
      free(hostbuf);
   }
   int num_hosts = (int) all_hosts.size();
   MPI_Bcast(&num_hosts, 1, MPI_INT, 0, MPI_COMM_WORLD);

   char *hostset = (char *) calloc(num_hosts, HOST_NAME_MAX);
   int i;
   set<string>::iterator j;
   for (i = 0, j = all_hosts.begin(); j != all_hosts.end(); i++, j++) {
      strncpy(hostset + (i * HOST_NAME_MAX), j->c_str(), HOST_NAME_MAX);
   }
   MPI_Bcast(hostset, num_hosts * HOST_NAME_MAX, MPI_CHAR, 0, MPI_COMM_WORLD);

   if (rank != 0) {
      char *hostbuf = (char *) calloc(HOST_NAME_MAX, 1);
      for (i = 0; i < num_hosts; i++) {
         memset(hostbuf, 0, HOST_NAME_MAX);
         strncpy(hostbuf, hostset + (i * HOST_NAME_MAX), HOST_NAME_MAX);
         hostbuf[HOST_NAME_MAX-1] = '\0';
         all_hosts.insert(hostbuf);
      }
      free(hostbuf);
   }

   free(hostset);
   free(to_send);
   free(recvd);
#else
   all_hosts.insert(host);
#endif
   return true;   
}

static void printHost(int rank)
{
   bool first = true;
   cout << rank << ": ";
   for (set<string>::iterator i = all_hosts.begin(); i != all_hosts.end(); i++) {
      if (!first)
         cout << ", ";
      first = false;
      cout << *i;
   }
   cout << endl;
}

static void printAllHosts()
{
#if defined(HAVE_MPI)
   int rank, size, i;
   MPI_Comm_rank(MPI_COMM_WORLD, &rank);
   MPI_Comm_size(MPI_COMM_WORLD, &size);

   for (i = 0; i < size; i++) {
      if (i == rank)
         printHost(rank);
      MPI_Barrier(MPI_COMM_WORLD);
   }
#else
   printHost(0);
#endif
}

static bool setupHosts()
{
   char hostname[HOST_NAME_MAX+1];
   memset(hostname, 0, sizeof(hostname));
   gethostname(hostname, sizeof(hostname));
   hostname[sizeof(hostname)-1] = '\0';

   host = string(hostname);
   return setupHostSet();
}

static bool shouldTerminate()
{
   int mypos = -1, j = 0;
   for (set<string>::iterator i = all_hosts.begin(); i != all_hosts.end(); i++, j++) {
      if (*i == host) {
         mypos = j;
         break;
      }
   }
   assert(mypos != -1);
   
   if (all_hosts.size() == 1) {
      return true;
   }
   else if (all_hosts.size() == 2) {
      return (mypos == 1);
   }
   else if (all_hosts.size() == 3) {
      return (mypos == 1);
   }
   else {
      return (mypos % 4) == 2;
   }
}

static bool loadFiles()
{
   void *libresult;
   int result, fd;
   bool had_error = false;
   struct stat buf;
   FILE *f;
   char linktarg[64];

   libresult = dlopen("libtest10.so", RTLD_NOW);
   if (!libresult) {
      fprintf(stderr, "Failed to open libtest10.so\n");
      had_error = true;
   }

   memset(&buf, 0, sizeof(buf));
   result = stat("hello_r.py", &buf);
   if (result == -1 || !buf.st_size) {
      fprintf(stderr, "Failed to stat hello_r.py\n");
      had_error = true;
   }

   fd = open("hello_rx.py", O_RDONLY);
   if (fd == -1) {
      fprintf(stderr, "Failed to open hello_rx.py\n");
      had_error = true;
   }
   else {
      memset(&buf, 0, sizeof(buf));
      result = fstat(fd, &buf);
      if (result == -1 || !buf.st_size) {
         fprintf(stderr, "Failed to stat open fd for hello_rx.py\n");
         had_error = true;
      }
      close(fd);
   }

   f = fopen("hello_rx.py", "r");
   if (!f) {
      fprintf(stderr, "Failed to fopen hello_rx.py\n");
      had_error = true;
   }
   else {
      fclose(f);
   }
   
   memset(linktarg, 0, sizeof(linktarg));
   result = readlink("hello_l.py", linktarg, sizeof(linktarg));
   if (result == -1 || strcmp(linktarg, "hello_x.py") != 0) {
      fprintf(stderr, "Failed to readlink on hello_l.py\n");
      had_error = true;
   }   
 
   return !had_error;
}

int main(int argc, char **argv)
{
   bool result;
   init(&argc, &argv);
   
   setupHosts();
   //printAllHosts();

   if (shouldTerminate()) {
      dlopen("libtrigger_spindle_server_exit_8WgNSgb0.so", RTLD_NOW);
   }

   result = loadFiles();
   
   fini();

   if (result) {
      printf("PASSED\n");
   }
   else {
      printf("FAILED\n");
   }
      
   return result ? 0 : -1;
}
   
