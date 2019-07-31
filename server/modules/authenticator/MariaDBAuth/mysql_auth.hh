/*
 * Copyright (c) 2018 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2023-01-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */
#pragma once

#define MXS_MODULE_NAME "MariaDBAuth"

#include <maxscale/ccdefs.hh>

#include <stdint.h>
#include <arpa/inet.h>

#include <maxscale/authenticator.hh>
#include <maxscale/dcb.hh>
#include <maxscale/buffer.hh>
#include <maxscale/service.hh>
#include <maxscale/sqlite3.h>
#include <maxscale/protocol/mysql.hh>
#include <maxscale/authenticator2.hh>

/** Cache directory and file names */
static const char DBUSERS_DIR[] = "cache";
static const char DBUSERS_FILE[] = "dbusers.db";

/** The table name where we store the users */
#define MYSQLAUTH_USERS_TABLE_NAME "mysqlauth_users"

/** The table name where we store the users */
#define MYSQLAUTH_DATABASES_TABLE_NAME "mysqlauth_databases"

/** CREATE TABLE statement for the in-memory users table */
static const char users_create_sql[] =
    "CREATE TABLE IF NOT EXISTS " MYSQLAUTH_USERS_TABLE_NAME
    "(user varchar(255), host varchar(255), db varchar(255), anydb boolean, password text)";

/** CREATE TABLE statement for the in-memory databases table */
static const char databases_create_sql[] =
    "CREATE TABLE IF NOT EXISTS " MYSQLAUTH_DATABASES_TABLE_NAME "(db varchar(255))";

/** PRAGMA configuration options for SQLite */
static const char pragma_sql[] = "PRAGMA JOURNAL_MODE=NONE";

/** Query that checks if there's a grant for the user being authenticated */
static const char mysqlauth_validate_user_query[] =
    "SELECT password FROM " MYSQLAUTH_USERS_TABLE_NAME
    " WHERE user = '%s' AND ( '%s' = host OR '%s' LIKE host)"
    " AND (anydb = '1' OR '%s' IN ('', 'information_schema') OR '%s' LIKE db)"
    " LIMIT 1";

/** Query that checks if there's a grant for the user being authenticated */
static const char mysqlauth_validate_user_query_lower[] =
    "SELECT password FROM " MYSQLAUTH_USERS_TABLE_NAME
    " WHERE user = '%s' AND ( '%s' = host OR '%s' LIKE host)"
    " AND (anydb = '1' OR LOWER('%s') IN ('', 'information_schema') OR LOWER('%s') LIKE LOWER(db)"
    " LIMIT 1";

/** Query that only checks if there's a matching user */
static const char mysqlauth_skip_auth_query[] =
    "SELECT password FROM " MYSQLAUTH_USERS_TABLE_NAME
    " WHERE user = '%s' AND (anydb = '1' OR '%s' IN ('', 'information_schema') OR '%s' LIKE db)"
    " LIMIT 1";

/** Query that checks that the database exists */
static const char mysqlauth_validate_database_query[] =
    "SELECT * FROM " MYSQLAUTH_DATABASES_TABLE_NAME " WHERE db = '%s' LIMIT 1";
static const char mysqlauth_validate_database_query_lower[] =
    "SELECT * FROM " MYSQLAUTH_DATABASES_TABLE_NAME " WHERE LOWER(db) = LOWER('%s') LIMIT 1";

/** Delete query used to clean up the database before loading new users */
static const char delete_users_query[] = "DELETE FROM " MYSQLAUTH_USERS_TABLE_NAME;

/** Delete query used to clean up the database before loading new users */
static const char delete_databases_query[] = "DELETE FROM " MYSQLAUTH_DATABASES_TABLE_NAME;

/** The insert query template which adds users to the mysqlauth_users table */
static const char insert_user_query[] =
    "INSERT OR REPLACE INTO " MYSQLAUTH_USERS_TABLE_NAME " VALUES ('%s', '%s', %s, %s, %s)";

/** The insert query template which adds the databases to the table */
static const char insert_database_query[] =
    "INSERT OR REPLACE INTO " MYSQLAUTH_DATABASES_TABLE_NAME " VALUES ('%s')";

static const char dump_users_query[] =
    "SELECT user, host, db, anydb, password FROM " MYSQLAUTH_USERS_TABLE_NAME;

