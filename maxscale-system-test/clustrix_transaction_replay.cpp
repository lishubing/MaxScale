/*
 * Copyright (c) 2016 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2022-01-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

#include <iostream>
#include <map>
#include "testconnections.h"
#include "maxrest.hh"

using namespace std;

namespace
{

const std::string monitor_name = "Clustrix-Monitor";

map<string, MaxRest::Server> static_by_address;
map<string, MaxRest::Server> dynamic_by_address;
map<string, int> node_by_address;

void collect_information(TestConnections& test)
{
    MaxRest maxrest(&test);

    auto servers = maxrest.list_servers();

    string prefix = "@@" + monitor_name;

    for (const auto& server : servers)
    {
        string name = server.name;

        if (name.find("@@") == 0)
        {
            dynamic_by_address.insert(make_pair(server.address, server));
        }
        else
        {
            static_by_address.insert(make_pair(server.address, server));
        }

        if (!node_by_address.count(server.address) == 0)
        {
            Clustrix_nodes* pClustrix = test.clustrix;

            for (auto i = 0; i < pClustrix->N; ++i)
            {
                if (pClustrix->IP_private[i] == server.address)
                {
                    node_by_address[server.address] = i;
                    break;
                }
            }
        }
    }
}

void drop_table(TestConnections& test, MYSQL* pMysql)
{
    test.try_query(pMysql, "DROP TABLE IF EXISTS test.clustrix_tr");
}

void create_table(TestConnections& test, MYSQL* pMysql)
{
    test.try_query(pMysql, "CREATE TABLE test.clustrix_tr (a INT)");
    test.try_query(pMysql, "INSERT INTO test.clustrix_tr VALUES (42)");
}

void setup_database(TestConnections& test)
{
    MYSQL* pMysql = test.maxscales->open_rwsplit_connection();
    test.expect(pMysql, "Could not open connection to rws.");

    drop_table(test, pMysql);
    create_table(test, pMysql);

    mysql_close(pMysql);
}

bool stop_server(TestConnections& test, const std::string& name, int node)
{
    bool stopped = false;

    Clustrix_nodes* pClustrix = test.clustrix;

    auto rv = pClustrix->ssh_output("service clustrix stop", node, true);
    test.expect(rv.first == 0, "Could not stop Clustrix on node %d.", node);

    if (rv.first == 0)
    {
        MaxRest maxrest(&test);
        MaxRest::Server server;

        do
        {
            server = maxrest.show_server(name);

            if (server.state.find("Down") == string::npos)
            {
                sleep(1);
            }
        }
        while (server.state.find("Down") == string::npos);

        cout << "Clustrix on node " << node << " is down." << endl;
        stopped = true;
    }

    return stopped;
}

bool start_server(TestConnections& test, const std::string& name, int node, int timeout)
{
    bool started = false;

    Clustrix_nodes* pClustrix = test.clustrix;

    auto rv = pClustrix->ssh_output("service clustrix start", node, true);
    test.expect(rv.first == 0, "Could not start Clustrix on node %d.", node);

    if (rv.first == 0)
    {
        MaxRest maxrest(&test);
        MaxRest::Server server;

        time_t start = time(nullptr);
        time_t end;

        do
        {
            server = maxrest.show_server(name);

            if (server.state.find("Down") != string::npos)
            {
                cout << "Still down..." << endl;
                sleep(1);
            }

            end = time(nullptr);
        }
        while ((server.state.find("Down") != string::npos) && (end - start < timeout));

        test.expect(end - start < timeout, "Clustrix node %d did not start.", node);

        if (server.state.find("Master") != string::npos)
        {
            started = true;
        }
    }

    return started;
}

void run_test(TestConnections& test)
{
    MaxRest maxrest(&test);

    Maxscales* pMaxscales = test.maxscales;
    test.add_result(pMaxscales->connect_rwsplit(), "Could not connect to RWS.");

    MYSQL* pMysql = pMaxscales->conn_rwsplit[0];

    // What node are we connected to?
    Row row = get_row(pMysql, "SELECT iface_ip FROM system.nodeinfo WHERE nodeid=gtmnid()");

    test.expect(row.size() == 1, "1 row expected, %d received.", (int)row.size());

    string ip = row[0];
    string static_name = static_by_address[ip].name;
    string dynamic_name = dynamic_by_address[ip].name;
    int node = node_by_address[ip];

    cout << "Connected to " << ip << ", which is "
         << dynamic_name << "(" << static_name << ") "
         << " running on node " << node << "."
         << endl;

    test.try_query(pMysql, "BEGIN");
    test.try_query(pMysql, "SELECT * FROM test.clustrix_tr");

    cout << "Stopping server " << dynamic_name << " on node " << node << "." << endl;
    if (stop_server(test, dynamic_name, node))
    {
        // The server we were connected to is now down. If the following
        // succeeds, then reconnect + transaction replay worked as specified.
        test.try_query(pMysql, "SELECT * FROM test.clustrix_tr");
        test.try_query(pMysql, "COMMIT");

        cout << "Starting Clustrix " << dynamic_name << " on node " << node << "." << endl;

        int timeout = 3 * 60;
        start_server(test, dynamic_name, node, timeout);
    }
}

}

int main(int argc, char* argv[])
{
    TestConnections test(argc, argv);

    try
    {
        collect_information(test);
        setup_database(test);

        run_test(test);
    }
    catch (const std::exception& x)
    {
        cout << "Exception: " << x.what() << endl;
    }

    return test.global_result;
}
