/*
  Copyright 2023 Northern.tech AS

  This file is part of CFEngine 3 - written and maintained by Northern.tech AS.

  This program is free software; you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by the
  Free Software Foundation; version 3.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA

  To the extent this program is licensed as part of the Enterprise
  versions of CFEngine, the applicable Commercial Open Source License
  (COSL) may apply to this file if you as a licensee so wish it. See
  included file COSL.txt.
*/

#include <platform.h>
#include <file_lib.h>

#include <mutex.h>                                            /* ThreadLock */
#include <dbm_api.h>
#include <dbm_priv.h>
#include <dbm_migration.h>
#include <cleanup.h>
#include <logging.h>
#include <misc_lib.h>
#include <known_dirs.h>
#include <string_lib.h>
#include <time.h>          /* time() */


static bool DBPathLock(FileLock *lock, const char *filename);
static void DBPathUnLock(FileLock *lock);
static void DBPathMoveBroken(const char *filename);

struct DBHandle_
{
    /* Filename of database file */
    char *filename;

    /* Name of specific sub-db */
    char *subname;

    /* Actual database-specific data */
    DBPriv *priv;

    int refcount;

    /* This lock protects initialization of .priv element, and .refcount manipulation */
    pthread_mutex_t lock;

    /* Record when the DB was opened (to check if possible corruptions are
     * already repaired) */
    time_t open_tstamp;

    /**
     * @see FreezeDB()
     */
    bool frozen;
};

struct DBCursor_
{
    DBCursorPriv *cursor;
};

typedef struct dynamic_db_handles_
{
    DBHandle *handle;
    struct dynamic_db_handles_ *next;
} DynamicDBHandles;

/******************************************************************************/

/*
 * This lock protects on-demand initialization of db_handles[i].lock and
 * db_handles[i].name.
 */
static pthread_mutex_t db_handles_lock = PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP; /* GLOBAL_T */

static DBHandle db_handles[dbid_max] = { { 0 } }; /* GLOBAL_X */
static DynamicDBHandles *db_dynamic_handles;

static pthread_once_t db_shutdown_once = PTHREAD_ONCE_INIT; /* GLOBAL_T */

/******************************************************************************/

// Only append to the end, keep in sync with dbid enum in dbm_api.h
static const char *const DB_PATHS_STATEDIR[] = {
    [dbid_classes] = "cf_classes",
    [dbid_variables] = "cf_variables",
    [dbid_performance] = "performance",
    [dbid_checksums] = "checksum_digests",
    [dbid_filestats] = "stats",
    [dbid_changes] = "cf_changes",
    [dbid_observations] = "cf_observations",
    [dbid_state] = "cf_state",
    [dbid_lastseen] = "cf_lastseen",
    [dbid_audit] = "cf_audit",
    [dbid_locks] = "cf_lock",
    [dbid_history] = "history",
    [dbid_measure] = "nova_measures",
    [dbid_static] = "nova_static",
    [dbid_scalars] = "nova_pscalar",
    [dbid_windows_registry] = "mswin",
    [dbid_cache] = "nova_cache",
    [dbid_license] = "nova_track",
    [dbid_value] = "nova_value",
    [dbid_agent_execution] = "nova_agent_execution",
    [dbid_bundles] = "bundles",
    [dbid_packages_installed] = "packages_installed",
    [dbid_packages_updates] = "packages_updates",
    [dbid_cookies] = "nova_cookies",
};

/*
  These are the old (pre 3.7) paths in workdir, supported for installations that
  still have them. We will never create a database here. NULL means that the
  database was always in the state directory.
*/
static const char *const DB_PATHS_WORKDIR[sizeof(DB_PATHS_STATEDIR) / sizeof(const char * const)] = {
    [dbid_classes] = "cf_classes",
    [dbid_variables] = NULL,
    [dbid_performance] = "performance",
    [dbid_checksums] = "checksum_digests",
    [dbid_filestats] = "stats",
    [dbid_changes] = NULL,
    [dbid_observations] = NULL,
    [dbid_state] = NULL,
    [dbid_lastseen] = "cf_lastseen",
    [dbid_audit] = "cf_audit",
    [dbid_locks] = NULL,
    [dbid_history] = NULL,
    [dbid_measure] = NULL,
    [dbid_static] = NULL,
    [dbid_scalars] = NULL,
    [dbid_windows_registry] = "mswin",
    [dbid_cache] = "nova_cache",
    [dbid_license] = "nova_track",
    [dbid_value] = "nova_value",
    [dbid_agent_execution] = "nova_agent_execution",
    [dbid_bundles] = "bundles",
};

