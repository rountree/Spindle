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

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>
#include <netdb.h>
#include <errno.h>
#include <stdarg.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <poll.h>
#include <assert.h>

#include "ldcs_cobo.h"
#include "spindle_debug.h"
#include "config.h"
#include "fileutil.h"

#define SPINDLE_ENABLE_RELIABILITY 1

/* Reads environment variable, bails if not set */
#define ENV_REQUIRED (0)
#define ENV_OPTIONAL (1)

/* set env variable to configure socket timeout parameters */
#ifndef COBO_CONNECT_TIMEOUT       
#define COBO_CONNECT_TIMEOUT (10) /* milliseconds -- wait this long before a connect() call times out*/
#endif
#ifndef COBO_CONNECT_BACKOFF
#define COBO_CONNECT_BACKOFF (2) /* exponential backoff factor for timeout */
#endif
#ifndef COBO_CONNECT_SLEEP  
#define COBO_CONNECT_SLEEP   (10) /* milliseconds -- wait this long before trying a new round of connects() */
#endif
#ifndef COBO_CONNECT_TIMELIMIT
#define COBO_CONNECT_TIMELIMIT (60) /* seconds -- wait this long before giving up for good */
#endif

#if defined(_IA64_)
#undef htons
#undef ntohs
#define htons(__bsx) ((((__bsx) >> 8) & 0xff) | (((__bsx) & 0xff) << 8))
#define ntohs(__bsx) ((((__bsx) >> 8) & 0xff) | (((__bsx) & 0xff) << 8))
#endif

/*
 * ==========================================================================
 * ==========================================================================
 * Globals
 * ==========================================================================
 * ==========================================================================
 */

/* Ranks:
 *   -3     ==> unitialized task (may be server or client task)
 *   -2     ==> server task
 *   -1     ==> client task before rank has been assigned
 *   0..N-1 ==> client task
 */
static int cobo_me     = -3;
static int cobo_nprocs = -1;

/* connection settings */
static int cobo_connect_timeout       = COBO_CONNECT_TIMEOUT;   /* milliseconds */
static int cobo_connect_backoff       = COBO_CONNECT_BACKOFF;   /* exponential backoff factor for connect timeout */
static int cobo_connect_sleep         = COBO_CONNECT_SLEEP;     /* milliseconds to sleep before rescanning ports */
static double cobo_connect_timelimit  = COBO_CONNECT_TIMELIMIT; /* seconds */

/* random ids that establish what type of operation we're doing upcon connection */
static unsigned int cobo_serviceid = 3059238577u;
static uint64_t cobo_sessionid = 0;
static unsigned int cobo_acceptid  = 2348104830u;
static unsigned int cobo_newchild_id = 0xfa172db4;

/* number of ports and list of ports in the available port range */
static int  cobo_num_ports = 0;
static int* cobo_ports     = NULL;
static int tree_port;
/* size (in bytes) and pointer to hostlist data structure */
static int   cobo_hostlist_size = 0;
static void* cobo_hostlist      = NULL;

/* tree data structures */
static int  cobo_incoming_sockfd = -1;    /* socket for accepting connections  */
static int  cobo_parent     = -3;    /* rank of parent */
static int  cobo_parent_fd  = -1;    /* socket to parent */
static int* cobo_child      = NULL;  /* ranks of children */
static int* cobo_child_fd   = NULL;  /* sockets to children */
static int  cobo_num_child  = 0;     /* number of children */
static int  cobo_max_children = 0;
static int cobo_root_fd = -1;

static handshake_protocol_t cobo_handshake;

double __cobo_ts = 0.0f;

/* startup time, time between starting cobo_open and finishing cobo_close */
static struct timeval time_open, time_close;
static struct timeval tree_start, tree_end;

/*
 * ==========================================================================
 * ==========================================================================
 * Private Functions
 * ==========================================================================
 * ==========================================================================
 */
#if defined(SIONDEBUG) 
#include "sion_debug.h"
#endif

static cobo_preconnect_cb_t preconnect_cb = NULL;

void cobo_register_preconnect_cb(cobo_preconnect_cb_t f)
{
   preconnect_cb = f;
}


/* Return the number of secs as a double between two timeval structs (tv2-tv1) */
static double cobo_getsecs(struct timeval* tv2, struct timeval* tv1)
{
    struct timeval result;
    timersub(tv2, tv1, &result);
    return (double) result.tv_sec + (double) result.tv_usec / 1000000.0;
}

/* Fills in timeval via gettimeofday */
static void cobo_gettimeofday(struct timeval* tv)
{
    if (gettimeofday(tv, NULL) < 0) {
        err_printf("Getting time (gettimeofday() %m errno=%d)\n", errno);
    }
}

/* Reads environment variable, bails if not set */
static char* cobo_getenv(char* envvar, int type)
{
    char* str = getenv(envvar);
    if (str == NULL && type == ENV_REQUIRED) {
        err_printf("Missing required environment variable: %s\n", envvar);
        exit(1);
    }
    return str;
}

/* malloc n bytes, and bail out with error msg if fails */
static void* cobo_malloc(size_t n, char* msg)
{
    void* p = malloc(n);
    if (!p) {
        err_printf("Call to malloc(%lu) failed: %s (%m errno %d)\n", n, msg, errno);
        exit(1);
    }
    return p;
}

static void setCloseOnExec(int fd) {
   int flags = fcntl(fd, F_GETFD);
   if (flags != -1) {
      fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
   }
}

/* macro to free the pointer if set, then set it to NULL */
#define cobo_free(p) { if(p) { free((void*)p); p=NULL; } }

static int _cobo_opt_socket(int sockfd)  {
  int flag = 1;
  int result = setsockopt(sockfd,            /* socket affected */
			  IPPROTO_TCP,     /* set option at TCP level */
			  TCP_NODELAY,     /* name of option */
			  (char *) &flag,  /* the cast is historical
					      cruft */
			  sizeof(int));    /* length of option value */
  debug_printf3("_cobo_opt_socket (sockfd=%d) flag=%d\n",sockfd, flag);
#ifdef CHANGENNODELAY
  flag=0;
  result = setsockopt(sockfd,            /* socket affected */
		      IPPROTO_TCP,     /* set option at TCP level */
		      TCP_NODELAY,     /* name of option */
		      (char *) &flag,  /* the cast is historical
					  cruft */
		      sizeof(int));    /* length of option value */
#endif
  setCloseOnExec(sockfd);
  return(result);
}

