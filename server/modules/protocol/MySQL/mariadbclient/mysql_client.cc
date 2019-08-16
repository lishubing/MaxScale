/*
 *
 * Copyright (c) 2016 MariaDB Corporation Ab
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

#define MXS_MODULE_NAME "mariadbclient"

#include <maxscale/ccdefs.hh>

#include <inttypes.h>
#include <limits.h>
#include <netinet/tcp.h>
#include <sys/stat.h>
#include <algorithm>
#include <string>
#include <vector>
#include <sstream>

#include <maxbase/alloc.h>
#include <maxscale/authenticator2.hh>
#include <maxscale/modinfo.hh>
#include <maxscale/modutil.hh>
#include <maxscale/poll.hh>
#include <maxscale/protocol.hh>
#include <maxscale/protocol/mysql.hh>
#include <maxscale/query_classifier.hh>
#include <maxscale/router.hh>
#include <maxscale/routingworker.hh>
#include <maxscale/session.hh>
#include <maxscale/ssl.hh>
#include <maxscale/utils.h>

#include "setparser.hh"
#include "sqlmodeparser.hh"

namespace
{

/** Return type of process_special_commands() */
typedef enum spec_com_res_t
{
    RES_CONTINUE,   // No special command detected, proceed as normal.
    RES_END,        // Query handling completed, do not send to filters/router.
} spec_com_res_t;

const char WORD_KILL[] = "KILL";

std::string get_version_string(SERVICE* service)
{
    std::string rval = DEFAULT_VERSION_STRING;

    if (!service->config().version_string.empty())
    {
        // User-defined version string, use it
        rval = service->config().version_string;
    }
    else
    {
        uint64_t smallest_found = UINT64_MAX;
        for (auto server : service->reachable_servers())
        {
            auto version = server->version();
            if (version.total > 0 && version.total < smallest_found)
            {
                rval = server->version_string();
                smallest_found = version.total;
            }
        }
    }

    // Older applications don't understand versions other than 5 and cause strange problems
    if (rval[0] != '5')
    {
        const char prefix[] = "5.5.5-";
        rval = prefix + rval;
    }

    return rval;
}

uint8_t get_charset(SERVICE* service)
{
    uint8_t rval = 0;

    for (SERVER* s : service->reachable_servers())
    {
        if (s->is_master())
        {
            // Master found, stop searching
            rval = s->charset;
            break;
        }
        else if (s->is_slave() || (s->is_running() && rval == 0))
        {
            // Slaves precede Running servers
            rval = s->charset;
        }
    }

    if (rval == 0)
    {
        // Charset 8 is latin1, the server default
        rval = 8;
    }

    return rval;
}

bool supports_extended_caps(SERVICE* service)
{
    bool rval = false;

    for (SERVER* s : service->reachable_servers())
    {
        if (s->version().total >= 100200)
        {
            rval = true;
            break;
        }
    }

    return rval;
}

/**
 * @brief Check whether a DCB requires SSL.
 *
 * This is a very simple test, but is placed in an SSL function so that
 * the knowledge of the SSL process is removed from the more general
 * handling of a connection in the protocols.
 *
 * @param dcb Request handler DCB connected to the client
 * @return Boolean indicating whether SSL is required.
 */
bool ssl_required_by_dcb(DCB* dcb)
{
    mxb_assert(dcb->session()->listener);
    return dcb->session()->listener->ssl().context();
}

/**
 * @brief Check whether a DCB requires SSL, but SSL is not yet negotiated.
 *
 * This is a very simple test, but is placed in an SSL function so that
 * the knowledge of the SSL process is removed from the more general
 * handling of a connection in the protocols.
 *
 * @param dcb Request handler DCB connected to the client
 * @return Boolean indicating whether SSL is required and not negotiated.
 */
bool ssl_required_but_not_negotiated(DCB* dcb)
{
    return ssl_required_by_dcb(dcb) && DCB::SSLState::HANDSHAKE_UNKNOWN == dcb->ssl_state();
}

/**
 * send_mysql_client_handshake
 *
 * @param dcb The descriptor control block to use for sending the handshake request
 * @return      The packet length sent
 */