/******************************************************************************/

char *DBIdToSubPath(dbid id, const char *subdb_name)
{
    char *filename;
    if (xasprintf(&filename, "%s/%s_%s.%s", GetStateDir(), DB_PATHS_STATEDIR[id],
            subdb_name, DBPrivGetFileExtension()) == -1)
    {
        ProgrammingError("Unable to construct sub database filename for file"
                "%s_%s", DB_PATHS_STATEDIR[id], subdb_name);
    }

    char *native_filename = MapNameCopy(filename);
    free(filename);

    return native_filename;
}

char *DBIdToPath(dbid id)
{
    assert(DB_PATHS_STATEDIR[id] != NULL);

    char *filename = NULL;

    if (DB_PATHS_WORKDIR[id])
    {
        xasprintf(&filename, "%s/%s.%s", GetWorkDir(), DB_PATHS_WORKDIR[id],
                  DBPrivGetFileExtension());
        struct stat statbuf;
        if (stat(filename, &statbuf) == -1)
        {
            // Old database in workdir is not there. Use new database in statedir.
            free(filename);
            filename = NULL;
        }
    }

    if (!filename)
    {
        xasprintf(&filename, "%s/%s.%s", GetStateDir(), DB_PATHS_STATEDIR[id],
                  DBPrivGetFileExtension());
    }

    char *native_filename = MapNameCopy(filename);
    free(filename);

    return native_filename;
}

static
bool IsSubHandle(DBHandle *handle, dbid id, const char *name)
{
    char *sub_path = DBIdToSubPath(id, name);
    bool result = StringEqual(handle->filename, sub_path);
    free(sub_path);
    return result;
}

static DBHandle *DBHandleGetSubDB(dbid id, const char *name)
{
    ThreadLock(&db_handles_lock);

    DynamicDBHandles *handles_list = db_dynamic_handles;

    while (handles_list)
    {
        if (IsSubHandle(handles_list->handle, id, name))
        {
            ThreadUnlock(&db_handles_lock);
            return handles_list->handle;
        }
        handles_list = handles_list->next;
    }

    DBHandle *handle = xcalloc(1, sizeof(DBHandle));
    handle->filename = DBIdToSubPath(id, name);
    handle->subname = SafeStringDuplicate(name);

    /* Initialize mutexes as error-checking ones. */
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
    pthread_mutex_init(&handle->lock, &attr);
    pthread_mutexattr_destroy(&attr);

    /* Prepend handle to global list. */
    handles_list = xcalloc(1, sizeof(DynamicDBHandles));
    handles_list->handle = handle;
    handles_list->next = db_dynamic_handles;
    db_dynamic_handles = handles_list;

    ThreadUnlock(&db_handles_lock);

    return handle;
}

static DBHandle *DBHandleGet(int id)
{
    assert(id >= 0 && id < dbid_max);

    ThreadLock(&db_handles_lock);
    if (db_handles[id].filename == NULL)
    {
        db_handles[id].filename = DBIdToPath(id);

        /* Initialize mutexes as error-checking ones. */
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
        pthread_mutex_init(&db_handles[id].lock, &attr);
        pthread_mutexattr_destroy(&attr);
    }

    ThreadUnlock(&db_handles_lock);

    return &db_handles[id];
}

static inline
void CloseDBInstance(DBHandle *handle)
{
    /* Wait until all DB users are served, or a threshold is reached */
    int count = 0;
    ThreadLock(&handle->lock);
    if (handle->frozen)
    {
        /* Just clean some allocated memory, but don't touch the DB itself. */
        free(handle->filename);
        free(handle->subname);
        ThreadUnlock(&handle->lock);
        return;
    }
    while (handle->refcount > 0 && count < 1000)
    {
        ThreadUnlock(&handle->lock);

        struct timespec sleeptime = {
            .tv_sec = 0,
            .tv_nsec = 10000000 /* 10 ms */
        };
        nanosleep(&sleeptime, NULL);
        count++;

        ThreadLock(&handle->lock);
    }
    /* Keep mutex locked. */

    /* If we exited because of timeout make sure we Log() it. */
    if (handle->refcount != 0)
    {
        Log(LOG_LEVEL_ERR,
                "Database %s refcount is still not zero (%d), forcing CloseDB()!",
                handle->filename, handle->refcount);
        DBPrivCloseDB(handle->priv);
    }
    else /* TODO: can we clean this up unconditionally ? */
    {
        free(handle->filename);
        free(handle->subname);
        handle->filename = NULL;
    }
}


