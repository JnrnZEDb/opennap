/* Copyright (C) 2000 drscholl@users.sourceforge.net
   This is free software distributed under the terms of the
   GNU Public License.  See the file COPYING for details.

   $Id$ */

#include <stdio.h>
#ifndef WIN32
#include <unistd.h>
#endif
#include <string.h>
#include "opennap.h"
#include "debug.h"

/* [ :<sender> ] <channel> <text> */
/*  public message to a channel */
HANDLER (public)
{
    CHANNEL *chan;
    USER *chanUser, *sender;
    LIST *list;
    int l;
    char *ptr;

    (void) tag;
    (void) len;
    ASSERT (validate_connection (con));
    /* save the starting position of the pkt */
    ptr = pkt;
    if (pop_user (con, &pkt, &sender))
	return;
    ASSERT (validate_user (sender));

    /* protect against DoS attack against the windows napster client */
    if (len - (pkt - ptr) > 180)
    {
	pkt[180] = 0;	/* crop the message */
	log ("public(): cropped %d byte message from user %s", len, sender->nick);
    }

    /* can't use split line here because the text field is considered all
       one item */
    /* extract the channel name. NOTE: we don't use next_arg() here because
       it will strip leading space from the text being sent */
    ptr = next_arg_noskip (&pkt);
    if (!pkt)
    {
	log ("public(): too few fields");
	if (ISUSER (con))
	    send_cmd (con, MSG_SERVER_NOSUCH,
		      "too few parameters for command");
	return;
    }
    /* find the channel this message is going to */
    chan = hash_lookup (Channels, ptr);
    if (!chan)
    {
	if (ISUSER (con))
	{
	    /* channel does not exist */
	    send_cmd (con, MSG_SERVER_NOSUCH, "Channel %s does not exist.",
		      ptr);
	}
	else
	    log ("public(): server sent message to nonexistent channel %s",
		 ptr);
	return;
    }
    ASSERT (validate_channel (chan));

    /* make sure this user is a member of the channel */
    list = list_find (sender->channels, chan);
    if (!list)
    {
	/* user is not a member of this channel */
	log ("public(): %s is not a member of channel %s", sender->nick,
	     chan->name);
	if (ISUSER (con))
	    send_cmd (con, MSG_SERVER_NOSUCH,
		      "you are not on channel %s", chan->name);
	return;
    }

    if (sender->muzzled)
    {
	log ("public(): %s is muzzled", sender->nick);
	if (ISUSER (con))
	    send_cmd (con, MSG_SERVER_NOSUCH, "You are muzzled.");
	return;
    }

    /* relay this message to peer servers */
    pass_message_args (con, tag, ":%s %s %s", sender->nick, chan->name, pkt);

    /* format the message */
    l = form_message (Buf, sizeof (Buf), MSG_SERVER_PUBLIC, "%s %s %s",
	    chan->name, sender->nick, pkt);

    /* send this message to everyone in the channel */
    for (list = chan->users; list; list = list->next)
    {
	chanUser = list->data;
	if (chanUser->local)
	    queue_data (chanUser->con, Buf, l);
    }
}

/* 824 [ :<user> ] <channel> "<text>" */
HANDLER (emote)
{
    USER *user, *chanUser;
    CHANNEL *chan;
    int buflen;
    char *ptr, *av[2];
    LIST *list;

    (void) tag;
    (void) len;
    ASSERT (validate_connection (con));
    ptr=pkt;	/* save initial location */
    if (pop_user (con, &pkt, &user) != 0)
	return;
    if (user->muzzled)
    {
	if (ISUSER (con))
	    send_cmd (con, MSG_SERVER_NOSUCH, "You are muzzled");
	return;
    }

    /* protect against DoS attack against the windows napster client */
    if (len - (pkt - ptr) > 180)
    {
	/* crop message */
	pkt[179]='"';
	pkt[180]=0;
	log ("emote(): cropped %d byte message from user %s", len, user->nick);
    }

    if (split_line (av, sizeof (av) / sizeof (char *), pkt) != 2)
    {
	if (ISUSER (con))
	    send_cmd (con, MSG_SERVER_ERROR,
		      "Wrong number of parameters for command.");
	return;
    }

    /* make sure this user is on the channel they are sending to */
    chan = hash_lookup (Channels, av[0]);
    if (!chan)
    {
	if (ISUSER (con))
	    send_cmd (con, MSG_SERVER_NOSUCH, "No such channel %s", av[0]);
	return;
    }
    if (list_find (chan->users, user) == 0)
    {
	if (ISUSER (con))
	    send_cmd (con, MSG_SERVER_NOSUCH,
		      "You are not a member of channel %s", chan->name);
	return;
    }

    /* relay to peer servers */
    pass_message_args (con, tag, ":%s %s \"%s\"", user->nick, chan->name,
		       av[1]);

    /* since we send the same data to multiple clients, format the data once
       and queue it up directly */
    buflen = form_message (Buf, sizeof (Buf), MSG_CLIENT_EMOTE,
	    "%s %s \"%s\"", chan->name, user->nick, av[1]);

    /* send this message to all channel members */
    for (list = chan->users; list; list = list->next)
    {
	chanUser = list->data;
	if (chanUser->local)
	    queue_data (chanUser->con, Buf, buflen);
    }
}