int send_mysql_client_handshake(DCB* dcb, MySQLProtocol* protocol)
{
    uint8_t* outbuf = NULL;
    uint32_t mysql_payload_size = 0;
    uint8_t mysql_packet_header[4];
    uint8_t mysql_packet_id = 0;
    /* uint8_t mysql_filler = GW_MYSQL_HANDSHAKE_FILLER; not needed*/
    uint8_t mysql_protocol_version = GW_MYSQL_PROTOCOL_VERSION;
    uint8_t* mysql_handshake_payload = NULL;
    uint8_t mysql_thread_id_num[4];
    uint8_t mysql_scramble_buf[9] = "";
    uint8_t mysql_plugin_data[13] = "";
    uint8_t mysql_server_capabilities_one[2];
    uint8_t mysql_server_capabilities_two[2];
    uint8_t mysql_server_language = get_charset(dcb->service());
    uint8_t mysql_server_status[2];
    uint8_t mysql_scramble_len = 21;
    uint8_t mysql_filler_ten[10] = {};
    /* uint8_t mysql_last_byte = 0x00; not needed */
    char server_scramble[GW_MYSQL_SCRAMBLE_SIZE + 1] = "";
    bool is_maria = supports_extended_caps(dcb->service());

    GWBUF* buf;
    std::string version = get_version_string(dcb->service());

    gw_generate_random_str(server_scramble, GW_MYSQL_SCRAMBLE_SIZE);

    // copy back to the caller
    memcpy(protocol->scramble, server_scramble, GW_MYSQL_SCRAMBLE_SIZE);

    if (is_maria)
    {
        /**
         * The new 10.2 capability flags are stored in the last 4 bytes of the
         * 10 byte filler block.
         */
        uint32_t new_flags = MXS_MARIA_CAP_STMT_BULK_OPERATIONS;
        memcpy(mysql_filler_ten + 6, &new_flags, sizeof(new_flags));
    }

    // Get the equivalent of the server thread id.
    protocol->thread_id = dcb->session()->id();
    // Send only the low 32bits in the handshake.
    gw_mysql_set_byte4(mysql_thread_id_num, (uint32_t)(protocol->thread_id));
    memcpy(mysql_scramble_buf, server_scramble, 8);

    memcpy(mysql_plugin_data, server_scramble + 8, 12);

    /**
     * Use the default authentication plugin name in the initial handshake. If the
     * authenticator needs to change the authentication method, it should send
     * an AuthSwitchRequest packet to the client.
     */
    const char* plugin_name = DEFAULT_MYSQL_AUTH_PLUGIN;
    int plugin_name_len = strlen(plugin_name);

    mysql_payload_size =
        sizeof(mysql_protocol_version) + (version.length() + 1) + sizeof(mysql_thread_id_num) + 8
        + sizeof(    /* mysql_filler */ uint8_t) + sizeof(mysql_server_capabilities_one)
        + sizeof(mysql_server_language)
        + sizeof(mysql_server_status) + sizeof(mysql_server_capabilities_two) + sizeof(mysql_scramble_len)
        + sizeof(mysql_filler_ten) + 12 + sizeof(    /* mysql_last_byte */ uint8_t) + plugin_name_len
        + sizeof(    /* mysql_last_byte */ uint8_t);

    // allocate memory for packet header + payload
    if ((buf = gwbuf_alloc(sizeof(mysql_packet_header) + mysql_payload_size)) == NULL)
    {
        mxb_assert(buf != NULL);
        return 0;
    }
    outbuf = GWBUF_DATA(buf);

    // write packet header with mysql_payload_size
    gw_mysql_set_byte3(mysql_packet_header, mysql_payload_size);

    // write packet number, now is 0
    mysql_packet_header[3] = mysql_packet_id;
    memcpy(outbuf, mysql_packet_header, sizeof(mysql_packet_header));

    // current buffer pointer
    mysql_handshake_payload = outbuf + sizeof(mysql_packet_header);

    // write protocol version
    memcpy(mysql_handshake_payload, &mysql_protocol_version, sizeof(mysql_protocol_version));
    mysql_handshake_payload = mysql_handshake_payload + sizeof(mysql_protocol_version);

    // write server version plus 0 filler
    strcpy((char*)mysql_handshake_payload, version.c_str());
    mysql_handshake_payload = mysql_handshake_payload + version.length();

    *mysql_handshake_payload = 0x00;

    mysql_handshake_payload++;

    // write thread id
    memcpy(mysql_handshake_payload, mysql_thread_id_num, sizeof(mysql_thread_id_num));
    mysql_handshake_payload = mysql_handshake_payload + sizeof(mysql_thread_id_num);

    // write scramble buf
    memcpy(mysql_handshake_payload, mysql_scramble_buf, 8);
    mysql_handshake_payload = mysql_handshake_payload + 8;
    *mysql_handshake_payload = GW_MYSQL_HANDSHAKE_FILLER;
    mysql_handshake_payload++;

    // write server capabilities part one
    mysql_server_capabilities_one[0] = (uint8_t)GW_MYSQL_CAPABILITIES_SERVER;
    mysql_server_capabilities_one[1] = (uint8_t)(GW_MYSQL_CAPABILITIES_SERVER >> 8);

    if (is_maria)
    {
        /** A MariaDB 10.2 server doesn't send the CLIENT_MYSQL capability
         * to signal that it supports extended capabilities */
        mysql_server_capabilities_one[0] &= ~(uint8_t)GW_MYSQL_CAPABILITIES_CLIENT_MYSQL;
    }

    if (ssl_required_by_dcb(dcb))
    {
        mysql_server_capabilities_one[1] |= (int)GW_MYSQL_CAPABILITIES_SSL >> 8;
    }

    memcpy(mysql_handshake_payload, mysql_server_capabilities_one, sizeof(mysql_server_capabilities_one));
    mysql_handshake_payload = mysql_handshake_payload + sizeof(mysql_server_capabilities_one);

    // write server language
    memcpy(mysql_handshake_payload, &mysql_server_language, sizeof(mysql_server_language));
    mysql_handshake_payload = mysql_handshake_payload + sizeof(mysql_server_language);

    // write server status
    mysql_server_status[0] = 2;
    mysql_server_status[1] = 0;
    memcpy(mysql_handshake_payload, mysql_server_status, sizeof(mysql_server_status));
    mysql_handshake_payload = mysql_handshake_payload + sizeof(mysql_server_status);

    // write server capabilities part two
    mysql_server_capabilities_two[0] = (uint8_t)(GW_MYSQL_CAPABILITIES_SERVER >> 16);
    mysql_server_capabilities_two[1] = (uint8_t)(GW_MYSQL_CAPABILITIES_SERVER >> 24);

    // Check that we match the old values
    mxb_assert(mysql_server_capabilities_two[0] == 15);
    /** NOTE: pre-2.1 versions sent the fourth byte of the capabilities as
     *  the value 128 even though there's no such capability. */

    memcpy(mysql_handshake_payload, mysql_server_capabilities_two, sizeof(mysql_server_capabilities_two));
    mysql_handshake_payload = mysql_handshake_payload + sizeof(mysql_server_capabilities_two);

    // write scramble_len
    memcpy(mysql_handshake_payload, &mysql_scramble_len, sizeof(mysql_scramble_len));
    mysql_handshake_payload = mysql_handshake_payload + sizeof(mysql_scramble_len);

    // write 10 filler
    memcpy(mysql_handshake_payload, mysql_filler_ten, sizeof(mysql_filler_ten));
    mysql_handshake_payload = mysql_handshake_payload + sizeof(mysql_filler_ten);

    // write plugin data
    memcpy(mysql_handshake_payload, mysql_plugin_data, 12);
    mysql_handshake_payload = mysql_handshake_payload + 12;

    // write last byte, 0
    *mysql_handshake_payload = 0x00;
    mysql_handshake_payload++;

    // to be understanded ????
    memcpy(mysql_handshake_payload, plugin_name, plugin_name_len);
    mysql_handshake_payload = mysql_handshake_payload + plugin_name_len;

    // write last byte, 0
    *mysql_handshake_payload = 0x00;

    // writing data in the Client buffer queue
    dcb->protocol_write(buf);
    protocol->protocol_auth_state = MXS_AUTH_STATE_MESSAGE_READ;

    return sizeof(mysql_packet_header) + mysql_payload_size;
}

/**
 * Get length of a null-terminated string
 *
 * @param str String to measure
 * @param len Maximum length to read
 *
 * @return Length of @c str or -1 if the string is not null-terminated
 */
int get_zstr_len(const char* str, int len)
{
    const char* end = str + len;
    int slen = 0;

    while (str < end && *str)
    {
        str++;
        slen++;
    }

    if (str == end)
    {
        // The string is not null terminated
        slen = -1;
    }

    return slen;
}

/**
 * @brief Store client connection information into the DCB
 * @param dcb Client DCB
 * @param buffer Buffer containing the handshake response packet
 */
void store_client_information(DCB* dcb, GWBUF* buffer)
{
    size_t len = gwbuf_length(buffer);
    uint8_t data[len];
    MySQLProtocol* proto = (MySQLProtocol*)dcb->protocol_session();
    MYSQL_session* ses = (MYSQL_session*)dcb->m_data;

    gwbuf_copy_data(buffer, 0, len, data);
    mxb_assert(MYSQL_GET_PAYLOAD_LEN(data) + MYSQL_HEADER_LEN == len
               || len == MYSQL_AUTH_PACKET_BASE_SIZE);      // For SSL request packet

    // We OR the capability bits in order to retain the starting bits sent
    // when an SSL connection is opened. Oracle Connector/J 8.0 appears to drop
    // the SSL capability bit mid-authentication which causes MaxScale to think
    // that SSL is not used.
    proto->client_capabilities |= gw_mysql_get_byte4(data + MYSQL_CLIENT_CAP_OFFSET);
    proto->charset = data[MYSQL_CHARSET_OFFSET];

    /** MariaDB 10.2 compatible clients don't set the first bit to signal that
     * there are extra capabilities stored in the last 4 bytes of the 23 byte filler. */
    if ((proto->client_capabilities & GW_MYSQL_CAPABILITIES_CLIENT_MYSQL) == 0)
    {
        proto->extra_capabilities = gw_mysql_get_byte4(data + MARIADB_CAP_OFFSET);
    }

    if (len > MYSQL_AUTH_PACKET_BASE_SIZE)
    {
        const char* username = (const char*)data + MYSQL_AUTH_PACKET_BASE_SIZE;
        int userlen = get_zstr_len(username, len - MYSQL_AUTH_PACKET_BASE_SIZE);

        if (userlen != -1)
        {
            if ((int)sizeof(ses->user) > userlen)
            {
                strcpy(ses->user, username);
            }

            // Include the null terminator in the user length
            userlen++;

            if (proto->client_capabilities & GW_MYSQL_CAPABILITIES_CONNECT_WITH_DB)
            {
                /** Client is connecting with a default database */
                uint8_t authlen = data[MYSQL_AUTH_PACKET_BASE_SIZE + userlen];
                size_t dboffset = MYSQL_AUTH_PACKET_BASE_SIZE + userlen + authlen + 1;

                if (dboffset < len)
                {
                    int dblen = get_zstr_len((const char*)data + dboffset, len - dboffset);

                    if (dblen != -1 && (int)sizeof(ses->db) > dblen)
                    {
                        strcpy(ses->db, (const char*)data + dboffset);
                    }
                }
            }
        }
    }
}

/**
 * @brief Debug check function for authentication packets
 *
 * Check that the packet is consistent with how the protocol works and that no
 * unexpected data is processed.
 *
 * @param dcb Client DCB
 * @param buf Buffer containing packet
 * @param bytes Number of bytes available
 */
