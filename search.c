/* Copyright (C) 2000 drscholl@users.sourceforge.net
   This is free software distributed under the terms of the
   GNU Public License.  See the file COPYING for details.

   $Id$ */

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include "opennap.h"
#include "debug.h"

/* structure used when handing a search for a remote user */
typedef struct
{
    CONNECTION *con;		/* connection to user that issused the search,
				   or the server they are connected to if
				   remote */
    char *nick;			/* user who issued the search */
    char *id;			/* the id for this search */
    short count;		/* how many ACKS have been recieved? */
    short numServers;		/* how many servers were connected at the time
				   this search was issued? */
}
DSEARCH;

static LIST *Remote_Search = 0;

/* parameters for searching */
typedef struct
{
    CONNECTION *con;		/* connection for user that issued search */
    USER *user;			/* user that issued the search */
    int minbitrate;
    int maxbitrate;
    int minfreq;
    int maxfreq;
    int minspeed;
    int maxspeed;
    int type;			/* -1 means any type */
    char *id;			/* if doing a remote search */
}
SEARCH;

/* returns 0 if the match is not acceptable, nonzero if it is */
static int
search_callback (DATUM * match, SEARCH * parms)
{
    /* don't return matches for a user's own files */
    if (match->user == parms->user)
	return 0;
    if (match->bitrate < parms->minbitrate)
	return 0;
    if (match->bitrate > parms->maxbitrate)
	return 0;
    if (match->user->speed < parms->minspeed)
	return 0;
    if (match->user->speed > parms->maxspeed)
	return 0;
    if (match->frequency < parms->minfreq)
	return 0;
    if (match->frequency > parms->maxfreq)
	return 0;
    if (parms->type != -1 && parms->type != match->type)
	return 0;		/* wrong content type */

    /* send the result to the server that requested it */
    if (parms->id)
    {
	ASSERT (ISSERVER (parms->con));
	/* 10016 <id> <user> "<filename>" <md5> <size> <bitrate> <frequency> <duration> */
	send_cmd (parms->con, MSG_SERVER_REMOTE_SEARCH_RESULT,
		  "%s %s \"%s\" %s %d %d %d %d",
		  parms->id, match->user->nick, match->filename, match->hash,
		  match->size, match->bitrate, match->frequency,
		  match->duration);
    }
    /* if a local user issued the search, notify them of the match */
    else
    {
	send_cmd (parms->con, MSG_SERVER_SEARCH_RESULT,
		  "\"%s\" %s %d %d %d %d %s %u %d",
		  match->filename,
		  match->hash,
		  match->size,
		  match->bitrate,
		  match->frequency,
		  match->duration,
		  match->user->nick, match->user->host, match->user->speed);
    }

    return 1;			/* accept match */
}

void
free_flist (FLIST * ptr)
{
    ASSERT ((ptr->count == 0) ^ (ptr->list != 0));
    FREE (ptr->key);
    list_free (ptr->list, (list_destroy_t) free_datum);
    FREE (ptr);
}

/* returns nonzero if there is already the token specified by `s' in the
   list */
static int
duplicate (LIST * list, const char *s)
{
    ASSERT (s != 0);
    for (; list; list = list->next)
    {
	ASSERT (list->data != 0);
	if (!strcmp (s, list->data))
	    return 1;
    }
    return 0;
}

/* consider the apostrophe to be part of the word since it doesn't make
   sense on its own */
#define WORD_CHAR(c) (isalnum((unsigned char)c)||c=='\'')