/* given a pointer to an array of ints, allocate and return a copy of the array */
static int* cobo_int_dup(int* src, int n)
{
    /* check that we have something to copy from */
    if (src == NULL) {
        return NULL;
    }

    /* allocate space to copy the array */
    size_t size = n * sizeof(int);
    int* dst = cobo_malloc(size, "Buffer for integer array");

    /* if we allocated space, copy over the elements */
    if (dst != NULL) {
        memcpy(dst, src, size);
    }

    return dst;
}

/* write size bytes from buf into fd, retry if necessary */
static int cobo_write_fd_w_suppress(int fd, void* buf, int size, int suppress)
{
    int rc;
    int n = 0;
    char* offset = (char*) buf;

    while (n < size) {
       rc = write(fd, offset, size - n);

	if (rc < 0) {
	    if(errno == EINTR || errno == EAGAIN) { continue; }
            if (!suppress) {
                err_printf("Writing to file descriptor (write(fd=%d,offset=%p,size=%d) %m errno=%d %s)\n",
                           fd, offset, size-n, errno, strerror(errno));
            } else {
                debug_printf3("Writing to file descriptor (write(fd=%d,offset=%p,size=%d) %m errno=%d)\n",
                           fd, offset, size-n, errno);
            }
	    return rc;
	} else if(rc == 0) {
            if (!suppress) {
                err_printf("Unexpected return code of 0 from write to file descriptor (write(fd=%d,offset=%p,size=%d))\n",
                           fd, offset, size-n);
            } else {
                err_printf("Unexpected return code of 0 from write to file descriptor (write(fd=%d,offset=%p,size=%d))\n",
                           fd, offset, size-n);
            }
	    return -1;
	}

	offset += rc;
	n += rc;
    }

    return n;
}

/* write size bytes from buf into fd, retry if necessary */
static int cobo_write_fd(int fd, void* buf, int size)
{
    return cobo_write_fd_w_suppress(fd, buf, size, 0);
}

/* read size bytes into buf from fd, retry if necessary */
static int cobo_read_fd_w_timeout(int fd, void* buf, int size, int usecs)
{
    int rc;
    int n = 0;
    char* offset = (char*) buf;

    struct pollfd fds;
    fds.fd      = fd;
    fds.events  = POLLIN;
    fds.revents = 0x0;

    while (n < size) {
        /* poll the connection with a timeout value */
        int poll_rc = poll(&fds, 1, usecs);
        if (poll_rc < 0) {
            err_printf("Polling file descriptor for read (read(fd=%d,offset=%p,size=%d) %m errno=%d)\n",
                       fd, offset, size-n, errno);
            return -1;
        } else if (poll_rc == 0) {
            return -1;
        }

        /* check the revents field for errors */
        if (fds.revents & POLLHUP) {
            debug_printf3("Hang up error on poll for read(fd=%d,offset=%p,size=%d)\n",
                       fd, offset, size-n);
            return -1;
        }

        if (fds.revents & POLLERR) {
            debug_printf3("Error on poll for read(fd=%d,offset=%p,size=%d)\n",
                       fd, offset, size-n);
            return -1;
        }

        if (fds.revents & POLLNVAL) {
            err_printf("Invalid request on poll for read(fd=%d,offset=%p,size=%d)\n",
                       fd, offset, size-n);
            return -1;
        }

        if (!(fds.revents & POLLIN)) {
            err_printf("No errors found, but POLLIN is not set for read(fd=%d,offset=%p,size=%d)\n",
                       fd, offset, size-n);
            return -1;
        }

        /* poll returned that fd is ready for reading */
	rc = read(fd, offset, size - n);

	if (rc < 0) {
	    if(errno == EINTR || errno == EAGAIN) { continue; }
            err_printf("Reading from file descriptor (read(fd=%d,offset=%p,size=%d) %m errno=%d)\n",
                       fd, offset, size-n, errno);
	    return rc;
	} else if(rc == 0) {
            err_printf("Unexpected return code of 0 from read from file descriptor (read(fd=%d,offset=%p,size=%d) revents=%x)\n",
                       fd, offset, size-n, fds.revents);
	    return -1;
	}

	offset += rc;
	n += rc;
    }

    return n;
}

/* read size bytes into buf from fd, retry if necessary */
static int cobo_read_fd(int fd, void* buf, int size)
{
    return cobo_read_fd_w_timeout(fd, buf, size, -1);
}

/* Open a connection on socket FD to peer at ADDR (which LEN bytes long).
 * This function uses a non-blocking filedescriptor for the connect(),
 * and then does a bounded poll() for the connection to complete.  This
 * allows us to timeout the connect() earlier than TCP might do it on
 * its own.  We have seen timeouts that failed after several minutes,
 * where we would really prefer to time out earlier and retry the connect.
 *
 * Return 0 on success, -1 for errors.
 */
static int cobo_connect_w_timeout(int fd, struct sockaddr const * addr, socklen_t len, int millisec)
{
    int rc, flags, err;
    socklen_t err_len;
    struct pollfd ufds;

    flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    err = 0;
    rc = connect(fd , addr , len);
    if (rc < 0 && errno != EINPROGRESS) {
/*
        err_printf("Nonblocking connect failed immediately (connect() %m errno=%d)\n",
                   errno);
*/
        return -1;
    }
    if (rc == 0) {
        goto done;  /* connect completed immediately */
    }

    ufds.fd = fd;
    ufds.events = POLLIN | POLLOUT;
    ufds.revents = 0;

again:	rc = poll(&ufds, 1, millisec);
    if (rc == -1) {
        /* poll failed */
        if (errno == EINTR) {
            /* NOTE: connect() is non-interruptible in Linux */
            goto again;
        } else {
/*
            err_printf("Polling connection (poll() %m errno=%d)\n",
                       errno);
*/
        }
        return -1;
    } else if (rc == 0) {
        /* poll timed out before any socket events */
        /* perror("cobo_connect_w_timeout poll timeout"); */
        return -1;
    } else {
        /* poll saw some event on the socket
         * We need to check if the connection succeeded by
         * using getsockopt.  The revent is not necessarily
         * POLLERR when the connection fails! */
        err_len = (socklen_t) sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &err_len) < 0) {
/*
            err_printf("Failed to read event on socket (getsockopt() %m errno=%d)\n",
                       errno);
*/
            return -1; /* solaris pending error */
        }
    }

done:
    fcntl(fd, F_SETFL, flags);

    /* NOTE: Connection refused is typically reported for
     * non-responsive nodes plus attempts to communicate
     * with terminated launcher. */
    if (err) {
/*
        err_printf("Error on socket in cobo_connect_w_timeout() (getsockopt() set err=%d)\n",
                   err);
*/
        return -1;
    }
 
    return 0;
}

