#include <stdio.h>
#include "list.h"

typedef struct
{
    FILE *stream;
    char *path;
} TEXTDB;

typedef struct
{
    LIST *columns;
    TEXTDB *db;
} TEXTDB_RES;

TEXTDB * textdb_init (const char *path);
void textdb_free_result (TEXTDB_RES *p);
TEXTDB_RES * textdb_new_result (TEXTDB *db, LIST *columns);
TEXTDB_RES * textdb_fetch (TEXTDB *db, const char *key);
int textdb_store (TEXTDB_RES *result);
void textdb_close (TEXTDB *);
