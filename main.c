/* Copyright (C) 2000 drscholl@users.sourceforge.net
   This is free software distributed under the terms of the
   GNU Public License.  See the file COPYING for details.

   $Id$ */

#ifdef WIN32
#include <windows.h>
#include <winsock.h>
#endif /* WIN32 */
#include <stdlib.h>
#include <signal.h>
#include <stdio.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#ifndef WIN32
#include <unistd.h>
#include <time.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#if HAVE_POLL
#include <sys/poll.h>
#else
/* use select() instead of poll() */
#include <sys/time.h>
#endif /* HAVE_POLL */
#endif /* !WIN32 */
#include "opennap.h"
#include "debug.h"

/*
** Global Variables
*/

char *Motd_Path = 0;
char *Db_User = 0;
char *Db_Pass = 0;
char *Db_Host = 0;
char *Db_Name = 0;
char *Listen_Addr = 0;
char *Server_Name = 0;
char *Server_Pass = 0;
unsigned int Server_Ip = 0;
unsigned int Server_Flags = 0;
int Max_User_Channels;		/* default, can be changed in config */
int Stat_Click;			/* interval (in seconds) to send server stats */
int Server_Port;		/* which port to listen on for connections */
int Server_Queue_Length;
int Client_Queue_Length;
int Max_Search_Results;
int Compression_Level;
int Compression_Threshold;
int Max_Shared;
int Max_Connections;
int Nick_Expire;
int Check_Expire;
int Max_Browse_Result;
unsigned int Interface = INADDR_ANY;

/* bans on ip addresses / users */
BAN **Ban = 0;
int Ban_Size = 0;

/* local clients (can be users or servers) */
CONNECTION **Clients = NULL;
int Num_Clients = 0;

/* global users list */
HASH *Users;

/* global file list */
HASH *File_Table;

/* global hash list */
HASH *MD5;

/* local server list.  NOTE that this contains pointers into the Clients
   list to speed up server-server message passing */
CONNECTION **Servers = NULL;
int Num_Servers = 0;

int Num_Files = 0;
int Num_Gigs = 0;		/* in kB */
int SigCaught = 0;
char Buf[1024];			/* global scratch buffer */

/* global channel list */
HASH *Channels;

/* global hotlist */
HASH *Hotlist;

#define BACKLOG 5

static void
sighandler (int sig)
{
    (void) sig;			/* unused */
    SigCaught = 1;
}

HANDLER (server_stats)
{
    (void) pkt;
    (void) tag;
    (void) len;
    send_cmd (con, MSG_SERVER_STATS, "%d %d %d", Users->dbsize, Num_Files,
	      Num_Gigs / (1024 * 1024));
}

typedef struct
{
	unsigned int message;
	HANDLER ((*handler));
}
HANDLER;