/* return a list of word tokens from the input string */
LIST *
tokenize (char *s)
{
    LIST *r = 0, *cur = 0;
    char *ptr;

    while (*s)
    {
	while (*s && !WORD_CHAR (*s))
	    s++;
	ptr = s;
	while (*ptr && WORD_CHAR (*ptr))
	    ptr++;
	if (*ptr)
	    *ptr++ = 0;
	strlower (s);		/* convert to lower case to save time */
	/* don't bother with common words, if there is more than 1,000 of
	   any of these it doesnt do any good for the search engine because
	   it won't match on them.  its doubtful that these would narrow
	   searches down any even after the selection of the bin to search */
	if (!strcmp ("a", s) || !strcmp ("i", s) ||
	    !strcmp ("the", s) || !strcmp ("and", s) || !strcmp ("in", s) ||
	    !strcmp ("of", s) || !strcmp ("you", s) ||
	    !strcmp ("me", s) || !strcmp ("to", s) || !strcmp("on", s) ||
	    /* the following are common path names and don't really
	       provide useful information */
	    !strcmp ("mp3", s) || !strcmp ("c", s) || !strcmp ("d", s) ||
	    !strcmp ("e", s) || !strcmp ("napster", s) ||
	    !strcmp ("music", s) || !strcmp ("program", s) ||
	    !strcmp ("files", s) || !strcmp ("windows", s) ||
	    !strcmp ("songs", s) || !strcmp ("desktop", s) ||
	    !strcmp ("my", s) || !strcmp ("documents", s) ||
	    !strcmp ("mp3's", s) || !strcmp ("rock", s) ||
	    !strcmp ("new", s) || !strcmp ("winamp", s) ||
	    !strcmp ("scour", s) || !strcmp ("media", s) ||
	    !strcmp ("agent", s) || !strcmp ("stuff", s) ||
	    !strcmp ("mp3s", s) || !strcmp ("2", s))
	{
	    s = ptr;
	    continue;
	}
	/* don't add duplicate tokens to the list.  this will cause searches
	   on files that have the same token more than once to show up how
	   ever many times the token appears in the filename */
	if (duplicate (r, s))
	{
	    s = ptr;
	    continue;
	}
	if (cur)
	{
	    cur->next = CALLOC (1, sizeof (LIST));
	    if (!cur->next)
	    {
		OUTOFMEMORY ("tokenize");
		return r;
	    }
	    cur = cur->next;
	}
	else
	{
	    cur = r = CALLOC (1, sizeof (LIST));
	    if (!cur)
	    {
		OUTOFMEMORY ("tokenize");
		return 0;
	    }
	}
	cur->data = s;
	s = ptr;
    }
    return r;
}

void
free_datum (DATUM * d)
{
    ASSERT (d->refcount > 0);
    d->valid = 0;
    d->user = 0;
    d->refcount--;
    if (d->refcount == 0)
    {
	/* no more references, we can free this memory */
	FREE (d->filename);
	FREE (d->hash);
	FREE (d);
    }
}

typedef struct
{
    int reaped;
    HASH *table;
}
GARBAGE;

#define THRESH 5000

static void
collect_garbage (FLIST * files, GARBAGE * data)
{
    LIST **ptr, *tmp;
    DATUM *d;

    /* print some info about large bins so we can consider adding them to
       the list of words to ignore in tokenize() */
    if (files->count >= THRESH)
    {
	log ("collect garbage(): bin for \"%s\" exceeds %d entries",
	     files->key, THRESH);
    }
    ptr = &files->list;
    while (*ptr)
    {
	d = (*ptr)->data;
	if (!d->valid)
	{
	    files->count--;
	    ++data->reaped;
	    tmp = *ptr;
	    *ptr = (*ptr)->next;
	    tmp->next = 0;
	    list_free (tmp, (list_destroy_t) free_datum);
	    continue;
	}
	ptr = &(*ptr)->next;
    }

    if (files->count == 0)
    {
	/* no more files, remove this entry from the hash table */
	hash_remove (data->table, files->key);
    }
}

/* walk the table and remove invalid entries */
void
fdb_garbage_collect (HASH * table)
{
    GARBAGE data;

    data.reaped = 0;
    data.table = table;

    log ("fdb_garbage_collect(): collecting garbage");
    hash_foreach (table, (hash_callback_t) collect_garbage, &data);
    log ("fdb_garbage_collect(): reaped %d dead entries", data.reaped);
}

/* check to see if all the strings in list of tokens are present in the
   filename.  returns 1 if all tokens were found, 0 otherwise */
