/* Copyright (C) 2000 drscholl@users.sourceforge.net
   This is free software distributed under the terms of the
   GNU Public License.  See the file COPYING for details.

   $Id$ */

#include <stdio.h>
#include <limits.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <ctype.h>
#include "opennap.h"
#include "debug.h"

void
load_channels (void)
{
    char path[_POSIX_PATH_MAX], *name, *slimit, *slevel, *topic, *ptr;
    FILE *fp;
    int limit, level, line = 0;
    CHANNEL *chan;
    LIST *list;

    snprintf (path, sizeof (path), "%s/channels", Config_Dir);
    fp = fopen (path, "r");
    if (!fp)
    {
	if (errno != ENOENT)
	    logerr ("load_channels", path);
	return;
    }
    while (fgets (Buf, sizeof (Buf), fp))
    {
	line++;
	ptr = Buf;
	while (ISSPACE (*ptr))
	    ptr++;
	if (*ptr == '#' || *ptr == 0)
	    continue;		/* blank or comment line */
	name = next_arg (&ptr);
	slimit = next_arg (&ptr);
	slevel = next_arg (&ptr);
	topic = next_arg (&ptr);
	if (!name || !slimit || !slevel || !topic)
	{
	    log ("load_channels(): %s:%d: too few parameters", path, line);
	    continue;
	}
	if (invalid_channel (name))
	{
	    log ("load_channels(): %s:%d: %s: invalid channel name", name);
	    continue;
	}
	level = get_level (slevel);
	if (level == -1)
	{
	    log ("load_channels(): %s:%d: %s: invalid level",
		 path, line, slevel);
	    continue;
	}
	limit = atoi (slimit);
	if (limit < 0 || limit > 65535)
	{
	    log ("load_channels(): %s:%d: %d: invalid limit",
		 path, line, limit);
	    continue;
	}
	chan = CALLOC (1, sizeof (CHANNEL));
	if (chan)
	{
#if DEBUG
	    chan->magic = MAGIC_CHANNEL;
#endif
	    chan->name = STRDUP (name);
	    chan->topic = STRDUP (topic);
	    chan->limit = limit;
	    chan->level = level;

	    /* add the list of defined operators */
	    while (ptr)
	    {
		name = next_arg (&ptr);
		list = CALLOC (1, sizeof (LIST));
		list->data = STRDUP (name);
		list->next = chan->ops;
		chan->ops = list;
	    }
	}
	if (hash_add (Channels, chan->name, chan))
	    free_channel (chan);
    }
}

static void
dump_channel_cb (CHANNEL * chan, FILE * fp)
{
    /* only save non-user created channels */
    if (!chan->userCreated)
    {
	fprintf (fp, "%s %d %s \"%s\"", chan->name, chan->limit,
		 Levels[chan->level], chan->topic);
	if (chan->ops)
	{
	    LIST *list;

	    for (list = chan->ops; list; list = list->next)
		fprintf (fp, " %s", (char *) list->data);
	}
#if WIN32
	fputc ('\r', fp);
#endif
	fputc ('\n', fp);
    }
}

void
dump_channels (void)
{
    char path[_POSIX_PATH_MAX];
    FILE *fp;

    snprintf (path, sizeof (path), "%s/channels", Config_Dir);
    fp = fopen (path, "w");
    if (!fp)
    {
	logerr ("dump_channels", path);
	return;
    }
    fprintf (fp,
	     "# auto generated by %s %s\r\n# Don't edit this file while %s is running or changes will be lost!\r\n",
	     PACKAGE, VERSION, PACKAGE);
    hash_foreach (Channels, (hash_callback_t) dump_channel_cb, fp);
    if (fclose (fp))
	logerr ("dump_channels", "fclose");
}