/* this is the table of valid commands we accept from both users and servers */
static HANDLER Protocol[] = {
    {MSG_CLIENT_LOGIN, login},	/* 2 */
    {MSG_CLIENT_LOGIN_REGISTER, login},	/* 6 */
    {MSG_CLIENT_REGISTER, register_nick}, /* 7 */
    {MSG_CLIENT_ADD_FILE, add_file},	/* 100 */
    {MSG_CLIENT_REMOVE_FILE, remove_file},	/* 102 */
    {MSG_CLIENT_SEARCH, search},	/* 200 */
    {MSG_CLIENT_PRIVMSG, privmsg},	/* 205 */
    {MSG_CLIENT_ADD_HOTLIST, add_hotlist},	/* 207 */
    {MSG_CLIENT_ADD_HOTLIST_SEQ, add_hotlist},	/* 208 */
    {MSG_CLIENT_BROWSE, browse},	/* 211 */
    {MSG_SERVER_STATS, server_stats},	/* 214 */
    {MSG_CLIENT_RESUME_REQUEST, resume},	/* 215 */
    {MSG_CLIENT_DOWNLOAD_START, download_start},	/* 218 */
    {MSG_CLIENT_DOWNLOAD_END, download_end},	/* 219 */
    {MSG_CLIENT_UPLOAD_START, upload_start},	/* 220 */
    {MSG_CLIENT_UPLOAD_END, upload_end},	/* 221 */
    {MSG_CLIENT_REMOVE_HOTLIST, remove_hotlist},	/* 303 */
    {MSG_SERVER_NOSUCH, server_error},	/* 404 */
    {MSG_CLIENT_DOWNLOAD_FIREWALL, download},	/* 500 */
    {MSG_CLIENT_WHOIS, whois},
    {MSG_CLIENT_JOIN, join},
    {MSG_CLIENT_PART, part},
    {MSG_CLIENT_PUBLIC, public},
    {MSG_SERVER_PUBLIC, public},
    {MSG_CLIENT_USERSPEED, user_speed},	/* 600 */
    {MSG_CLIENT_KILL, kill_user},
    {MSG_CLIENT_DOWNLOAD, download},
    {MSG_CLIENT_UPLOAD_OK, upload_ok},
    {MSG_SERVER_UPLOAD_REQUEST, upload_request},	/* 607 */
    {MSG_SERVER_TOPIC, topic},
    {MSG_CLIENT_MUZZLE, muzzle},
    {MSG_CLIENT_UNMUZZLE, unmuzzle},
    {MSG_CLIENT_BAN, ban},	/* 612 */
    {MSG_CLIENT_ALTER_PORT, alter_port},	/* 613 */
    {MSG_CLIENT_UNBAN, unban},	/* 614 */
    {MSG_CLIENT_BANLIST, banlist},	/* 615 */
    {MSG_CLIENT_LIST_CHANNELS, list_channels},	/* 618 */
    {MSG_CLIENT_LIMIT, queue_limit},	/* 619 */
    {MSG_CLIENT_MOTD, show_motd},	/* 621 */
    {MSG_CLIENT_DATA_PORT_ERROR, data_port_error},	/* 626 */
    {MSG_CLIENT_WALLOP, wallop},	/* 627 */
    {MSG_CLIENT_ANNOUNCE, announce},	/* 628 */
    {MSG_CLIENT_SETUSERLEVEL, level},
    {MSG_CLIENT_CHANGE_SPEED, change_speed},	/* 700 */
    {MSG_CLIENT_CHANGE_PASS, change_pass},	/* 701 */
    {MSG_CLIENT_CHANGE_EMAIL, change_email},	/* 702 */
    {MSG_CLIENT_CHANGE_DATA_PORT, change_data_port}, /* 703 */
    {MSG_CLIENT_PING, ping},	/* 751 */
    {MSG_CLIENT_PONG, ping},	/* 752 */
    {MSG_CLIENT_SERVER_RECONFIG, server_reconfig},	/* 800 */
    {MSG_CLIENT_SERVER_VERSION, server_version},	/* 801 */
    {MSG_CLIENT_SERVER_CONFIG, server_config},	/* 810 */
    {MSG_CLIENT_EMOTE, emote},	/* 824 */
    {MSG_CLIENT_NAMES_LIST, list_users},	/* 830 */

    /* non-standard messages */
    {MSG_CLIENT_QUIT, client_quit},
    {MSG_SERVER_LOGIN, server_login},
    {MSG_SERVER_LOGIN, server_login},
    {MSG_SERVER_LOGIN_ACK, server_login_ack},
    {MSG_SERVER_USER_IP, user_ip},	/* 10013 */
    {MSG_CLIENT_CONNECT, server_connect},	/* 10100 */
    {MSG_CLIENT_DISCONNECT, server_disconnect},	/* 10101 */
    {MSG_CLIENT_KILL_SERVER, kill_server},	/* 10110 */
    {MSG_CLIENT_REMOVE_SERVER, remove_server},	/* 10111 */
    {MSG_SERVER_REGINFO, reginfo },		/* 10114 */
#if 0
    {MSG_SERVER_COMPRESSED_DATA, compressed_data},	/* 10200 */
#endif
    {MSG_CLIENT_SHARE_FILE, share_file},
    {MSG_SERVER_REMOTE_ERROR, priv_errmsg},	/* 10404 */
};
static int Protocol_Size = sizeof (Protocol) / sizeof (HANDLER);

