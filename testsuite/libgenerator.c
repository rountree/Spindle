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

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void writeFile(FILE *f, char *prefix, int size)
{
   int i;
   fprintf(f, "__thread int tlsvar_%d = -1;\n", size);
   fprintf(f, "static void on_load() __attribute__((constructor));\n");
   fprintf(f, "static void on_load() { tlsvar_%d = %d; }\n", size, size);
   fprintf(f, "int get_tlsvar_%s() { return tlsvar_%d; }\n", prefix, size);
   fprintf(f, "#if defined(FUNC_NAME)\n");
   fprintf(f, "int %s_cache[%d];\n", prefix, size+1);
   fprintf(f, "volatile int always_zero = 0;\n");
   fprintf(f, "volatile int vals[16];\n");
   fprintf(f, "#define SPACE_FILLER \\\n");
   fprintf(f, "   if (always_zero) { \\\n");
   for (i = 0; i < 16; i++)
      fprintf(f, "    vals[%d]++;\\\n", i);
   fprintf(f, "}\n");
   fprintf(f, "int %s_fib0(volatile int *v) { return 0; }\n", prefix);
   fprintf(f, "int %s_fib1(volatile int *v) { return 1; }\n", prefix);
   for (i = 2; i <= size; i++) {
      fprintf(f, "int %s_fib%d(volatile int *v) { SPACE_FILLER ; if (!%s_cache[%d]) %s_cache[%d] = %s_fib%d(v) + %s_fib%d(v); return %s_cache[%d]; }\n", 
              prefix, i, /* function sig */
              prefix, i, /* if statement */
              prefix, i, /* assignment lvalue */
              prefix, i-1, prefix, i-2, /* assignment rvalue */
              prefix, i); /* return */
   }
   fprintf(f, "int %s_calc() { int count = 0; return %s_fib%d(&count); }\n", prefix, prefix, size);
   fprintf(f, "#endif\n");
   fclose(f);
};

int main(int argc, char *argv[])
{
   char *prefix;
   FILE *f;
   int size;

   if (argc != 4) {
      fprintf(stderr, "Argument error\n");
      return -1;
   }
      
   f = fopen(argv[1], "w");
   if (!f) {
      fprintf(stderr, "Failed to open %s: %s\n", argv[2], strerror(errno));
      return -1;
   }
   size = atoi(argv[2]);
   prefix = argv[3];

   writeFile(f, prefix, size);
   return 0;
}