static int
match (LIST * tokens, const char *file)
{
    const char *b;
    char c[3], *a;
    int l;

    c[2] = 0;
    for (; tokens; tokens = tokens->next)
    {
	a = tokens->data;
	/* there doesn't appear to be a case-insensitive strchr() function
	   so we fake it by using strpbrk() with a buffer that contains the
	   upper and lower case versions of the char */
	c[0] = tolower (*a);
	c[1] = toupper (*a);
	l = strlen (a);
	b = file;
	while (*b)
	{
	    b = strpbrk (b, c);
	    if (!b)
		return 0;
	    /* already compared the first char, see the if the rest of the
	       string matches */
	    if (!strncasecmp (b + 1, a + 1, l - 1))
		break;		/* matched, we are done with this token */
	    b++;		/* skip the matched char to find the next occurance */
	}
	if (!*b)
	    return 0;		/* hit the end of the string before matching */
    }
    return 1;
}

static int
fdb_search (HASH * table,
	    LIST * tokens,
	    int maxhits, int (*cb) (DATUM *, SEARCH *), SEARCH * cbdata)
{
    LIST *ptok;
    FLIST *flist = 0, *tmp;
    DATUM *d;
    int hits = 0;

    /* find the file list with the fewest files in it */
    for (ptok = tokens; ptok; ptok = ptok->next)
    {
	tmp = hash_lookup (table, ptok->data);
	if (!tmp)
	{
	    /* if there is no entry for this word in the hash table, then
	       we know there are no matches */
	    return 0;
	}
	if (!flist || tmp->count < flist->count)
	    flist = tmp;
    }
    if (!flist)
	return 0;		/* no matches */
    /* find the list of files which contain all search tokens */
    for (ptok = flist->list; ptok; ptok = ptok->next)
    {
	d = (DATUM *) ptok->data;
	if (d->valid && match (tokens, d->filename) && cb (d, cbdata))
	{
	    /* callback accepted match */
	    hits++;
	    if (hits == maxhits)
		break;		/* finished */
	}
    }
    return hits;
}

static void
generate_qualifier (char *d, int dsize, char *attr, int min, int max,
		    int hardmax)
{
    if (min > 0)
	snprintf (d, dsize, " %s \"%s\" %d",
		  attr, (min == max) ? "EQUAL TO" : "AT LEAST", min);
    else if (max < hardmax)
	snprintf (d, dsize, "BITRATE \"AT MOST\" %d", max);
}

#define MAX_SPEED 10
#define MAX_BITRATE 0xffff
#define MAX_FREQUENCY 0xffff

static void
generate_request (char *d, int dsize, int results, LIST * tokens,
		  SEARCH * parms)
{
    int l;

    snprintf (d, dsize, "FILENAME CONTAINS \"");
    l = strlen (d);
    d += l;
    dsize -= l;
    for (; tokens; tokens = tokens->next)
    {
	snprintf (d, dsize, "%s ", (char *) tokens->data);
	l = strlen (d);
	d += l;
	dsize -= l;
    }
    snprintf (d, dsize, "\" MAX_RESULTS %d", results);
    l = strlen (d);
    d += l;
    dsize -= l;
    if (parms->type != CT_MP3)
    {
	snprintf (d, dsize, " TYPE %s", Content_Types[parms->type]);
	l = strlen (d);
	d += l;
	dsize -= l;
    }
    generate_qualifier (d, dsize, "BITRATE", parms->minbitrate,
			parms->maxbitrate, MAX_BITRATE);
    l = strlen (d);
    d += l;
    dsize -= l;
    generate_qualifier (d, dsize, "FREQ", parms->minfreq, parms->maxfreq,
			MAX_FREQUENCY);
    l = strlen (d);
    d += l;
    dsize -= l;
    generate_qualifier (d, dsize, "LINESPEED", parms->minspeed,
			parms->maxspeed, MAX_SPEED);
}

static char *
generate_search_id (void)
{
    char *id = MALLOC (9);

    if (!id)
    {
	OUTOFMEMORY ("generate_search_id");
	return 0;
    }
    get_random_bytes (id, 4);
    expand_hex (id, 4);
    id[8] = 0;
    return id;
}

static void
free_dsearch (DSEARCH * d)
{
    if (d)
    {
	if (d->id)
	    FREE (d->id);
	if (d->nick)
	    FREE (d->nick);
	FREE (d);
    }
}