/* this is not a real handler, but takes the same arguments as one */
HANDLER (dispatch_command)
{
    int l;
    unsigned char byte;

    ASSERT (validate_connection (con));

    /* HACK ALERT
       the handler routines all assume that the `pkt' argument is nul (\0)
       terminated, so we have to replace the byte after the last byte in
       this packet with a \0 to make sure we dont read overflow in the
       handlers.  the buffer_read() function should always allocate 1 byte
       more than necessary for this purpose */
    ASSERT (VALID_LEN (con->recvbuf->data, con->recvbuf->consumed + 4 + len + 1));
    byte = *(pkt + len);
    *(pkt + len) = 0;

    for (l = 0; l < Protocol_Size; l++)
    {
	if (Protocol[l].message == tag)
	{
	    ASSERT (Protocol[l].handler != 0);
	    /* note that we pass only the data part of the packet */
	    Protocol[l].handler (con, tag, len, pkt);
	    break;
	}
    }

    if (l == Protocol_Size)
    {
	log
	    ("dispatch_command(): unknown message: tag=%hu, length=%hu, data=%s",
	     tag, len,
	     len ? (char *) con->recvbuf->data +
	     con->recvbuf->consumed + 4 : "(empty)");

	send_cmd (con, MSG_SERVER_NOSUCH, "unknown command code %hu", tag);
    }

    /* restore the byte we overwrite at the beginning of this function */
    *(pkt + len) = byte;
}

static void
handle_connection (CONNECTION * con)
{
    unsigned short len, tag;

    ASSERT (validate_connection (con));

#if HAVE_LIBZ
    /* decompress server input stream */
    if (con->class == CLASS_SERVER)
    {
	BUFFER *b;

	ASSERT (con->zip != 0);
	if (con->zip->inbuf
	    && (b = buffer_uncompress (con->zip->zin, &con->zip->inbuf)))
	    con->recvbuf = buffer_append (con->recvbuf, b);
    }
#endif /* HAVE_LIBZ */

    /* check if there is enough data in the buffer to read the packet header */
    if (buffer_size (con->recvbuf) < 4)
    {
	/* we set this flag here to avoid busy waiting in the main select()
	   loop.  we can't process any more input until we get some more
	   data */
	con->incomplete = 1;
	return;
    }
    /* make sure all 4 bytes of the header are in the first block */
    if (buffer_group (con->recvbuf, 4) == -1)
    {
	/* probably a memory allocation error, close this connection since
	   we can't handle it */
	log ("handle_connection(): could not read packet header from buffer");
	con->destroy = 1;
	return;
    }
    memcpy (&len, con->recvbuf->data + con->recvbuf->consumed, 2);
    memcpy (&tag, con->recvbuf->data + con->recvbuf->consumed + 2, 2);

    /* need to convert to little endian */
    len = BSWAP16 (len);
    tag = BSWAP16 (tag);

    /* see if all of the packet body is present */
    if (buffer_size (con->recvbuf) < 4 + len)
    {
	/* nope, wait until more data arrives */
#if 0
	log ("handle_connection(): waiting for %d bytes from client (tag=%d)",
		len, tag);
#endif
	con->incomplete = 1;
	return;
    }

    con->incomplete = 0;	/* found all the data we wanted */

    /* the packet may be fragmented so make sure all of the bytes for this
       packet end up in the first buffer so its easy to handle */
    if (buffer_group (con->recvbuf, 4 + len) == -1)
    {
	/* probably a memory allocation error, close this connection since
	   we can't handle it */
	log ("handle_connection(): could not read packet body from buffer");
	con->destroy = 1;
	return;
    }

#ifndef HAVE_DEV_RANDOM
    add_random_bytes (con->recvbuf->data + con->recvbuf->consumed, 4 + len);
#endif /* !HAVE_DEV_RANDOM */

    /* require that the client register before doing anything else */
    if (con->class == CLASS_UNKNOWN &&
	(tag != MSG_CLIENT_LOGIN && tag != MSG_CLIENT_LOGIN_REGISTER &&
	 tag != MSG_CLIENT_REGISTER && tag != MSG_SERVER_LOGIN &&
	 tag != MSG_SERVER_LOGIN_ACK && tag != MSG_SERVER_ERROR &&
	 tag != 4)) /* unknown: v2.0 beta 5a sends this? */
    {
	log ("handle_connection(): %s is not registered, closing connection",
	     con->host);
	log ("handle_connection(): tag=%hu, len=%hu, data=%s",
		tag, len, con->recvbuf->data + con->recvbuf->consumed + 4);
	con->destroy = 1;
	return;
    }

    /* if we received this message from a peer server, pass it
       along to the other servers behind us.  the ONLY messages we don't
       propogate are an ACK from a peer server that we've requested a link
       with, and an error message from a peer server */
    if (con->class == CLASS_SERVER && tag != MSG_SERVER_LOGIN_ACK &&
	tag != MSG_SERVER_ERROR && tag != MSG_SERVER_NOSUCH && Num_Servers)
	pass_message (con, con->recvbuf->data + con->recvbuf->consumed,
		      4 + len);

    dispatch_command (con, tag, len,
		      con->recvbuf->data + con->recvbuf->consumed + 4);

    /* mark that we read this data and it is ok to free it */
    con->recvbuf = buffer_consume (con->recvbuf, len + 4);
}

