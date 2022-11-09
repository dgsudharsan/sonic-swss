#pragma once

#include <thread>
#include <chrono>
#include <map>
#include "orch.h"
#include "table.h"
#include "flex_counter_manager.h"

extern "C" {
#include "sai.h"
}


class EcmpStatOrch : public Orch
{
public:
    EcmpStatOrch(swss::DBConnector *db, std::string tableName);

private:
    std::map<std::string, std::vector<std::string>> nhg_to_nh_map;
    std::map<std::string, sai_object_id_t> counters_oid_map;
    sai_object_id_t getPortOid(std::string key);
    FlexCounterManager m_nhgm_counter_manager;
    void doTask(Consumer &consumer);
    void handleSetCommand(const std::string& key, const std::vector<swss::FieldValueTuple>& data);
    void create_nexthop_group_counters(const std::string &nhg_key, const std::string &oid);
    void handleDelCommand(const std::string& key);
    void delete_nexthop_group_counters(const std::string &nhg_key);
    sai_object_id_t create_counter(std::string nh_str, std::string nhgm_str, std::string nhg_key);
    void delete_counter(sai_object_id_t counter_id);

};