/* 422 [ :<sender> ] <channel> <user|ip> [ "<reason>" ] */
HANDLER (channel_ban)
{
    CHANNEL *chan;
    char *av[3], *sender;
    int ac = -1;
    LIST *list;
    BAN *b;

    (void) len;
    ASSERT (validate_connection (con));
    if (ISSERVER (con))
    {
	if (*pkt != ':')
	{
	    log ("channel_ban(): missing sender");
	    return;
	}
	pkt++;
	sender = next_arg (&pkt);
    }
    else
	sender = con->user->nick;
    if (pkt)
	ac = split_line (av, FIELDS (av), pkt);
    if (ac < 2)
    {
	unparsable (con);
	return;
    }
    chan = hash_lookup (Channels, av[0]);
    if (!chan)
    {
	nosuchchannel (con);
	return;
    }
    /* check for permission */
    if (ISUSER (con))
    {
	if (con->user->level < LEVEL_MODERATOR
	    && !is_chanop (chan, con->user))
	{
	    permission_denied (con);
	    return;
	}
    }

    /* check for valid input */
    if (!is_ip (av[1]) && invalid_nick (av[1]))
    {
	invalid_nick_msg(con);
	return;
    }

    /* ensure this user/ip is not already banned */
    for (list = chan->bans; list; list = list->next)
    {
	b = list->data;
	if (!strcasecmp (b->target, av[1]))
	{
	    if (ISUSER (con))
		send_cmd (con, MSG_SERVER_NOSUCH,
			  "%s is already banned from channel %s", b->target,
			  chan->name);
	    return;
	}
    }

    b = CALLOC (1, sizeof (BAN));
    if (b)
    {
	b->setby = STRDUP (sender);
	b->target = STRDUP (av[1]);
	b->type = is_ip (av[1]) ? BAN_IP : BAN_USER;
	b->when = Current_Time;
	if (ac > 2)
	    b->reason = STRDUP (av[2]);
    }
    if (!b || !b->setby || !b->target || (ac > 2 && !b->reason))
    {
	OUTOFMEMORY ("channel_ban");
	return;
    }
    list = CALLOC (1, sizeof (LIST));
    if (!list)
    {
	OUTOFMEMORY ("channel_ban");
	free_ban (b);
	return;
    }
    list->data = b;
    list->next = chan->bans;
    chan->bans = list;

    pass_message_args (con, tag, ":%s %s %s%s%s%s", sender, chan->name,
		       b->target, ac > 2 ? " \"" : "", ac > 2 ? av[2] : "",
		       ac > 2 ? "\"" : "");
    notify_mods (BANLOG_MODE, "%s banned %s from %s: %s", sender, b->target,
		 chan->name, NONULL (b->reason));
    notify_ops (chan, "%s banned %s from %s: %s", sender, b->target,
		chan->name, NONULL (b->reason));
}

/* 423 [ :<sender> ] <channel> <user|ip> [ "<reason>" ] */
HANDLER (channel_unban)
{
    char *sender, *av[3];
    int ac = -1;
    LIST **list, *tmpList;
    BAN *b;
    CHANNEL *chan;

    (void) len;
    ASSERT (validate_connection (con));

    if (ISSERVER (con))
    {
	if (*pkt != ':')
	{
	    log ("channel_unban(): missing sender");
	    return;
	}
	pkt++;
	sender = next_arg (&pkt);
    }
    else
	sender = con->user->nick;
    if (pkt)
	ac = split_line (av, FIELDS (av), pkt);
    if (ac < 2)
    {
	unparsable (con);
	return;
    }
    chan = hash_lookup (Channels, av[0]);
    if (!chan)
    {
	nosuchchannel (con);
	return;
    }
    if (ISUSER (con))
    {
	if (con->user->level < LEVEL_MODERATOR
	    && !is_chanop (chan, con->user))
	{
	    permission_denied (con);
	    return;
	}
    }
    if(!is_ip(av[1]) && invalid_nick(av[1]))
    {
	invalid_nick_msg(con);
	return;
    }
    ASSERT (validate_channel (chan));
    for (list = &chan->bans; *list; list = &(*list)->next)
    {
	b = (*list)->data;
	if (!strcasecmp (av[1], b->target))
	{
	    pass_message_args (con, tag, ":%s %s %s%s%s%s",
			       sender, chan->name, b->target,
			       ac > 2 ? " \"" : "",
			       ac > 2 ? av[2] : "", ac > 2 ? "\"" : "");
	    notify_mods (BANLOG_MODE, "%s unbanned %s from %s: %s",
			 sender, b->target, chan->name,
			 (ac > 2) ? av[2] : "");
	    notify_ops (chan, "%s unbanned %s from %s: %s",
			sender, b->target, chan->name, (ac > 2) ? av[2] : "");
	    free_ban (b);
	    tmpList = *list;
	    *list = (*list)->next;
	    FREE (tmpList);
	    return;
	}
    }
    if (ISUSER (con))
	send_cmd (con, MSG_SERVER_NOSUCH, "no such ban");
}

