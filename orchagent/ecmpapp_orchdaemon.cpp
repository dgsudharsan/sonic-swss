#include <unistd.h>
#include <unordered_map>
#include <chrono>
#include <limits.h>
#include "ecmpapp_orchdaemon.h"
#include "logger.h"
#include <sairedis.h>

#include "sairedis.h"
#include "ecmpstatorch.h"
#include "table.h"

using namespace std;
using namespace swss;

/* select() function timeout retry time */
#define SELECT_TIMEOUT 1000
#define APP_PORT_EXT_TABLE "PORT_EXT"

extern sai_switch_api_t*           sai_switch_api;
extern sai_object_id_t             gSwitchId;

/*
 * Global orch daemon variables
 */
EcmpStatOrch *gEcmpStatOrch;

EcmpAppOrchDaemon::EcmpAppOrchDaemon(DBConnector *applDb, DBConnector *asicDb, DBConnector *stateDb) :
        m_applDb(applDb),
        m_asicDb(asicDb),
        m_stateDb(stateDb)
{
    SWSS_LOG_ENTER();
    m_select = new Select();
}

EcmpAppOrchDaemon::~EcmpAppOrchDaemon()
{
    SWSS_LOG_ENTER();

    /*
     * Some orchagents call other agents in their destructor.
     * To avoid accessing deleted agent, do deletion in reverse order.
     * NOTE: This is still not a robust solution, as order in this list
     *       does not strictly match the order of construction of agents.
     * For a robust solution, first some cleaning/house-keeping in
     * orchagents management is in order.
     * For now it fixes, possible crash during process exit.
     */
    auto it = m_orchList.rbegin();
    for(; it != m_orchList.rend(); ++it) {
        delete(*it);
    }
    delete m_select;
}
/* To check the port init is done or not */
bool EcmpAppOrchDaemon::isPortInitDone()
{
    bool portInit = 0;
    long cnt = 0;

    while(!portInit) {
        Table  *portTable = new Table(m_applDb, APP_PORT_TABLE_NAME);
        std::vector<FieldValueTuple> tuples;
        portInit = portTable->get("PortInitDone", tuples);

        if(portInit)
            break;
        sleep(1);
        cnt++;
    }
    SWSS_LOG_NOTICE("PORT_INIT_DONE : %d %ld", portInit, cnt);
    return portInit;
}

void EcmpAppOrchDaemon::initSwitchId()
{
    string switch_table = "ASIC_STATE:SAI_OBJECT_TYPE_SWITCH";
    Table *swTable = new Table(m_asicDb, switch_table);
    std::vector<string> keys;
    swTable->getKeys(keys);

    sai_deserialize_object_id(keys[0], gSwitchId);
    SWSS_LOG_NOTICE("Switch_id : %s", keys[0].c_str());
}
bool EcmpAppOrchDaemon::init()
{
    SWSS_LOG_ENTER();


    if(!isPortInitDone())
    {
        SWSS_LOG_ERROR("Port init failed. Exiting");
        return false;
    }
    initSwitchId();
    gEcmpStatOrch = new EcmpStatOrch(m_stateDb, "NEXTHOP_GROUP_TABLE");
    m_orchList = {gEcmpStatOrch};


    return true;
}

void EcmpAppOrchDaemon::start()
{
    SWSS_LOG_ENTER();

    for (Orch *o : m_orchList)
    {
        m_select->addSelectables(o->getSelectables());
    }


    while (true)
    {
        Selectable *s;
        int ret;

        ret = m_select->select(&s, SELECT_TIMEOUT);

        if (ret == Select::ERROR)
        {
            SWSS_LOG_NOTICE("Error: %s!\n", strerror(errno));
            continue;
        }

        if (ret == Select::TIMEOUT)
        {
            continue;
        }

        auto *c = (Executor *)s;
        c->execute();

        /* After each iteration, periodically check all m_toSync map to
         * execute all the remaining tasks that need to be retried. */

        /* TODO: Abstract Orch class to have a specific todo list */
        for (Orch *o : m_orchList)
            o->doTask();
    }
}
