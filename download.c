/* Copyright (C) 2000 drscholl@users.sourceforge.net
   This is free software distributed under the terms of the
   GNU Public License.  See the file COPYING for details. */

#include <unistd.h>
#include <stdio.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "opennap.h"
#include "debug.h"

/* 203 <nick> <filename> */
/* 500 <nick> <filename> */
/* handle client request for download of a file */
HANDLER (download)
{
    char *fields[2];
    USER *user;
    short msg;

    ASSERT (VALID (con));

    CHECK_USER_CLASS ("download");

    if (split_line (fields, sizeof (fields) / sizeof (char *), pkt) != 2)
    {
	log ("download(): malformed user request");
	return;
    }
    user = hash_lookup (Users, fields[0]);
    if (!user)
    {
	nosuchuser (con, fields[0]);
	return;
    }
    ASSERT (VALID (user));

    /* peek at the message type since we use this for both 203 and 500 */
    memcpy (&msg, con->recvhdr + 2, 2);
#if WORDS_BIGENDIAN
    msg = BSWAP16 (msg);
#endif

    /* make sure both parties are not firewalled
       -and-
       client is not making a 203 request to a firewalled user (this isn't
       really necessary it seems, but to maintain compatibility with the
       official server, we'll return an error */
    if (user->port == 0 &&
	    (con->user->port == 0 || msg == MSG_CLIENT_DOWNLOAD))
    {
	send_cmd (con, MSG_SERVER_FILE_READY,
		"%s %lu %d \"%s\" firewallerror 0", user->nick, user->host,
		user->port, fields[1]);
	return;
    }

    /* send a message to the requestee */
    log ("download(): sending upload request to %s", user->nick);

    /* if the requestee is a local user, send the request directly */
    if (user->con)
    {
	send_cmd (user->con, MSG_SERVER_UPLOAD_REQUEST, "%s \"%s\"",
		con->user->nick, fields[1]);
    }
    else
    {
	/* otherwise pass it to our peer servers for delivery */
	send_cmd (user->serv, MSG_SERVER_UPLOAD_REQUEST, ":%s %s \"%s\"",
		con->user->nick, fields[0], fields[1]);
    }
}

/* 220 */
HANDLER(upload_start)
{
    (void)pkt;
    ASSERT(VALID(con));
    CHECK_USER_CLASS("upload_start");
    con->user->uploads++;
}

/* 221 */
HANDLER(upload_end)
{
    (void)pkt;
    ASSERT(VALID(con));
    CHECK_USER_CLASS("upload_end");
    con->user->uploads--;
}

/* 218 */
HANDLER(download_start)
{
    (void)pkt;
    ASSERT(VALID(con));
    CHECK_USER_CLASS("download_start");
    con->user->downloads++;
}

/* 219 */
HANDLER(download_end)
{
    (void)pkt;
    ASSERT(VALID(con));
    CHECK_USER_CLASS("download_end");
    con->user->downloads--;
}

/* 600 <user> */
/* client is requesting the link speed of <user> */
HANDLER(user_speed)
{
    USER *user;
    CHECK_USER_CLASS("user_speed");
    user=hash_lookup(Users,pkt);
    if(!user)
    {
	/* TODO: what error does the server return here? */
	log("user_speed():no such user %s", pkt);
	return;
    }
    send_cmd(con,MSG_SERVER_USER_SPEED /* 601 */, "%s %d",
	    user->nick,user->speed);
}

static char *
my_ntoa (unsigned long ip)
{
    struct in_addr a;
    memset(&a,0,sizeof(a));
    a.s_addr = ip;
    return (inet_ntoa (a));
}

/* 626 [ :<nick> ] <user> */
/* client is notifying other party of a failure to connect to their data
   port */
HANDLER (data_port_error)
{
    USER *sender, *user;

    ASSERT (validate_connection (con));
    if (pop_user (con, &pkt, &sender) != 0)
	return;
    ASSERT (validate_user (sender));
    user = hash_lookup (Users, pkt);
    if (!user)
    {
	log ("data_port_error(): no such user %s", pkt);
	return;
    }
    ASSERT (validate_user (user));
    if (user->con)
	send_cmd (user->con, MSG_SERVER_DATA_PORT_ERROR, "%s", user->nick);
    else if (con->class == CLASS_USER)
	send_cmd (user->serv, MSG_SERVER_DATA_PORT_ERROR, "%s", user->nick);

    if (con->class == CLASS_USER)
    {
	notify_mods ("Notification from %s: %s (%s) - configured data port %d is unreachable.",
	    sender->nick, user->nick, my_ntoa (user->host), user->port);
    }
}
