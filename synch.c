/* Copyright (C) 2000 drscholl@users.sourceforge.net
   This is free software distributed under the terms of the
   GNU Public License.  See the file COPYING for details.

   $Id$ */

#include <stdio.h>
#include "opennap.h"
#include "debug.h"

char *Levels[LEVEL_ELITE + 1] = {
    "Leech",
    "User",
    "Moderator",
    "Admin",
    "Elite"
};

static void
sync_user (USER * user, CONNECTION * con)
{
    LIST *list;

    ASSERT (validate_connection (con));
    ASSERT (validate_user (user));

    /* we should never tell a peer server about a user that is behind
       them */
    ASSERT (user->con != con);
    if (user->con == con)
    {
	/* this really shouldnt happen! */
	ASSERT (0);
	return;
    }

    /* send a login message for this user */
    send_cmd (con, MSG_CLIENT_LOGIN,
	      "%s %s %d \"%s\" %d unknown %d %u %s %hu", user->nick,
	      user->pass, user->port, user->clientinfo, user->speed,
	      user->connected, user->ip, user->server, user->conport);

    /* update the user's level */
    if (user->level != LEVEL_USER)
    {
	/* get the timestamp from the user db entry */
	USERDB *db = hash_lookup (User_Db, user->nick);

	ASSERT (db != 0);
	send_cmd (con, MSG_CLIENT_SETUSERLEVEL, ":%s %s %s %d",
		  Server_Name, user->nick, Levels[user->level],
		  db->timestamp);
    }

    if (user->cloaked)
	send_cmd (con, MSG_CLIENT_CLOAK, ":%s 1", user->nick);

    /* do this before the joins so the user's already in the channel see
       the real file count */
    if (user->shared)
	send_cmd (con, MSG_SERVER_USER_SHARING, "%s %hu %u", user->nick,
		  user->shared, user->libsize);

    /* send the channels this user is listening on */
    for (list = user->channels; list; list = list->next)
    {
	send_cmd (con, MSG_CLIENT_JOIN, ":%s %s",
		  user->nick, ((CHANNEL *) list->data)->name);
    }

    /* MUST be after the join's since muzzled users cant join */
    if (user->muzzled)
	send_cmd (con, MSG_CLIENT_MUZZLE, ":%s %s", Server_Name, user->nick);
}

static void
sync_chan (CHANNEL * chan, CONNECTION * con)
{
    CHANUSER *chanUser;
    LIST *list;

    if (chan->level != LEVEL_USER)
	send_cmd (con, MSG_CLIENT_SET_CHAN_LEVEL, ":%s %s %s %d",
		  Server_Name, chan->name, Levels[chan->level],
		  chan->timestamp);
    if (chan->limit != Channel_Limit)
	send_cmd (con, MSG_CLIENT_CHANNEL_LIMIT, ":%s %s %d %d",
		  Server_Name, chan->name, chan->limit, chan->timestamp);

    /* synch the list of channel operators if present */
    if (chan->ops)
    {
	char buf[2048];
	LIST *list;
	int len;

	buf[0] = 0;
	for (list = chan->ops; list; list = list->next)
	{
	    len = strlen (buf);
	    snprintf (buf + len, sizeof (buf) - len, " %s",
		      (char *) list->data);
	}
	send_cmd (con, MSG_CLIENT_OP, ":%s %s%s", Server_Name, chan->name,
		  buf);
    }

    if (chan->flags & (ON_CHANNEL_PRIVATE | ON_CHANNEL_MODERATED))
	send_cmd (con, MSG_CLIENT_CHANNEL_MODE, ":%s %s%s%s",
		  Server_Name, chan->name,
		  (chan->flags & ON_CHANNEL_PRIVATE) ? " +PRIVATE" : "",
		  (chan->flags & ON_CHANNEL_MODERATED) ? " +MODERATED" : "",
		  (chan->flags & ON_CHANNEL_INVITE) ? " +INVITE" : "",
		  (chan->flags & ON_CHANNEL_TOPIC) ? " +TOPIC" : "");

    for (list = chan->users; list; list = list->next)
    {
	chanUser = list->data;
	if (chanUser->flags & ON_CHANNEL_VOICE)
	    send_cmd (con, MSG_CLIENT_CHANNEL_VOICE, ":%s %s %s",
		      Server_Name, chan->name, chanUser->user->nick);
	else if (chanUser->flags & ON_CHANNEL_MUZZLED)
	    send_cmd (con, MSG_CLIENT_CHANNEL_MUZZLE, ":%s %s %s",
		      Server_Name, chan->name, chanUser->user->nick);
    }
}

static void
sync_server_list (CONNECTION * con)
{
    LIST *list;
    LINK *slink;
    CONNECTION *serv;

    /* sync local servers */
    for (list = Servers; list; list = list->next)
    {
	serv = list->data;
	if (serv != con)
	{
	    send_cmd (con, MSG_SERVER_LINK_INFO, "%s %d %s %d 1",
		      Server_Name, get_local_port (serv->fd),
		      serv->host, serv->port);
	}
    }

    /* sync remote servers */
    for (list = Server_Links; list; list = list->next)
    {
	slink = list->data;
	send_cmd (con, MSG_SERVER_LINK_INFO, "%s %d %s %d %d",
		  slink->server, slink->port, slink->peer, slink->peerport,
		  slink->hops + 1);
    }
}

static void
sync_banlist (CONNECTION * con)
{
    LIST *list;
    BAN *b;

    ASSERT (validate_connection (con));
    for (list = Bans; list; list = list->next)
    {
	b = list->data;
	ASSERT (b != 0);
	send_cmd (con, MSG_CLIENT_BAN, ":%s %s \"%s\"", b->setby,
		  b->target, b->reason);
    }
}

void
synch_server (CONNECTION * con)
{
    ASSERT (validate_connection (con));

    log ("synch_server(): syncing");
    sync_server_list (con);
    /* send our peer server a list of all users we know about */
    hash_foreach (Users, (hash_callback_t) sync_user, con);
    /* sync the channel level */
    hash_foreach (Channels, (hash_callback_t) sync_chan, con);
    sync_banlist (con);
    log ("synch_server(): done");
}
