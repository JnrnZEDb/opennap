/* Copyright (C) 2000 drscholl@users.sourceforge.net
   This is free software distributed under the terms of the
   GNU Public License.

   $Id$ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "opennap.h"
#include "debug.h"

#if 0
static void
compute_soundex (char *d, int dsize, const char *s)
{
    int n = 0;

    /* if it's not big enough to hold one soundex word, quit without
       doing anything */
    if (dsize < 4)
    {
	ASSERT (0);		/* this is a programming error */
	if (dsize > 0)
	    *d = 0;
	return;
    }
    dsize--;			/* save room for the terminatin nul (\0) */

    while (*s && !isalpha ((unsigned char) *s))
	s++;
    if (!*s)
    {
	*d = 0;
	return;
    }

    *d++ = toupper ((unsigned char) *s);
    dsize--;
    s++;

    while (*s && dsize > 0)
    {
	switch (tolower ((unsigned char) *s))
	{
	case 'b':
	case 'p':
	case 'f':
	case 'v':
	    if (n < 3)
	    {
		*d++ = '1';
		dsize--;
		n++;
	    }
	    break;
	case 'c':
	case 's':
	case 'k':
	case 'g':
	case 'j':
	case 'q':
	case 'x':
	case 'z':
	    if (n < 3)
	    {
		*d++ = '2';
		dsize--;
		n++;
	    }
	    break;
	case 'd':
	case 't':
	    if (n < 3)
	    {
		*d++ = '3';
		dsize--;
		n++;
	    }
	    break;
	case 'l':
	    if (n < 3)
	    {
		*d++ = '4';
		dsize--;
		n++;
	    }
	    break;
	case 'm':
	case 'n':
	    if (n < 3)
	    {
		*d++ = '5';
		dsize--;
		n++;
	    }
	    break;
	case 'r':
	    if (n < 3)
	    {
		*d++ = '6';
		dsize--;
		n++;
	    }
	    break;
	default:
	    if (!isalpha ((unsigned char) *s))
	    {
		/* pad short words with 0's */
		while (n < 3 && dsize > 0)
		{
		    *d++ = '0';
		    dsize--;
		    n++;
		}
		n = 0;		/* reset */
		/* skip forward until we find the next word */
		s++;
		while (*s && !isalpha ((unsigned char) *s))
		    s++;
		if (!*s)
		{
		    *d = 0;
		    return;
		}
		if (dsize > 0)
		{
		    *d++ = ',';
		    dsize--;
		    if (dsize > 0)
		    {
			*d++ = toupper ((unsigned char) *s);
			dsize--;
		    }
		}
	    }
	    /* else it's a vowel and we ignore it */
	    break;
	}
	/* skip over duplicate letters */
	while (*(s + 1) == *s)
	    s++;

	/* next letter */
	s++;
    }
    /* pad short words with 0's */
    while (n < 3 && dsize > 0)
    {
	*d++ = '0';
	dsize--;
	n++;
    }
    *d = 0;
}
#endif

static void
fdb_add (HASH * table, char *key, DATUM * d)
{
    FLIST *files;

    files = hash_lookup (table, key);
    /* if there is no entry for this particular word, create one now */
    if (!files)
    {
	files = CALLOC (1, sizeof (FLIST));
	if (!files)
	{
	    log ("fdb_add(): OUT OF MEMORY");
	    return;
	}
	files->key = STRDUP (key);
	if (!files->key)
	{
	    log ("fdb_add(): OUT OF MEMORY");
	    FREE (files);
	    return;
	}
	hash_add (table, files->key, files);
    }
    files->list = list_append (files->list, d);
    files->count++;
    d->refcount++;
}

static void
share_common (USER * user, int fsize /* file size in kbytes */ )
{
    user->shared++;

    /* to avoid rounding errors in the total approximate size, we first
       subtract what we know this client has contributed, then recalculate
       the size in gigabytes */
    user->libsize += fsize;
    Num_Gigs += fsize;		/* this is actually kB, not gB */
    Num_Files++;
}

/* common code for inserting a file into the various hash tables */
static void
insert_datum (DATUM * info, char *av)
{
    LIST *tok;

    /* split the filename into words */
    info->tokens = tokenize (av);

    /* add this entry to the hash table for this user's files */
    hash_add (info->user->files, info->filename, info);
    info->refcount++;

    /* add this entry to the global file list */
    for (tok = info->tokens; tok; tok = tok->next)
	fdb_add (File_Table, tok->data, info);

    /* index by md5 hash */
    fdb_add (MD5, info->hash, info);
}