/* common code for local and remote searching */
static void
search_internal (CONNECTION * con, USER * user, char *id, char *pkt)
{
    char *av[32];
    int ac, i, n, max_results = Max_Search_Results, done = 1;
    LIST *tokens = 0;
    SEARCH parms;

    ASSERT (validate_connection (con));

    ac = split_line (av, sizeof (av) / sizeof (char *), pkt);

    ASSERT (ac != 32);		/* check to see if we had more av */

    memset (&parms, 0, sizeof (parms));
    parms.con = con;
    parms.user = user;
    parms.maxspeed = MAX_SPEED;
    parms.maxbitrate = MAX_BITRATE;
    parms.maxfreq = MAX_FREQUENCY;
    parms.type = CT_MP3;	/* search for audio/mp3 by default */
    parms.id = id;

    /* parse the request */
    for (i = 0; i < ac; i++)
    {
	if (!strcasecmp ("filename", av[i]))
	{
	    i++;
	    /* next word should be "contains" */
	    if (strcasecmp ("contains", av[i]) != 0)
	    {
		if (ISUSER (con))
		    send_cmd (con, MSG_SERVER_NOSUCH,
			      "invalid search request");
		log
		    ("search(): error in search string, expected '%s CONTAINS'",
		     av[i - 1]);
		goto done;
	    }
	    i++;
	    /* do an implicit AND operation if multiple FILENAME CONTAINS
	       clauses are specified */
	    tokens = list_append (tokens, tokenize (av[i]));
	}
	else if (strcasecmp ("max_results", av[i]) == 0)
	{
	    /* the LIMIT clause goes last, so we save it for later
	       processing */
	    i++;
	    max_results = atoi (av[i]);
	    if (Max_Search_Results && max_results > Max_Search_Results)
	    {
		log ("search(): client requested a maximum of %d results",
		     max_results);
		max_results = Max_Search_Results;
	    }
	}
	else if (!strcasecmp ("type", av[i]))
	{
	    i++;
	    parms.type = -1;
	    for (n = CT_MP3; n < CT_UNKNOWN; n++)
	    {
		if (!strcasecmp (av[i], Content_Types[n]))
		{
		    parms.type = n;
		    break;
		}
	    }
	    if (parms.type == -1)
	    {
		if (ISUSER (con))
		    send_cmd (con, MSG_SERVER_NOSUCH, "%s is an invalid type",
			      av[i]);
		goto done;
	    }
	}
	else if (!strcasecmp ("linespeed", av[i]))
	{
	    i++;
	    if (i == ac - 1)
	    {
		if (ISUSER (con))
		    send_cmd (con, MSG_SERVER_NOSUCH,
			      "not enough parameters");
		goto done;
	    }
	    n = atoi (av[i + 1]);
	    if (!strcasecmp ("at least", av[i]))
		parms.minspeed = n;
	    else if (!strcasecmp ("at most", av[i]))
		parms.maxspeed = n;
	    else if (!strcasecmp ("equal to", av[i]))
		parms.minspeed = parms.maxspeed = n;
	    else
	    {
		if (ISUSER (con))
		    send_cmd (con, MSG_SERVER_NOSUCH,
			      "\"%s\" is an unknown comparison", av[i]);
		goto done;
	    }
	    i++;
	}
	else if (!strcasecmp ("bitrate", av[i]))
	{
	    i++;
	    if (i == ac - 1)
	    {
		if (ISUSER (con))
		    send_cmd (con, MSG_SERVER_NOSUCH,
			      "not enough parameters");
		goto done;
	    }
	    n = atoi (av[i + 1]);
	    if (!strcasecmp ("at least", av[i]))
		parms.minbitrate = n;
	    else if (!strcasecmp ("at most", av[i]))
		parms.maxbitrate = n;
	    else if (!strcasecmp ("equal to", av[i]))
		parms.minbitrate = parms.maxbitrate = n;
	    else
	    {
		if (ISUSER (con))
		    send_cmd (con, MSG_SERVER_NOSUCH,
			      "\"%s\" is an unknown comparison", av[i]);
		goto done;
	    }
	    i++;
	}
	else if (!strcasecmp ("freq", av[i]))
	{
	    i++;
	    if (i == ac - 1)
	    {
		if (ISUSER (con))
		    send_cmd (con, MSG_SERVER_NOSUCH,
			      "not enough parameters");
		goto done;
	    }
	    n = atoi (av[i + 1]);
	    if (!strcasecmp ("at least", av[i]))
		parms.minfreq = n;
	    else if (!strcasecmp ("at most", av[i]))
		parms.maxfreq = n;
	    else if (!strcasecmp ("equal to", av[i]))
		parms.minfreq = parms.maxfreq = n;
	    else
	    {
		if (ISUSER (con))
		    send_cmd (con, MSG_SERVER_NOSUCH,
			      "\"%s\" is an unknown comparison", av[i]);
		goto done;
	    }
	    i++;
	}
	else
	{
	    log ("search(): unknown search field: %s", av[i]);
	    if (ISUSER (con))
		send_cmd (con, MSG_SERVER_NOSUCH, "invalid search request");
	    goto done;
	}
    }

    n = fdb_search (File_Table, tokens, max_results, search_callback, &parms);

    if ((n < max_results) &&
	((ISSERVER (con) && list_count (Servers) > 1) ||
	 (ISUSER (con) && Servers)))
    {
	char *request;
	DSEARCH *dsearch;
	LIST *ptr;

	/* generate a new request structure */
	dsearch = CALLOC (1, sizeof (DSEARCH));
	if (!dsearch)
	{
	    OUTOFMEMORY ("search_internal");
	    goto done;
	}
	if (id)
	{
	    if ((dsearch->id = STRDUP (id)) == 0)
	    {
		OUTOFMEMORY ("search_internal");
		FREE (dsearch);
		goto done;
	    }
	}
	else if ((dsearch->id = generate_search_id ()) == 0)
	{
	    FREE (dsearch);
	    goto done;
	}
	dsearch->con = con;
	if (!(dsearch->nick = STRDUP (user->nick)))
	{
	    OUTOFMEMORY ("search_internal");
	    free_dsearch (dsearch);
	    goto done;
	}
	/* keep track of how many replies we expect back */
	dsearch->numServers = list_count (Servers);
	/* if we recieved this from a server, we expect 1 less reply since
	   we don't send the search request back to the server that issued
	   it */
	if (ISSERVER (con))
	    dsearch->numServers--;
	ptr = CALLOC (1, sizeof (LIST));
	if (!ptr)
	{
	    OUTOFMEMORY ("search_internal");
	    free_dsearch (dsearch);
	    goto done;
	}
	ptr->data = dsearch;
	Remote_Search = list_append (Remote_Search, ptr);
	/* reform the search request to send to the remote servers */
	generate_request (Buf, sizeof (Buf), max_results - n, tokens, &parms);
	/* make a copy since pass_message_args() uses Buf[] */
	request = STRDUP (Buf);
	/* pass this message to all servers EXCEPT the one we recieved
	   it from (if this was a remote search) */
	pass_message_args (con, MSG_SERVER_REMOTE_SEARCH, "%s %s %s",
			   dsearch->nick, dsearch->id, request);
	FREE (request);
	done = 0;		/* delay sending the end-of-search message */
    }

  done:

    list_free (tokens, 0);

    if (done)
    {
	if (ISUSER (con))
	    send_cmd (con, MSG_SERVER_SEARCH_END, "");
	else
	{
	    ASSERT (ISSERVER (con));
	    ASSERT (id != 0);
	    send_cmd (con, MSG_SERVER_REMOTE_SEARCH_END, "%s", id);
	}
    }
}

