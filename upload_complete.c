/* Copyright (C) 2000 drscholl@users.sourceforge.net
   This is free software distributed under the terms of the
   GNU Public License.  See the file COPYING for details.

   $Id$ */

#ifdef WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
#include <mysql.h>
#include <stdio.h>
#include "opennap.h"
#include "debug.h"

extern MYSQL *Db;

/* 608 [ :<sender> ] <recip> <filename> */
/* a client sends this message when another user has requested a file from
   them and they are accepting the connection.  this should be a
   response to the 607 upload request */
HANDLER (upload_ok)
{
    char *field[2];
    USER *sender, *recip;
    MYSQL_RES *result = 0;
    MYSQL_ROW row;
    char *hash = 0;
    char path[256];

    (void) tag;
    (void) len;
    ASSERT (validate_connection (con));

    if (pop_user (con, &pkt, &sender) != 0)
	return;

    if (split_line (field, sizeof (field) / sizeof (char *), pkt) != 2)
    {
	log ("upload_ok(): malformed message from %s", sender->nick);
	return;
    }

    recip = hash_lookup (Users, field[0]);
    if (!recip)
    {
	log ("upload_ok(): no such user %s", field[0]);
	return;
    }

    if (con->class == CLASS_USER || recip->con)
    {
	/* pull the has from the data base */
	fudge_path (field[1], path, sizeof (path));
	snprintf (Buf, sizeof (Buf),
	    "SELECT md5 FROM library WHERE owner = '%s' && filename = '%s'",
	    sender->nick, path);
	if (mysql_query (Db, Buf) != 0)
	{
	    sql_error ("upload_ok", Buf);
	    return;
	}
	result = mysql_store_result (Db);
	if (mysql_num_rows (result) != 1)
	{
	    log ("upload_ok(): query did not return 1 row!");
	    mysql_free_result (result);

	    /* notify the client that there was an error */
	    if (con->class == CLASS_USER)
		send_cmd (con, MSG_SERVER_SEND_ERROR, "%s \"%s\"",
			  recip->nick, field[1]);
	    return;
	}
	row = mysql_fetch_row (result);
	hash = row[0];
    }

    log ("upload_ok(): ACK \"%s\" %s => %s", field[1], sender->nick,
	recip->nick);

    if (sender->port == 0)
    {
	/* firewalled user */
	ASSERT (con->class == CLASS_USER);
	send_cmd (con, MSG_SERVER_UPLOAD_FIREWALL /* 501 */ ,
		  "%s %lu %d \"%s\" %s %d",
		  recip->nick, recip->host, recip->port, field[1], hash,
		  recip->speed);
    }
    else if (recip->con)
    {
	/* local connection */
	send_cmd (recip->con, MSG_SERVER_FILE_READY /* 204 */ ,
		  "%s %lu %d \"%s\" %s %d", sender->nick, sender->host,
		  sender->port, field[1], hash, sender->speed);
    }
    else if (con->class == CLASS_USER)
    {
	/* send this message to the server the recip is on */
	log ("upload_ok(): %s is remote, relaying message", recip->nick);
	ASSERT (recip->serv != 0);
	send_cmd (recip->serv, MSG_CLIENT_UPLOAD_OK, ":%s %s \"%s\"",
		  sender->nick, recip->nick, field[1]);
    }

    if (result)
	mysql_free_result (result);
}
