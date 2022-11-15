#include <inttypes.h>
#include <string.h>
#include <fstream>
#include <map>
#include <logger.h>
#include <sairedis.h>
#include <set>
#include <tuple>
#include <vector>
#include <linux/limits.h>
#include <net/if.h>
#include "timestamp.h"
#include "appentrycontext.h"

using namespace std;
using namespace swss;

AppEntryContext& AppEntryContext::getInstance()
{
    static AppEntryContext context;
    return context;
}

void AppEntryContext::reset_key()
{
    auto& c = getInstance();
    c._key = "";
    /* c._oids.clear(); */
}

void AppEntryContext::reset_table_name()
{
    auto& c = getInstance();
    c._table_name = "";
    /* c._oids.clear(); */
}

void AppEntryContext::set_table_name(std::string table_name)
{
    auto& c = getInstance();
    c._table_name = table_name;
}

void AppEntryContext::set_key(std::string key)
{
    auto& c = getInstance();
    c._key = key;
}

std::string AppEntryContext::get()
{
    auto& c = getInstance();
    return c._table_name + ":" + c._key;
}

AppEntryContext::AppEntryContext() : _db("STATE_DB", 0)
{}

void AppEntryContext::add_oid(sai_object_id_t oid)
{
    auto& c = getInstance();
    if (c._oid_map.find(c.get()) == c._oid_map.end())
    {
        std::unordered_set<sai_object_id_t> oids;
        oids.emplace(oid);
        c._oid_map.emplace(c.get(), oids);
    }
    else
    {
        c._oid_map[c.get()].emplace(oid);
    }
    /* c._oids.emplace(oid); */
    SWSS_LOG_INFO("Creating oid %s %" PRIx64 "",c._key.c_str(), oid);
    write_db();
}

void AppEntryContext::remove_oid(sai_object_id_t oid)
{
    auto& c = getInstance();
    if (c._oid_map.find(c.get()) == c._oid_map.end())
    {
        SWSS_LOG_ERROR("Erasing oid from key that doesn't exist?");
        return;
    }
    c._oid_map[c.get()].erase(oid);
    /* c._oids.erase(oid); */
    SWSS_LOG_INFO("Removing oid %s %" PRIx64 "",c._key.c_str(), oid);
    write_db();
    if (c._oid_map[c.get()].empty())
    {
        c._oid_map.erase(c.get());
    }
}

void AppEntryContext::write_db()
{
    auto& c = getInstance();
    if (c._tables.find(c._table_name) == c._tables.end())
    {
        c._tables[c._table_name] = std::make_unique<swss::Table>(&c._db, c._table_name);
    }

    if (c._oid_map[c.get()].empty())
    {
        c._tables[c._table_name]->del(c._key);
    }
    else
    {
        std::vector<swss::FieldValueTuple> attrs;
        std::string oids_str;
        char oid_str[64];
        auto oids = c._oid_map[c.get()];

        for (auto oid : oids)
        {
            sprintf(oid_str, "0x%lx,", oid);
            oids_str += oid_str;
        }

        attrs.push_back(swss::FieldValueTuple("oids", oids_str));
        c._tables[c._table_name]->set(c._key, attrs);
    }
}
