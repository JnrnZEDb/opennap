/* Copyright (C) 2000 drscholl@users.sourceforge.net
   This is free software distributed under the terms of the
   GNU Public License.  See the file COPYING for details.

   $Id$

   This file contains various utility functions useful elsewhere in this
   server */

#include <fcntl.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/time.h>
#include "global.h"
#include "md5.h"
#include "opennap.h"
#include "debug.h"

/* no such user */
void
nosuchuser (CONNECTION * con, char *nick)
{
    ASSERT (VALID (con));
    send_cmd (con, MSG_SERVER_NOSUCH, "User %s is not currently online.", nick);
}

void
permission_denied (CONNECTION *con)
{
    send_cmd (con, MSG_SERVER_NOSUCH, "permission denied");
}

/* writes `val' as a two-byte value in little-endian format */
void
set_val (char *d, unsigned short val)
{
#if WORDS_BIGENDIAN
    val = BSWAP16 (val);
#endif
    memcpy (d, &val, 2);
}

void
send_cmd (CONNECTION *con, unsigned long msgtype, const char *fmt, ...)
{
    va_list ap;
    size_t l;

    va_start (ap, fmt);
    vsnprintf (Buf + 4, sizeof (Buf) - 4, fmt, ap);
    va_end (ap);

    set_tag (Buf, msgtype);
    l = strlen (Buf + 4);
    set_len (Buf, l);
    queue_data (con, Buf, 4 + l);
}

/* adds a pointer to `c' to the list of servers for quick access */
void
add_server (CONNECTION *c)
{
    Servers = REALLOC (Servers, sizeof (CONNECTION *) * (Num_Servers + 1));
    Servers[Num_Servers] = c;
    Num_Servers++;
}

void
add_client (CONNECTION *con)
{
    Clients = REALLOC (Clients, sizeof (CONNECTION *) * (Num_Clients + 1));
    Clients[Num_Clients] = con;
    con->id = Num_Clients;
    Num_Clients++;
}

/* send a message to all peer servers.  `con' is the connection the message
   was received from and is used to avoid sending the message back from where
   it originated. */
void
pass_message (CONNECTION *con, char *pkt, size_t pktlen)
{
    int i;

    ASSERT (validate_connection (con));
    for (i = 0; i < Num_Servers; i++)
	if (Servers[i] != con)
	    queue_data (Servers[i], pkt, pktlen);
}

/* wrapper for pass_message() */
void
pass_message_args (CONNECTION *con, unsigned long msgtype, const char *fmt, ...)
{
    va_list ap;
    size_t l;

    va_start (ap, fmt);
    vsnprintf (Buf + 4, sizeof (Buf) - 4, fmt, ap);
    va_end (ap);
    set_tag (Buf, msgtype);
    l = strlen (Buf + 4);
    set_len (Buf, l);
    pass_message (con, Buf, 4 + l);
}

/* destroys memory associated with the CHANNEL struct.  this is usually
   not called directly, but in association with the hash_remove() and
   hash_destroy() calls */
void
free_channel (CHANNEL * chan)
{
    ASSERT(validate_channel (chan));
    ASSERT (chan->numusers == 0);
    FREE (chan->name);
    if (chan->topic)
	FREE (chan->topic);
    if (chan->users)
	FREE (chan->users);
    FREE (chan);
}

/* this is like strtok(2), except that all fields are returned as once.  nul
   bytes are written into `pkt' and `template' is updated with pointers to
   each field in `pkt' */
/* returns: number of fields found. */
int
split_line (char **template, int templatecount, char *pkt)
{
    int i = 0;

    while (pkt && i < templatecount)
    {
	if (*pkt == '"')
	{
	    /* quoted string */
	    pkt++;
	    template[i++] = pkt;
	    pkt = strchr (pkt, '"');
	    if (!pkt)
	    {
		/* bogus line */
		return -1;
	    }
	    *pkt++ = 0;
	    if (!*pkt)
		break;
	    pkt++;		/* skip the space */
	}
	else
	{
	    template[i++] = pkt;
	    pkt = strchr (pkt, ' ');
	    if (!pkt)
		break;
	    *pkt++ = 0;
	}

    }
    return i;
}