void check_packet(DCB* dcb, GWBUF* buf, int bytes)
{
    uint8_t hdr[MYSQL_HEADER_LEN];
    mxb_assert(gwbuf_copy_data(buf, 0, MYSQL_HEADER_LEN, hdr) == MYSQL_HEADER_LEN);

    int buflen = gwbuf_length(buf);
    int pktlen = MYSQL_GET_PAYLOAD_LEN(hdr) + MYSQL_HEADER_LEN;

    if (bytes == MYSQL_AUTH_PACKET_BASE_SIZE)
    {
        /** This is an SSL request packet */
        mxb_assert(dcb->session()->listener->ssl().context());
        mxb_assert(buflen == bytes && pktlen >= buflen);
    }
    else
    {
        /** Normal packet */
        mxb_assert(buflen == pktlen);
    }
}

/**
 * @brief If an SSL connection is required, check that it has been established.
 *
 * This is called at the end of the authentication of a new connection.
 * If the result is not true, the data packet is abandoned with further
 * data expected from the client.
 *
 * @param dcb Request handler DCB connected to the client
 * @return Boolean to indicate whether connection is healthy
 */
bool ssl_is_connection_healthy(DCB* dcb)
{
    /**
     * If SSL was never expected, or if the connection has state SSL_ESTABLISHED
     * then everything is as we wish. Otherwise, either there is a problem or
     * more to be done.
     */
    return !dcb->session()->listener->ssl().context() || dcb->ssl_state() == DCB::SSLState::ESTABLISHED;
}

/* Looks to be redundant - can remove include for ioctl too */
bool ssl_check_data_to_process(DCB* dcb)
{
    /** SSL authentication is still going on, we need to call DCB::ssl_handshake
     * until it return 1 for success or -1 for error */
    if (dcb->ssl_state() == DCB::SSLState::HANDSHAKE_REQUIRED && 1 == dcb->ssl_handshake())
    {
        int b = 0;
        ioctl(dcb->fd(), FIONREAD, &b);
        if (b != 0)
        {
            return true;
        }
        else
        {
            MXS_DEBUG("[mariadbclient_read] No data in socket after SSL auth");
        }
    }
    return false;
}

/**
 * @brief Check client's SSL capability and start SSL if appropriate.
 *
 * The protocol should determine whether the client is SSL capable and pass
 * the result as the second parameter. If the listener requires SSL but the
 * client is not SSL capable, an error message is recorded and failure return
 * given. If both sides want SSL, and SSL is not already established, the
 * process is triggered by calling DCB::ssl_handshake.
 *
 * @param dcb Request handler DCB connected to the client
 * @param is_capable Indicates if the client can handle SSL
 * @return 0 if ok, >0 if a problem - see return codes defined in ssl.h
 */
int ssl_authenticate_client(DCB* dcb, bool is_capable)
{
    const char* user = dcb->m_user ? dcb->m_user : "";
    const char* remote = dcb->m_remote ? dcb->m_remote : "";
    const char* service = (dcb->service() && dcb->service()->name()) ? dcb->service()->name() : "";

    if (!dcb->session()->listener->ssl().context())
    {
        /* Not an SSL connection on account of listener configuration */
        return SSL_AUTH_CHECKS_OK;
    }
    /* Now we require an SSL connection */
    if (!is_capable)
    {
        /* Should be SSL, but client is not SSL capable */
        MXS_INFO("User %s@%s connected to service '%s' without SSL when SSL was required.",
                 user,
                 remote,
                 service);
        return SSL_ERROR_CLIENT_NOT_SSL;
    }
    /* Now we know SSL is required and client is capable */
    if (dcb->ssl_state() != DCB::SSLState::HANDSHAKE_DONE && dcb->ssl_state() != DCB::SSLState::ESTABLISHED)
    {
        int return_code;
        /** Do the SSL Handshake */
        if (DCB::SSLState::HANDSHAKE_UNKNOWN == dcb->ssl_state())
        {
            dcb->set_ssl_state(DCB::SSLState::HANDSHAKE_REQUIRED);
        }
        /**
         * Note that this will often fail to achieve its result, because further
         * reading (or possibly writing) of SSL related information is needed.
         * When that happens, there is a call in poll.c so that an EPOLLIN
         * event that arrives while the SSL state is SSL_HANDSHAKE_REQUIRED
         * will trigger DCB::ssl_handshake. This situation does not result in a
         * negative return code - that indicates a real failure.
         */
        return_code = dcb->ssl_handshake();
        if (return_code < 0)
        {
            MXS_INFO("User %s@%s failed to connect to service '%s' with SSL.",
                     user,
                     remote,
                     service);
            return SSL_ERROR_ACCEPT_FAILED;
        }
        else if (mxs_log_is_priority_enabled(LOG_INFO))
        {
            if (1 == return_code)
            {
                MXS_INFO("User %s@%s connected to service '%s' with SSL.",
                         user,
                         remote,
                         service);
            }
            else
            {
                MXS_INFO("User %s@%s connect to service '%s' with SSL in progress.",
                         user,
                         remote,
                         service);
            }
        }
    }
    return SSL_AUTH_CHECKS_OK;
}

int ssl_authenticate_check_status(DCB* generic_dcb)
{
    int rval = MXS_AUTH_FAILED;
    /**
     * We record the SSL status before and after ssl authentication. This allows
     * us to detect if the SSL handshake is immediately completed, which means more
     * data needs to be read from the socket.
     */
    auto dcb = static_cast<ClientDCB*>(generic_dcb);
    bool health_before = ssl_is_connection_healthy(dcb);
    int ssl_ret = ssl_authenticate_client(dcb, dcb->m_auth_session->ssl_capable(dcb));
    bool health_after = ssl_is_connection_healthy(dcb);

    if (ssl_ret != 0)
    {
        rval = (ssl_ret == SSL_ERROR_CLIENT_NOT_SSL) ? MXS_AUTH_FAILED_SSL : MXS_AUTH_FAILED;
    }
    else if (!health_after)
    {
        rval = MXS_AUTH_SSL_INCOMPLETE;
    }
    else if (!health_before && health_after)
    {
        rval = MXS_AUTH_SSL_INCOMPLETE;
        poll_add_epollin_event_to_dcb(dcb, NULL);
    }
    else if (health_before && health_after)
    {
        rval = MXS_AUTH_SSL_COMPLETE;
    }
    return rval;
}

/**
 * @brief Analyse authentication errors and write appropriate log messages
 *
 * @param dcb Request handler DCB connected to the client
 * @param auth_val The type of authentication failure
 * @note Authentication status codes are defined in maxscale/protocol/mysql.h
 */
void handle_authentication_errors(DCB* dcb, int auth_val, int packet_number)
{
    int message_len;
    char* fail_str = NULL;
    MYSQL_session* session = (MYSQL_session*)dcb->m_data;

    switch (auth_val)
    {
    case MXS_AUTH_NO_SESSION:
        MXS_DEBUG("session creation failed. fd %d, state = MYSQL_AUTH_NO_SESSION.", dcb->fd());

        /** Send ERR 1045 to client */
        mysql_send_auth_error(dcb, packet_number, 0, "failed to create new session");
        break;

    case MXS_AUTH_FAILED_DB:
        MXS_DEBUG("database specified was not valid. fd %d, state = MYSQL_FAILED_AUTH_DB.", dcb->fd());
        /** Send error 1049 to client */
        message_len = 25 + MYSQL_DATABASE_MAXLEN;

        fail_str = (char*)MXS_CALLOC(1, message_len + 1);
        MXS_ABORT_IF_NULL(fail_str);
        snprintf(fail_str, message_len, "Unknown database '%s'", session->db);

        modutil_send_mysql_err_packet(dcb, packet_number, 0, 1049, "42000", fail_str);
        break;

    case MXS_AUTH_FAILED_SSL:
        MXS_DEBUG("client is "
                  "not SSL capable for SSL listener. fd %d, "
                  "state = MYSQL_FAILED_AUTH_SSL.",
                  dcb->fd());

        /** Send ERR 1045 to client */
        mysql_send_auth_error(dcb, packet_number, 0, "Access without SSL denied");
        break;

    case MXS_AUTH_SSL_INCOMPLETE:
        MXS_DEBUG("unable to complete SSL authentication. fd %d, "
                  "state = MYSQL_AUTH_SSL_INCOMPLETE.",
                  dcb->fd());

        /** Send ERR 1045 to client */
        mysql_send_auth_error(dcb,
                              packet_number,
                              0,
                              "failed to complete SSL authentication");
        break;

    case MXS_AUTH_FAILED:
        MXS_DEBUG("authentication failed. fd %d, state = MYSQL_FAILED_AUTH.", dcb->fd());
        /** Send error 1045 to client */
        fail_str = create_auth_fail_str(session->user,
                                        dcb->m_remote,
                                        session->auth_token_len > 0,
                                        session->db,
                                        auth_val);
        modutil_send_mysql_err_packet(dcb, packet_number, 0, 1045, "28000", fail_str);
        break;

    case MXS_AUTH_BAD_HANDSHAKE:
        modutil_send_mysql_err_packet(dcb, packet_number, 0, 1045, "08S01", "Bad handshake");
        break;

    default:
        MXS_DEBUG("authentication failed. fd %d, state unrecognized.", dcb->fd());
        /** Send error 1045 to client */
        fail_str = create_auth_fail_str(session->user,
                                        dcb->m_remote,
                                        session->auth_token_len > 0,
                                        session->db,
                                        auth_val);
        modutil_send_mysql_err_packet(dcb, packet_number, 0, 1045, "28000", fail_str);
    }
    MXS_FREE(fail_str);
}