/* 420 <channel> */
HANDLER (channel_banlist)
{
    CHANNEL *chan;
    LIST *list;
    BAN *b;

    (void) len;
    CHECK_USER_CLASS ("channel_banlist");
    chan = hash_lookup (Channels, pkt);
    if (!chan)
    {
	nosuchchannel (con);
	return;
    }
    for (list = chan->bans; list; list = list->next)
    {
	b = list->data;
	/* TODO: i have no idea what the real format of this is.  nap v1.0
	   just displays whatever the server returns */
	send_cmd (con, MSG_SERVER_CHANNEL_BAN_LIST,
		  "%s %s \"%s\" %d", b->target, b->setby,
		  NONULL (b->reason), (int) b->when);
    }
    /* TODO: i assume the list is terminated in the same fashion the other
       list commands are */
    send_cmd (con, tag, "");
}

/* 424 [ :<sender> ] <channel> */
HANDLER (channel_clear_bans)
{
    USER *sender;
    CHANNEL *chan;

    (void) len;
    ASSERT (validate_connection (con));
    if (pop_user (con, &pkt, &sender))
	return;
    if (!pkt)
    {
	unparsable (con);
	return;
    }
    chan = hash_lookup (Channels, pkt);
    if (!chan)
    {
	nosuchchannel (con);
	return;
    }
    if (list_find (sender->channels, chan) == 0)
    {
	/* not on the channel */
	if (ISUSER (con))
	    send_cmd (con, MSG_SERVER_NOSUCH, "You are not on that channel");
	return;
    }
    if (sender->level < LEVEL_MODERATOR && !is_chanop (chan, sender))
    {
	permission_denied (con);
	return;
    }
    /* pass just in case servers are desynched */
    pass_message_args (con, tag, ":%s %s", sender->nick, chan->name);
    if (!chan->bans)
    {
	if (ISUSER (con))
	    send_cmd (con, MSG_SERVER_NOSUCH, "There are no bans");
	return;
    }
    list_free (chan->bans, (list_destroy_t) free_ban);
    chan->bans = 0;
    notify_mods (BANLOG_MODE, "%s cleared the ban list on %s", sender->nick,
		 chan->name);
    notify_ops (chan, "%s cleared the ban list on %s", sender->nick,
		chan->name);
}

static CHANUSER *
find_chanuser (LIST * list, USER * user)
{
    CHANUSER *chanUser;

    for (; list; list = list->next)
    {
	chanUser = list->data;
	ASSERT (chanUser->magic == MAGIC_CHANUSER);
	if (chanUser->user == user)
	    return chanUser;
    }
    return 0;
}

/* 10204/10205 [ :<sender> ] <channel> <nick> [nick ...]
   op/deop channel user */
HANDLER (channel_op)
{
    char *sender, *schan, *suser;
    CHANNEL *chan;
    CHANUSER *chanUser;
    USER *user;
    LIST **list, *tmpList;

    ASSERT (validate_connection (con));
    (void) len;
    if (ISSERVER (con))
    {
	if (*pkt != ':')
	{
	    log ("channel_op(): missing sender from server message");
	    return;
	}
	pkt++;
	sender = next_arg (&pkt);
    }
    else
    {
	ASSERT (ISUSER (con));
	if (con->user->level < LEVEL_MODERATOR)
	    permission_denied (con);
	sender = con->user->nick;
    }
    schan = next_arg (&pkt);
    if (!schan)
    {
	unparsable (con);
	return;
    }
    chan = hash_lookup (Channels, schan);
    if (!chan)
    {
	nosuchchannel (con);
	return;
    }
    if (pkt)
	pass_message_args (con, tag, ":%s %s %s", sender, chan->name, pkt);
    while (pkt)
    {
	suser = next_arg (&pkt);
	if (invalid_nick(suser))
	{
	    invalid_nick_msg(con);
	    continue;
	}
	for (list = &chan->ops; *list; list = &(*list)->next)
	    if (!strcasecmp ((*list)->data, suser))
	    {
		if (tag == MSG_CLIENT_DEOP)
		{
		    tmpList = *list;
		    *list = (*list)->next;
		    FREE (tmpList->data);
		    FREE (tmpList);
		    /* if the user is present, change their status */
		    user = hash_lookup (Users, suser);
		    if (user)
		    {
			chanUser = find_chanuser (chan->users, user);
			if (chanUser)
			    chanUser->flags &= ~ON_OPERATOR;
			if (ISUSER (user->con))
			    send_cmd (user->con, MSG_SERVER_NOSUCH,
				      "%s removed your operator on channel %s",
				      sender, chan->name);
			notify_mods (CHANGELOG_MODE,
				     "%s removed %s as operator on channel %s",
				     sender, user->nick, chan->name);
			notify_ops (chan,
				    "%s removed %s as operator on channel %s",
				    sender, user->nick, chan->name);
		    }
		}
		break;		/* present */
	    }
	if (tag == MSG_CLIENT_OP && !*list)
	{
	    *list = CALLOC (1, sizeof (LIST));
	    (*list)->data = STRDUP (suser);
	    /* if the user is present, change their status */
	    user = hash_lookup (Users, suser);
	    if (user)
	    {
		chanUser = find_chanuser (chan->users, user);
		if (ISUSER (user->con))
		    send_cmd (user->con, MSG_SERVER_NOSUCH,
			      "%s set you as operator on channel %s",
			      sender, chan->name);
		notify_mods (CHANGELOG_MODE,
			     "%s set %s as operator on channel %s",
			     sender, user->nick, chan->name);
		notify_ops (chan, "%s set %s as operator on channel %s",
			    sender, user->nick, chan->name);
		/* only do the following if the user is in the channel */
		/* do this here so the user doesn't get the message twice */
		if (chanUser)
		    chanUser->flags |= ON_OPERATOR;
	    }
	    /* make sure we dump this channel to disk */
	    chan->userCreated = 0;
	}
    }
}