static DATUM *
new_datum (char *filename, char *hash)
{
    DATUM *info = CALLOC (1, sizeof (DATUM));

    if (!info)
    {
	log ("new_datum(): OUT OF MEMORY");
	return 0;
    }
    info->filename = STRDUP (filename);
    if (!info->filename)
    {
	log ("new_datum(): OUT OF MEMORY");
	FREE (info);
	return 0;
    }
    info->hash = STRDUP (hash);
    if (!info->hash)
    {
	log ("new_datum(): OUT OF MEMORY");
	FREE (info->filename);
	FREE (info);
	return 0;
    }
    return info;
}

/* 100 [ :<nick> ] <filename> <md5sum> <size> <bitrate> <frequency> <time>
   client adding file to the shared database */
HANDLER (add_file)
{
    char *av[6];
    USER *user;
    DATUM *info;

    (void) tag;
    (void) len;
    ASSERT (validate_connection (con));

    if (pop_user (con, &pkt, &user) != 0)
	return;

    ASSERT (validate_user (user));

    if (Max_Shared && user->shared > Max_Shared)
    {
	log ("add_file(): %s is already sharing %d files", user->nick,
		user->shared);
	if (user->con)
	    send_cmd (user->con, MSG_SERVER_NOSUCH,
		      "You may only share %d files.", Max_Shared);
	return;
    }

    if (split_line (av, sizeof (av) / sizeof (char *), pkt) != 6)
    {
	log ("add_file: wrong number of fields in message");
	if (con->class == CLASS_USER)
	    send_cmd (con, MSG_SERVER_NOSUCH,
		      "invalid parameter data to add a file");
	return;
    }

    /* make sure this isn't a duplicate */
    if (hash_lookup (user->files, av[0]))
    {
	log ("add_file(): duplicate for user %s: %s", user->nick, av[0]);
	if (con->class == CLASS_USER)
	    send_cmd (con, MSG_SERVER_NOSUCH, "duplicate file");
	return;
    }

    /* create the db record for this file */
    if (!(info = new_datum (av[0], av[1])))
	return;
    info->user = user;
    info->size = atoi (av[2]);
    info->bitrate = atoi (av[3]);
    info->frequency = atoi (av[4]);
    info->duration = atoi (av[5]);
    info->type = CT_MP3;
    info->valid = 1;

    insert_datum (info, av[0]);

    /* if this is a local connection, pass this information to our peer
       servers.  note that we prepend `:<nick>' so that the peer servers
       know who is adding the file. */
    if (con->class == CLASS_USER && Num_Servers)
    {
	pass_message_args (con, MSG_CLIENT_ADD_FILE,
			   ":%s \"%s\" %s %d %d %d %d", user->nick,
			   info->filename, info->hash,
			   info->size, info->bitrate, info->frequency,
			   info->duration);
    }

    share_common (user, info->size / 1024);
}

char *Content_Types[] = {
    "mp3",	/* not a real type, but what we use for audio/mp3 */
    "audio",
    "video",
    "application",
    "image",
    "text"
};

/* 10300 [ :<user> ] "<filename>" <size> <hash> <content-type> */
HANDLER (share_file)
{
    char *av[4];
    USER *user;
    DATUM *info;
    int i, type;

    (void) len;
    (void) tag;

    ASSERT (validate_connection (con));

    if (pop_user (con, &pkt, &user) != 0)
	return;

    if (Max_Shared && user->shared > Max_Shared)
    {
	log ("add_file(): %s is already sharing %d files", user->nick,
		user->shared);
	if (user->con)
	    send_cmd (user->con, MSG_SERVER_NOSUCH,
		      "You may only share %d files.", Max_Shared);
	return;
    }

    if (split_line (av, sizeof (av) / sizeof (char *), pkt) != 4)
    {
	log ("share_file(): wrong number of fields");
	if (user->con)
	    send_cmd (user->con, MSG_SERVER_NOSUCH, "wrong number of fields");
	return;
    }

    /* make sure the content-type looks correct */
    type = -1;
    for (i = CT_AUDIO; i < CT_UNKNOWN; i++)
    {
	if (!strcasecmp (Content_Types[i], av[3]))
	{
	    type = i;
	    break;
	}
    }
    if (type == -1)
    {
	log ("share_file(): not a valid type: %s", av[3]);
	if (user->con)
	    send_cmd (user->con, MSG_SERVER_NOSUCH,
		      "%s is not a valid type", av[3]);
	return;
    }

    if (!(info = new_datum (av[0], av[2])))
	return;
    info->user = user;
    info->size = atoi (av[1]);
    info->type = type;
    info->valid = 1;

    insert_datum (info, av[0]);

    if (con->class == CLASS_USER && Num_Servers)
    {
	pass_message_args (con, MSG_CLIENT_SHARE_FILE, ":%s \"%s\" %d %s %s",
			   user->nick, info->filename, info->size,
			   info->hash, Content_Types[info->type]);
    }

    share_common (user, info->size / 1024);
}