/**
 * @brief Client read event, process when client not yet authenticated
 *
 * @param generic_dcb   Descriptor control block
 * @param read_buffer   A buffer containing the data read from client
 * @param nbytes_read   The number of bytes of data read
 * @return 0 if succeed, 1 otherwise
 */
int perform_authentication(DCB* generic_dcb, GWBUF* read_buffer, int nbytes_read)
{
    auto dcb = static_cast<ClientDCB*>(generic_dcb);
    MXB_AT_DEBUG(check_packet(dcb, read_buffer, nbytes_read));

    /** Allocate the shared session structure */
    if (dcb->m_data == NULL && (dcb->m_data = mysql_session_alloc()) == NULL)
    {
        dcb_close(dcb);
        return 1;
    }

    /** Read the client's packet sequence and increment that by one */
    uint8_t next_sequence;
    gwbuf_copy_data(read_buffer, MYSQL_SEQ_OFFSET, 1, &next_sequence);

    if (next_sequence == 1 || (ssl_required_by_dcb(dcb) && next_sequence == 2))
    {
        /** This is the first response from the client, read the connection
         * information and store them in the shared structure. For SSL connections,
         * this will be packet number two since the first packet will be the
         * Protocol::SSLRequest packet.
         *
         * @see
         * https://dev.mysql.com/doc/internals/en/connection-phase-packets.html#packet-Protocol::SSLRequest
         */
        store_client_information(dcb, read_buffer);
    }

    next_sequence++;
    ((MYSQL_session*)(dcb->m_data))->next_sequence = next_sequence;

    /**
     * The first step in the authentication process is to extract the
     * relevant information from the buffer supplied and place it
     * into a data structure pointed to by the DCB.  The "success"
     * result is not final, it implies only that the process is so
     * far successful, not that authentication has completed.  If the
     * data extraction succeeds, then a call is made to the actual
     * authenticate function to carry out the user checks.
     */
    int auth_val = MXS_AUTH_FAILED;
    if (dcb->m_auth_session->extract(dcb, read_buffer))
    {
        auth_val = ssl_authenticate_check_status(dcb);

        if (auth_val == MXS_AUTH_SSL_COMPLETE)
        {
            // TLS connection phase complete
            auth_val = dcb->m_auth_session->authenticate(dcb);
        }
    }
    else
    {
        auth_val = MXS_AUTH_BAD_HANDSHAKE;
    }

    MySQLProtocol* protocol = (MySQLProtocol*)dcb->protocol_session();

    /**
     * At this point, if the auth_val return code indicates success
     * the user authentication has been successfully completed.
     * But in order to have a working connection, a session has to
     * be created.  Provided that is also successful (indicated by a
     * non-null session) then the whole process has succeeded. In all
     * other cases an error return is made.
     */
    if (MXS_AUTH_SUCCEEDED == auth_val)
    {
        if (dcb->m_user == NULL)
        {
            /** User authentication complete, copy the username to the DCB */
            MYSQL_session* ses = (MYSQL_session*)dcb->m_data;
            if ((dcb->m_user = MXS_STRDUP(ses->user)) == NULL)
            {
                dcb_close(dcb);
                gwbuf_free(read_buffer);
                return 0;
            }
        }

        protocol->protocol_auth_state = MXS_AUTH_STATE_RESPONSE_SENT;
        /**
         * Start session, and a router session for it.
         * If successful, there will be backend connection(s)
         * after this point. The protocol authentication state
         * is changed so that future data will go through the
         * normal data handling function instead of this one.
         */
        if (session_start(dcb->session()))
        {
            mxb_assert(dcb->session()->state() != MXS_SESSION::State::CREATED);
            // For the time being only the sql_mode is stored in MXS_SESSION::client_protocol_data.
            dcb->session()->client_protocol_data = QC_SQL_MODE_DEFAULT;
            protocol->protocol_auth_state = MXS_AUTH_STATE_COMPLETE;
            mxs_mysql_send_ok(dcb, next_sequence, 0, NULL);

            if (dcb->m_readq)
            {
                // The user has already send more data, process it
                poll_fake_read_event(dcb);
            }
        }
        else
        {
            auth_val = MXS_AUTH_NO_SESSION;
        }
    }
    /**
     * If we did not get success throughout or authentication is not yet complete,
     * then the protocol state is updated, the client is notified of the failure
     * and the DCB is closed.
     */
    if (MXS_AUTH_SUCCEEDED != auth_val
        && MXS_AUTH_INCOMPLETE != auth_val
        && MXS_AUTH_SSL_INCOMPLETE != auth_val)
    {
        protocol->protocol_auth_state = MXS_AUTH_STATE_FAILED;
        handle_authentication_errors(dcb, auth_val, next_sequence);
        mxb_assert(dcb->session()->listener);

        // MXS_AUTH_NO_SESSION is for failure to start session, not authentication failure
        if (auth_val != MXS_AUTH_NO_SESSION)
        {
            dcb->session()->listener->mark_auth_as_failed(dcb->m_remote);
        }

        /**
         * Close DCB and which will release MYSQL_session
         */
        dcb_close(dcb);
    }
    /* One way or another, the buffer is now fully processed */
    gwbuf_free(read_buffer);
    return 0;
}

/**
 * Handle relevant variables
 *
 * @param session      The session for which the query classifier mode is adjusted.
 * @param read_buffer  Pointer to a buffer, assumed to contain a statement.
 *                     May be reallocated if not contiguous.
 *
 * @return NULL if successful, otherwise dynamically allocated error message.
 */
char* handle_variables(MXS_SESSION* session, GWBUF** read_buffer)
{
    char* message = NULL;

    SetParser set_parser;
    SetParser::Result result;

    switch (set_parser.check(read_buffer, &result))
    {
    case SetParser::ERROR:
        // In practice only OOM.
        break;

    case SetParser::IS_SET_SQL_MODE:
        {
            SqlModeParser sql_mode_parser;

            const SetParser::Result::Items& values = result.values();

            for (SetParser::Result::Items::const_iterator i = values.begin(); i != values.end(); ++i)
            {
                const SetParser::Result::Item& value = *i;

                switch (sql_mode_parser.get_sql_mode(value.first, value.second))
                {
                case SqlModeParser::ORACLE:
                    session_set_autocommit(session, false);
                    session->client_protocol_data = QC_SQL_MODE_ORACLE;
                    break;

                case SqlModeParser::DEFAULT:
                    session_set_autocommit(session, true);
                    session->client_protocol_data = QC_SQL_MODE_DEFAULT;
                    break;

                case SqlModeParser::SOMETHING:
                    break;

                default:
                    mxb_assert(!true);
                }
            }
        }
        break;

    case SetParser::IS_SET_MAXSCALE:
        {
            const SetParser::Result::Items& variables = result.variables();
            const SetParser::Result::Items& values = result.values();

            SetParser::Result::Items::const_iterator i = variables.begin();
            SetParser::Result::Items::const_iterator j = values.begin();

            while (!message && (i != variables.end()))
            {
                const SetParser::Result::Item& variable = *i;
                const SetParser::Result::Item& value = *j;

                message = session_set_variable_value(session,
                                                     variable.first,
                                                     variable.second,
                                                     value.first,
                                                     value.second);

                ++i;
                ++j;
            }
        }
        break;

    case SetParser::NOT_RELEVANT:
        break;

    default:
        mxb_assert(!true);
    }

    return message;
}