/* Connect to given IP:port.  Upon successful connection, cobo_connect
 * shall return the connected socket file descriptor.  Otherwise, -1 shall be
 * returned.
 */
static int cobo_connect(struct in_addr ip, int port, int timeout)
{
    struct sockaddr_in sockaddr;

    /* set up address to connect to */
    sockaddr.sin_family = AF_INET;
    sockaddr.sin_addr = ip;
    sockaddr.sin_port = port;

    /* create a socket */
    int s = socket(AF_INET, SOCK_STREAM, 0); /* IPPROTO_TCP */
    if (s < 0) {
        err_printf("Creating socket (socket() %m errno=%d)\n",
                   errno);
        return -1;
    }

    /* connect socket to address */
    if (cobo_connect_w_timeout(s, (struct sockaddr *) &sockaddr, sizeof(sockaddr), timeout) < 0) {
        close(s);
        return -1;
    }

    _cobo_opt_socket(s);

    return s;
}

/* Attempts to connect to a given hostname using a port list and timeouts */
static int cobo_connect_hostname(char* hostname, int rank)
{
    int s = -1;
    struct in_addr saddr;

    /* lookup host address by name */
    struct hostent* he = gethostbyname(hostname);
    if (!he) {
       /* gethostbyname doesn't know how to resolve hostname, trying inet_addr */ 
       saddr.s_addr = inet_addr(hostname);
       if (saddr.s_addr == -1) {
           err_printf("Hostname lookup failed (gethostbyname(%s) %s h_errno=%d)\n",
                hostname, hstrerror(h_errno), h_errno);
           return s;
       }
    }
    else {
      saddr = *((struct in_addr *) (*he->h_addr_list));
    }

    /* Loop until we make a connection or until our timeout expires. */
    struct timeval start, end;
    cobo_gettimeofday(&start);
    double secs = 0;
    int connected = 0;
    int connect_timeout = cobo_connect_timeout;
    int reply_timeout = cobo_connect_timeout * 10;
    int result;
    while (!connected && secs < cobo_connect_timelimit) {
        /* iterate over our ports trying to find a connection */
        int i;
        for (i=0; i < cobo_num_ports; i++) {
            /* get our port */
            int port = cobo_ports[i];

            /* attempt to connect to hostname on this port */
            debug_printf3("Trying rank %d port %d on %s\n", rank, port, hostname);
            /* s = cobo_connect(*(struct in_addr *) (*he->h_addr_list), htons(port)); */
            s = cobo_connect(saddr, htons(port), connect_timeout);
            if (s != -1) {
                /* got a connection, let's test it out */
                debug_printf3("Connected to rank %d port %d on %s\n", rank, port, hostname);
                int test_failed = 0;

                result = spindle_handshake_client(s, &cobo_handshake, cobo_sessionid);
                switch (result) {
                   case HSHAKE_SUCCESS:
                      break;
                   case HSHAKE_INTERNAL_ERROR:
                      err_printf("Internal error doing handshake: %s", spindle_handshake_last_error_str());
                      exit(-1);
                      break;
                   case HSHAKE_DROP_CONNECTION:
                      debug_printf3("Handshake said to drop connection\n");
                      close(s);
                      continue;
                   case HSHAKE_ABORT:
                      handle_security_error(spindle_handshake_last_error_str());
                      abort();
                   default:
                      assert(0 && "Unknown return value from handshake_server\n");
                }
                
                /* write cobo service id */
                if (!test_failed && cobo_write_fd_w_suppress(s, &cobo_serviceid, sizeof(cobo_serviceid), 1) < 0) {
                    debug_printf3("Writing service id to %s on port %d\n",
                                  hostname, port);
                    test_failed = 1;
                }

                /* write our session id */
                if (!test_failed && cobo_write_fd_w_suppress(s, &cobo_sessionid, sizeof(cobo_sessionid), 1) < 0) {
                    debug_printf3("Writing session id to %s on port %d\n",
                                  hostname, port);
                    test_failed = 1;
                }

                /* read the service id */
                unsigned int received_serviceid = 0;
                if (!test_failed && cobo_read_fd_w_timeout(s, &received_serviceid, sizeof(received_serviceid), reply_timeout) < 0) {
                    debug_printf3("Receiving service id from %s on port %d failed\n",
                              hostname, port);
                    test_failed = 1;
                }

                /* read the accept id */
                unsigned int received_acceptid = 0;
                if (!test_failed && cobo_read_fd_w_timeout(s, &received_acceptid, sizeof(received_acceptid), reply_timeout) < 0) {
                    debug_printf3("Receiving accept id from %s on port %d failed\n",
                              hostname, port);
                    test_failed = 1;
                }

                /* check that we got the expected service and accept ids */
                if (!test_failed && (received_serviceid != cobo_serviceid || received_acceptid != cobo_acceptid)) {
                    test_failed = 1;
                }

                /* write ack to finalize connection (no need to suppress write errors any longer) */
                unsigned int ack = 1;
                if (!test_failed && cobo_write_fd(s, &ack, sizeof(ack)) < 0) {
                    debug_printf3("Writing ack to finalize connection to rank %d on %s port %d\n",
                               rank, hostname, port);
                    test_failed = 1;
                }

                /* if the connection test failed, close the socket, otherwise we've got a good connection */
                if (test_failed) {
                    close(s);
                } else {
                    connected = 1;
                    break;
                }
            }
        }

        /* sleep for some time before we try another port scan */
        if (!connected) {
            usleep(cobo_connect_sleep * 1000);

            /* maybe we connected ok, but we were too impatient waiting for a reply, extend the reply timeout for the next attempt */
            if (connect_timeout < 30000) {
              connect_timeout *= cobo_connect_backoff;
              reply_timeout   *= cobo_connect_backoff;
            }
        }

        /* compute how many seconds we've spent trying to connect */
        cobo_gettimeofday(&end);
        secs = cobo_getsecs(&end, &start);
        if (secs >= cobo_connect_timelimit) {
            err_printf("Time limit to connect to rank %d on %s expired\n",
                       rank, hostname);
        }
    }

    /* check that we successfully opened a socket */
    if (s == -1) {
        err_printf("Connecting socket to %s at %s failed\n",
                   hostname, inet_ntoa(saddr));
        return s;
    }

    return s;
}