int
pop_user (CONNECTION *con, char **pkt, USER **user)
{
    ASSERT (validate_connection (con));
    if (con->class == CLASS_SERVER)
    {
	char *ptr;

	if (**pkt != ':')
	{
	    log ("pop_user(): server message did not contain nick");
	    return -1;
	}
	ptr = *pkt + 1;
	*pkt = strchr (ptr, ' ');
	if (!*pkt)
	{
	    log ("pop_user(): too few fields in server message");
	    return -1;
	}
	*(*pkt)++ = 0;
	*user = hash_lookup (Users, ptr);
	if (!*user)
	{
	    log ("pop_user(): could not find user %s", ptr);
	    return -1;
	}

	/* this should not return a user who is local to us.  if so, it
	   means that some other server has passed us back a message we
	   sent to them */
	if ((*user)->con)
	{
	    log ("pop_user(): fatal error, received server message for local user!");
	    return -1;
	}
    }
    else
    {
	ASSERT (con->class == CLASS_USER);
	ASSERT (con->user != 0);
	*user = con->user;
    }
    return 0;

}

/* how much uncompressed data we can fit in one packet */
#if 1
#define MAX_UNCOMPRESSED_SIZE	131070
#else
/* small value for testing purposes only!  this will force send_queued_data()
   to fragment the packet */
#define MAX_UNCOMPRESSED_SIZE	2000
#endif

#if 0
static int
calculate_chunk_length (CONNECTION *con)
{
    int offset;
    unsigned short len;

    ASSERT (validate_connection (con));

    /* we start with the end of the already compressed data */
    offset = con->sendbufcompressed;
    while (offset + 4 <= con->sendbuflen)
    {
	/* read the packet length */
	memcpy (&len, con->sendbuf + offset, 2);
	len = BSWAP16 (len);

	/* text compresses at around 50%, so make sure we dont try to
	   send more than 131070 bytes in one packet */
	if (offset - con->sendbufcompressed + len + 4 > MAX_UNCOMPRESSED_SIZE)
	    break;

	offset += len + 4; /* skip over the packet header and body */
    }

    return (offset - con->sendbufcompressed); /* number of bytes to compress */
}