/**
 * Perform re-authentication of the client
 *
 * @param session   Client session
 * @param packetbuf Client's response to the AuthSwitchRequest
 *
 * @return True if the user is allowed access
 */
bool reauthenticate_client(MXS_SESSION* session, GWBUF* packetbuf)
{
    bool rval = false;
    ClientDCB* client_dcb = session->client_dcb;
    auto& client_auth = client_dcb->m_auth_session;
    if (client_auth->capabilities() & mxs::AuthenticatorModule::CAP_REAUTHENTICATE)
    {
        MySQLProtocol* proto = (MySQLProtocol*)client_dcb->protocol_session();


        std::vector<uint8_t> orig_payload;
        uint32_t orig_len = gwbuf_length(proto->stored_query);
        orig_payload.resize(orig_len);
        gwbuf_copy_data(proto->stored_query, 0, orig_len, orig_payload.data());

        auto it = orig_payload.begin();
        it += MYSQL_HEADER_LEN + 1;     // Skip header and command byte
        auto user_end = std::find(it, orig_payload.end(), '\0');

        if (user_end == orig_payload.end())
        {
            mysql_send_auth_error(client_dcb, 3, 0, "Malformed AuthSwitchRequest packet");
            return false;
        }

        std::string user(it, user_end);
        it = user_end;
        ++it;

        // Skip the auth token
        auto token_len = *it++;
        it += token_len;

        auto db_end = std::find(it, orig_payload.end(), '\0');

        if (db_end == orig_payload.end())
        {
            mysql_send_auth_error(client_dcb, 3, 0, "Malformed AuthSwitchRequest packet");
            return false;
        }

        std::string db(it, db_end);

        it = db_end;
        ++it;

        proto->charset = *it++;
        proto->charset |= (*it++) << 8;

        // Copy the new username to the session data
        MYSQL_session* data = (MYSQL_session*)client_dcb->m_data;
        strcpy(data->user, user.c_str());
        strcpy(data->db, db.c_str());

        std::vector<uint8_t> payload;
        uint64_t payloadlen = gwbuf_length(packetbuf) - MYSQL_HEADER_LEN;
        payload.resize(payloadlen);
        gwbuf_copy_data(packetbuf, MYSQL_HEADER_LEN, payloadlen, &payload[0]);

        int rc = client_auth->reauthenticate(
            client_dcb, data->user, &payload[0], payload.size(),
            proto->scramble, sizeof(proto->scramble), data->client_sha1, sizeof(data->client_sha1));

        if (rc == MXS_AUTH_SUCCEEDED)
        {
            // Re-authentication successful, route the original COM_CHANGE_USER
            rval = true;
        }
        else
        {
            /**
             * Authentication failed. To prevent the COM_CHANGE_USER from reaching
             * the backend servers (and possibly causing problems) the client
             * connection will be closed.
             *
             * First packet is COM_CHANGE_USER, the second is AuthSwitchRequest,
             * third is the response and the fourth is the following error.
             */
            handle_authentication_errors(client_dcb, rc, 3);
        }
    }

    return rval;
}

void track_transaction_state(MXS_SESSION* session, GWBUF* packetbuf)
{
    mxb_assert(GWBUF_IS_CONTIGUOUS(packetbuf));

    if (session_trx_is_ending(session))
    {
        session_set_trx_state(session, SESSION_TRX_INACTIVE);
    }

    if (mxs_mysql_get_command(packetbuf) == MXS_COM_QUERY)
    {
        uint32_t type = qc_get_trx_type_mask(packetbuf);

        if (type & QUERY_TYPE_BEGIN_TRX)
        {
            if (type & QUERY_TYPE_DISABLE_AUTOCOMMIT)
            {
                session_set_autocommit(session, false);
                session_set_trx_state(session, SESSION_TRX_INACTIVE);
            }
            else
            {
                mxs_session_trx_state_t trx_state;
                if (type & QUERY_TYPE_WRITE)
                {
                    trx_state = SESSION_TRX_READ_WRITE;
                }
                else if (type & QUERY_TYPE_READ)
                {
                    trx_state = SESSION_TRX_READ_ONLY;
                }
                else
                {
                    trx_state = SESSION_TRX_ACTIVE;
                }

                session_set_trx_state(session, trx_state);
            }
        }
        else if ((type & QUERY_TYPE_COMMIT) || (type & QUERY_TYPE_ROLLBACK))
        {
            uint32_t trx_state = session_get_trx_state(session);
            trx_state |= SESSION_TRX_ENDING_BIT;
            session_set_trx_state(session, (mxs_session_trx_state_t)trx_state);

            if (type & QUERY_TYPE_ENABLE_AUTOCOMMIT)
            {
                session_set_autocommit(session, true);
            }
        }
    }
}

bool handle_change_user(MySQLProtocol* proto, bool* changed_user, GWBUF** packetbuf)
{
    bool ok = true;

    if (!proto->changing_user && proto->reply().command() == MXS_COM_CHANGE_USER)
    {
        // Track the COM_CHANGE_USER progress at the session level
        auto s = (MYSQL_session*)proto->session()->client_dcb->m_data;
        s->changing_user = true;

        *changed_user = true;
        send_auth_switch_request_packet(proto->session()->client_dcb);

        // Store the original COM_CHANGE_USER for later
        proto->stored_query = *packetbuf;
        *packetbuf = NULL;
    }
    else if (proto->changing_user)
    {
        mxb_assert(proto->reply().command() == MXS_COM_CHANGE_USER);
        proto->changing_user = false;
        bool ok = reauthenticate_client(proto->session(), *packetbuf);
        gwbuf_free(*packetbuf);
        *packetbuf = proto->stored_query;
        proto->stored_query = nullptr;
    }

    return ok;
}

void extract_user(char* token, std::string* user)
{
    char* end = strchr(token, ';');

    if (end)
    {
        user->assign(token, end - token);
    }
    else
    {
        user->assign(token);
    }
}

/**
 * Parse a "KILL [CONNECTION | QUERY] [ <process_id> | USER <username> ]" query.
 * Will modify the argument string even if unsuccessful.
 *
 * @param query Query string to parse
 * @paran thread_id_out Thread id output
 * @param kt_out Kill command type output
 * @return true on success, false on error
 */
