/* Copyright (C) 2000 drscholl@users.sourceforge.net
   This is free software distributed under the terms of the
   GNU Public License.  See the file COPYING for details.

   $Id$ */

#include <stdlib.h>
#include "opennap.h"
#include "debug.h"

void
free_user (USER * user)
{
    HOTLIST *hotlist;
    LIST *list;

    ASSERT (validate_user (user));

    if (user->local && Servers)
    {
	/* local user, notify peers of this user's departure */
	pass_message_args (user->con, MSG_CLIENT_QUIT, "%s", user->nick);
    }

    /* this marks all the files this user was sharing as invalid.  they
       get reaped in the process of garbage collection in the main loop. */
    if (user->files)
	free_hash (user->files);

    /* remove this user from any channels they were on */
    if (user->channels)
    {
	for (list = user->channels; list; list = list->next)
	{
	    /* notify locally connected clients in the same channel that
	       this user has parted */
	    part_channel (list->data, user);
	}
	list_free (user->channels, 0);
    }

    ASSERT (Num_Files >= user->shared);
    Num_Files -= user->shared;
    Num_Gigs -= user->libsize;	/* this is in kB */
    if (user->local)
	Local_Files -= user->shared;

    /* check the global hotlist for this user to see if anyone wants notice
       of this user's departure */
    hotlist = hash_lookup (Hotlist, user->nick);
    if (hotlist)
    {
	ASSERT (validate_hotlist (hotlist));
	ASSERT (hotlist->users != 0);
	for (list = hotlist->users; list; list = list->next)
	    send_cmd (list->data, MSG_SERVER_USER_SIGNOFF, "%s", user->nick);
    }

    FREE (user->nick);
    FREE (user->pass);
    FREE (user->clientinfo);
    FREE (user->server);
    if (user->email)
	FREE (user->email);
    FREE (user);
}
