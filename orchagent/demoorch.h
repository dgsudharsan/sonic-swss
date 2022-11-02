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


class DemoOrch : public Orch
{
public:
    DemoOrch(swss::DBConnector *db, std::string tableName);

private:
    sai_object_id_t getPortOid(std::string key);
    FlexCounterManager m_nhgm_counter_manager;
    void doTask(Consumer &consumer);
    void handleSetCommand(const std::string& key, const std::vector<swss::FieldValueTuple>& data);
    void create_nexthop_group_counters(const std::string &nhg_key, const std::string &oid);
};