static const char dump_databases_query[] =
    "SELECT db FROM " MYSQLAUTH_DATABASES_TABLE_NAME;

/** Used for NULL value creation in the INSERT query */
static const char null_token[] = "NULL";

/** Flags for sqlite3_open_v2() */
static int db_flags = SQLITE_OPEN_READWRITE
    | SQLITE_OPEN_CREATE
    | SQLITE_OPEN_NOMUTEX;

class MariaDBAuthenticatorSession : public mxs::AuthenticatorSession
{
public:
    ~MariaDBAuthenticatorSession() override = default;
    bool extract(DCB* client, GWBUF* buffer) override;
    bool ssl_capable(DCB* client) override;
    int authenticate(DCB* client) override;
    void free_data(DCB* client) override;
    int reauthenticate(DCB* client, const char* user, uint8_t* token, size_t token_len,
                               uint8_t* scramble, size_t scramble_len,
                               uint8_t* output, size_t output_len) override;

    // No fields, as authentication data is managed by the protocol.
};

class MYSQL_AUTH : public mxs::Authenticator
{
public:
    static MYSQL_AUTH* create(char** options);

    ~MYSQL_AUTH() override = default;
    MariaDBAuthenticatorSession* createSession() override;
    int load_users(Listener* listener) override;
    void diagnostics(DCB* output) override;
    json_t* diagnostics_json() override;

    sqlite3** handles;              /**< SQLite3 database handle */
    char*     cache_dir;            /**< Custom cache directory location */
    bool      inject_service_user;  /**< Inject the service user into the list of users */
    bool      skip_auth;            /**< Authentication will always be successful */
    bool      check_permissions;
    bool      lower_case_table_names;   /**< Disable database case-sensitivity */
};



/**
 * MySQL user and host data structure
 */
typedef struct mysql_user_host_key
{
    char*              user;
    struct sockaddr_in ipv4;
    int                netmask;
    char*              resource;
    char               hostname[MYSQL_HOST_MAXLEN + 1];
} MYSQL_USER_HOST;

/**
 * @brief Get the thread-specific SQLite handle
 *
 * @param instance Authenticator instance
 *
 * @return The thread-specific handle
 */
sqlite3* get_handle(MYSQL_AUTH* instance);

/**
 * @brief Add new MySQL user to the internal user database
 *
 * @param handle Database handle
 * @param user   Username
 * @param host   Host
 * @param db     Database
 * @param anydb  Global access to databases
 */
void add_mysql_user(sqlite3* handle,
                    const char* user,
                    const char* host,
                    const char* db,
                    bool anydb,
                    const char* pw);

/**
 * @brief Check if the service user has all required permissions to operate properly.
 *
 * This checks for SELECT permissions on mysql.user, mysql.db and mysql.tables_priv
 * tables and for SHOW DATABASES permissions. If permissions are not adequate,
 * an error message is logged and the service is not started.
 *
 * @param service Service to inspect
 *
 * @return True if service permissions are correct on at least one server, false
 * if permissions are missing or if an error occurred.
 */
bool check_service_permissions(SERVICE* service);

/**
 * Load users from persisted database
 *
 * @param dest Open SQLite handle where contents are loaded
 *
 * @return True on success
 */
bool dbusers_load(sqlite3* handle, const char* filename);

/**
 * Save users to persisted database
 *
 * @param dest Open SQLite handle where contents are stored
 *
 * @return True on success
 */
bool dbusers_save(sqlite3* src, const char* filename);

/**
 * Reload and replace the currently loaded database users
 *
 * @param service    The current service
 * @param skip_local Skip loading of users on local MaxScale services
 * @param srv        Server where the users were loaded from (output)
 *
 * @return -1 on any error or the number of users inserted (0 means no users at all)
 */
int replace_mysql_users(Listener* listener, bool skip_local, SERVER** srv);

/**
 * @brief Verify the user has access to the database
 *
 * @param instance     MySQLAuth instance
 * @param dcb          Client DCB
 * @param session      Shared MySQL session
 * @param scramble     The scramble sent to the client in the initial handshake
 * @param scramble_len Length of @c scramble
 *
 * @return MXS_AUTH_SUCCEEDED if the user has access to the database
 */
int validate_mysql_user(MYSQL_AUTH* instance,
                        DCB* dcb,
                        MYSQL_session* session,
                        uint8_t* scramble,
                        size_t scramble_len);