/* 200 ... */
HANDLER (search)
{
    (void) tag;
    (void) len;
    ASSERT (validate_connection (con));
    CHECK_USER_CLASS ("search");
    search_internal (con, con->user, 0, pkt);
}

static DSEARCH *
find_search (const char *id)
{
    LIST *list;

    for (list = Remote_Search; list; list = list->next)
    {
	ASSERT (list->data != 0);
	if (!strcmp (((DSEARCH *) list->data)->id, id))
	    return list->data;
    }
    return 0;
}

/* 10015 <sender> <id> ...
   remote search request */
HANDLER (remote_search)
{
    USER *user;
    char *nick, *id;

    (void) tag;
    (void) len;
    ASSERT (validate_connection (con));
    CHECK_SERVER_CLASS ("remote_search");
    nick = next_arg (&pkt);
    id = next_arg (&pkt);
    if (!nick || !id || !pkt)
    {
	log ("remote_search(): too few parameters");
	return;
    }
    user = hash_lookup (Users, nick);
    if (!user)
    {
	log ("remote_search(): could not locate user %s", nick);
	/* imediately notify the peer that we don't have any matches */
	send_cmd (con, MSG_SERVER_REMOTE_SEARCH_END, "%s", id);
	return;
    }
    search_internal (con, user, id, pkt);
}