void
send_queued_data (CONNECTION *con)
{
    int l, qlen;
    struct timeval t;
    fd_set wfd;

    ASSERT (validate_connection (con));
    
    if (con->sendbuflen == 0)
	return; /* nothing to do */

#if HAVE_LIBZ
    /* see if there is any data we can compress
       only compress if we have enough data to justify it */
    if (con->class == CLASS_SERVER && Compression_Level > 0)
    {
	long datasize;
	unsigned char *data = 0;
	int compressed = 0; /* how many compressed packets so far */

	/* we have to make sure that the size of the compressed packet is
	   less than 65535 bytes, which is what fits in a 16-bit integer.
	   so we loop here to compress the data.  we have to semi-parse
	   the packets so that we make sure we compress on the boundaries,
	   because the underlying packets can't be split between
	   compressed packets.

	   since we could have a lot of data to send on a server join,
	   we keep track of how many compressed packets so far, and if
	   we reach the threshold `Max_Compress' we stop to avoid blocking
	   the rest of the server.  Administrators will have to tune this
	   value so that it compresses data fast enough to keep up with
	   what we can write, without blocking client connections */
	while (con->sendbuflen - con->sendbufcompressed >= Compression_Threshold &&
	    compressed < Max_Compress)
	{
	    datasize = calculate_chunk_length (con);
	    data = REALLOC (data, datasize);
	    l = datasize;
	    log ("send_queued_data(): attempting to compress %d bytes", l);
	    if (compress2 (data, (unsigned long *) &datasize,
			(unsigned char *) con->sendbuf + con->sendbufcompressed,
			l, Compression_Level) == Z_OK)
	    {
		/* save some information about the current state of the
		   buffer */

		/* end of uncompressed region */
		int u_end = con->sendbufcompressed + l;
		/* end of compressed region */
		int c_end = con->sendbufcompressed + datasize + 8;
		/* change in size */
		int delta = u_end - c_end;

		unsigned int len;

		if (datasize > 65535)
		{
		    log ("send_queued_data(): compressed data is too large (%lu bytes)",
			    datasize);
		    break;
		}

		/* make sure there is enough room to hold the compressed
		   packet */
		if (con->sendbufcompressed + datasize + 8 > con->sendbufmax)
		{
		    log ("send_queued_data(): compressed packet is larger, not compressing");
		    break;
		}

		/* compressed packet header */
		set_val (con->sendbuf + con->sendbufcompressed, datasize + 4);
		con->sendbufcompressed += 2;
		set_val (con->sendbuf + con->sendbufcompressed,
			MSG_SERVER_COMPRESSED_DATA);
		con->sendbufcompressed += 2;

		/* uncompressed size, 4 bytes */
		ASSERT (sizeof (unsigned int) == 4);
		len = BSWAP32 (l);
		memcpy (con->sendbuf + con->sendbufcompressed, &len, 4);
		con->sendbufcompressed += 4;

		/* compressed data */
		memcpy (con->sendbuf + con->sendbufcompressed, data, datasize);
		con->sendbufcompressed += datasize;

		ASSERT (con->sendbufcompressed == c_end);

		log ("send_queued_data(): compressed %d bytes to %d (%d%%).",
			l, datasize + 8, (100 * delta) / l);

		/* if there were leftovers, we need to shift them down
		   to the end of the compressed packet we just wrote */
		if (u_end < con->sendbuflen)
		{
		    memmove (con->sendbuf + c_end,
			    con->sendbuf + u_end,
			    con->sendbuflen - u_end);
		}

		con->sendbuflen -= delta;

		compressed++; /* keep track of what we've done so we
				    don't block */
		if (compressed == Max_Compress)
		{
		    log ("send_queued_data(): max_compress reached (%d) with %d bytes left",
			Max_Compress, con->sendbuflen - con->sendbufcompressed);
		}
	    }
	    else
	    {
		/* this should not happen under normal circumstances */
		log ("send_queued_data(): error compressing data");
		break;
	    }
	}
	if (data)
	    FREE (data);
    }
#endif /* HAVE_LIBZ */

    /* see if we are ready for writing, with no wait */
    t.tv_sec = 0;
    t.tv_usec = 0;
    FD_ZERO (&wfd);
    FD_SET (con->fd, &wfd);
    l = select (con->fd + 1, 0, &wfd, 0, &t);
    if (l == -1)
    {
	log ("send_queued_data: select: %s (errno %d).", strerror (errno),
	    errno);
	con->sendbuflen = 0;
	remove_connection (con);
	return;
    }
    else if (l == 1)
    {
	/* write as much of the queued data as we can */
	l = write (con->fd, con->sendbuf, con->sendbuflen);
	if (l == -1)
	{
	    log ("send_queued_data: write: %s (errno %d).", strerror (errno), errno);
	    con->sendbuflen = 0; /* avoid an infinite loop */
	    remove_connection (con);
	    return;
	}
	con->sendbuflen -= l;
#if HAVE_LIBZ
	if (con->sendbufcompressed >= l)
	    con->sendbufcompressed -= l;
	else
	    con->sendbufcompressed = 0;
#endif /* HAVE_LIBZ */

	/* shift any data that was left down to the begin of the buf */
	/* TODO: this should probably be implemented as a circular buffer to
	   avoid having to move the memory after every write call */
	if (con->sendbuflen)
	{
	    log ("send_queued_data: %d bytes remain in the queue for %s",
		    con->sendbuflen, con->host);
	    memmove (con->sendbuf, con->sendbuf + l, con->sendbuflen);
	}
    }

    /* if there is more than 2kbytes left to send, close the connection
       since the peer is likely dead */
    qlen = (con->class == CLASS_USER) ? Client_Queue_Length : Server_Queue_Length;
    if (con->sendbuflen > qlen)
    {
	log ("send_queued_data(): closing link for %s (sendq exceeded %d bytes)",
	    con->host, qlen);
	con->sendbuflen = 0; /* avoid an infinite loop */
	remove_connection (con);
    }
}
#endif /* 0 */