/* send rank id and hostlist data to specified hostname */
static int cobo_send_hostlist(int s, char* hostname, int rank, int ranks, void* hostlist, int bytes)
{
    debug_printf3("Sending hostlist to rank %d on %s\n", rank, hostname);

    /* check that we have an open socket */
    if (s == -1) {
        err_printf("No connection to rank %d on %s to send hostlist\n",
                   rank, hostname);
        return (!COBO_SUCCESS);
    }

    /* forward the rank of hostname to hostname */
    if (cobo_write_fd(s, &rank, sizeof(rank)) < 0) {
        err_printf("Writing hostname table to rank %d on %s failed\n",
                   rank, hostname);
        return (!COBO_SUCCESS);
    }

    /* forward the number of ranks in the job to hostname */
    if (cobo_write_fd(s, &ranks, sizeof(ranks)) < 0) {
        err_printf("Writing hostname table to rank %d on %s failed\n",
                   rank, hostname);
        return (!COBO_SUCCESS);
    }

    /* forward the size of the hostlist in bytes */
    if (cobo_write_fd(s, &bytes, sizeof(bytes)) < 0) {
        err_printf("Writing hostname table to rank %d on %s failed\n",
                   rank, hostname);
        return (!COBO_SUCCESS);
    }

    /* and finally, forward the hostlist table */
    if (cobo_write_fd(s, hostlist, bytes) < 0) {
        err_printf("Writing hostname table to child (rank %d) at %s failed\n",
                   rank, hostname);
        return (!COBO_SUCCESS);
    }

    return COBO_SUCCESS;
}

/* 
 * =============================
 * Functions to open/close the TCP/socket tree.
 * =============================
*/

/* Allocates a string containing the hostname for specified rank.
 * The return string must be freed by the caller. */
static char* cobo_expand_hostname(int rank)
{
    if (cobo_hostlist == NULL) {
        return NULL;
    }

    int* offset = (int*) (cobo_hostlist + rank * sizeof(int));
    char* hostname = (char*) (cobo_hostlist + *offset);

    return strdup(hostname);
}

/* given cobo_me and cobo_nprocs, fills in parent and children ranks -- currently implements a binomial tree */
static int cobo_compute_children(int rank, int* max_children, int* parent, int* num_child, int** child, int** child_fd)
{
    /* compute the maximum number of children this task may have */
    int n = 1;
    *max_children = 0;
    while (n < cobo_nprocs) {
        n <<= 1;
        (*max_children)++;
    }

    /* prepare data structures to store our parent and children */
    *parent = 0;
    *num_child = 0;
    *child      = (int*) cobo_malloc((*max_children) * sizeof(int), "Child rank array");
    if (child_fd)
       *child_fd    = (int*) cobo_malloc((*max_children) * sizeof(int), "Child socket fd array");

    /* find our parent rank and the ranks of our children */
    int low  = 0;
    int high = cobo_nprocs - 1;
    while (high - low > 0) {
        int mid = (high - low) / 2 + (high - low) % 2 + low;
        if (low == rank) {
           (*child)[*num_child] = mid;
           (*num_child)++;
        }
        if (mid == rank) { *parent = low; }
        if (mid <= rank) { low  = mid; }
        else                { high = mid-1; }
    }

    return COBO_SUCCESS;
}

static void print_cobo_layer(int spaces, int rank)
{
   int max_children, parent, num_child, *child, i;
   char *str;
   int pos = 0, strsize;

   int offset = *((int*) (cobo_hostlist + rank * sizeof(int)));
   char* hostname = (char*) (cobo_hostlist + offset);
   

   strsize = (spaces * 4) + 4 + 32 + 3 + strlen(hostname);
   str = (char *) cobo_malloc(strsize, "print cobo");

   cobo_compute_children(rank, &max_children, &parent, &num_child, &child, NULL);
   
   for (i = 0; i < spaces-1; i++) {
      pos += snprintf(str+pos, strsize-pos, "|   ");
   }
   if (spaces) {
      pos += snprintf(str+pos, strsize-pos, "|---");      
   }
   pos += snprintf(str+pos, strsize-pos, "%d (%s)\n", rank, hostname);
   debug_printf3("%s", str);
   cobo_free(str);
   for (i = 0; i < num_child; i++) {
      print_cobo_layer(spaces+1, child[i]);
   }
   cobo_free(child);
}

static void print_cobo_tree()
{
   if (spindle_debug_prints > 2) {
      debug_printf3("Cobo tree layout:\n");
      print_cobo_layer(0, 0);
   }
}

#define ACCEPT_AND_HANDSHAKE_AGAIN -1
#define ACCEPT_AND_HANDSHAKE_ERROR -2

static int accept_connection_and_handshake(int reply_timeout)
{
   struct sockaddr parent_addr;
   socklen_t parent_len = sizeof(parent_addr);
   int new_connection_fd;
   int error;
   int retries = 16;

   for (;;) {
      new_connection_fd = accept(cobo_incoming_sockfd, (struct sockaddr *) &parent_addr, &parent_len);
      if (new_connection_fd != -1)
         break;
      error = errno;
      if (--retries <= 0) {
         err_printf("Too many errors retries accepting new socket: %s\n", strerror(error));
         return ACCEPT_AND_HANDSHAKE_ERROR;
      }
      switch (error) {
         case ENETDOWN:
         case EPROTO:
         case ENOPROTOOPT:
         case EHOSTDOWN:
         case ENONET:
         case EHOSTUNREACH:
         case EOPNOTSUPP:
         case ENETUNREACH:
         case EAGAIN:
         case EINTR:
            debug_printf3("Again-type error accepting new socket. Retrying. Error was %s\n", strerror(error));
            continue;
         default:
            err_printf("Error accepting new socket: %s\n", strerror(error));
            return ACCEPT_AND_HANDSHAKE_ERROR;
      }
   } 
   
   _cobo_opt_socket(new_connection_fd);
   
   /* handshake/authenticate our connection to make sure it one of our processes */
   int result = spindle_handshake_server(new_connection_fd, &cobo_handshake, cobo_sessionid);
   switch (result) {
      case HSHAKE_SUCCESS:
         break;
      case HSHAKE_INTERNAL_ERROR:
         err_printf("Internal error doing handshake: %s", spindle_handshake_last_error_str());
         exit(-1);
         break;
      case HSHAKE_DROP_CONNECTION:
         debug_printf3("Handshake said to drop connection\n");
         close(new_connection_fd);
         return ACCEPT_AND_HANDSHAKE_AGAIN;
      case HSHAKE_ABORT:
         handle_security_error(spindle_handshake_last_error_str());
         abort();
      default:
         assert(0 && "Unknown return value from handshake_server\n");
   }
   
   /* read the service id */
   unsigned int received_serviceid = 0;
   if (cobo_read_fd_w_timeout(new_connection_fd, &received_serviceid, sizeof(received_serviceid), reply_timeout) < 0) {
      debug_printf3("Receiving service id from new connection failed\n");
      close(new_connection_fd);
      return ACCEPT_AND_HANDSHAKE_AGAIN;
   }
   
   /* read the session id */
   uint64_t received_sessionid = 0;
   if (cobo_read_fd_w_timeout(new_connection_fd, &received_sessionid, sizeof(received_sessionid), reply_timeout) < 0) {
      debug_printf3("Receiving session id from new connection failed\n");
      close(new_connection_fd);
      return ACCEPT_AND_HANDSHAKE_AGAIN;
   }
   
   /* check that we got the expected service and session ids */
   /* TODO: reply with some sort of error message if no match? */
   if (received_serviceid != cobo_serviceid || received_sessionid != cobo_sessionid) {
      close(new_connection_fd);
      return ACCEPT_AND_HANDSHAKE_AGAIN;
   }
   
   /* write our service id back as a reply */
   if (cobo_write_fd_w_suppress(new_connection_fd, &cobo_serviceid, sizeof(cobo_serviceid), 1) < 0) {
      debug_printf3("Writing service id to new connection failed\n");
      close(new_connection_fd);
      return ACCEPT_AND_HANDSHAKE_AGAIN;
   }
   
   /* write our accept id back as a reply */
   if (cobo_write_fd_w_suppress(new_connection_fd, &cobo_acceptid, sizeof(cobo_acceptid), 1) < 0) {
      debug_printf3("Writing accept id to new connection failed\n");
      close(new_connection_fd);
      return ACCEPT_AND_HANDSHAKE_AGAIN;
   }
   
   /* our parent may have dropped us if he was too impatient waiting for our reply,
    * read his ack to know that he completed the connection */
   unsigned int ack = 0;
   if (cobo_read_fd_w_timeout(new_connection_fd, &ack, sizeof(ack), reply_timeout) < 0) {
      debug_printf3("Receiving ack to finalize connection\n");
      close(new_connection_fd);
      return ACCEPT_AND_HANDSHAKE_AGAIN;
   }

   return new_connection_fd;
}