/**
 * @brief Wait for all users of all databases to close the DBs. Then acquire
 * the mutexes *AND KEEP THEM LOCKED* so that no background thread can open
 * any database. So make sure you exit soon...
 *
 * @warning This is usually register with atexit(), however you have to make
 * sure no other DB-cleaning exit hook was registered before, so that this is
 * called last.
 **/
void CloseAllDBExit()
{
    ThreadLock(&db_handles_lock);

    for (int i = 0; i < dbid_max; i++)
    {
        if (db_handles[i].filename)
        {
            CloseDBInstance(&db_handles[i]);
        }
    }

    DynamicDBHandles *db_dynamic_handles_list = db_dynamic_handles;
    while (db_dynamic_handles_list)
    {
        DBHandle *handle = db_dynamic_handles_list->handle;
        CloseDBInstance(handle);

        DynamicDBHandles *next_free = db_dynamic_handles_list;
        db_dynamic_handles_list = db_dynamic_handles_list->next;

        FREE_AND_NULL(handle);
        FREE_AND_NULL(next_free);
    }
}

static void RegisterShutdownHandler(void)
{
    RegisterCleanupFunction(&CloseAllDBExit);
}

/**
 * Keeps track of the maximum number of concurrent transactions, which is
 * expected to be set by agents as they start up. If it is not set it will use
 * the existing value. If it is set, but the database cannot honor it, CFEngine
 * will warn.
 * @param max_txn Maximum number of concurrent transactions for a single
 *                database.
 */
void DBSetMaximumConcurrentTransactions(int max_txn)
{
    DBPrivSetMaximumConcurrentTransactions(max_txn);
}

static inline
bool OpenDBInstance(DBHandle **dbp, dbid id, DBHandle *handle)
{
    assert(handle != NULL);

    ThreadLock(&handle->lock);
    if (handle->frozen)
    {
        Log(LOG_LEVEL_WARNING, "Attempt to open a frozen DB '%s'", handle->filename);
        ThreadUnlock(&handle->lock);
        return false;
    }
    if (handle->refcount == 0)
    {
        FileLock lock = EMPTY_FILE_LOCK;
        if (DBPathLock(&lock, handle->filename))
        {
            handle->open_tstamp = time(NULL);
            handle->priv = DBPrivOpenDB(handle->filename, id);

            if (handle->priv == DB_PRIV_DATABASE_BROKEN)
            {
                DBPathMoveBroken(handle->filename);
                handle->priv = DBPrivOpenDB(handle->filename, id);
                if (handle->priv == DB_PRIV_DATABASE_BROKEN)
                {
                    handle->priv = NULL;
                }
            }

            DBPathUnLock(&lock);
        }

        if (handle->priv)
        {
            if (!DBMigrate(handle, id))
            {
                DBPrivCloseDB(handle->priv);
                handle->priv = NULL;
                handle->open_tstamp = -1;
            }
        }
    }

    if (handle->priv)
    {
        handle->refcount++;
        *dbp = handle;

        /* Only register shutdown handler if any database was opened
         * correctly. Otherwise this shutdown caller may be called too early,
         * and shutdown handler installed by the database library may end up
         * being called before CloseAllDB function */

        pthread_once(&db_shutdown_once, RegisterShutdownHandler);
    }
    else
    {
        *dbp = NULL;
    }

    ThreadUnlock(&handle->lock);
    return *dbp != NULL;
}

bool OpenSubDB(DBHandle **dbp, dbid id, const char *sub_name)
{
    DBHandle *handle = DBHandleGetSubDB(id, sub_name);
    return OpenDBInstance(dbp, id, handle);
}

bool OpenDB(DBHandle **dbp, dbid id)
{
    DBHandle *handle = DBHandleGet(id);
    return OpenDBInstance(dbp, id, handle);
}

