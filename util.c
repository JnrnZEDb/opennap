/* Copyright (C) 2000 drscholl@sourceforge.net
   This is free software distributed under the terms of the
   GNU Public License.  See the file COPYING for details. */

/* this file contains various utility functions useful elsewhere in this
   server */

#include <fcntl.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
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

/* read() can return less than the amount requested if the data was sent in
   different packets.  however, we know exactly how much to read so we want
   to read it all at once.  */
size_t
read_bytes (int fd, char *d, size_t bytes)
{
    size_t l, n = 0;

    while (n < bytes)
    {
	l = read (fd, d + n, bytes - n);
	if (l == (size_t) - 1)
	    return -1;
	if (l == 0)
	    break;
	n += l;
    }
    return n;
}

/* writes `val' as a two-byte value in little-endian format */
void
set_val (char *d, unsigned short val)
{
#if __BYTE_ORDER == __BIG_ENDIAN
    val = bswap_16 (val);
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
    ASSERT(VALID(chan));
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
    ASSERT (VALID (con));
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

void
queue_data (CONNECTION *con, char *s, int ssize)
{
    ASSERT (VALID (con));
    if (con->sendbuflen + ssize > con->sendbufmax)
    {
	con->sendbufmax = con->sendbuflen + ssize;
	con->sendbuf = REALLOC (con->sendbuf, con->sendbufmax);
    }
    memcpy (con->sendbuf + con->sendbuflen, s, ssize);
    con->sendbuflen += ssize;
}

void
send_queued_data (CONNECTION *con)
{
    int l;

    /* write as much of the queued data as we can */
    l = write (con->fd, con->sendbuf, con->sendbuflen);
    if (l == -1)
    {
	log ("flush_queued_data(): write: %s", strerror (errno));
	remove_connection (con);
	return;
    }
    con->sendbuflen -= l;
    /* shift any data that was left down to the begin of the buf */
    if (con->sendbuflen)
	memmove (con->sendbuf, con->sendbuf + l, con->sendbuflen);

    /* if there is more than 2kbytes left to send, close the connection
       since the peer is likely dead */
    if (con->sendbuflen > 2048)
    {
	log ("send_queued_data(): closing link for %s (sendq exceeded)",
		con->host);
	remove_connection (con);
    }
}

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

char *
generate_nonce (void)
{
    int f;
    char *nonce;

    /* generate our own nonce value */
    f = open ("/dev/random", O_RDONLY);
    if (f < 0)
    {
	log ("generate_nonce(): /dev/random: %s", strerror (errno));
	return NULL;
    }

    nonce = MALLOC (17);
    nonce[16] = 0;

    if (read (f, nonce, 8) != 8)
    {
	log ("generate_nonce(): could not read enough random bytes");
	close (f);
	FREE (nonce);
	return NULL;
    }

    close (f);

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

    ASSERT (VALID (list));
    ASSERT (VALID (ptr));
    list = REALLOC (list, sizeof (char *) * (*listsize + 1));
    plist = (char **) list;
    plist[*listsize] = ptr;
    ++*listsize;
    return list;
}

/* removes `ptr' from the array `list'.  note that this does not reclaim
   the space left over, it just shifts down the remaining entries */
int
array_remove (void *list, int *listsize, void *ptr)
{
    int i;
    char **plist = (char **) list;

    ASSERT (VALID (list));
    ASSERT (VALID (ptr));
    for (i=0;i<*listsize;i++)
    {
	if (ptr == plist[i])
	{
	    if (i < *listsize - 1)
		memmove (&plist[i], &plist[i + 1], sizeof (char *) * (*listsize - i - 1));
	    --*listsize;
	    return 0;
	}
    }
    return -1; /* not found */
}