/* open socket tree across tasks */
static int cobo_open_tree()
{
    /* create a socket to accept connection from parent IPPROTO_TCP */
   cobo_incoming_sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (cobo_incoming_sockfd < 0) {
        err_printf("Creating parent socket (socket() %m errno=%d)\n",
                   errno);
        exit(1);
    }
    setCloseOnExec(cobo_incoming_sockfd);

    /* TODO: could recycle over port numbers, trying to bind to one for some time */
    /* try to bind the socket to one the ports in our allowed range */
    int i = 0;
    int port_is_bound = 0;
    while (i < cobo_num_ports && !port_is_bound) {
        /* pick a port */
        int port = cobo_ports[i];
        i++;

        /* set up an address using our selected port */
        struct sockaddr_in sin;
        memset(&sin, 0, sizeof(sin));
        sin.sin_family = AF_INET;
        sin.sin_addr.s_addr = htonl(INADDR_ANY);
        sin.sin_port = htons(port);

        /* attempt to bind a socket on this port */
        if (bind(cobo_incoming_sockfd, (struct sockaddr *) &sin, sizeof(sin)) < 0) {
            debug_printf3("Binding parent socket (bind() %m errno=%d) port=%d\n",
                errno, port);
            continue;
        }

        struct linger slinger;
        slinger.l_onoff = 1;
        slinger.l_linger = 0;
        setsockopt(cobo_incoming_sockfd, SOL_SOCKET, SO_LINGER, &slinger, sizeof(slinger));

        /* set the socket to listen for connections */
        if (listen(cobo_incoming_sockfd, 1) < 0) {
           debug_printf3("Setting parent socket to listen (listen() %m errno=%d) port=%d\n",
                errno, port);
           continue;
        }

        /* bound and listening on our port */
        debug_printf3("Opened socket on port %d\n", port);
        port_is_bound = 1;
        tree_port = port;
    }

    /* failed to bind to a port, this is fatal */
    if (!port_is_bound) {
        /* TODO: would like to send an abort back to server */
        err_printf("Failed to open socket on any port\n");
        exit(1);
    }

    /* accept a connection from parent and receive socket table */
    int reply_timeout = cobo_connect_timeout * 100;
    int have_parent = 0;
    while (!have_parent) {
       cobo_parent_fd = accept_connection_and_handshake(reply_timeout);
       if (cobo_parent_fd == ACCEPT_AND_HANDSHAKE_AGAIN)
          continue;
       if (cobo_parent_fd == ACCEPT_AND_HANDSHAKE_ERROR)
          return -1;
       
        /* if we get here, we've got a good connection to our parent */
        have_parent = 1;
    }

    cobo_gettimeofday(&tree_start);

    /* TODO: exchange protocol version number */

    /* read our rank number */
    if (cobo_read_fd(cobo_parent_fd, &cobo_me, sizeof(int)) < 0) {
        err_printf("Receiving my rank from parent failed\n");
        exit(1);
    }

    /* discover how many ranks are in our world */
    if (cobo_read_fd(cobo_parent_fd, &cobo_nprocs, sizeof(int)) < 0) {
        err_printf("Receiving number of tasks from parent failed\n");
        exit(1);
    }

    /* read the size of the hostlist (in bytes) */
    if (cobo_read_fd(cobo_parent_fd, &cobo_hostlist_size, sizeof(int)) < 0) {
        err_printf("Receiving size of hostname table from parent failed\n");
        exit(1);
    }

    /* allocate space for the hostlist and read it in */
    cobo_hostlist = (void*) cobo_malloc(cobo_hostlist_size, "Hostlist data buffer");
    if (cobo_read_fd(cobo_parent_fd, cobo_hostlist, cobo_hostlist_size) < 0) {
        err_printf("Receiving hostname table from parent failed\n");
        exit(1);
    }

/*
    if (cobo_me == 0) {
      for (i=0; i < cobo_nprocs; i++) {
        char* tmp = cobo_expand_hostname(i);
        printf("rank %d = %s\n", i, tmp);
        free(tmp);
      }
    }
*/

    /* given our rank and the number of ranks, compute the ranks of our children */
    cobo_compute_children(cobo_me, &cobo_max_children, &cobo_parent, &cobo_num_child, &cobo_child, &cobo_child_fd);
    print_cobo_tree();    
    /* cobo_compute_children_root_C1(); */

    char **child_names = (char **) malloc(sizeof(char *) * cobo_num_child);
    for (i = 0; i < cobo_num_child; i++) {
       int c = cobo_child[i];
       child_names[i] = cobo_expand_hostname(c);
    }

    /* Spindle can register a pre-connect callback, which can be used to
       spawn daemons */
    if (preconnect_cb) {
       for (i = 0; i < cobo_num_child; i++)
          preconnect_cb(child_names[i]);
    }

    /* for each child, open socket connection and forward hostname table */
    for(i=0; i < cobo_num_child; i++) {
        /* get rank and hostname for this child */
        int c = cobo_child[i];
        char* child_hostname = child_names[i];

        debug_printf3("%d: on COBO%02d: connect to child #%02d (%s)\n",i,cobo_me,c,child_hostname);

        /* connect to child */
        cobo_child_fd[i] = cobo_connect_hostname(child_hostname, c);
        if (cobo_child_fd[i] == -1) {
            err_printf("Failed to connect to child (rank %d) on %s failed\n",
                       c, child_hostname);
            exit(1);
        }

        /* tell child what rank he is and forward the hostname table to him */
        int forward = cobo_send_hostlist(cobo_child_fd[i], child_hostname, c,
                          cobo_nprocs, cobo_hostlist, cobo_hostlist_size);
        if (forward != COBO_SUCCESS) {
            err_printf("Failed to forward hostname table to child (rank %d) on %s failed\n",
                       c, child_hostname);
            exit(1);
        }

        /* free the child hostname string */
        free(child_hostname);
    }

    free(child_names);

#if defined(SPINDLE_ENABLE_RELIABILITY)
    debug_printf3("Leaving cobo_incoming_sockfd = %d open for new children after failure\n", cobo_incoming_sockfd);
#else
    /* we've got the connection to our parent, so close the listening socket */
    close(cobo_incoming_sockfd);
    cobo_incoming_sockfd = -1;
#endif
    return COBO_SUCCESS;
}