void
notify_ops (CHANNEL * chan, const char *fmt, ...)
{
    LIST *list;
    CHANUSER *chanUser;
    char buf[256];
    int len;

    va_list ap;

    va_start (ap, fmt);
    vsnprintf (buf + 4, sizeof (buf) - 4, fmt, ap);
    va_end (ap);
    len = strlen (buf + 4);
    set_len (buf, len);
    set_tag (buf, MSG_SERVER_NOSUCH);
    for (list = chan->users; list; list = list->next)
    {
	chanUser = list->data;
	ASSERT (chanUser->magic == MAGIC_CHANUSER);
	if (ISUSER (chanUser->user->con) && (chanUser->flags & ON_OPERATOR))
	    queue_data (chanUser->user->con, buf, 4 + len);
    }
}

/* 10206 <channel>
   list channel ops */
HANDLER (channel_op_list)
{
    CHANNEL *chan;
    LIST *list;

    (void) tag;
    (void) len;
    ASSERT (validate_connection (con));
    CHECK_USER_CLASS ("channel_op_list");
    chan = hash_lookup (Channels, pkt);
    if (!chan)
    {
	nosuchchannel (con);
	return;
    }
    if (con->user->level < LEVEL_MODERATOR && !is_chanop (chan, con->user))
    {
	permission_denied (con);
	return;
    }
    send_cmd (con, MSG_CLIENT_PRIVMSG, "ChanServ Operators for channel %s:",
	      chan->name);
    for (list = chan->ops; list; list = list->next)
	send_cmd (con, MSG_CLIENT_PRIVMSG, "ChanServ %s", list->data);
    send_cmd (con, MSG_CLIENT_PRIVMSG,
	      "ChanServ END of operators for channel %s", chan->name);
}

/* 10207 [ :<sender> ] <channel> [ "<reason>" ]
   drop channel */
HANDLER (channel_drop)
{
    USER *sender;
    char *av[2];
    int ac=-1;
    CHANNEL *chan;

    ASSERT(validate_connection(con));
    if(pop_user(con,&pkt,&sender))
	return;
    if(pkt)
	ac=split_line(av,FIELDS(av),pkt);
    if(ac<1)
    {
	unparsable(con);
	return;
    }
    chan=hash_lookup(Channels,av[0]);
    if(!chan)
    {
	nosuchchannel(con);
	return;
    }
    if(!chan->userCreated)
    {
	if(ISUSER(con))
	    send_cmd(con,MSG_SERVER_NOSUCH,"channel %s is not registered",
		     chan->name);
	return;
    }
    /* dont allow just anyone to drop channels */
    if(sender->level < LEVEL_MODERATOR || sender->level < chan->level)
    {
	permission_denied(con);
	return;
    }
    pass_message_args(con,tag,":%s %s \"%s\"",sender->nick,chan->name,
		      (ac>1)?av[1]:"");
    notify_mods(CHANGELOG_MODE,"%s dropped channel %s: %s",
		sender->nick, chan->name, (ac>1)?av[1]:"");
    notify_ops(chan,"%s dropped channel %s: %s",
		sender->nick, chan->name, (ac>1)?av[1]:"");

    /* if there are no users left, destroy the channel */
    if(!chan->users)
	hash_remove(Channels,chan->name);
    else
	chan->userCreated = 1;	/* otherwise just set as a normal channel */
}