bool parse_kill_query(char* query, uint64_t* thread_id_out, kill_type_t* kt_out, std::string* user)
{
    const char WORD_CONNECTION[] = "CONNECTION";
    const char WORD_QUERY[] = "QUERY";
    const char WORD_HARD[] = "HARD";
    const char WORD_SOFT[] = "SOFT";
    const char WORD_USER[] = "USER";
    const char DELIM[] = " \n\t";

    int kill_type = KT_CONNECTION;
    unsigned long long int thread_id = 0;
    std::string tmpuser;

    enum kill_parse_state_t
    {
        KILL,
        CONN_QUERY,
        ID,
        USER,
        SEMICOLON,
        DONE
    } state = KILL;
    char* saveptr = NULL;
    bool error = false;

    char* token = strtok_r(query, DELIM, &saveptr);

    while (token && !error)
    {
        bool get_next = false;
        switch (state)
        {
        case KILL:
            if (strncasecmp(token, WORD_KILL, sizeof(WORD_KILL) - 1) == 0)
            {
                state = CONN_QUERY;
                get_next = true;
            }
            else
            {
                error = true;
            }
            break;

        case CONN_QUERY:
            if (strncasecmp(token, WORD_QUERY, sizeof(WORD_QUERY) - 1) == 0)
            {
                kill_type &= ~KT_CONNECTION;
                kill_type |= KT_QUERY;
                get_next = true;
            }
            else if (strncasecmp(token, WORD_CONNECTION, sizeof(WORD_CONNECTION) - 1) == 0)
            {
                get_next = true;
            }

            if (strncasecmp(token, WORD_HARD, sizeof(WORD_HARD) - 1) == 0)
            {
                kill_type |= KT_HARD;
                get_next = true;
            }
            else if (strncasecmp(token, WORD_SOFT, sizeof(WORD_SOFT) - 1) == 0)
            {
                kill_type |= KT_SOFT;
                get_next = true;
            }
            else
            {
                /* Move to next state regardless of comparison result. The current
                 * part is optional and the process id may already be in the token. */
                state = ID;
            }
            break;

        case ID:
            if (strncasecmp(token, WORD_USER, sizeof(WORD_USER) - 1) == 0)
            {
                state = USER;
                get_next = true;
                break;
            }
            else
            {
                char* endptr_id = NULL;

                long long int l = strtoll(token, &endptr_id, 0);

                if ((l == LLONG_MAX && errno == ERANGE)
                    || (*endptr_id != '\0' && *endptr_id != ';')
                    || l <= 0 || endptr_id == token)
                {
                    // Not a positive 32-bit integer
                    error = true;
                }
                else
                {
                    mxb_assert(*endptr_id == '\0' || *endptr_id == ';');
                    state = SEMICOLON;      // In case we have space before ;
                    get_next = true;
                    thread_id = l;
                }
            }
            break;

        case USER:
            extract_user(token, &tmpuser);
            state = SEMICOLON;
            get_next = true;
            break;

        case SEMICOLON:
            if (strncmp(token, ";", 1) == 0)
            {
                state = DONE;
                get_next = true;
            }
            else
            {
                error = true;
            }
            break;

        default:
            error = true;
            break;
        }

        if (get_next)
        {
            token = strtok_r(NULL, DELIM, &saveptr);
        }
    }

    if (error || (state != DONE && state != SEMICOLON))
    {
        return false;
    }
    else
    {
        *thread_id_out = thread_id;
        *kt_out = (kill_type_t)kill_type;
        *user = tmpuser;
        return true;
    }
}

/**
 * Handle text version of KILL [CONNECTION | QUERY] <process_id>. Only detects
 * commands in the beginning of the packet and with no comments.
 * Increased parsing would slow down the handling of every single query.
 *
 * @param dcb         Client dcb
 * @param read_buffer Input buffer
 * @param packet_len  Length of the protocol packet
 *
 * @return RES_CONTINUE or RES_END
 */
spec_com_res_t handle_query_kill(DCB* dcb, GWBUF* read_buffer, uint32_t packet_len)
{
    spec_com_res_t rval = RES_CONTINUE;
    /* First, we need to detect the text "KILL" (ignorecase) in the start
     * of the packet. Copy just enough characters. */
    const size_t KILL_BEGIN_LEN = sizeof(WORD_KILL) - 1;
    char startbuf[KILL_BEGIN_LEN];      // Not 0-terminated, careful...
    size_t copied_len = gwbuf_copy_data(read_buffer,
                                        MYSQL_HEADER_LEN + 1,
                                        KILL_BEGIN_LEN,
                                        (uint8_t*)startbuf);

    if (strncasecmp(WORD_KILL, startbuf, KILL_BEGIN_LEN) == 0)
    {
        /* Good chance that the query is a KILL-query. Copy the entire
         * buffer and process. */
        size_t buffer_len = packet_len - (MYSQL_HEADER_LEN + 1);
        char querybuf[buffer_len + 1];          // 0-terminated
        copied_len = gwbuf_copy_data(read_buffer,
                                     MYSQL_HEADER_LEN + 1,
                                     buffer_len,
                                     (uint8_t*)querybuf);
        querybuf[copied_len] = '\0';
        kill_type_t kt = KT_CONNECTION;
        uint64_t thread_id = 0;
        std::string user;

        if (parse_kill_query(querybuf, &thread_id, &kt, &user))
        {
            rval = RES_END;

            if (thread_id > 0)
            {
                mxs_mysql_execute_kill(dcb->session(), thread_id, kt);
            }
            else if (!user.empty())
            {
                mxs_mysql_execute_kill_user(dcb->session(), user.c_str(), kt);
            }

            mxs_mysql_send_ok(dcb, 1, 0, NULL);
        }
    }

    return rval;
}

/**
 * Some SQL commands/queries need to be detected and handled by the protocol
 * and MaxScale instead of being routed forward as is.
 *
 * @param dcb         Client dcb
 * @param read_buffer The current read buffer
 * @param cmd         Current command being executed
 *
 * @return see @c spec_com_res_t
 */
spec_com_res_t process_special_commands(DCB* dcb, GWBUF* read_buffer, uint8_t cmd)
{
    spec_com_res_t rval = RES_CONTINUE;

    if (cmd == MXS_COM_QUIT)
    {
        /** The client is closing the connection. We know that this will be the
         * last command the client sends so the backend connections are very likely
         * to be in an idle state.
         *
         * If the client is pipelining the queries (i.e. sending N request as
         * a batch and then expecting N responses) then it is possible that
         * the backend connections are not idle when the COM_QUIT is received.
         * In most cases we can assume that the connections are idle. */
        session_qualify_for_pool(dcb->session());
    }
    else if (cmd == MXS_COM_SET_OPTION)
    {
        /**
         * This seems to be only used by some versions of PHP.
         *
         * The option is stored as a two byte integer with the values 0 for enabling
         * multi-statements and 1 for disabling it.
         */
        MySQLProtocol* proto = (MySQLProtocol*)dcb->protocol_session();

        if (GWBUF_DATA(read_buffer)[MYSQL_HEADER_LEN + 2])
        {
            proto->client_capabilities &= ~GW_MYSQL_CAPABILITIES_MULTI_STATEMENTS;
        }
        else
        {
            proto->client_capabilities |= GW_MYSQL_CAPABILITIES_MULTI_STATEMENTS;
        }
    }
    else if (cmd == MXS_COM_PROCESS_KILL)
    {
        uint64_t process_id = gw_mysql_get_byte4(GWBUF_DATA(read_buffer) + MYSQL_HEADER_LEN + 1);
        mxs_mysql_execute_kill(dcb->session(), process_id, KT_CONNECTION);
        mxs_mysql_send_ok(dcb, 1, 0, NULL);
        rval = RES_END;
    }
    else if (cmd == MXS_COM_QUERY)
    {
        /* Limits on the length of the queries in which "KILL" is searched for. Reducing
         * LONGEST_KILL will reduce overhead but also limit the range of accepted queries. */
        const int SHORTEST_KILL = sizeof("KILL 1") - 1;
        const int LONGEST_KILL = sizeof("KILL CONNECTION 12345678901234567890 ;");
        auto packet_len = gwbuf_length(read_buffer);

        /* Is length within limits for a kill-type query? */
        if (packet_len >= (MYSQL_HEADER_LEN + 1 + SHORTEST_KILL)
            && packet_len <= (MYSQL_HEADER_LEN + 1 + LONGEST_KILL))
        {
            rval = handle_query_kill(dcb, read_buffer, packet_len);
        }
    }

    return rval;
}

/**
 * Detect if buffer includes partial mysql packet or multiple packets.
 * Store partial packet to dcb_readqueue. Send complete packets one by one
 * to router.
 *
 * It is assumed readbuf includes at least one complete packet.
 * Return 1 in success. If the last packet is incomplete return success but
 * leave incomplete packet to readbuf.
 *
 * @param session       Session pointer
 * @param capabilities  The capabilities of the service.
 * @param p_readbuf     Pointer to the address of GWBUF including the query
 *
 * @return 1 if succeed,
 */
