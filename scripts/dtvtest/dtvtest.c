#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>

void onsig(int sig)
{
   exit(-1);
}

typedef void (*set_f)(int);
typedef int (*get_f)(void);
int openlib(char *num)
{
   char name[32];
   void *result;
   set_f s;

   snprintf(name, 32, "./libdlopentls%s.so", num);
   result = dlopen(name, RTLD_NOW);
   if (!result) {
      fprintf(stderr, "Test error opening %s: %s\n", name, dlerror());
      return -1;
   }
   
   snprintf(name, 32, "dlopen_set_%s", num);
   s = (set_f) dlsym(result, name);
   if (!s) {
      fprintf(stderr, "Test error looking up name %s\n", name);
      return -1;
   }
   s(4);
   return 0;
}

int main(int argc, char *argv[])
{
   int result;
   signal(SIGABRT, onsig);
   signal(SIGSEGV, onsig);
   signal(SIGTERM, onsig);
   
   if (argc == 1) {
      fprintf(stderr, "Test error: Expected to be run with list of numbers to dlopen\n");
      return -1;
   }
   for (int i = 1; i < argc; i++) {
      result = openlib(argv[i]);
      if (result == -1) {
         return -1;
      }
   }
   return 0;
}
