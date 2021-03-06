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
    char realname[256];
    FILE *fp;
    int limit, level, line = 0;
    CHANNEL *chan;
    LIST *list;
    int version = 0;

    snprintf (path, sizeof (path), "%s/channels", Config_Dir);
    fp = fopen (path, "r");
    if (!fp)
    {
	if (errno != ENOENT)
	    logerr ("load_channels", path);
	return;
    }
    if(fgets(Buf,sizeof(Buf),fp)==NULL)
    {
	fclose(fp);
	return;
    }
    if(!strncmp(":version 1", Buf, 10))
	version = 1;
    else
	rewind(fp);

    while (fgets (Buf, sizeof (Buf), fp))
    {
	line++;
	ptr = Buf;
	while (ISSPACE (*ptr))
	    ptr++;
	if (*ptr == 0 || (version == 0 && *ptr == '#'))
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
	/*force new channel name restrictions*/
	if(*name!='#' && *name!='&')
	{
	    snprintf(realname,sizeof(realname),"#%s",name);
	    name=realname;
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
    if ((chan->flags & ON_CHANNEL_USER) == 0)
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
    fputs(":version 1\n",fp);
    hash_foreach (Channels, (hash_callback_t) dump_channel_cb, fp);
    if (fclose (fp))
	logerr ("dump_channels", "fclose");
}

/* 422 [ :<sender> ] <channel> <user!ip> [ "<reason>" ] */
HANDLER (channel_ban)
{
    CHANNEL *chan;
    char *av[3], *sender;
    int ac = -1;
    LIST *list;
    BAN *b;
    char *banptr, realban[256];
    USER *senderUser;

    (void) len;
    ASSERT (validate_connection (con));
    if(pop_user_server(con,tag,&pkt,&sender,&senderUser))
	return;
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
    if (senderUser && senderUser->level < LEVEL_MODERATOR
	    && !is_chanop (chan, senderUser))
    {
	permission_denied (con);
	return;
    }

    banptr=normalize_ban(av[1],realban,sizeof(realban));

    /* ensure this user/ip is not already banned */
    for (list = chan->bans; list; list = list->next)
    {
	b = list->data;
	if (!strcasecmp (b->target, banptr))
	    return;	/* ignore, already banned */
    }

    b = CALLOC (1, sizeof (BAN));
    if (b)
    {
	b->setby = STRDUP (sender);
	b->target = STRDUP (banptr);
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
    notify_ops (chan, "%s banned %s from %s: %s", sender, b->target,
		chan->name, NONULL (b->reason));
}

/* 423 [ :<sender> ] <channel> <user!ip> [ "<reason>" ] */
HANDLER (channel_unban)
{
    char *sender, *av[3];
    int ac = -1;
    LIST **list, *tmpList;
    BAN *b;
    CHANNEL *chan;
    char *banptr, realban[256];
    USER *senderUser;

    (void) len;
    ASSERT (validate_connection (con));

    if(pop_user_server(con,tag,&pkt,&sender,&senderUser))
	return;
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
    if (senderUser && senderUser->level < LEVEL_MODERATOR
	    && !is_chanop (chan, senderUser))
    {
	permission_denied (con);
	return;
    }

    banptr=normalize_ban(av[1],realban,sizeof(realban));
    ASSERT (validate_channel (chan));
    for (list = &chan->bans; *list; list = &(*list)->next)
    {
	b = (*list)->data;
	if (!strcasecmp (banptr, b->target))
	{
	    pass_message_args (con, tag, ":%s %s %s%s%s%s",
			       sender, chan->name, b->target,
			       ac > 2 ? " \"" : "",
			       ac > 2 ? av[2] : "", ac > 2 ? "\"" : "");
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
		  "%s %s \"%s\" %d %d", b->target, b->setby,
		  NONULL (b->reason), (int) b->when, b->timeout);
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
    USER *user, *senderUser = 0;
    LIST **list, *tmpList;

    ASSERT (validate_connection (con));
    (void) len;
    if (pop_user_server (con, tag, &pkt, &sender, &senderUser))
	return;
    /* check for permission */
    if (senderUser && senderUser->level < LEVEL_MODERATOR)
    {
	permission_denied (con);
	return;
    }
    schan = next_arg (&pkt);
    if (!schan || !pkt)
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
    pass_message_args (con, tag, ":%s %s %s", sender, chan->name, pkt);
    while (pkt)
    {
	suser = next_arg (&pkt);
	if (invalid_nick (suser))
	{
	    invalid_nick_msg (con);
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
			{
			    if (ISUSER (user->con))
				send_cmd (user->con, MSG_SERVER_NOSUCH,
					  "%s removed you as operator on channel %s",
					  sender, chan->name);
			    chanUser->flags &= ~ON_OPERATOR;
			    notify_ops (chan,
					"%s removed %s as operator on channel %s",
					sender, user->nick, chan->name);
			}
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
		if (chanUser)
		{
		    if (ISUSER (user->con))
			send_cmd (user->con, MSG_SERVER_NOSUCH,
				  "%s set you as operator on channel %s",
				  sender, chan->name);
		    notify_ops (chan, "%s set %s as operator on channel %s",
				sender, suser, chan->name);
		    /* do this here so the user doesn't get the message twice */
		    chanUser->flags |= ON_OPERATOR;
		}
	    }
	    /* make sure we dump this channel to disk */
	    chan->flags &= ~ON_CHANNEL_USER;
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
	if (ISUSER (chanUser->user->con) &&
	    ((chanUser->flags & ON_OPERATOR) ||
	     chanUser->user->level > LEVEL_USER))
	    queue_data (chanUser->user->con, buf, 4 + len);
    }
}

/* 10206 <channel>
   list channel ops */
HANDLER (channel_op_list)
{
    CHANNEL *chan;
    LIST *list;
    int count = 0;

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
    for (list = chan->ops; list; count++, list = list->next)
	send_cmd (con, MSG_CLIENT_PRIVMSG, "ChanServ %s", list->data);
    send_cmd (con, MSG_CLIENT_PRIVMSG,
	      "ChanServ END of operators for channel %s (%d total)",
	      chan->name, count);
}

/* 10207 [ :<sender> ] <channel> [ "<reason>" ]
   drop channel */
HANDLER (channel_drop)
{
    USER *sender;
    char *av[2];
    int ac = -1;
    CHANNEL *chan;

    (void) len;
    ASSERT (validate_connection (con));
    if (pop_user (con, &pkt, &sender))
	return;
    if (pkt)
	ac = split_line (av, FIELDS (av), pkt);
    if (ac < 1)
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
    if (chan->flags & ON_CHANNEL_USER)
    {
	if (ISUSER (con))
	    send_cmd (con, MSG_SERVER_NOSUCH, "channel %s is not registered",
		      chan->name);
	return;
    }
    /* dont allow just anyone to drop channels */
    if (sender->level < LEVEL_MODERATOR || sender->level < chan->level)
    {
	permission_denied (con);
	return;
    }
    pass_message_args (con, tag, ":%s %s \"%s\"", sender->nick, chan->name,
		       (ac > 1) ? av[1] : "");
    notify_ops (chan, "%s dropped channel %s: %s",
		sender->nick, chan->name, (ac > 1) ? av[1] : "");

    /* if there are no users left, destroy the channel */
    if (!chan->users)
	hash_remove (Channels, chan->name);
    else
    {
	/* otherwise just set as a normal channel */
	chan->flags |= ON_CHANNEL_USER;
    }
}

/* 10208 [ :<sender> ] <channel> <text>
   sends a message to all channel ops/mods on a given channel */
HANDLER (channel_wallop)
{
    USER *sender;
    CHANNEL *chan;
    char *chanName;

    (void) len;
    ASSERT (validate_connection (con));
    if (pop_user (con, &pkt, &sender))
	return;
    if (sender->muzzled)
    {
	if (ISUSER (con))
	    send_cmd (con, MSG_SERVER_NOSUCH,
		      "channel wallop failed: you are muzzled");
	return;
    }
    chanName = next_arg (&pkt);
    if (!chanName || !pkt)
    {
	unparsable (con);
	return;
    }
    chan = hash_lookup (Channels, chanName);
    if (!chan)
    {
	invalid_channel_msg (con);
	return;
    }
    if (sender->level < LEVEL_MODERATOR && !is_chanop (chan, sender))
    {
	permission_denied (con);
	return;
    }
    /* NOTE: there is no check to make sure the sender is actually a member
       of the channel.  this should be ok since channel ops have to be present
       in the channel to issue the command (since they would not have op
       status otherwise, and is_chanop() will fail).  mods+ are assumed to
       be trusted enough that the check for membership is not required. */
    pass_message_args (con, tag, ":%s %s %s", sender->nick, chan->name, pkt);
    notify_ops (chan, "%s [ops/%s]: %s", sender->nick, chan->name, pkt);
}

static void
add_flag (char *d, int dsize, char *flag, int bit, int onmask, int offmask)
{
    if ((onmask & bit) || (offmask & bit))
    {
	int len = strlen (d);

	snprintf (d + len, dsize - len, "%s%c%s", dsize > 0 ? " " : "",
		  (onmask & bit) ? '+' : '-', flag);
    }
}

/* 10209 [ :<sender> ] <channel> [mode]
   change/display channel mode */
HANDLER (channel_mode)
{
    char *senderName, *chanName;
    USER *sender;
    CHANNEL *chan;
    int onmask = 0, offmask = 0, bit;

    (void) len;
    ASSERT (validate_connection (con));
    if (pop_user_server (con, tag, &pkt, &senderName, &sender))
	return;
    chanName = next_arg (&pkt);
    if (!chanName)
    {
	unparsable (con);
	return;
    }
    chan = hash_lookup (Channels, chanName);
    if (!chan)
    {
	nosuchchannel (con);
	return;
    }

    /* check for permission */
    if (sender && (sender->level < chan->level ||
		   (sender->level < LEVEL_MODERATOR
		    && !is_chanop (chan, sender))))
    {
	permission_denied (con);
	return;
    }

    if (!pkt)
    {
	if (ISUSER (con))
	    send_cmd_pre (con, tag, "mode for channel ", "%s%s%s%s",
			  chan->name,
			  (chan->flags & ON_CHANNEL_PRIVATE) ? " +PRIVATE" :
			  "",
			  (chan->flags & ON_CHANNEL_MODERATED) ? " +MODERATED"
			  : "",
			  (chan->flags & ON_CHANNEL_INVITE) ? " +INVITE" : "",
			  (chan->flags & ON_CHANNEL_TOPIC) ? " +TOPIC" : "");
	return;
    }
    while (pkt)
    {
	char *arg = next_arg (&pkt);

	ASSERT (arg != 0);
	if (!strcasecmp ("PRIVATE", arg + 1))
	    bit = ON_CHANNEL_PRIVATE;
	else if (!strcasecmp ("MODERATED", arg + 1))
	    bit = ON_CHANNEL_MODERATED;
	else if (!strcasecmp ("INVITE", arg + 1))
	    bit = ON_CHANNEL_INVITE;
	else if (!strcasecmp ("TOPIC", arg + 1))
	    bit = ON_CHANNEL_TOPIC;
	else
	{
	    if (ISUSER (con))
		send_cmd (con, MSG_SERVER_NOSUCH, "invalid channel mode: %s",
			  arg + 1);
	    continue;		/* unknown flag */
	}
	if (*arg == '+')
	{
	    onmask |= bit;
	    offmask &= ~bit;
	    if ((chan->flags & bit) == 0)
		chan->flags |= bit;
	    else
		onmask &= ~bit;	/* already set */
	}
	else if (*arg == '-')
	{
	    offmask |= bit;
	    onmask &= ~bit;
	    if (chan->flags & bit)
		chan->flags &= ~bit;
	    else
		offmask &= ~bit;	/* not set */
	}
	else
	{
	    if (ISUSER (con))
		send_cmd (con, MSG_SERVER_NOSUCH,
			  "invalid prefix for channel mode %s: %c", arg + 1,
			  *arg);
	    continue;
	}
    }

    /* only take action if something actually changed */
    if (onmask || offmask)
    {
	char msg[512];

	chan->timestamp = Current_Time;
	msg[0] = 0;
	add_flag (msg, sizeof (msg), "PRIVATE", ON_CHANNEL_PRIVATE, onmask,
		  offmask);
	add_flag (msg, sizeof (msg), "MODERATED", ON_CHANNEL_MODERATED,
		  onmask, offmask);
	add_flag (msg, sizeof (msg), "INVITE", ON_CHANNEL_INVITE, onmask,
		  offmask);
	add_flag (msg, sizeof (msg), "TOPIC", ON_CHANNEL_TOPIC, onmask,
		  offmask);

	notify_ops (chan, "%s changed mode on channel %s:%s",
		    senderName, chan->name, msg);
	pass_message_args (con, tag, ":%s %s %s", senderName,
			   chan->name, msg);
    }
}

static int
is_member (CHANNEL * chan, USER * user)
{
    LIST *list;
    CHANUSER *chanUser;

    for (list = chan->users; list; list = list->next)
    {
	chanUser = list->data;
	if (chanUser->user == user)
	    return 1;
    }
    return 0;
}

/* 10210 [ :<sender> ] <channel> <user>
   invite a user to a channel */
HANDLER (channel_invite)
{
    USER *sender, *user;
    int ac = -1;
    char *av[2];
    LIST *list;
    CHANNEL *chan;

    (void) len;
    ASSERT (validate_connection (con));
    if (pop_user (con, &pkt, &sender))
	return;
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
    if (!(chan->flags & ON_CHANNEL_INVITE))
    {
	if (ISUSER (con))
	    send_cmd (con, MSG_SERVER_NOSUCH, "channel is not invite only");
	return;
    }
    /* ensure this user meets the minimum level requirements */
    if (sender->level < chan->level)
    {
	permission_denied (con);
	return;
    }
    /*ensure the user is on this channel */
    if (!is_member (chan, sender))
    {
	permission_denied (con);
	return;
    }
    user = hash_lookup (Users, av[1]);
    if (!user)
    {
	nosuchuser (con);
	return;
    }
    if (is_member (chan, user))
    {
	if (ISUSER (con))
	    send_cmd (con, MSG_SERVER_NOSUCH, "user is already in channel");
	return;
    }
    /*ensure the user is not already invited */
    if (list_find (user->invited, chan))
	return;			/* already invited */

    list = CALLOC (1, sizeof (LIST));
    list->data = chan;
    list->next = user->invited;
    user->invited = list;

    list = CALLOC (1, sizeof (LIST));
    list->data = user;
    list->next = chan->invited;
    chan->invited = list;

    pass_message_args (con, tag, ":%s %s %s", sender->nick, chan->name,
		       user->nick);

    if (ISUSER (user->con))
    {
	if (user->con->numerics)
	    send_cmd (user->con, tag, "%s %s", chan->name, sender->nick);
	else
	    send_cmd (user->con, MSG_SERVER_NOSUCH,
		      "%s invited you to channel %s", sender->nick,
		      chan->name);
    }

    notify_ops (chan, "%s invited %s to channel %s", sender->nick, user->nick,
		chan->name);
}

/* 10211/10212 [:sender] <channel> [user [user ...]]
   voice/devoice users in a channel */
HANDLER (channel_voice)
{
    char *senderName;
    char *chanName;
    char *nick;
    USER *sender = 0, *user;
    CHANUSER *chanUser;
    LIST *list;
    CHANNEL *chan;

    (void) len;
    ASSERT (validate_connection (con));
    if (pop_user_server (con, tag, &pkt, &senderName, &sender))
	return;
    chanName = next_arg (&pkt);
    if (!chanName)
    {
	unparsable (con);
	return;
    }
    chan = hash_lookup (Channels, chanName);
    if (!chan)
    {
	nosuchchannel (con);
	return;
    }
    if (!pkt)
    {
	if (ISUSER (con))
	{
	    /* return a list of voiced users */
	    send_cmd (con, MSG_CLIENT_PRIVMSG,
		      "ChanServ Voiced users on channel %s", chan->name);
	    for (list = chan->users; list; list = list->next)
	    {
		chanUser = list->data;
		if (chanUser->flags & ON_CHANNEL_VOICE)
		    send_cmd (con, MSG_CLIENT_PRIVMSG, "ChanServ %s",
			      chanUser->user->nick);
	    }
	    send_cmd (con, MSG_CLIENT_PRIVMSG,
		      "ChanServ End of voiced users on channel %s",
		      chan->name);
	}
	return;
    }
    /* check for permission here so that normal users can see who is
     * currently voiced
     */
    if (sender && sender->level < LEVEL_MODERATOR
	&& !is_chanop (chan, sender))
    {
	permission_denied (con);
	return;
    }
    pass_message_args (con, tag, ":%s %s %s", senderName, chan->name, pkt);
    while (pkt)
    {
	nick = next_arg (&pkt);
	user = hash_lookup (Users, nick);
	if (!user)
	{
	    if (ISUSER (con))
		send_cmd (con, MSG_SERVER_NOSUCH,
			  "channel voice: %s is not online", nick);
	    continue;
	}
	for (list = chan->users; list; list = list->next)
	{
	    chanUser = list->data;
	    if (chanUser->user == user)
	    {
		if (tag == MSG_CLIENT_CHANNEL_UNVOICE)
		    chanUser->flags &= ~ON_CHANNEL_VOICE;
		else
		{
		    chanUser->flags |= ON_CHANNEL_VOICE;
		    chanUser->flags &= ~ON_CHANNEL_MUZZLED;
		}
		if (ISUSER (chanUser->user->con))
		{
		    send_cmd (chanUser->user->con, MSG_SERVER_NOSUCH,
			      "%s %s voice on channel %s",
			      (sender
			       && sender->cloaked) ? "Operator" : senderName,
			      (chanUser->flags & ON_CHANNEL_VOICE) ?
			      "gave you" : "removed your", chan->name);
		}
		notify_ops (chan, "%s %s voice %s %s on channel %s",
			    senderName,
			    (tag ==
			     MSG_CLIENT_CHANNEL_VOICE) ? "gave" : "removed",
			    (tag == MSG_CLIENT_CHANNEL_VOICE) ? "to" : "from",
			    user->nick, chan->name);
		break;
	    }
	}
	if (!list)
	{
	    if (ISUSER (con))
		send_cmd (con, MSG_SERVER_NOSUCH,
			  "channel voice: %s is not on channel %s",
			  user->nick, chan->name);
	}
    }
}

/* 10213/10214 [:sender] <channel> <user> ["reason"]
   channel muzzle/unmuzzle */
HANDLER (channel_muzzle)
{
    char *senderName;
    USER *sender, *user;
    CHANNEL *chan;
    LIST *list;
    CHANUSER *chanUser;
    int ac = -1;
    char *av[3];

    (void) len;
    ASSERT (validate_connection (con));
    if (pop_user_server (con, tag, &pkt, &senderName, &sender))
	return;
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
    if (sender
	&& (sender->level < LEVEL_MODERATOR && !is_chanop (chan, sender)))
    {
	permission_denied (con);
	return;
    }
    user = hash_lookup (Users, av[1]);
    if (!user)
    {
	nosuchuser (con);
	return;
    }
    /* don't allow ops to muzzle mods+ */
    if (sender && sender->level != LEVEL_ELITE && sender->level < user->level)
    {
	permission_denied (con);
	return;
    }
    for (list = chan->users; list; list = list->next)
    {
	chanUser = list->data;
	if (chanUser->user == user)
	{
	    if (tag == MSG_CLIENT_CHANNEL_MUZZLE)
	    {
		chanUser->flags |= ON_CHANNEL_MUZZLED;
		chanUser->flags &= ~ON_CHANNEL_VOICE;
	    }
	    else
		chanUser->flags &= ~ON_CHANNEL_MUZZLED;
	    if (ISUSER (chanUser->user->con))
		send_cmd (chanUser->user->con, MSG_SERVER_NOSUCH,
			  "%s %smuzzled you on channel %s: %s",
			  (sender
			   && sender->cloaked) ? "Operator" : senderName,
			  (chanUser->flags & ON_CHANNEL_MUZZLED) ? "" : "un",
			  chan->name, (ac > 2) ? av[2] : "");
	    notify_ops (chan, "%s %smuzzled %s on channel %s: %s", senderName,
			(chanUser->flags & ON_CHANNEL_MUZZLED) ? "" : "un",
			user->nick, chan->name, (ac > 2) ? av[2] : "");
	    break;
	}
    }
    pass_message_args (con, tag, ":%s %s %s \"%s\"", senderName, chan->name,
		       user->nick, (ac > 2) ? av[2] : "");
}
