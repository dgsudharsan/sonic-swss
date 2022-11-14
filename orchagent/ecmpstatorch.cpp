#include <sstream>
#include <inttypes.h>

#include "ecmpstatorch.h"
#include "dbconnector.h"
#include "flex_counter_manager.h"
#include "flow_counter_handler.h"
#include <unistd.h>
#include <algorithm>
#include "tokenize.h"
#include "timer.h"

extern sai_object_id_t gSwitchId;
extern sai_switch_api_t *sai_switch_api;
extern sai_port_api_t *sai_port_api;
extern sai_next_hop_group_api_t* sai_next_hop_group_api;
extern sai_next_hop_api_t* sai_next_hop_api;

using namespace std;
using namespace swss;
#define NHGM_TRAP_FLEX_COUNTER_GROUP "NHGM_TRAP_FLOW_COUNTER"
#define FLEX_COUNTER_UPD_INTERVAL 1

EcmpStatOrch::EcmpStatOrch(DBConnector *db, string tableName):
    Orch(db, tableName),
    m_nhgm_counter_manager(NHGM_TRAP_FLEX_COUNTER_GROUP, StatsMode::READ, 1000, true)
{
    m_asic_db = std::shared_ptr<DBConnector>(new DBConnector("ASIC_DB", 0));
    m_vidToRidTable = std::unique_ptr<Table>(new Table(m_asic_db.get(), "VIDTORID"));
    auto intervT = timespec { .tv_sec = FLEX_COUNTER_UPD_INTERVAL , .tv_nsec = 0 };
    m_FlexCounterUpdTimer = new SelectableTimer(intervT);
    auto executorT = new ExecutableTimer(m_FlexCounterUpdTimer, this, "FLEX_COUNTER_UPD_TIMER");
    Orch::addExecutor(executorT);
}

sai_object_id_t EcmpStatOrch::getPortOid(string key)
{
    DBConnector counters_db("COUNTERS_DB", 0);
    Table port_map(&counters_db, "COUNTERS_PORT_NAME_MAP");

    std::vector<FieldValueTuple> fvs;
    port_map.get("", fvs);
     
    for(auto fv: fvs)
    {
        if(fvField(fv) == key)
        {
            sai_object_id_t oid;
            sai_deserialize_object_id(fvValue(fv), oid);
            if (oid == SAI_NULL_OBJECT_ID)
            {
                SWSS_LOG_ERROR("Failed to deserialize: %s", fvValue(fv).c_str());
                return SAI_NULL_OBJECT_ID;
            }
            return oid;
        }
    }
    return SAI_NULL_OBJECT_ID;
}


void EcmpStatOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string key = kfvKey(t);
        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            handleSetCommand(key, kfvFieldsValues(t));
        }
        else if (op == DEL_COMMAND)
        {
            handleDelCommand(key);
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s\n", op.c_str());
        }

        consumer.m_toSync.erase(it++);
    }
}

void EcmpStatOrch::doTask(SelectableTimer &timer)
{
    SWSS_LOG_ENTER();

    string value;
    for (auto it = m_pendingAddToFlexCntr.begin(); it != m_pendingAddToFlexCntr.end(); )
    {
        const auto id = sai_serialize_object_id(it->first);
        if (m_vidToRidTable->hget("", id, value))
        {
            SWSS_LOG_NOTICE("Registering %s, id %s", it->second.c_str(), id.c_str());

            std::unordered_set<std::string> counter_stats;
            FlowCounterHandler::getGenericCounterStatIdList(counter_stats);
            m_nhgm_counter_manager.setCounterIdList(it->first, CounterType::FLOW_COUNTER, counter_stats);
            it = m_pendingAddToFlexCntr.erase(it);
        }
        else
        {
            ++it;
        }
    }

    if (m_pendingAddToFlexCntr.empty())
    {
        m_FlexCounterUpdTimer->stop();
    }
}

sai_object_id_t EcmpStatOrch::create_counter(string nh_str, string nhgm_str, string nhg_key)
{
    sai_object_id_t nh_obj;
    sai_deserialize_object_id(nh_str, nh_obj);
    string nh_ip;
    sai_attribute_t attr;
    DBConnector counter_db("COUNTERS_DB", 0);
    Table *m_counter_table = new Table(&counter_db, "COUNTERS_NHGM_NAME_MAP");

    memset(&attr, 0, sizeof(attr));
    attr.id = SAI_NEXT_HOP_ATTR_IP;

    sai_status_t status = sai_next_hop_api->get_next_hop_attribute(nh_obj, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Unable to obtain ip address for nh: %" PRIx64 "", nh_obj);
        return SAI_NULL_OBJECT_ID;
    }
    nh_ip = sai_serialize_ip_address(attr.value.ipaddr);
    sai_object_id_t counter_id = SAI_NULL_OBJECT_ID;
    if (!FlowCounterHandler::createGenericCounter(counter_id))
    {
        return SAI_NULL_OBJECT_ID;
    }
    memset(&attr, 0, sizeof(attr));
    attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_COUNTER_ID;
    attr.value.oid = counter_id;

    sai_object_id_t nhgm_oid;
    sai_deserialize_object_id(nhgm_str, nhgm_oid);

    status = sai_next_hop_group_api->set_next_hop_group_member_attribute(nhgm_oid,
                                                                     &attr);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Unable to set group member attr for oid %" PRIx64 "",nhgm_oid);
        return SAI_NULL_OBJECT_ID;
    }
    string counter_str = sai_serialize_object_id(counter_id);
    SWSS_LOG_NOTICE("counter %s nh_ip %s", counter_str.c_str(), nh_ip.c_str());
                    
    if (!nh_ip.empty())
    {
        auto map_key = nhg_key + ":" + nh_ip; 
        vector<FieldValueTuple> nameMapFvs;
        nameMapFvs.emplace_back(map_key, counter_str);
        m_counter_table->set("", nameMapFvs);
        auto was_empty = m_pendingAddToFlexCntr.empty();
        m_pendingAddToFlexCntr[counter_id] = map_key;

        if (was_empty)
        {
            m_FlexCounterUpdTimer->start();
        }
    }
    return counter_id;
}

