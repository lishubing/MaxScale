/*
 * Copyright (c) 2019 MariaDB Corporation Ab
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
#include <maxbase/host.hh>

#include <ostream>
#include <vector>
#include <algorithm>
#include <arpa/inet.h>
#include <netdb.h>
#include <maxbase/assert.h>
#include <maxbase/format.hh>
#include <maxbase/string.hh>

namespace
{
// Simple but not exhaustive address validation functions.
// An ipv4 address "x.x.x.x" cannot be a hostname (where x is a number), but pretty
// much anything else can. Call is_valid_hostname() last.
bool is_valid_ipv4(const std::string& ip)
{
    bool ret = ip.find_first_not_of("0123456789.") == std::string::npos
        && (ip.length() <= 15 && ip.length() >= 7)
        && std::count(begin(ip), end(ip), '.') == 3;

    return ret;
}

bool is_valid_ipv6(const std::string& ip)
{
    auto invalid_char = [](char ch) {
            bool valid = std::isxdigit(ch) || ch == ':' || ch == '.';
            return !valid;
        };

    bool ret = std::count(begin(ip), end(ip), ':') >= 2
        && std::none_of(begin(ip), end(ip), invalid_char)
        && (ip.length() <= 45 && ip.length() >= 2);

    return ret;
}

bool is_valid_hostname(const std::string& hn)
{
    auto invalid_char = [](char ch) {
            bool valid = std::isalnum(ch) || ch == '_' || ch == '.';
            return !valid;
        };

    bool ret = std::none_of(begin(hn), end(hn), invalid_char)
        && hn.front() != '_'
        && (hn.length() <= 253 && hn.length() > 0);

    return ret;
}

bool is_valid_socket(const std::string& addr)
{
    // Can't check the file system, the socket may not have been created yet.
    // Just not bothering to check much, file names can be almost anything and errors are easy to spot.
    bool ret = addr.front() == '/'
        && addr.back() != '/';      // avoids the confusing error: Address already in use

    return ret;
}

bool is_valid_port(int port)
{
    return 0 < port && port < (1 << 16);
}

// Make sure the order here is the same as in Host::Type.
static std::vector<std::string> host_type_names = {"Invalid", "UnixDomainSocket", "HostName", "IPV4", "IPV6"};
}

namespace maxbase
{
std::string to_string(Host::Type type)
{
    size_t i = size_t(type);
    return i >= host_type_names.size() ? "UNKNOWN" : host_type_names[i];
}

void Host::set_type(bool port_string_specified)
{
    if (is_valid_socket(m_address))
    {
        if (!port_string_specified)
        {
            m_type = Type::UnixDomainSocket;
        }
    }
    else if (is_valid_port(m_port))
    {
        if (is_valid_ipv4(m_address))
        {
            m_type = Type::IPV4;
        }
        else if (is_valid_ipv6(m_address))
        {
            m_type = Type::IPV6;
        }
        else if (is_valid_hostname(m_address))
        {
            m_type = Type::HostName;
        }
    }
}

Host::Host(const std::string& in)
{
    m_org_input = in;
    std::string input = maxbase::trimmed_copy(in);

    if (input.empty())
    {
        return;
    }

    std::string port_part;

    // 'ite' is left pointing into the input if there is an error in parsing. Not exhaustive error checking.
    auto ite = input.begin();

    if (*ite == '[')
    {   // expecting [address]:port, where :port is optional
        auto last = std::find(begin(input), end(input), ']');
        std::copy(++ite, last, std::back_inserter(m_address));
        if (last != end(input))
        {
            if (++last != end(input) && *last == ':' && last + 1 != end(input))
            {
                ++last;
                std::copy(last, end(input), std::back_inserter(port_part));
                last = end(input);
            }
            ite = last;
        }
    }
    else
    {
        if (is_valid_ipv6(input))
        {
            m_address = input;
            ite = end(input);
        }
        else
        {
            // expecting address:port, where :port is optional => (hostnames with colons must use [xxx]:port)
            auto colon = std::find(begin(input), end(input), ':');
            std::copy(begin(input), colon, std::back_inserter(m_address));
            ite = colon;
            if (colon != end(input) && ++colon != end(input))
            {
                std::copy(colon, end(input), std::back_inserter(port_part));
                ite = end(input);
            }
        }
    }

    if (ite == end(input))      // if all input consumed
    {
        if (!port_part.empty())
        {
            bool all_digits = std::all_of(begin(port_part), end(port_part),
                                          [](char ch) {
                                              return std::isdigit(ch);
                                          });
            m_port = all_digits ? std::atoi(port_part.c_str()) : -1;
        }

        set_type(!port_part.empty());
    }
}

Host::Host(const std::string& addr, int port)
{
    m_org_input = addr;
    m_address = addr;
    m_port = port;

    if (!m_address.empty() && m_address.front() != '[')
    {
        set_type(false);
    }
}

std::ostream& operator<<(std::ostream& os, const Host& host)
{
    switch (host.type())
    {
    case Host::Type::Invalid:
        os << "INVALID input: '" << host.org_input() << "' parsed to "
           << host.address() << ":" << host.port();
        break;

    case Host::Type::UnixDomainSocket:
        os << host.address();
        break;

    case Host::Type::HostName:
    case Host::Type::IPV4:
        os << host.address() << ':' << host.port();
        break;

    case Host::Type::IPV6:
        os << '[' << host.address() << "]:" << host.port();
        break;
    }
    return os;
}

std::istream& operator>>(std::istream& is, Host& host)
{
    std::string input;
    is >> input;
    host = Host(input);
    return is;
}

bool dns(const std::string& host, std::string* addr_out, std::string* error_out)
{
    addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET6;     /* Only return IPv6-addresses, possibly mapped from IPv4 */
    hints.ai_socktype = SOCK_DGRAM; /* Datagram socket */
    hints.ai_flags = AI_V4MAPPED;   /* Mapped IPv4 */
    hints.ai_protocol = 0;          /* Any protocol */
    hints.ai_canonname = NULL;
    hints.ai_addr = NULL;
    hints.ai_next = NULL;

    addrinfo* results = nullptr;
    bool success = false;
    std::string error_msg;
    int rv_addrinfo = getaddrinfo(host.c_str(), nullptr, &hints, &results);
    if (rv_addrinfo == 0)
    {
        mxb_assert(results);
        if (results)
        {
            // getaddrinfo may return multiple result addresses. Only consider the first.
            char buf[INET6_ADDRSTRLEN];
            if (inet_ntop(AF_INET6, results->ai_addr, buf, sizeof(buf)))
            {
                *addr_out = buf;
                success = true;
            }
            else
            {
                error_msg = mxb::string_printf("inet_ntop() failed: '%s'.", strerror(errno));
            }
            freeaddrinfo(results);
        }
    }
    else
    {
        error_msg = mxb::string_printf("getaddrinfo() failed: '%s'.", gai_strerror(rv_addrinfo));
    }

    if (error_out)
    {
        *error_out = error_msg;
    }
    return success;
}