static char hex[] = "0123456789ABCDEF";

void
expand_hex (char *v, int vsize)
{
    int i;

    for (i = vsize - 1; i >= 0; i--)
    {
	v[2 * i + 1] = hex [v[i] & 0xf];
	v[2 * i] = hex [(v[i] >> 4) & 0xf];
    }
}

#ifndef HAVE_DEV_RANDOM
static int Stale_Random = 1;
static MD5_CTX Random_Context;

void
init_random (void)
{
    MD5Init (&Random_Context);
    Stale_Random = 1;
}

void
add_random_bytes (char *s, int ssize)
{
    MD5Update (&Random_Context, (unsigned char *) s, (unsigned int) ssize);
    Stale_Random = 0;
}

static void
get_random_bytes (char *d, int dsize)
{
    char buf[16];

    ASSERT (Stale_Random == 0);
    ASSERT (dsize <= 16);
    MD5Final ((unsigned char *) buf, &Random_Context);
    memcpy (d, buf, dsize);
    Stale_Random = 1;
    MD5Init (&Random_Context);
    MD5Update (&Random_Context, (unsigned char *) buf, 16);
}
#endif /* ! HAVE_DEV_RANDOM */

char *
generate_nonce (void)
{
#if HAVE_DEV_RANDOM
    int f;
#endif /* HAVE_DEV_RANDOM */
    char *nonce;

    nonce = MALLOC (17);
    nonce[16] = 0;

#if HAVE_DEV_RANDOM
    /* generate our own nonce value */
    f = open ("/dev/random", O_RDONLY);
    if (f < 0)
    {
	log ("generate_nonce(): /dev/random: %s", strerror (errno));
	return NULL;
    }

    if (read (f, nonce, 8) != 8)
    {
	log ("generate_nonce(): could not read enough random bytes");
	close (f);
	FREE (nonce);
	return NULL;
    }

    close (f);
#else
    get_random_bytes (nonce, 8);
#endif

    /* expand the binary data into hex for transport */
    expand_hex (nonce, 8);

    return nonce;
}

/* array magic.  this assumes that all pointers are of the same size as
   `char*' */
/* appends `ptr' to the array `list' */
void *
array_add (void *list, int *listsize, void *ptr)
{
    char **plist;

#ifdef DEBUG
    if (list)
	ASSERT (VALID (list));
#endif /* DEBUG */
    ASSERT (VALID (ptr));
    list = REALLOC (list, sizeof (char *) * (*listsize + 1));
    plist = (char **) list;
    plist[*listsize] = ptr;
    ++*listsize;
    return list;
}

/* removes `ptr' from the array `list'.  note that this does not reclaim
   the space left over, it just shifts down the remaining entries */
void *
array_remove (void *list, int *listsize, void *ptr)
{
    int i;
    char **plist = (char **) list;

    ASSERT (VALID_LEN (list, *listsize * sizeof (char *)));
    ASSERT (VALID (ptr));
    for (i=0;i<*listsize;i++)
    {
	if (ptr == plist[i])
	{
	    if (i < *listsize - 1)
		memmove (&plist[i], &plist[i + 1], sizeof (char *) * (*listsize - i - 1));
	    --*listsize;
	    list = REALLOC (list, *listsize * sizeof (char *));
	    break;
	}
    }
    return list;
}

void
fudge_path (const char *in, char *out, int outsize)
{
    while (*in && outsize > 1)
    {
	if (*in == '\\' || *in=='\'')
	{
	    *out++ = '\\';
	    outsize--;
	}
	*out++ = *in++;
	outsize--;
    }
    *out = 0;
}