/*
 * close down socket connections for tree (parent and any children), free
 * related memory
 */
static int cobo_close_tree()
{
    /* close socket connection with parent */
    close(cobo_parent_fd);

    /* and all my children */
    int i;
    for(i=0; i<cobo_num_child; i++) {
        close(cobo_child_fd[i]);
    }

    /* free data structures */
    cobo_free(cobo_child);
    cobo_free(cobo_child_fd);
    cobo_free(cobo_hostlist);

    return COBO_SUCCESS;
}

/* 
 * =============================
 * Functions to bcast/gather/scatter with root as rank 0 using the TCP/socket tree.
 * =============================
*/

/* broadcast size bytes from buf on rank 0 using socket tree */
static int cobo_bcast_tree(void* buf, int size)
{
    int rc = COBO_SUCCESS;
    int i;

    /* if i'm not rank 0, receive data from parent */
    if (cobo_me != 0) {
        if (cobo_read_fd(cobo_parent_fd, buf, size) < 0) {
            err_printf("Receiving broadcast data from parent failed\n");
            exit(1);
        }
    }

    /* for each child, forward data */
    for(i=0; i<cobo_num_child; i++) {
        if (cobo_write_fd(cobo_child_fd[i], buf, size) < 0) {
            err_printf("Broadcasting data to child (rank %d) failed\n",
                       cobo_child[i]);
            exit(1);
        }
    }

    return rc;
}

int cobo_bcast_down(void* buf, int size)
{
   int rc = COBO_SUCCESS;
   int i;
   /* for each child, forward data */
   for(i=0; i<cobo_num_child; i++) {
      if (cobo_write_fd(cobo_child_fd[i], buf, size) < 0) {
         err_printf("Broadcasting data to child (rank %d) failed\n",
                    cobo_child[i]);
         exit(1);
      }
   }   
   return rc;
}

/* reduce maximum integer to rank 0 */
static int cobo_allreduce_max_int_tree(int* sendbuf, int* recvbuf)
{
    int rc = COBO_SUCCESS;

    /* init our current maximum to our own value */
    int max_val = *sendbuf;

    /* if i have any children, receive their data */
    int i;
    int child_val;
    for(i=cobo_num_child-1; i>=0; i--) {
        /* read integer from child */
        if (cobo_read_fd(cobo_child_fd[i], &child_val, sizeof(child_val)) < 0) {
            err_printf("Reducing data from child (rank %d) failed\n",
                       cobo_child[i]);
            exit(1);
        }

        /* compare child's max to our current max */
        if (child_val > max_val) {
            max_val = child_val;
        }
    }

    /* forward data to parent if we're not rank 0, otherwise set the recvbuf */
    if (cobo_me != 0) {
        /* not the root, so forward our reduction result to our parent */
        if (cobo_write_fd(cobo_parent_fd, &max_val, sizeof(max_val)) < 0) {
            err_printf("Sending reduced data to parent failed\n");
            exit(1);
        }
    } else {
        /* we're the root, got the result, set the recvbuf */
        *recvbuf = max_val;
    }

    /* broadcast result of reduction from rank 0 to all tasks */
    cobo_bcast_tree(recvbuf, sizeof(int));

    return rc;
}

int cobo_get_child_socket(int num, int *fd)
{
   assert(num < cobo_num_child);
   *fd = cobo_child_fd[num];
   return COBO_SUCCESS;
}

/*
 * ==========================================================================
 * ==========================================================================
 * Client Interface Functions
 * ==========================================================================
 * ==========================================================================
 */

/* NEW */
int cobo_get_num_childs(int* num_childs) {
  *num_childs=cobo_num_child;
  return COBO_SUCCESS;
}



/* fills in fd with socket file desriptor to our parent */
/* TODO: the upside here is that the upper layer can directly use our
 * communication tree, but the downside is that it exposes the implementation
 * and forces sockets */
int cobo_get_parent_socket(int* fd)
{
    if (cobo_parent_fd != -1) {
       *fd = cobo_parent_fd;
       return COBO_SUCCESS;
    }

    return -1; /* failure RCs? */ 
}

/* Perform barrier, each task writes an int then waits for an int */
int cobo_barrier()
{
    struct timeval start, end;
    cobo_gettimeofday(&start);
    debug_printf3("Starting cobo_barrier()\n");

    /* use allreduce of an int for our barrier */
    int dummy;
    int myint = 1;
    cobo_allreduce_max_int_tree(&myint, &dummy);

    cobo_gettimeofday(&end);
    debug_printf3("Exiting cobo_barrier(), took %f seconds for %d procs\n", cobo_getsecs(&end,&start), cobo_nprocs);
    return COBO_SUCCESS;
}

/*
 * Perform MPI-like Broadcast, root writes sendcount bytes from buf,
 * all receive sendcount bytes into buf
 */
