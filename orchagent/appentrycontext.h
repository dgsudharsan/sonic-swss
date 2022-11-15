#pragma once

extern "C" {

#include "sai.h"
}
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <set>
#include <memory>
#include <utility>
#include "dbconnector.h"
#include "table.h"
#include "consumertable.h"
#include "consumerstatetable.h"
#include "notificationconsumer.h"

class AppEntryContext
{
public:
    AppEntryContext();
    static AppEntryContext& getInstance();
    static void reset_table_name();
    static void reset_key();
    static void set_table_name(std::string table_name);
    static void set_key(std::string key);
    static std::string get();
    static void add_oid(sai_object_id_t oid);
    static void remove_oid(sai_object_id_t oid);
    static void write_db();

private:
    std::string _table_name;
    std::string _key;
    swss::DBConnector _db;
    // Maps table names to tables.
    std::unordered_map<std::string, std::unique_ptr<swss::Table>> _tables;
    std::unordered_map<std::string, std::unordered_set<sai_object_id_t>> _oid_map;
};