int route_by_statement(MySQLProtocol* proto, uint64_t capabilities, GWBUF** p_readbuf)
{
    int rc = 1;
    auto session = proto->session();
    auto dcb = session->client_dcb;

    while (GWBUF* packetbuf = modutil_get_next_MySQL_packet(p_readbuf))
    {
        // TODO: Do this only when RCAP_TYPE_CONTIGUOUS_INPUT is requested
        packetbuf = gwbuf_make_contiguous(packetbuf);
        session_retain_statement(session, packetbuf);

        // Track the command being executed
        proto->track_query(packetbuf);

        if (char* message = handle_variables(session, &packetbuf))
        {
            rc = dcb->protocol_write(modutil_create_mysql_err_msg(1, 0, 1193, "HY000", message));
            MXS_FREE(message);
            continue;
        }

        // Must be done whether or not there were any changes, as the query classifier
        // is thread and not session specific.
        qc_set_sql_mode(static_cast<qc_sql_mode_t>(session->client_protocol_data));

        if (process_special_commands(dcb, packetbuf, proto->reply().command()) == RES_END)
        {
            gwbuf_free(packetbuf);
            continue;
        }

        if (rcap_type_required(capabilities, RCAP_TYPE_TRANSACTION_TRACKING)
            && !session->service->config().session_track_trx_state
            && !session_is_load_active(session))
        {
            track_transaction_state(session, packetbuf);
        }

        bool changed_user = false;

        if (!handle_change_user(proto, &changed_user, &packetbuf))
        {
            MXS_ERROR("User reauthentication failed for %s", session->user_and_host().c_str());
            gwbuf_free(packetbuf);
            rc = 0;
            break;
        }

        if (packetbuf)
        {
            /** Route query */
            rc = proto->do_routeQuery(packetbuf);
        }

        proto->changing_user = changed_user;

        if (rc != 1)
        {
            break;
        }
    }

    return rc;
}

/**
 * @brief Client read event, process data, client already authenticated
 *
 * First do some checks and get the router capabilities.  If the router
 * wants to process each individual statement, then the data must be split
 * into individual SQL statements. Any data that is left over is held in the
 * DCB read queue.
 *
 * Finally, the general client data processing function is called.
 *
 * @param dcb           Descriptor control block
 * @param read_buffer   A buffer containing the data read from client
 * @param nbytes_read   The number of bytes of data read
 * @return 0 if succeed, 1 otherwise
 */
int perform_normal_read(DCB* dcb, GWBUF* read_buffer, uint32_t nbytes_read)
{
    MXS_SESSION* session = dcb->session();
    auto session_state_value = session->state();

    if (session_state_value != MXS_SESSION::State::STARTED)
    {
        if (session_state_value != MXS_SESSION::State::STOPPING)
        {
            MXS_ERROR("Session received a query in incorrect state: %s",
                      session_state_to_string(session_state_value));
        }
        gwbuf_free(read_buffer);
        dcb_close(dcb);
        return 1;
    }

    // Make sure that a complete packet is read before continuing
    uint8_t pktlen[MYSQL_HEADER_LEN];
    size_t n_copied = gwbuf_copy_data(read_buffer, 0, MYSQL_HEADER_LEN, pktlen);

    if (n_copied != sizeof(pktlen)
        || nbytes_read < MYSQL_GET_PAYLOAD_LEN(pktlen) + MYSQL_HEADER_LEN)
    {
        dcb_readq_append(dcb, read_buffer);
        return 0;
    }

    MySQLProtocol* proto = static_cast<MySQLProtocol*>(dcb->protocol_session());

    // The query classifier classifies according to the service's server that has the smallest version number
    qc_set_server_version(proto->version());

    /**
     * Feed each statement completely and separately to router.
     */
    auto capabilities = service_get_capabilities(session->service);
    int rval = route_by_statement(proto, capabilities, &read_buffer) ? 0 : 1;

    if (read_buffer != NULL)
    {
        // Must have been data left over, add incomplete mysql packet to read queue
        dcb_readq_append(dcb, read_buffer);
    }

    if (rval != 0)
    {
        /** Routing failed, close the client connection */
        dcb->session()->close_reason = SESSION_CLOSE_ROUTING_FAILED;
        dcb_close(dcb);
        MXS_ERROR("Routing the query failed. Session will be closed.");
    }
    else if (proto->reply().command() == MXS_COM_QUIT)
    {
        /** Close router session which causes closing of backends */
        mxb_assert_message(session_valid_for_pool(dcb->session()), "Session should qualify for pooling");
        dcb_close(dcb);
    }

    return rval;
}

/*
 * Mapping three session tracker's info to mxs_session_trx_state_t
 * SESSION_TRACK_STATE_CHANGE:
 *   Get lasted autocommit value;
 *   https://dev.mysql.com/worklog/task/?id=6885
 * SESSION_TRACK_TRANSACTION_TYPE:
 *   Get transaction boundaries
 *   TX_EMPTY                  => SESSION_TRX_INACTIVE
 *   TX_EXPLICIT | TX_IMPLICIT => SESSION_TRX_ACTIVE
 *   https://dev.mysql.com/worklog/task/?id=6885
 * SESSION_TRACK_TRANSACTION_CHARACTERISTICS
 *   Get trx characteristics such as read only, read write, snapshot ...
 *
 */
void parse_and_set_trx_state(MXS_SESSION* ses, GWBUF* data)
{
    char* autocommit = gwbuf_get_property(data, (char*)"autocommit");

    if (autocommit)
    {
        MXS_DEBUG("autocommit:%s", autocommit);
        if (strncasecmp(autocommit, "ON", 2) == 0)
        {
            session_set_autocommit(ses, true);
        }
        if (strncasecmp(autocommit, "OFF", 3) == 0)
        {
            session_set_autocommit(ses, false);
        }
    }
    char* trx_state = gwbuf_get_property(data, (char*)"trx_state");
    if (trx_state)
    {
        mysql_tx_state_t s = parse_trx_state(trx_state);

        if (s == TX_EMPTY)
        {
            session_set_trx_state(ses, SESSION_TRX_INACTIVE);
        }
        else if ((s & TX_EXPLICIT) || (s & TX_IMPLICIT))
        {
            session_set_trx_state(ses, SESSION_TRX_ACTIVE);
        }
    }
    char* trx_characteristics = gwbuf_get_property(data, (char*)"trx_characteristics");
    if (trx_characteristics)
    {
        if (strncmp(trx_characteristics, "START TRANSACTION READ ONLY;", 28) == 0)
        {
            session_set_trx_state(ses, SESSION_TRX_READ_ONLY);
        }

        if (strncmp(trx_characteristics, "START TRANSACTION READ WRITE;", 29) == 0)
        {
            session_set_trx_state(ses, SESSION_TRX_READ_WRITE);
        }
    }
    MXS_DEBUG("trx state:%s", session_trx_state_to_string(ses->trx_state));
    MXS_DEBUG("autcommit:%s", session_is_autocommit(ses) ? "ON" : "OFF");
}
}

/**
 * MXS_PROTOCOL_API implementation.
 */