int cobo_bcast(void* buf, int sendcount, int root)
{
    struct timeval start, end;
    cobo_gettimeofday(&start);
    debug_printf3("Starting cobo_bcast()");

    int rc = COBO_SUCCESS;

    /* if root is rank 0 and bcast tree is enabled, use it */
    /* (this is a common case) */
    if (root == 0) {
        rc = cobo_bcast_tree(buf, sendcount);
    } else {
        err_printf("Cannot execute bcast from non-zero root\n");
        exit(1);
    }

    cobo_gettimeofday(&end);
    debug_printf3("Exiting cobo_bcast(), took %f seconds for %d procs\n", cobo_getsecs(&end,&start), cobo_nprocs);
    return rc;
}

/* provide list of ports and number of ports as input, get number of tasks and my rank as output */
int cobo_open(uint64_t sessionid, int* portlist, int num_ports, int* rank, int* num_ranks)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    char *value;

    struct timeval start, end;
    cobo_gettimeofday(&start);

    /* we now know this process is a client, although we don't know what our rank is yet */
    cobo_me = -1;

    /* record the sessionid, which we'll use to verify our connections */
    cobo_sessionid = sessionid;

    /* =======================================================
     * Get information from environment
     * ======================================================= */

    /* milliseconds */
    if ((value = cobo_getenv("COBO_CONNECT_TIMEOUT\n", ENV_OPTIONAL))) {
        cobo_connect_timeout = atoi(value);
    }

    /* exponential backoff factor for connect */
    if ((value = cobo_getenv("COBO_CONNECT_BACKOFF\n", ENV_OPTIONAL))) {
        cobo_connect_backoff = atoi(value);
    }

    /* milliseconds to sleep before rescanning ports */
    if ((value = cobo_getenv("COBO_CONNECT_SLEEP\n", ENV_OPTIONAL))) {
        cobo_connect_sleep = atoi(value);
    }

    /* seconds */
    if ((value = cobo_getenv("COBO_CONNECT_TIMELIMIT\n", ENV_OPTIONAL))) {
        cobo_connect_timelimit = (double) atoi(value);
    }

    debug_printf3("In cobo_init():\n" \
        "COBO_CONNECT_TIMEOUT: %d, COBO_CONNECT_BACKOFF: %d, COBO_CONNECT_SLEEP: %d, COBO_CONNECT_TIMELIMIT: %d\n",
        cobo_connect_timeout, cobo_connect_backoff, cobo_connect_sleep, (int) cobo_connect_timelimit);

    /* copy port list from user */
    cobo_num_ports = num_ports;
    cobo_ports = cobo_int_dup(portlist, num_ports);
    if (cobo_ports == NULL) {
        err_printf("Failed to copy port list\n");
        exit(1);
    }

    /* open the tree */
    cobo_open_tree();

    /* need to check that tree opened successfully before returning, so do a barrier */
    if (cobo_barrier() != COBO_SUCCESS) {
        err_printf("Failed to open tree\n");
        exit(1);
    }

    if (cobo_me == 0) {
        cobo_gettimeofday(&tree_end);
        debug_printf3("Exiting cobo_close(), took %f seconds for %d procs\n", cobo_getsecs(&tree_end,&tree_start), cobo_nprocs);
    }

    /* return our rank and the number of ranks in our world */
    *rank      = cobo_me;
    *num_ranks = cobo_nprocs;

    cobo_gettimeofday(&end);
    debug_printf3("Exiting cobo_init(), took %f seconds for %d procs\n", cobo_getsecs(&end,&start), cobo_nprocs);
    return COBO_SUCCESS;
}

/* shut down the connections between tasks and free data structures */
int cobo_close()
{
    struct timeval start, end;
    cobo_gettimeofday(&start);
    debug_printf3("Starting cobo_close()");

    /* shut down the tree */
    cobo_close_tree();

    /* free our data structures */
    cobo_free(cobo_ports);

    cobo_gettimeofday(&end);
    cobo_gettimeofday(&time_close);
    debug_printf3("Exiting cobo_close(), took %f seconds for %d procs\n", cobo_getsecs(&end,&start), cobo_nprocs);
    debug_printf3("Total time from cobo_open() to cobo_close() took %f seconds for %d procs\n",
        cobo_getsecs(&time_close, &time_open), cobo_nprocs);
    return COBO_SUCCESS;
}

/*
 * ==========================================================================
 * ==========================================================================
 * Server Interface Functions
 * ==========================================================================
 * ==========================================================================
 */

/* fills in fd with socket file desriptor to the root client process (rank 0) */
/* TODO: the upside here is that the upper layer can directly use our
 * communication tree, but the downside is that it exposes the implementation
 * and forces sockets */
int cobo_server_get_root_socket(int* fd)
{
    if (cobo_root_fd != -1) {
        *fd = cobo_root_fd;
        return COBO_SUCCESS;
    }

    return -1;
}

/* given a hostlist and portlist where clients are running, open the tree and assign ranks to clients */
int cobo_server_open(uint64_t sessionid, char** hostlist, int num_hosts, int* portlist, int num_ports)
{
    /* at this point, we know this process is the server, so set its rank */
    cobo_me = -2;
    cobo_nprocs = num_hosts;
    cobo_sessionid = sessionid;

    /* check that we have some hosts in the hostlist */
    if (num_hosts <= 0) {
        return (!COBO_SUCCESS);
    }

    /* determine the total number of bytes to hold the strings including terminating NUL character */
    int i;
    int size = 0;
    for (i=0; i < num_hosts; i++) {
        size += strlen(hostlist[i]) + 1;
    }

    /* determine and allocate the total number of bytes to hold the strings plus offset table */
    cobo_hostlist_size = num_hosts * sizeof(int) + size;
    cobo_hostlist = cobo_malloc(cobo_hostlist_size, "Buffer for hostlist data structure");
    if (cobo_hostlist == NULL) {
        err_printf("Failed to allocate hostname table of %lu bytes\n",
                   (unsigned long) cobo_hostlist_size);
        return (!COBO_SUCCESS);
    }

    /* copy the strings in and fill in the offsets */
    int offset = num_hosts * sizeof(int);
    for (i=0; i < num_hosts; i++) {
        ((int*)cobo_hostlist)[i] = offset;
        strcpy((char*)(cobo_hostlist + offset), hostlist[i]);
        offset += strlen(hostlist[i]) + 1;
    }

    /* Spindle can register a pre-connect callback, which can be used to
       spawn daemons */
    if (preconnect_cb)
       preconnect_cb(hostlist[0]);
            
    /* copy the portlist */
    cobo_num_ports = num_ports;
    cobo_ports = cobo_int_dup(portlist, num_ports);
    if (cobo_ports == NULL) {
        err_printf("Failed to copy port list\n");
        return (!COBO_SUCCESS);
    }

    /* connect to first host */
    cobo_root_fd = cobo_connect_hostname(hostlist[0], 0);
    if (cobo_root_fd == -1) {
        err_printf("Failed to connect to child (rank %d) on %s failed\n",
                   0, hostlist[0]);
        return (!COBO_SUCCESS);
    }

    /* forward the hostlist table to the first host */
    int forward = cobo_send_hostlist(cobo_root_fd, hostlist[0], 0, num_hosts, cobo_hostlist, cobo_hostlist_size);
    if (forward != COBO_SUCCESS) {
        err_printf("Failed to forward hostname table to child (rank %d) on %s failed\n",
                   0, hostlist[0]);
        return (!COBO_SUCCESS);
    }

    return COBO_SUCCESS;
}