static void
lookup_hostname (void)
{
    struct hostent *he;

    /* get our canonical host name */
    gethostname (Buf, sizeof (Buf));
    he = gethostbyname (Buf);
    if (he)
    {
	Server_Name = STRDUP (he->h_name);
	Server_Ip = *(unsigned int *)he->h_addr_list[0];
    }
    else
    {
	log ("unable to find fqdn for %s", Buf);
	Server_Name = STRDUP (Buf);
    }
}

static void
update_stats (void)
{
    int i, l;
    time_t t;

    t = time (0);
    log ("update_stats(): current time is %s", ctime (&t));
    log
	("update_stats(): library is %d kilobytes (%d gigabytes), %d files, %d users",
	 Num_Gigs, Num_Gigs / (1024 * 1024), Num_Files, Users->dbsize);
    log ("update_stats: %d local clients, %d linked servers",
	 Num_Clients - Num_Servers, Num_Servers);

    /* since we send the same data to many people, optimize by forming
       the message once then writing it out */
    snprintf (Buf + 4, sizeof (Buf) - 4, "%d %d %d", Users->dbsize, Num_Files,
	      Num_Gigs / (1024 * 1024));
    set_tag (Buf, MSG_SERVER_STATS);
    l = strlen (Buf + 4);
    set_len (Buf, l);
    l += 4;

    for (i = 0; i < Num_Clients; i++)
    {
	if (Clients[i] && Clients[i]->class == CLASS_USER)
	    queue_data (Clients[i], Buf, l);
    }
}


static int
ip_glob_match (const char *pattern, const char *ip)
{
    int l;

    /* if `pattern' ends with a `.', we ban an entire subclass */
    l = strlen (pattern);
    ASSERT (l > 0);
    if (pattern[l - 1] == '.')
	return ((strncmp (pattern, ip, l) == 0));
    else
	return ((strcmp (pattern, ip) == 0));
}

static int
check_accept (CONNECTION * cli)
{
    int i;

    /* check for max connections */
    if (Num_Clients >= Max_Connections)
    {
	log
	    ("check_accept: maximum number of connections (%d) has been reached",
	     Max_Connections);
	send_cmd (cli, MSG_SERVER_ERROR,
		  "this server is full (%d local connections)", Num_Clients);
	return 0;
    }

    /* make sure this ip is not banned */
    for (i = 0; i < Ban_Size; i++)
    {
	if (Ban[i]->type == BAN_IP &&
	    ip_glob_match (Ban[i]->target, cli->host))
	{
	    /* TODO: this does not reach all mods, only the one on
	       this server */
	    log ("check_accept: connection attempt from banned ip %s (%s)",
		 cli->host, NONULL (Ban[i]->reason));
	    notify_mods ("Connection attempt from banned ip %s", cli->host);
	    send_cmd (cli, MSG_SERVER_ERROR,
		      "You are banned from this server (%s)",
		      NONULL (Ban[i]->reason));
	    return 0;
	}
    }

    return 1;
}

static void
accept_connection (int s)
{
    CONNECTION *cli;
    socklen_t sinsize;
    struct sockaddr_in sin;
    int f;

    sinsize = sizeof (sin);
    if ((f = accept (s, (struct sockaddr *) &sin, &sinsize)) < 0)
    {
	perror ("accept_connection(): accept");
	return;
    }

    cli = new_connection ();
    cli->fd = f;
    log ("accept_connection(): connection from %s, port %d",
	    inet_ntoa (sin.sin_addr), ntohs (sin.sin_port));
    /* if we have a local connection, use the external
       interface so others can download from them */
    if (sin.sin_addr.s_addr == inet_addr ("127.0.0.1"))
    {
	log ("accept_connection(): connected via loopback, using external ip");
	cli->ip = Server_Ip;
	cli->host = STRDUP (Server_Name);
    }
    else
    {
	cli->ip = sin.sin_addr.s_addr;
	cli->host = STRDUP (inet_ntoa (sin.sin_addr));
    }
    cli->port = ntohs (sin.sin_port);
    cli->class = CLASS_UNKNOWN;
    add_client (cli);

    set_nonblocking (f);
    set_keepalive (f, 1);	/* enable tcp keepalive messages */

    if (!check_accept (cli))
	cli->destroy = 1;
}

