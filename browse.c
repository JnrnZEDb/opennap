/* Copyright (C) 2000 drscholl@users.sourceforge.net
   This is free software distributed under the terms of the
   GNU Public License.  See the file COPYING for details.

   $Id$ */

#include <stdlib.h>
#include "opennap.h"
#include "debug.h"

typedef struct {
    short count;
    short max; 
    USER *sender;
    USER *user;
} BROWSE;

static void
browse_callback (DATUM *info, BROWSE *ctx)
{
    /* avoid flooding the client */
    if (ctx->max == 0 || ctx->count < ctx->max)
    {
	send_user (ctx->sender, MSG_SERVER_BROWSE_RESPONSE,
	    "%s \"%s\" %s %d %hu %hu %hu",
	    info->user->nick,
	    info->filename,
	    info->hash,
	    info->size,
	    info->bitrate,
	    info->frequency,
	    info->duration);

	ctx->count++;
    }
}

/* 211 [ :<sender> ] <nick> [ <max> ]
   browse a user's files */
HANDLER (browse)
{
    USER *sender, *user;
    BROWSE data;
    char *nick;

    (void) tag;
    (void) len;
    ASSERT (validate_connection (con));
    if(pop_user(con,&pkt,&sender))
	return;
    nick=next_arg(&pkt);
    user = hash_lookup (Users, nick);
    if (!user)
    {
	if(ISUSER(con))
	    nosuchuser (con, nick);
	return;
    }
    ASSERT (validate_user (user));

    if (user->local)
    {
	if(user->files)
	{
	    data.count = 0;
	    data.user = user;
	    data.sender = sender;
	    if(pkt)
		data.max = atoi (pkt);
	    if(Max_Browse_Result > 0 && data.max > Max_Browse_Result)
		data.max = Max_Browse_Result;
	    hash_foreach (user->files, (hash_callback_t) browse_callback, &data);
	}

	/* send end of browse list message */
	send_user (sender, MSG_SERVER_BROWSE_END, "%s", user->nick);
    }
    else
    {
	/* relay to the server that this user is connected to */
	log("browse(): %s is remote, relaying to %s", user->nick, user->con->host);
	send_cmd(user->con,tag,":%s %s %d", sender->nick, user->nick, pkt?atoi(pkt):Max_Browse_Result);
    }
}