bool reverse_dns(const std::string& ip, std::string* output)
{
    sockaddr_storage socket_address;
    memset(&socket_address, 0, sizeof(socket_address));
    socklen_t slen = 0;

    if (is_valid_ipv4(ip))
    {
        // Casts between the different sockaddr-types should work.
        int family = AF_INET;
        auto sa_in = reinterpret_cast<sockaddr_in*>(&socket_address);
        if (inet_pton(family, ip.c_str(), &sa_in->sin_addr) == 1)
        {
            sa_in->sin_family = family;
            slen = sizeof(sockaddr_in);
        }
    }
    else if (is_valid_ipv6(ip))
    {
        int family = AF_INET6;
        auto sa_in6 = reinterpret_cast<sockaddr_in6*>(&socket_address);
        if (inet_pton(family, ip.c_str(), &sa_in6->sin6_addr) == 1)
        {
            sa_in6->sin6_family = family;
            slen = sizeof(sockaddr_in6);
        }
    }

    bool success = false;
    if (slen > 0)
    {
        char host[NI_MAXHOST];
        auto sa = reinterpret_cast<sockaddr*>(&socket_address);
        if (getnameinfo(sa, slen, host, sizeof(host), nullptr, 0, NI_NAMEREQD) == 0)
        {
            *output = host;
            success = true;
        }
    }

    if (!success)
    {
        *output = ip;
    }
    return success;
}
}