/* 10016 <id> <user> "<filename>" <md5> <size> <bitrate> <frequency> <duration>
   send a search match to a remote user */
HANDLER (remote_search_result)
{
    DSEARCH *search;
    char *av[8];
    int ac;
    USER *user;

    (void) con;
    (void) tag;
    (void) len;
    ASSERT (validate_connection (con));
    ac = split_line (av, sizeof (av) / sizeof (char *), pkt);

    if (ac != 8)
    {
	log ("remote_search_result(): wrong number of args");
	print_args (ac, av);
	return;
    }
    search = find_search (av[0]);
    if (!search)
    {
	log ("remote_search_result(): could not find search id %s", av[0]);
	return;
    }
    if (ISUSER (search->con))
    {
	/* deliver the match to the client */
	user = hash_lookup (Users, av[1]);
	if (!user)
	{
	    log ("remote_search_result(): could not find user %s", av[1]);
	    return;
	}
	send_cmd (search->con, MSG_SERVER_SEARCH_RESULT,
		"\"%s\" %s %s %s %s %s %s %u %d",
		av[2], av[3], av[4], av[5], av[6], av[7], user->nick,
		user->host, user->speed);
    }
}

/* 10017 <id>
   indicates end of search results for <id> */
HANDLER (remote_search_end)
{
    DSEARCH *search;

    ASSERT (validate_connection (con));
    (void) con;
    (void) tag;
    (void) len;
    search = find_search (pkt);
    if (!search)
    {
	log ("remote_end_match(): could not find entry for search id %s",pkt);
	return;
    }
    ASSERT (search->numServers <= list_count (Servers));
    search->count++;
    if (search->count == search->numServers)
    {
	/* got the end of the search matches from all our peers, issue
	   final ack to the server that sent us this request, or deliver
	   end of search to user */
	ASSERT (validate_connection (search->con));
	if (ISUSER (search->con))
	    send_cmd (search->con, MSG_SERVER_SEARCH_END, "");
	else
	{
	    ASSERT (ISSERVER (search->con));
	    send_cmd (search->con, tag, "%s", search->id);
	}
	Remote_Search = list_delete (Remote_Search, search);
	free_dsearch (search);
    }
}

/* if a user logs out before the search is complete, we need to cancel
   the search so that we don't try to send the result to the client */
void
cancel_search (CONNECTION * con)
{
    LIST **list, *tmpList;
    DSEARCH *d;
    int isServer = ISSERVER (con);

    ASSERT (validate_connection (con));
    list = &Remote_Search;
    while (*list)
    {
	d = (*list)->data;
	if (isServer)
	    d->numServers--;
	if (d->con == con || d->count == d->numServers)
	{
	    if (d->con != con)
	    {
		/* send the final ack */
		log ("cancel_search(): sending final ACK for id %s", d->id);
		if (ISUSER (d->con))
		    send_cmd (d->con, MSG_SERVER_SEARCH_END, "");
		else
		    send_cmd (d->con, MSG_SERVER_REMOTE_SEARCH_END, d->id);
	    }
	    tmpList = *list;
	    *list = (*list)->next;
	    FREE (tmpList);
	    free_dsearch (d);
	    continue;
	}
	list = &(*list)->next;
    }
}