/**
 * @db_file_name Absolute path of the DB file
 */
DBHandle *GetDBHandleFromFilename(const char *db_file_name)
{
    ThreadLock(&db_handles_lock);
    for(dbid id=0; id < dbid_max; id++)
    {
        if (StringEqual(db_handles[id].filename, db_file_name))
        {
            ThreadUnlock(&db_handles_lock);
            return &(db_handles[id]);
        }
    }
    ThreadUnlock(&db_handles_lock);
    return NULL;
}


time_t GetDBOpenTimestamp(const DBHandle *handle)
{
    assert(handle != NULL);
    return handle->open_tstamp;
}

void CloseDB(DBHandle *handle)
{
    assert(handle != NULL);

    /* Skip in case of nested locking, for example signal handler.
     * DB behaviour becomes erratic otherwise (CFE-1996). */
    ThreadLock(&handle->lock);
    if (handle->frozen)
    {
        /* Just clean some allocated memory, but don't touch the DB itself. */
        free(handle->filename);
        free(handle->subname);
        ThreadUnlock(&handle->lock);
        return;
    }
    DBPrivCommit(handle->priv);

    if (handle->refcount < 1)
    {
        Log(LOG_LEVEL_ERR,
                "Trying to close database which is not open: %s",
                handle->filename);
    }
    else
    {
        handle->refcount--;
        if (handle->refcount == 0)
        {
            DBPrivCloseDB(handle->priv);
            handle->open_tstamp = -1;
        }
    }

    ThreadUnlock(&handle->lock);
}

bool CleanDB(DBHandle *handle)
{
    ThreadLock(&handle->lock);
    if (handle->frozen)
    {
        Log(LOG_LEVEL_WARNING, "Attempt to clean a frozen DB '%s'", handle->filename);
        ThreadUnlock(&handle->lock);
        return false;
    }
    bool ret = DBPrivClean(handle->priv);
    ThreadUnlock(&handle->lock);

    return ret;
}

/**
 * Freezes the DB so that it is never touched by this process again. In
 * particular, new OpenDB() calls are ignored and CloseAllDBExit() also ignores
 * the DB.
 */
void FreezeDB(DBHandle *handle)
{
    /* This is intentionally NOT using the handle->lock to avoid deadlocks.
     * Nothing ever sets this to 'false' explicitly, that's only done in the
     * initialization with '{ { 0 } }', so this bit-flip is safe. */
    Log(LOG_LEVEL_NOTICE, "Freezing the DB '%s'", handle->filename);
    handle->frozen = true;
}

/*****************************************************************************/

bool ReadComplexKeyDB(DBHandle *handle, const char *key, int key_size,
                      void *dest, int dest_size)
{
    return DBPrivRead(handle->priv, key, key_size, dest, dest_size);
}

bool WriteComplexKeyDB(DBHandle *handle, const char *key, int key_size,
                       const void *value, int value_size)
{
    return DBPrivWrite(handle->priv, key, key_size, value, value_size);
}

bool DeleteComplexKeyDB(DBHandle *handle, const char *key, int key_size)
{
    return DBPrivDelete(handle->priv, key, key_size);
}

bool ReadDB(DBHandle *handle, const char *key, void *dest, int destSz)
{
    return DBPrivRead(handle->priv, key, strlen(key) + 1, dest, destSz);
}

bool WriteDB(DBHandle *handle, const char *key, const void *src, int srcSz)
{
    return DBPrivWrite(handle->priv, key, strlen(key) + 1, src, srcSz);
}

bool OverwriteDB(DBHandle *handle, const char *key, const void *value, size_t value_size,
                 OverwriteCondition Condition, void *data)
{
    assert(handle != NULL);
    return DBPrivOverwrite(handle->priv, key, strlen(key) + 1, value, value_size, Condition, data);
}

bool HasKeyDB(DBHandle *handle, const char *key, int key_size)
{
    return DBPrivHasKey(handle->priv, key, key_size);
}

int ValueSizeDB(DBHandle *handle, const char *key, int key_size)
{
    return DBPrivGetValueSize(handle->priv, key, key_size);
}

bool DeleteDB(DBHandle *handle, const char *key)
{
    return DBPrivDelete(handle->priv, key, strlen(key) + 1);
}

