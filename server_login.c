/* Copyright (C) 2000 drscholl@users.sourceforge.net
   This is free software distributed under the terms of the
   GNU Public License. */

#include <unistd.h>
#include <mysql.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include "opennap.h"
#include "debug.h"
#include "global.h"
#include "md5.h"

extern MYSQL *Db;

/* process a request to establish a peer server connection */
/* <name> <nonce> */
HANDLER (server_login)
{
    char *fields[2];
    unsigned long ip;
    MD5_CTX md5;
    char hash[33];

    ASSERT (VALID (con));
    if (con->class != CLASS_UNKNOWN)
    {
	log ("server_login(): %s tried to login, but is already registered",
		con->host);
	send_cmd (con, MSG_SERVER_NOSUCH, "reregistration is not supported");
	remove_connection (con);
	return;
    }

    /* TODO: ensure that this server is not already connected */

    if (split_line (fields, sizeof (fields) / sizeof (char *), pkt) != 2)
    {
	log ("server_login(): wrong number of fields");
	send_cmd (con, MSG_SERVER_NOSUCH, "wrong number of fields");
	remove_connection (con);
	return;
    }

    /* make sure this connection is coming from where they say they are */
    /* TODO: make this nonblocking for the rest of the server */
    ip = lookup_ip (fields[0]);

    if (ip != con->ip)
    {
	send_cmd (con, MSG_SERVER_NOSUCH,
		"your ip address does not match that name");
	log ("server_login(): %s does not resolve to %s", fields[0],
		my_ntoa (con->ip));
	remove_connection (con);
	return;
    }

    FREE (con->host);
    con->host = STRDUP (fields[0]);

    con->sendernonce = STRDUP (fields[1]);

    if (!con->nonce)
    {
	if ((con->nonce = generate_nonce()) == NULL)
	{
	    remove_connection (con);
	    return;
	}

	/* respond with our own login request */
	send_cmd (con, MSG_SERVER_LOGIN, "%s %s", Server_Name, con->nonce);
    }

    /* send our challenge response */
    /* hash the peers nonce, our nonce and then our password */
    MD5Init (&md5);
    MD5Update (&md5, (uchar*)con->sendernonce, (uint)strlen (con->sendernonce));
    MD5Update (&md5, (uchar*)con->nonce, (uint)strlen (con->nonce));
    MD5Update (&md5, (uchar*)Server_Pass, (uint)strlen (Server_Pass));
    MD5Final ((uchar*)hash, &md5);
    expand_hex (hash, 16);
    hash[32] = 0;

    /* send the response */
    send_cmd (con, MSG_SERVER_LOGIN_ACK, hash);

    /* now we wait for the peers ACK */
}

HANDLER (server_login_ack)
{
    MYSQL_RES *result;
    MYSQL_ROW row;
    MD5_CTX md5;
    char hash[33];

    ASSERT (validate_connection (con));

    if (con->class != CLASS_UNKNOWN)
    {
	send_cmd (con, MSG_SERVER_NOSUCH, "reregistration is not supported");
	log ("server_login_ack(): already registered!");
	return;
    }
    if (!con->nonce || !con->sendernonce)
    {
	send_cmd (con, MSG_SERVER_NOSUCH, "you must login first");
	log ("server_login_ack(): received ACK with no LOGIN?");
	remove_connection (con); /* FATAL */
	return;
    }

    /* look up the entry in our peer servers database */
    snprintf (Buf, sizeof (Buf),
	"SELECT password FROM servers WHERE name = '%s'", con->host);
    if (mysql_query (Db, Buf) != 0)
    {
	send_cmd (con, MSG_SERVER_NOSUCH, "sql error");
	sql_error ("server_login",Buf);
	remove_connection (con);
	return;
    }

    result = mysql_store_result (Db);

    if (mysql_num_rows (result) != 1)
    {
	log ("server_login_ack(): expected 1 row returned from sql query");
	permission_denied (con);
	mysql_free_result (result);
	remove_connection (con);
	return;
    }

    row = mysql_fetch_row (result);

    /* check the peers challenge response */
    MD5Init (&md5);
    MD5Update (&md5, (uchar *) con->nonce, (uint) strlen (con->nonce));
    MD5Update (&md5, (uchar *) con->sendernonce, (uint) strlen (con->sendernonce));
    MD5Update (&md5, (uchar *) row[0], (uint) strlen (row[0])); /* password for them */
    MD5Final ((uchar *) hash, &md5);
    expand_hex (hash, 16);
    hash[32] = 0;

    mysql_free_result (result);

    if (strcmp (hash, pkt) != 0)
    {
	log ("server_login_ack(): incorrect response for server %s",
		con->host);
	permission_denied (con);
	remove_connection (con);
	return;
    }
		
    log ("server_login(): server %s has joined", con->host);

    con->class = CLASS_SERVER;

    /* put this connection in the shortcut list to the server conections */
    add_server (con);

    /* synchronize our state with this server */
    synch_server (con);

    notify_mods ("server %s has joined.", con->host);
}