/* shut down the tree connections (leaves processes running) */
int cobo_server_close()
{
    /* close the socket to our child */
    if (cobo_root_fd != -1) {
        close(cobo_root_fd);
    }

    /* free data structures */
    cobo_free(cobo_ports);
    cobo_free(cobo_hostlist);

    return COBO_SUCCESS;
}

void cobo_set_handshake(handshake_protocol_t *hs)
{
   cobo_handshake = *hs;
}

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

static int select_random_parent_rank(int *ranks_to_avoid)
{
   int new_parent, i;
   int attempts, num_attempts = 10;

   for (attempts = 0; attempts < num_attempts; attempts++) {
      //Make sure parent is always a smaller rank than me. This prevents cycles.
      new_parent = get_random_int(cobo_me);
      if (new_parent == -1) {
         err_printf("Could not pick random number for new parent rank\n");
         return -1;
      }

      for (i = 0; ranks_to_avoid[i] != -1 && new_parent != ranks_to_avoid[i]; i++);
      if (new_parent == ranks_to_avoid[i]) {
         debug_printf3("During new parent selection picked random parent %d from ranks_to_avoid list\n", new_parent);
         continue;
      }
      //TODO track dead ranks
      return new_parent;
   }
   
   err_printf("Could not find a random parent within %d attempts\n", num_attempts);
   return -1;
}

#define NEW_PARENT_RETRIES 8
int cobo_establish_new_parent()
{
   int new_parent, new_parent_fd, old_parent;
   int i, cur = 0, result;
   char *hostname;
   int ranks_to_avoid[NEW_PARENT_RETRIES + 3];
   int retries = NEW_PARENT_RETRIES;

   close(cobo_parent_fd);
   old_parent = cobo_parent;
   for (i = 0; i < NEW_PARENT_RETRIES + 3; i++) ranks_to_avoid[i] = -1;
   ranks_to_avoid[cur++] = cobo_me;
   ranks_to_avoid[cur++] = old_parent;

  again:
   {
      new_parent = select_random_parent_rank(ranks_to_avoid);
      if (new_parent == -1) {
         err_printf("Aborting attempt to find new parent\n");
         return -1;
      }

      hostname = cobo_expand_hostname(new_parent);

      debug_printf3("Attempting new parent connection to rank %d/host %s from rank %d\n",
                    new_parent, hostname, cobo_me);
      new_parent_fd = cobo_connect_hostname(hostname, new_parent);
      if (new_parent_fd == -1) {
         debug_printf3("Tried and failed to connect to rank %d on %s\n", new_parent, hostname);
         free(hostname);
         if (--retries <= 0) {
            err_printf("Failed to connect too many (%d) times. Aborting\n", NEW_PARENT_RETRIES);
            return -1;
         }
         assert(cur < sizeof(ranks_to_avoid)/sizeof(ranks_to_avoid[0]));
         ranks_to_avoid[cur++] = new_parent;
         goto again;
      }
   }

   cobo_parent = new_parent;
   cobo_parent_fd = new_parent_fd;

   result = cobo_write_fd(cobo_parent_fd, &cobo_newchild_id, sizeof(cobo_newchild_id));
   if (result == -1) {
      err_printf("Failed to write cobo_newchild_id to parent\n");
      close(cobo_parent_fd);
      cobo_parent = cobo_parent_fd = -1;
      return -1;
   }
   result = cobo_write_fd(cobo_parent_fd, &cobo_me, sizeof(cobo_me));
   if (result == -1) {
      err_printf("Failed to write my rank to new parent\n");
      close(cobo_parent_fd);
      cobo_parent = cobo_parent_fd = -1;      
      return -1;
   }
   debug_printf3("Connection to %s as new parent successful\n", hostname);
   return cobo_parent_fd;
}

int cobo_accept_new_child()
{
   int result;
   int new_child_rank, new_child_fd;
   unsigned int newchild_id;
   
   int reply_timeout = cobo_connect_timeout * 100;
   
   new_child_fd = accept_connection_and_handshake(reply_timeout);
   if (new_child_fd == ACCEPT_AND_HANDSHAKE_AGAIN) {
      debug_printf3("Soft error on child attempting to connect\n");
      return NC_SOFT_ERROR;
   }
   if (new_child_fd == ACCEPT_AND_HANDSHAKE_ERROR) {
      err_printf("Error during new child connection attempt\n");
      return NC_ERROR;
   }   

   result = cobo_read_fd_w_timeout(new_child_fd, &newchild_id, sizeof(newchild_id), reply_timeout);
   if (result == -1) {
      err_printf("Failure communicating new child id\n");
      close(new_child_fd);
      return NC_SOFT_ERROR;
   }
   if (newchild_id != cobo_newchild_id) {
      err_printf("New child provided incorrect id = %ud (%x)\n", newchild_id, cobo_newchild_id);
      close(new_child_fd);
      return NC_SOFT_ERROR;
   }
   
   result = cobo_read_fd_w_timeout(new_child_fd, &new_child_rank, sizeof(new_child_rank), reply_timeout);
   if (result == -1) {
      err_printf("Failure communicating with new child. Dropping them.\n");
      close(new_child_fd);
      return NC_ERROR;
   }
   if (new_child_rank < 0 || new_child_rank >= cobo_nprocs) {
      err_printf("Recvd junk child rank from new child: %d. Dropping them.\n", new_child_rank);
      close(new_child_fd);
      return NC_ERROR;      
   }
   
   if (cobo_num_child == cobo_max_children) {
      cobo_max_children *= 2;
      cobo_child = (int*) realloc(cobo_child, cobo_max_children * sizeof(int));
      cobo_child_fd = (int*) realloc(cobo_child_fd, cobo_max_children * sizeof(int));
   }
   cobo_child[cobo_num_child] = new_child_rank;
   cobo_child_fd[cobo_num_child] = new_child_fd;
   return new_child_fd;
}

int cobo_get_new_connection_socket()
{
   return cobo_incoming_sockfd;
}