bool NewDBCursor(DBHandle *handle, DBCursor **cursor)
{
    DBCursorPriv *priv = DBPrivOpenCursor(handle->priv);
    if (!priv)
    {
        return false;
    }

    *cursor = xcalloc(1, sizeof(DBCursor));
    (*cursor)->cursor = priv;
    return true;
}

bool NextDB(DBCursor *cursor, char **key, int *ksize,
            void **value, int *vsize)
{
    return DBPrivAdvanceCursor(cursor->cursor, (void **)key, ksize, value, vsize);
}

bool DBCursorDeleteEntry(DBCursor *cursor)
{
    return DBPrivDeleteCursorEntry(cursor->cursor);
}

bool DBCursorWriteEntry(DBCursor *cursor, const void *value, int value_size)
{
    return DBPrivWriteCursorEntry(cursor->cursor, value, value_size);
}

bool DeleteDBCursor(DBCursor *cursor)
{
    DBPrivCloseCursor(cursor->cursor);
    free(cursor);
    return true;
}

static bool DBPathLock(FileLock *lock, const char *filename)
{
    char *filename_lock;
    if (xasprintf(&filename_lock, "%s.lock", filename) == -1)
    {
        ProgrammingError("Unable to construct lock database filename for file %s", filename);
    }

    if (ExclusiveFileLockPath(lock, filename_lock, true) != 0)
    {
        Log(LOG_LEVEL_ERR, "Unable to lock database lock file '%s'.", filename_lock);
        free(filename_lock);
        return false;
    }

    free(filename_lock);

    return true;
}

static void DBPathUnLock(FileLock *lock)
{
    ExclusiveFileUnlock(lock, true);
}

static void DBPathMoveBroken(const char *filename)
{
    char *filename_broken;
    if (xasprintf(&filename_broken, "%s.broken", filename) == -1)
    {
        ProgrammingError("Unable to construct broken database filename for file '%s'", filename);
    }

    if(rename(filename, filename_broken) != 0)
    {
        Log(LOG_LEVEL_ERR, "Failed moving broken db out of the way '%s'", filename);
    }

    free(filename_broken);
}

StringMap *LoadDatabaseToStringMap(dbid database_id)
{
    CF_DB *db_conn = NULL;
    CF_DBC *db_cursor = NULL;
    char *key = NULL;
    void *value = NULL;
    int key_size = 0;
    int value_size = 0;

    if (!OpenDB(&db_conn, database_id))
    {
        return NULL;
    }

    if (!NewDBCursor(db_conn, &db_cursor))
    {
        Log(LOG_LEVEL_ERR, "Unable to scan db");
        CloseDB(db_conn);
        return NULL;
    }

    StringMap *db_map = StringMapNew();
    while (NextDB(db_cursor, &key, &key_size, &value, &value_size))
    {
        if (!key)
        {
            continue;
        }

        if (!value)
        {
            Log(LOG_LEVEL_VERBOSE, "Invalid entry (key='%s') in database.", key);
            continue;
        }

        void *val = xcalloc(1, value_size);
        val = memcpy(val, value, value_size);

        StringMapInsert(db_map, xstrdup(key), val);
    }

    DeleteDBCursor(db_cursor);
    CloseDB(db_conn);

    return db_map;
}

/**
 * Checks if a DB repair flag file is present and if it is, removes it.
 *
 * @return Whether the DB repair flag file was present or not.
 */
bool CheckDBRepairFlagFile()
{
    /* The DB repair flag file can be created by user or by some process
     * that hit an error condition potentially caused by local DB corruption
     * that it was not able to handle properly by repairing the corrupted DB
     * file(s). For example, if a process is killed by a signal. */
    char repair_flag_file[PATH_MAX] = { 0 };
    bool present = false;
    xsnprintf(repair_flag_file, PATH_MAX, "%s%c%s",
              GetStateDir(), FILE_SEPARATOR, CF_DB_REPAIR_TRIGGER);
    /* This is full of race-conditions, but it's just a best-effort
     * thing. If a force-repair is missed, it will happen next time. If it's
     * done twice, no big deal. */
    if (access(repair_flag_file, F_OK) == 0)
    {
        present = true;
        unlink(repair_flag_file);
    }
    return present;
}
