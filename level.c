/* Copyright (C) 2000 drscholl@users.sourceforge.net
   This is free software distributed under the terms of the
   GNU Public License.  See the file COPYING for details. */

#include <string.h>
#include "opennap.h"
#include "debug.h"

/* change the user level for a user */
/* [ :<nick> ] <user> <level> */
HANDLER (level)
{
    char *sender = 0, *fields[2];
    USER *user;
    LEVEL level;

    ASSERT (validate_connection (con));

    /* NOTE: we implicity trust that messages we receive from other servers
       are authentic, so we don't check the user privileges here.  we have
       to trust that the peer servers perform due dilegence before sending
       a message to us, otherwise we could never propogate initial user
       levels across all servers */
    if (con->class == CLASS_SERVER)
    {
	/* skip over who set the user level */
	sender = pkt;
	pkt = strchr (pkt, ' ');
	if (!pkt)
	{
	    log ("level(): request contained too few fields");
	    return;
	}
	pkt++;
    }

    if (split_line (fields, sizeof (fields) / sizeof (char *), pkt) != 2)
    {
	log ("level(): malformed client request");
	return;
    }

    user = hash_lookup (Users, fields[0]);
    if (!user)
    {
	if (con->class == CLASS_USER)
	    nosuchuser (con, fields[0]);
	else
	    log ("level(): user synch error, can't locate user %s", fields[0]);
	return;
    }

    ASSERT (validate_user (user));

    if (!strncasecmp ("elite", fields[1],2))
	level = LEVEL_ELITE;
    else if (!strncasecmp ("admin", fields[1],2))
	level = LEVEL_ADMIN;
    else if (!strncasecmp ("moderator", fields[1],2))
	level = LEVEL_MODERATOR;
    else if (!strncasecmp ("leech", fields[1],2))
	level = LEVEL_LEECH;
    else if (!strncasecmp ("user", fields[1],2))
	level = LEVEL_USER;
    else
    {
	log ("level(): tried to set %s to unknown level %s",
	    user->nick, fields[1]);
	return;
    }

    /* check for privilege */
    if (con->class == CLASS_USER)
    {
	ASSERT (validate_user (con->user));
	if ((level >= con->user->level && con->user->level < LEVEL_ELITE) ||
		user->level >= con->user->level)
	{
	    log ("level(): %s tried to set %s to level %s", con->user->nick,
		    user->nick, Levels[level]);
	    permission_denied (con);
	    return;
	}
	sender = con->user->nick;
    }

    user->level = level;

    /* pass the message to our peer servers if this came from a local user */
    if (con->class == CLASS_USER && Num_Servers)
    {
	pass_message_args (con, MSG_CLIENT_SETUSERLEVEL, ":%s %s %s",
		con->user->nick, user->nick, fields[1]);
    }

    notify_mods ("%s set %s's user level to %s (%d).", sender, user->nick,
	    Levels[user->level], user->level);

    /* if local, notify the user of their change in status */
    if (user->con)
    {
	send_cmd (user->con, MSG_SERVER_NOSUCH,
		"%s changed your level to %s (%d).",
		sender, Levels[user->level], user->level);
    }
}
