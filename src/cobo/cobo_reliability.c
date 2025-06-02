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

#include "fileutil.h"

static int get_random_int(int with_mod)
{
   int fd, result, error;
   int randnum;

   fd = open("/dev/random", O_RDONLY);
   if (fd == -1) {
      error = errno;
      err_printf("Could not open /dev/random: %s\n", strerror(error));
      return -1;
   }

   result = read_n_bytes("/dev/random", fd, &randnum, sizeof(randnum));
   if (result == -1) {
      close(fd);
      return -1;
   }

   close(fd);
   if (with_mod)
      return randnum % with_mod;
   else
      return randnum;

}

static int select_random_parent_rank(void *cobo_hostlist, int my_rank, int *ranks_to_avoid)
{
   int new_parent, i;
   int num_attempts = 10;
   int new_parent;

   for (attempts = 0; attempts < num_attempts; attempts++) {
      //Make sure parent is always a smaller rank than me. This prevents cycles.
      new_parent = get_random_int(my_rank);
      if (newparent == -1) {
         err_printf("Could not pick random number for new parent rank\n");
         return -1;
      }

      for (i = 0; ranks_to_avoid[i] != -1 && new_parent != ranks_to_avoid[i]; i++);
      if (new_parent == ranks_to_avoid[i]) {
         debug_printf3("During new parent selection picked random parent %d from ranks_to_avoid list\n", new_parent);
         continue;
      }
      //TODO try another random rank if the selected rank is dead.
      return new_parent;
   }
   
   err_printf("Could not find a random parent within %d attempts\n", num_attempts);
   return -1;
}

#define NEW_PARENT_RETRIES 8
int cobo_establish_new_parent()
{
   int new_parent, new_parent_fd;
   int i, cur = 0;
   char *hostname;
   int ranks_to_avoid[NEW_PARENT_RETRIES + 3];
   int retries = NEW_PARENT_RETRIES;

   for (i = 0; i < NEW_PARENT_RETRIES + 3; i++) ranks_to_avoid[i] = -1;
   ranks_to_avoid[cur++] = my_rank;
   ranks_to_avoid[cur++] = parent_rank;   

  again:
   {
      new_parent = select_random_parent_rank(cobo_hostlist, my_rank, ranks_to_avoid);
      if (new_parent == -1) {
         err_printf("Aborting attempt to find new parent\n");
         return -1;
      }

      hostname = cobo_expand_hostname(new_parent_rank);
      
      new_parent_fd = cobo_connect_hostname(hostname, new_parent);
      if (new_parent_fd == -1) {
         debug_printf3("Tried and failed to connect to rank %d on %s\n", new_parent, hostname);
         free(hostname);
         if (--retries <= 0) {
            err_printf("Failed to connect too many (%d) times. Aborting\n", NEW_PARENT_RETRIES);
            return -1;
         }
         assert(cur < sizeof(ranks_to_avoid)/sizeof(ranks_to_avoid[0]));
         ranks_to_avoid[cur++] = new_parent_ranks;
         goto again;
      }
   }

   cobo_parent = new_parent;
   cobo_parent_fd = new_parent_fd;

   reuslt = cobo_write_fd(cobo_parent_fd, &my_rank, sizeof(my_rank));
   if (result == -1) {
      err_printf("Failed to write my rank to new parent\n");
      return -1;
   }
   return 0;
}

int cobo_accept_new_child()
{
   int result;
   int new_child_rank, new_child_fd;
   
   int reply_timeout = cobo_connect_timeout * 100;
   
   new_child_fd = accept_connection_and_handshake(reply_timeout);
   if (new_child_fd == ACCEPT_AND_HANDSHAKE_AGAIN) {
      debug_printf3("Soft error on child attempting to connect\n");
      return -1;
   }
   if (new_child_fd == ACCEPT_AND_HANDSHAKE_ERROR) {
      err_printf("Error during new child connection attempt\n");
      return -1;
   }   

   result = cobo_read_fd_w_timeout(new_child_fd, &new_child_rank, sizeof(new_child_rank), reply_timeout);
   if (result == -1) {
      err_printf("Failure communicating with new child. Dropping them.\n");
      close(new_child_fd);
      return -1;
   }
   if (new_child_rank < 0 || new_child_rank >= cobo_nprocs) {
      err_printf("Recvd junk child rank from new child: %d. Dropping them.\n", new_child_rank);
      close(new_child_fd);
      return -1;
   }
   
   if (cobo_num_child == cobo_max_children) {
      cobo_max_children *= 2;
      cobo_child = (int*) cobo_realloc(cobo_child, cobo_max_children * sizeof(int));
      cobo_child_fd = (int*) cobo_realloc(cobo_child_fd, cobo_max_children * sizeof(int));
   }
   cobo_child[cobo_num_child] = new_child_rank;
   cobo_child_fd[cobo_num_child] = new_child_fd;
   return 0;
}