#ifdef DEBUG
int
validate_connection (CONNECTION *con)
{
    ASSERT_RETURN_IF_FAIL (VALID_LEN (con, sizeof (CONNECTION)), 0);
    ASSERT_RETURN_IF_FAIL (con->magic == MAGIC_CONNECTION, 0);
    ASSERT_RETURN_IF_FAIL ((con->class == CLASS_USER) ^ (con->user == 0), 0);
    ASSERT_RETURN_IF_FAIL (VALID (con->host), 0);
    if (con->sendbuf)
	ASSERT_RETURN_IF_FAIL (buffer_validate (con->sendbuf), 0);
    if (con->recvbuf)
	ASSERT_RETURN_IF_FAIL (buffer_validate (con->recvbuf), 0);
    ASSERT_RETURN_IF_FAIL (con->hotlistsize == 0 || VALID_LEN (con->hotlist, sizeof (HOTLIST *) * con->hotlistsize), 0);
    return 1;
}

int
validate_user (USER *user)
{
    ASSERT_RETURN_IF_FAIL (VALID_LEN (user, sizeof (USER)), 0);
    ASSERT_RETURN_IF_FAIL (user->magic == MAGIC_USER, 0);
    ASSERT_RETURN_IF_FAIL (VALID (user->nick), 0);
    ASSERT_RETURN_IF_FAIL (VALID (user->clientinfo), 0);
    ASSERT_RETURN_IF_FAIL (user->email == 0 || VALID (user->email), 0);
    ASSERT_RETURN_IF_FAIL ((user->channels != 0) ^ (user->numchannels == 0), 0);
    ASSERT_RETURN_IF_FAIL (user->numchannels == 0 || VALID_LEN (user->channels, sizeof (USER *) * user->numchannels), 0);
    return 1;
}

int
validate_channel (CHANNEL *chan)
{
    ASSERT_RETURN_IF_FAIL (VALID_LEN (chan, sizeof (CHANNEL)), 0);
    ASSERT_RETURN_IF_FAIL (chan->magic == MAGIC_CHANNEL, 0)
    ASSERT_RETURN_IF_FAIL (VALID (chan->name), 0);
    ASSERT_RETURN_IF_FAIL ((chan->users != 0) ^ (chan->numusers == 0), 0);
    ASSERT_RETURN_IF_FAIL (chan->numusers == 0 || VALID_LEN (chan->users, sizeof (USER *) * chan->numusers), 0);
    return 1;
}

int
validate_hotlist (HOTLIST *h)
{
    ASSERT_RETURN_IF_FAIL (VALID_LEN (h, sizeof (HOTLIST)), 0);
    ASSERT_RETURN_IF_FAIL (h->magic == MAGIC_HOTLIST, 0);
    ASSERT_RETURN_IF_FAIL (VALID (h->nick), 0);
    ASSERT_RETURN_IF_FAIL ((h->users != 0) ^ (h->numusers == 0), 0);
    ASSERT_RETURN_IF_FAIL (h->numusers == 0 || VALID_LEN (h->users, sizeof (CONNECTION *) * h->numusers), 0);
    return 1;
}
#endif

USER *
new_user (void)
{
    USER *u = CALLOC (1, sizeof (USER));

#ifdef DEBUG
    u->magic = MAGIC_USER;
#endif
    return u;
}

CHANNEL *
new_channel (void)
{
    CHANNEL *c = CALLOC (1, sizeof (CHANNEL));

#ifdef DEBUG
    c->magic = MAGIC_CHANNEL;
#endif
    return c;
}

HOTLIST *
new_hotlist (void)
{
    HOTLIST *h = CALLOC (1, sizeof (HOTLIST));

#ifdef DEBUG
    h->magic = MAGIC_HOTLIST;
#endif
    return h;
}

CONNECTION *
new_connection (void)
{
    CONNECTION *c = CALLOC (1, sizeof (CONNECTION));

#ifdef DEBUG
    c->magic = MAGIC_CONNECTION;
#endif
    return c;
}

void
log (const char *fmt, ...)
{
    va_list ap;

    printf ("%s: ", PACKAGE);
    va_start (ap, fmt);
    vprintf (fmt, ap);
    va_end (ap);
    fputc ('\n', stdout);
}