#ifndef WIN32
static void
init_signals (void)
{
    struct sigaction sa;

    memset (&sa, 0, sizeof (sa));
    sa.sa_handler = sighandler;
    sigaction (SIGHUP, &sa, NULL);
    sigaction (SIGTERM, &sa, NULL);
    sigaction (SIGINT, &sa, NULL);
}
#endif /* !WIN32 */

static void
usage (void)
{
    fprintf (stderr,
	     "usage: %s [ -hsv ] [ -c FILE ] [ -p PORT ] [ -l IP ]\n", PACKAGE);
    fprintf (stderr, "  -c FILE	read config from FILE (default: %s/config\n",
	     SHAREDIR);
    fputs ("  -h		print this help message\n", stderr);
    fputs ("  -l IP	listen only on IP instead of all interfaces", stderr);
    fputs ("  -p PORT	listen on PORT for connections (default: 8888)\n",
	   stderr);
    fputs
	("  -s		channels may only be created by privileged users\n",
	 stderr);
    fputs ("  -v		display version information\n", stderr);
    exit (0);
}

static void
version (void)
{
    fprintf (stderr, "%s %s\n", PACKAGE, VERSION);
    fprintf (stderr, "Copyright (C) 2000 drscholl@users.sourceforge.net\n");
    exit (0);
}

/* wrappers to make the code in main() much cleaner */
#if HAVE_POLL
#define READABLE(i) (ufd[i].revents & POLLIN)
#define WRITABLE(i) (ufd[i].revents & POLLOUT)
#define CHECKREAD(i) ufd[i].events |= POLLIN
#define CHECKWRITE(i) ufd[i].events |= POLLOUT
#else
#define READABLE(i) FD_ISSET(Clients[i]->fd, &set)
#define WRITABLE(i) FD_ISSET(Clients[i]->fd, &wset)
#define CHECKREAD(i) FD_SET(Clients[i]->fd, &set)
#define CHECKWRITE(i) FD_SET(Clients[i]->fd, &wset)
#endif /* HAVE_POLL */