void EcmpStatOrch::delete_counter(sai_object_id_t counter_id)
{
    DBConnector counter_db("COUNTERS_DB", 0);
    Table *m_counter_table = new Table(&counter_db, "COUNTERS_NHGM_NAME_MAP");

    auto counter_str = sai_serialize_object_id(counter_id);

    auto update_iter = m_pendingAddToFlexCntr.find(counter_id);
    if (update_iter == m_pendingAddToFlexCntr.end())
    {
        m_nhgm_counter_manager.clearCounterIdList(counter_id);
    }
    else
    {
        m_pendingAddToFlexCntr.erase(update_iter);
    }

    SWSS_LOG_NOTICE("Deleting counter %s", counter_str.c_str());
    vector<FieldValueTuple> counterMapFvs;
    m_counter_table->get("", counterMapFvs);
    for  (auto j: counterMapFvs)
    {
	const auto &cfield = fvField(j);
	const auto &cvalue = fvValue(j);
	if (cvalue == counter_str)
	{
            m_counter_table->hdel("", cfield);
            break;
        }
    }

    if (!FlowCounterHandler::removeGenericCounter(counter_id))
    {
        SWSS_LOG_ERROR("Unable to remove counter id: %" PRIx64 "", counter_id);
        return;
    }
}

void EcmpStatOrch::delete_nexthop_group_counters(const string &nhg_key)
{
    SWSS_LOG_NOTICE("Handle deleting key %s", nhg_key.c_str());

    for (const auto &nh: nhg_to_nh_map[nhg_key])
    {
        delete_counter(counters_oid_map[nh]);
        SWSS_LOG_NOTICE("Cleard counter fo nh : %s ", nh.c_str());
        counters_oid_map.erase(nh);
    }
    nhg_to_nh_map.erase(nhg_key);
}

string EcmpStatOrch::get_nhg_oid(string oid_list)
{
    vector<string> oids = tokenize(oid_list, ',');
    sai_object_id_t sai_oid;

    for (auto &oid: oids)
    {
        sai_deserialize_object_id(oid, sai_oid);
        if (sai_object_type_query(sai_oid) == SAI_OBJECT_TYPE_NEXT_HOP_GROUP)
        {
            return oid;
        }
    }
    return "";
}

void EcmpStatOrch::create_nexthop_group_counters(const string &nhg_key, const string &oid)
{
    string nhgm_table = "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER";
    Table *nhgmTable = new Table(m_asic_db.get(), nhgm_table);
    std::vector<string> keys;
    nhgmTable->getKeys(keys);
    std::vector<string> nh_ids;
    std::map<string, string> nhgm_map;

    for (const auto &key: keys)
    {
        std::vector<FieldValueTuple> nhgm_fvs;
        nhgmTable->get(key, nhgm_fvs);

        string nh_id = "", nhg_id = "";
        for (auto i : nhgm_fvs)
        {
            const auto &field = fvField(i);
            const auto &value = fvValue(i);

            if (field == "SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID")
            {
                nhg_id = value;
            }
            else if (field == "SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID")
            {
                nh_id = value;
                nhgm_map[nh_id] = key;
            }
        }

        if (nhg_id == oid)
        {
            nh_ids.emplace_back(nh_id);
        }

    }

    for (auto &nh: nhg_to_nh_map[nhg_key])
    {
        if (std::find(nh_ids.begin(), nh_ids.end(), nh) == nh_ids.end())
        {
            delete_counter(counters_oid_map[nh]);
            counters_oid_map.erase(nh);
        }
    }

    for (auto &nh: nh_ids)
    {
        if (std::find(nhg_to_nh_map[nhg_key].begin(), nhg_to_nh_map[nhg_key].end(), nh) == nhg_to_nh_map[nhg_key].end())
        {
            auto counter_id = create_counter(nh, nhgm_map[nh], nhg_key);
            if (counter_id != SAI_NULL_OBJECT_ID)
            {
                counters_oid_map[nh] = counter_id;
            }
        }
    }
    nhg_to_nh_map[nhg_key] = nh_ids;

}

void EcmpStatOrch::handleSetCommand(const string& key, const vector<FieldValueTuple>& data)
{
    SWSS_LOG_ENTER();

    for (auto i : data)
    {
        const auto &field = fvField(i);
        const auto &value = fvValue(i);

        try
        {
            if (field == "oid")
            {
                auto nh_oid = get_nhg_oid(value);
                if (nh_oid == "")
                {
                    delete_nexthop_group_counters(key);
                }
                else
                {
                    create_nexthop_group_counters(key, nh_oid);
                }
            }
        }
        catch (const exception& e)
        {
            SWSS_LOG_ERROR("Failed to parse %s attribute %s error: %s.", key.c_str(), field.c_str(), e.what());
            return;
        }
        catch (...)
        {
            SWSS_LOG_ERROR("Failed to parse %s attribute %s. Unknown error has been occurred", key.c_str(), field.c_str());
            return;
        }
    }
}

void EcmpStatOrch::handleDelCommand(const string& key)
{
    SWSS_LOG_ENTER();
    delete_nexthop_group_counters(key);
}