namespace
{

int mariadbclient_read(DCB* dcb)
{
    MySQLProtocol* protocol;
    GWBUF* read_buffer = NULL;
    int return_code = 0;
    uint32_t nbytes_read = 0;
    uint32_t max_bytes = 0;

    if (dcb->role() != DCB::Role::CLIENT)
    {
        MXS_ERROR("DCB must be a client handler for MySQL client protocol.");
        return 1;
    }

    protocol = (MySQLProtocol*)dcb->protocol_session();

    MXS_DEBUG("Protocol state: %s", gw_mysql_protocol_state2string(protocol->protocol_auth_state));

    /**
     * The use of max_bytes seems like a hack, but no better option is available
     * at the time of writing. When a MySQL server receives a new connection
     * request, it sends an Initial Handshake Packet. Where the client wants to
     * use SSL, it responds with an SSL Request Packet (in place of a Handshake
     * Response Packet). The SSL Request Packet contains only the basic header,
     * and not the user credentials. It is 36 bytes long.  The server then
     * initiates the SSL handshake (via calls to OpenSSL).
     *
     * In many cases, this is what happens. But occasionally, the client seems
     * to send a packet much larger than 36 bytes (in tests it was 333 bytes).
     * If the whole of the packet is read, it is then lost to the SSL handshake
     * process. Why this happens is presently unknown. Reading just 36 bytes
     * when the server requires SSL and SSL has not yet been negotiated seems
     * to solve the problem.
     *
     * If a neater solution can be found, so much the better.
     */
    if (ssl_required_but_not_negotiated(dcb))
    {
        max_bytes = 36;
    }

    const uint32_t max_single_read = GW_MYSQL_MAX_PACKET_LEN + MYSQL_HEADER_LEN;
    return_code = dcb_read(dcb, &read_buffer, max_bytes > 0 ? max_bytes : max_single_read);

    if (return_code < 0)
    {
        dcb_close(dcb);
    }

    if (read_buffer)
    {
        nbytes_read = gwbuf_length(read_buffer);
    }

    if (nbytes_read == 0)
    {
        return return_code;
    }

    if (nbytes_read == max_single_read && dcb_bytes_readable(dcb) > 0)
    {
        // We read a maximally long packet, route it first. This is done in case there's a lot more data
        // waiting and we have to start throttling the reads.
        poll_fake_read_event(dcb);
    }

    return_code = 0;

    switch (protocol->protocol_auth_state)
    {
    /**
     *
     * When a listener receives a new connection request, it creates a
     * request handler DCB to for the client connection. The listener also
     * sends the initial authentication request to the client. The first
     * time this function is called from the poll loop, the client reply
     * to the authentication request should be available.
     *
     * If the authentication is successful the protocol authentication state
     * will be changed to MYSQL_IDLE (see below).
     *
     */
    case MXS_AUTH_STATE_MESSAGE_READ:
        if (nbytes_read < 3
            || (0 == max_bytes && nbytes_read < MYSQL_GET_PACKET_LEN(read_buffer))
            || (0 != max_bytes && nbytes_read < max_bytes))
        {
            dcb_readq_append(dcb, read_buffer);
        }
        else
        {
            if (nbytes_read > MYSQL_GET_PACKET_LEN(read_buffer))
            {
                // We read more data than was needed
                dcb_readq_append(dcb, read_buffer);
                read_buffer = modutil_get_next_MySQL_packet(&dcb->m_readq);
            }

            return_code = perform_authentication(dcb, read_buffer, nbytes_read);
        }
        break;

    /**
     *
     * Once a client connection is authenticated, the protocol authentication
     * state will be MYSQL_IDLE and so every event of data received will
     * result in a call that comes to this section of code.
     *
     */
    case MXS_AUTH_STATE_COMPLETE:
        /* After this call read_buffer will point to freed data */
        return_code = perform_normal_read(dcb, read_buffer, nbytes_read);
        break;

    case MXS_AUTH_STATE_FAILED:
        gwbuf_free(read_buffer);
        return_code = 1;
        break;

    default:
        MXS_ERROR("In mysql_client.c unexpected protocol authentication state");
        break;
    }

    return return_code;
}

int mariadbclient_write(DCB* dcb, GWBUF* queue)
{
    if (GWBUF_IS_REPLY_OK(queue) && dcb->service()->config().session_track_trx_state)
    {
        parse_and_set_trx_state(dcb->session(), queue);
    }
    return dcb_write(dcb, queue);
}

int mariadbclient_write_ready(DCB* dcb)
{
    MySQLProtocol* protocol = NULL;

    mxb_assert(dcb->state() != DCB::State::DISCONNECTED);

    if (dcb == NULL)
    {
        goto return_1;
    }

    if (dcb->state() == DCB::State::DISCONNECTED)
    {
        goto return_1;
    }

    if (dcb->protocol_session() == NULL)
    {
        goto return_1;
    }
    protocol = (MySQLProtocol*)dcb->protocol_session();

    if (protocol->protocol_auth_state == MXS_AUTH_STATE_COMPLETE)
    {
        dcb_drain_writeq(dcb);
        goto return_1;
    }

return_1:
    return 1;
}

int mariadbclient_error(DCB* dcb)
{
    mxb_assert(dcb->session()->state() != MXS_SESSION::State::STOPPING);
    dcb_close(dcb);
    return 1;
}

int mariadbclient_hangup(DCB* dcb)
{
    // We simply close the DCB, this will propagate the closure to any
    // backend descriptors and perform the session cleanup.

    MXS_SESSION* session = dcb->session();

    if (session && !session_valid_for_pool(session))
    {
        if (session_get_dump_statements() == SESSION_DUMP_STATEMENTS_ON_ERROR)
        {
            session_dump_statements(session);
        }

        if (session_get_session_trace())
        {
            session_dump_log(session);
        }

        // The client did not send a COM_QUIT packet
        std::string errmsg {"Connection killed by MaxScale"};
        std::string extra {session_get_close_reason(dcb->session())};

        if (!extra.empty())
        {
            errmsg += ": " + extra;
        }

        int seqno = 1;

        if (dcb->m_data && ((MYSQL_session*)dcb->m_data)->changing_user)
        {
            // In case a COM_CHANGE_USER is in progress, we need to send the error with the seqno 3
            seqno = 3;
        }

        modutil_send_mysql_err_packet(dcb, seqno, 0, 1927, "08S01", errmsg.c_str());
    }

    dcb_close(dcb);

    return 1;
}

MXS_PROTOCOL_SESSION* mariadbclient_new_client_session(MXS_SESSION* session, mxs::Component* component)
{
    return new(std::nothrow) MySQLProtocol(session, nullptr, component);
}

void mariadbclient_free_session(MXS_PROTOCOL_SESSION* protocol_session)
{
    delete static_cast<MySQLProtocol*>(protocol_session);
}

bool mariadbclient_init_connection(DCB* client_dcb)
{
    send_mysql_client_handshake(client_dcb, static_cast<MySQLProtocol*>(client_dcb->protocol_session()));
    return true;
}

void mariadbclient_finish_connection(DCB* client_dcb)
{
}

char* mariadbclient_auth_default()
{
    return (char*)"mariadbauth";
}

int mariadbclient_connlimit(DCB* dcb, int limit)
{
    return mysql_send_standard_error(dcb, 0, 1040, "Too many connections");
}

GWBUF* mariadbclient_reject(const char* host)
{
    std::stringstream ss;
    ss << "Host '" << host << "' is temporarily blocked due to too many authentication failures.";
    return modutil_create_mysql_err_msg(0, 0, 1129, "HY000", ss.str().c_str());
}
}

/**
 * Module API implementation.
 */

namespace
{

int process_init(void)
{
    int rv = mysql_library_init(0, NULL, NULL);

    if (rv != 0)
    {
        MXS_ERROR("MySQL initialization failed, MariaDB MaxScale will exit. "
                  "MySQL Error: %d, %s.",
                  mysql_errno(NULL),
                  mysql_error(NULL));
    }

    return rv;
}

void process_finish(void)
{
    mysql_library_end();
}

int thread_init(void)
{
    int rv = mysql_thread_init();

    if (rv != 0)
    {
        MXS_ERROR("MySQL thread initialization failed, the thread will exit.");
    }

    return rv;
}

void thread_finish(void)
{
    mysql_thread_end();
}
}

/**
 * mariadbclient module entry point.
 *
 * @return The module object
 */
extern "C" MXS_MODULE* MXS_CREATE_MODULE()
{
    static MXS_PROTOCOL_API MyObject =
    {
        mariadbclient_read,                 /* EPOLLIN handler                    */
        mariadbclient_write,                /* Write data from MaxScale to client */
        mariadbclient_write_ready,          /* EPOLLOUT handler                   */
        mariadbclient_error,                /* EPOLLERR handler                   */
        mariadbclient_hangup,               /* EPOLLHUP handler                   */
        mariadbclient_new_client_session,
        NULL,                               /* new_backend_session                */
        mariadbclient_free_session,
        mariadbclient_init_connection,
        mariadbclient_finish_connection,
        mariadbclient_auth_default,
        mariadbclient_connlimit,
        NULL,
        NULL,
        mariadbclient_reject
    };

    static MXS_MODULE info =
    {
        MXS_MODULE_API_PROTOCOL,
        MXS_MODULE_GA,
        MXS_PROTOCOL_VERSION,
        "The client to MaxScale MySQL protocol implementation",
        "V1.1.0",
        MXS_NO_MODULE_CAPABILITIES,
        &MyObject,
        process_init,
        process_finish,
        thread_init,
        thread_finish,
        {
            {MXS_END_MODULE_PARAMS}
        }
    };

    return &info;
}