int
main (int argc, char **argv)
{
    int s;			/* server socket */
    int i;			/* generic counter */
    int n;			/* number of ready sockets */
    int f, pending = 0, port = 0, iface = INADDR_ANY;
#if HAVE_POLL
    struct pollfd *ufd = 0;
    int ufdsize = 0;		/* real number of pollfd structs allocated */
#else
    fd_set set, wset;
    struct timeval t;
    int maxfd;
#endif /* HAVE_POLL */
    char *config_file = 0;
    time_t next_update = 0;
#ifdef WIN32
    WSADATA wsa;
#endif /* WIN32 */

#ifndef WIN32
    while ((n = getopt (argc, argv, "c:hl:p:v")) != EOF)
    {
	switch (n)
	{
	    case 'c':
		config_file = optarg;
		break;
	    case 'l':
		iface = inet_addr (optarg);
		break;
	    case 'p':
		port = atoi (optarg);
		break;
	    case 's':
		Server_Flags |= OPTION_STRICT_CHANNELS;
		break;
	    case 'v':
		version ();
		break;
	    default:
		usage ();
	}
    }
#endif /* !WIN32 */

    log ("version %s starting", VERSION);

#ifndef WIN32
    init_signals ();
#else
    WSAStartup (MAKEWORD (1, 1), &wsa);
#endif /* !WIN32 */

    /* load default configuration values */
    config_defaults ();
    lookup_hostname ();

    /* load the config file */
    config (config_file ? config_file : SHAREDIR "/config");

    Interface = inet_addr (Listen_Addr);
    /* if the interface was specified on the command line, override the
       value from the config file */
    if (iface != INADDR_ANY)
	Interface = iface;

    /* if a port was specified on the command line, override the value
       specified in the config file */
    if (port != 0)
	Server_Port = port;

    log ("my hostname is %s", Server_Name);

    /* initialize the connection to the SQL database server */
    if (init_db () != 0)
	exit (1);

    /* initialize hash tables.  the size of the hash table roughly cuts
       the max number of matches required to find any given entry by the same
       factor.  so a 256 entry hash table with 1024 entries will take rougly
       4 comparisons max to find any one entry.  we use prime numbers here
       because that gives the table a little better spread */
    Users = hash_init (257, (hash_destroy) free_user);
    Channels = hash_init (257, (hash_destroy) free_channel);
    Hotlist = hash_init (257, (hash_destroy) free_hotlist);
    File_Table = hash_init (2053, (hash_destroy) free_flist);
    MD5 = hash_init (2053, (hash_destroy) free_flist);

    /* create the incoming connections socket */
    s = new_tcp_socket ();
    if (s < 0)
	exit (1);

    n = 1;
    if (setsockopt (s, SOL_SOCKET, SO_REUSEADDR, SOCKOPTCAST &n, sizeof (n)) != 0)
    {
	perror ("setsockopt");
	exit (1);
    }

    if (bind_interface (s, Interface, Server_Port) == -1)
	exit (1);

    if (listen (s, BACKLOG) < 0)
    {
	perror ("listen");
	exit (1);
    }

    log ("listening on %s port %d", my_ntoa (Interface), Server_Port);

#ifndef HAVE_DEV_RANDOM
    init_random ();
#endif /* !HAVE_DEV_RANDOM */

    /* main event loop */
    while (!SigCaught)
    {
#if HAVE_POLL
	/* ensure that we have enough pollfd structs.  add one extra for
	   the incoming connections port */
	if (Num_Clients + 1 > ufdsize)
	{
	    ufd = REALLOC (ufd, sizeof (struct pollfd) * (Num_Clients + 1));
	    ufdsize = Num_Clients + 1;
	}
#else
	FD_ZERO (&set);
	FD_ZERO (&wset);
	maxfd = s;
	FD_SET (s, &set);
#endif /* HAVE_POLL */

	for (n = 0, i = 0; i < Num_Clients; i++)
	{
	    /* several of the message handlers might cause connections to
	       disappear during the course of this loop, so we must check to
	       make sure this connection is still valid.  if its missing, we
	       shift down the array to fill the holes */
	    if (Clients[i])
	    {
		/* if there are holes, we shift down the upper structs
		   to fill them */
		if (i != n)
		{
		    Clients[n] = Clients[i];
		    Clients[n]->id = n;
		}
#if HAVE_POLL
		ufd[n].fd = Clients[n]->fd;
		ufd[n].events = 0;
#endif /* HAVE_POLL */

		n++;

		/* check sockets for writing */
		if ((Clients[i]->flags & FLAG_CONNECTING) ||
		    (Clients[i]->sendbuf ||
		     (Clients[i]->zip && Clients[i]->zip->outbuf)))
		    CHECKWRITE(i);

		/* always check for incoming data */
		CHECKREAD(i);

#ifndef HAVE_POLL
		if (Clients[i]->fd > maxfd)
		    maxfd = Clients[i]->fd;
#endif /* !HAVE_POLL */

		/* note if their is unprocessed data in the input
		   buffers so we dont block on select().  the incomplete
		   flag is checked here to avoid busy waiting when we really
		   do need more data from the client connection */
		if ((Clients[i]->incomplete == 0 && Clients[i]->recvbuf) ||
		    (Clients[i]->zip && Clients[i]->zip->inbuf))
		    pending++;
	    }
	}

	Num_Clients = n;	/* actual number of clients */

	/* if there is pending data in client queues, don't block on the
	   select call */
#if HAVE_POLL
	/* add an entry for the incoming connections socket */
	ufd[Num_Clients].fd = s;
	ufd[Num_Clients].events = POLLIN;

	if ((n = poll (ufd, Num_Clients + 1, pending ? 0 : Stat_Click * 1000)) < 0)
	{
	    perror ("poll");
	    continue;
	}
#else
	t.tv_sec = pending ? 0 : Stat_Click;
	t.tv_usec = 0;
	if ((n = select (maxfd + 1, &set, &wset, NULL, &t)) < 0)
	{
	    perror ("select");
	    continue;
	}
#endif /* HAVE_POLL */

	pending = 0;		/* reset */

	/* check for new incoming connections */
#if HAVE_POLL
	if (ufd[Num_Clients].revents & POLLIN)
#else
	if (FD_ISSET (s, &set))
#endif
	{
	    accept_connection (s);
	    n--;
	}

	/* read incoming data into buffers, but don't process it */
	for (i = 0; !SigCaught && n > 0 && i < Num_Clients; i++)
	{
	    if ((Clients[i]->flags & FLAG_CONNECTING) && WRITABLE (i))
	    {
		complete_connect (Clients[i]);
		n--;	/* keep track of how many we've handled */
	    }
	    else if (READABLE (i))
	    {
		n--;	/* keep track of how many we've handled */
		f = buffer_read (Clients[i]->fd,
		    (Clients[i]->zip !=
		     0) ? &Clients[i]->zip->
		    inbuf : &Clients[i]->recvbuf);
		if (f <= 0)
		{
		    if (f == 0)
			log ("main(): EOF from %s", Clients[i]->host);
		    Clients[i]->destroy = 1;
		}
	    }
#if HAVE_POLL
	    /* when does this occur?  i would have thought POLLHUP
	       would be when the connection hangs up, but it still
	       sets POLLIN and read() returns 0 */
	    else if ((ufd[i].revents & (POLLHUP | POLLERR)) != 0)
	    {
		ASSERT ((ufd[i].revents & POLLHUP) == 0);
		ASSERT ((ufd[i].revents & POLLERR) == 0);
		Clients[i]->destroy = 1;
	    }
#endif /* HAVE_POLL */
	}

	if (SigCaught)
	    break;

	/* handle client requests */
	for (i = 0; !SigCaught && i < Num_Clients; i++)
	{
	    /* if the connection is going to be shut down, don't process it */
	    if (!Clients[i]->destroy)
	    {
		/* if there is input pending, handle it now */
		if (Clients[i]->recvbuf ||
		    (Clients[i]->zip && Clients[i]->zip->inbuf))
		    handle_connection (Clients[i]);
	    }
	}

	if (SigCaught)
	    break;

	/* we should send the clients updated server stats every so often */
	if (next_update < time (0))
	{
	    update_stats ();
	    next_update = time (0) + Stat_Click;
	}

	/* write out data and reap dead client connections */
	for (i = 0; !SigCaught && i < Num_Clients; i++)
	{
	    if (Clients[i]->zip)
	    {
		/* server - strategy is call send_queued_data() if there
		   there is data to be compressed, or if the socket is
		   writable and there is some compressed output */
		if (Clients[i]->sendbuf
		    || (Clients[i]->zip->outbuf && WRITABLE (i)))
		{
		    if (send_queued_data (Clients[i]) == -1)
			Clients[i]->destroy = 1;
		}
	    }
	    /* client.  if there is output to send and either the socket is
	       writable or we're about to close the connection, send any
	       queued data */
	    else if (Clients[i]->sendbuf &&
		(WRITABLE (i) || Clients[i]->destroy))
	    {
		if (send_queued_data (Clients[i]) == -1)
		    Clients[i]->destroy = 1;
	    }

	    /* check to see if this connection should be shut down */
	    if (Clients[i]->destroy)
	    {
		/* should have flushed everything */
		ASSERT (Clients[i]->sendbuf == 0);
		log ("main(): closing connection for %s", Clients[i]->host);
		remove_connection (Clients[i]);
	    }
	}
    }

    if (SigCaught)
	log ("caught signal");

    log ("shutting down");

    /* disallow incoming connections */
    CLOSE (s);

    /* close all client connections */
    for (i = 0; i < Num_Clients; i++)
    {
	if (Clients[i])
	    remove_connection (Clients[i]);
    }

    close_db ();

    /* clean up */
#if HAVE_POLL
    if (ufd)
	FREE (ufd);
#endif /* HAVE_POLL */

    if (Clients)
	FREE (Clients);

    if (Servers)
	FREE (Servers);

    //free_hash (File_Table);
    free_hash (MD5);
    free_hash (Users);
    free_hash (Channels);
    free_hash (Hotlist);

    for (i = 0; i < Ban_Size; i++)
	free_ban (Ban[i]);
    if (Ban)
	FREE (Ban);

    /* free up memory associated with global configuration variables */
    free_config ();

    /* this displays a list of leaked memory.  pay attention to this. */
    CLEANUP ();

#ifdef WIN32
    WSACleanup ();
#endif

    exit (0);
}
