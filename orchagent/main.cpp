extern "C" {
#include "sai.h"
#include "saistatus.h"
}

#include <fstream>
#include <iostream>
#include <unordered_map>
#include <map>
#include <memory>
#include <thread>
#include <chrono>
#include <getopt.h>
#include <unistd.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include <sys/time.h>
#include "timestamp.h"

#include <sairedis.h>
#include <logger.h>

#include "orchdaemon.h"
#include "sai_serialize.h"
#include "saihelper.h"
#include "notifications.h"
#include <signal.h>
#include "warm_restart.h"
#include "gearboxutils.h"

using namespace std;
using namespace swss;

extern sai_switch_api_t *sai_switch_api;
extern sai_router_interface_api_t *sai_router_intfs_api;

#define UNREFERENCED_PARAMETER(P)       (P)

#define UNDERLAY_RIF_DEFAULT_MTU 9100

/* Global variables */
sai_object_id_t gVirtualRouterId;
sai_object_id_t gUnderlayIfId;
sai_object_id_t gSwitchId = SAI_NULL_OBJECT_ID;
MacAddress gMacAddress;
MacAddress gVxlanMacAddress;

extern size_t gMaxBulkSize;

#define DEFAULT_BATCH_SIZE  128
int gBatchSize = DEFAULT_BATCH_SIZE;

bool gSairedisRecord = true;
bool gSwssRecord = true;
bool gLogRotate = false;
bool gSaiRedisLogRotate = false;
bool gSyncMode = false;
sai_redis_communication_mode_t gRedisCommunicationMode = SAI_REDIS_COMMUNICATION_MODE_REDIS_ASYNC;
string gAsicInstance;

extern bool gIsNatSupported;

ofstream gRecordOfs;
string gRecordFile;

void usage()
{
    cout << "usage: orchagent [-h] [-r record_type] [-d record_location] [-f swss_rec_filename] [-j sairedis_rec_filename] [-b batch_size] [-m MAC] [-i INST_ID] [-s] [-z mode] [-k bulk_size]" << endl;
    cout << "    -h: display this message" << endl;
    cout << "    -r record_type: record orchagent logs with type (default 3)" << endl;
    cout << "                    0: do not record logs" << endl;
    cout << "                    1: record SAI call sequence as sairedis.rec" << endl;
    cout << "                    2: record SwSS task sequence as swss.rec" << endl;
    cout << "                    3: enable both above two records" << endl;
    cout << "    -d record_location: set record logs folder location (default .)" << endl;
    cout << "    -b batch_size: set consumer table pop operation batch size (default 128)" << endl;
    cout << "    -m MAC: set switch MAC address" << endl;
    cout << "    -i INST_ID: set the ASIC instance_id in multi-asic platform" << endl;
    cout << "    -s: enable synchronous mode (deprecated, use -z)" << endl;
    cout << "    -z: redis communication mode (redis_async|redis_sync|zmq_sync), default: redis_async" << endl;
    cout << "    -k max bulk size in bulk mode (default 1000)";
    cout << "    -f swss_rec_filename: swss record log filename(default 'swss.rec')" << endl;
    cout << "    -j sairedis_rec_filename:  sairedis record log filename(default sairedis.rec)" << endl;
}

void sighup_handler(int signo)
{
    /*
     * Don't do any logging since they are using mutexes.
     */
    gLogRotate = true;
    gSaiRedisLogRotate = true;
}

void syncd_apply_view()
{
    SWSS_LOG_NOTICE("Notify syncd APPLY_VIEW");

    sai_status_t status;
    sai_attribute_t attr;
    attr.id = SAI_REDIS_SWITCH_ATTR_NOTIFY_SYNCD;
    attr.value.s32 = SAI_REDIS_NOTIFY_SYNCD_APPLY_VIEW;
    status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to notify syncd APPLY_VIEW %d", status);
        exit(EXIT_FAILURE);
    } 
}

/*
 * If Gearbox is enabled...
 * Create and initialize the external Gearbox PHYs. Upon success, store the
 * new PHY OID in the database to be used later when creating the Gearbox
 * ports.
*/
void init_gearbox_phys(DBConnector *applDb)
{
    Table* tmpGearboxTable = new Table(applDb, "_GEARBOX_TABLE");
    map<int, gearbox_phy_t> gearboxPhyMap;
    GearboxUtils gearbox;

    if (gearbox.isGearboxEnabled(tmpGearboxTable))
    {
        gearboxPhyMap = gearbox.loadPhyMap(tmpGearboxTable);
        SWSS_LOG_DEBUG("BOX: gearboxPhyMap size = %d.", (int) gearboxPhyMap.size());

        for (auto it = gearboxPhyMap.begin(); it != gearboxPhyMap.end(); ++it)
        {
            SWSS_LOG_NOTICE("BOX: Initialize PHY %d.", it->first);

            if (initSaiPhyApi(&it->second) != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("BOX: Failed to initialize PHY %d.", it->first);
            }
            else
            {
                SWSS_LOG_NOTICE("BOX: Created new PHY phy_id:%d phy_oid:%s.", it->second.phy_id, it->second.phy_oid.c_str());
                tmpGearboxTable->hset("phy:"+to_string(it->second.phy_id), "phy_oid", it->second.phy_oid.c_str());
                tmpGearboxTable->hset("phy:"+to_string(it->second.phy_id), "firmware_major_version", it->second.firmware_major_version.c_str());
            }
        }
    }
    delete tmpGearboxTable;
}

int main(int argc, char **argv)
{
    swss::Logger::linkToDbNative("orchagent");

    SWSS_LOG_ENTER();

    WarmStart::initialize("orchagent", "swss");
    WarmStart::checkWarmStart("orchagent", "swss");

    if (signal(SIGHUP, sighup_handler) == SIG_ERR)
    {
        SWSS_LOG_ERROR("failed to setup SIGHUP action");
        exit(1);
    }

    int opt;
    sai_status_t status;

    string record_location = ".";
    string swss_rec_filename = "swss.rec";
    string sairedis_rec_filename = "sairedis.rec";

    while ((opt = getopt(argc, argv, "b:m:r:f:j:d:i:hsz:k:")) != -1)
    {
        switch (opt)
        {
        case 'b':
            gBatchSize = atoi(optarg);
            break;
        case 'i':
            {
                // Limit asic instance string max length
                size_t len = strnlen(optarg, SAI_MAX_HARDWARE_ID_LEN);
                // Check if input is longer and warn
                if (len == SAI_MAX_HARDWARE_ID_LEN && optarg[len+1] != '\0')
                {
                    SWSS_LOG_WARN("ASIC instance_id length > SAI_MAX_HARDWARE_ID_LEN, LIMITING !!");
                }
                // If longer, truncate into a string
                gAsicInstance.assign(optarg, len);
            }
            break;
        case 'm':
            gMacAddress = MacAddress(optarg);
            break;
        case 'r':
            if (!strcmp(optarg, "0"))
            {
                gSairedisRecord = false;
                gSwssRecord = false;
            }
            else if (!strcmp(optarg, "1"))
            {
                gSwssRecord = false;
            }
            else if (!strcmp(optarg, "2"))
            {
                gSairedisRecord = false;
            }
            else if (!strcmp(optarg, "3"))
            {
                continue; /* default behavior */
            }
            else
            {
                usage();
                exit(EXIT_FAILURE);
            }
            break;
        case 'd':
            record_location = optarg;
            if (access(record_location.c_str(), W_OK))
            {
                SWSS_LOG_ERROR("Failed to access writable directory %s", record_location.c_str());
                exit(EXIT_FAILURE);
            }
            break;
        case 'h':
            usage();
            exit(EXIT_SUCCESS);
        case 's':
            gSyncMode = true;
            SWSS_LOG_NOTICE("Enabling synchronous mode");
            break;
        case 'z':
            sai_deserialize_redis_communication_mode(optarg, gRedisCommunicationMode);
            break;
        case 'k':
            {
                auto limit = atoi(optarg);
                if (limit > 0)
                {
                    gMaxBulkSize = limit;
                    SWSS_LOG_NOTICE("Setting maximum bulk size in bulk mode as %zu", gMaxBulkSize);
                }
                else
                {
                    SWSS_LOG_ERROR("Invalid input for maximum bulk size in bulk mode: %d. Ignoring.", limit);
                }
            }
            break;
         case 'f':
            if (optarg)
             {
                swss_rec_filename = optarg;
             }
             break;
         case 'j':
            if (optarg)
             {
                sairedis_rec_filename = optarg;
             }
             break;
        default: /* '?' */
            exit(EXIT_FAILURE);
        }
    }

    SWSS_LOG_NOTICE("--- Starting Orchestration Agent ---");

    initSaiApi();
    initSaiRedis(record_location, sairedis_rec_filename);

    sai_attribute_t attr;
    vector<sai_attribute_t> attrs;

    attr.id = SAI_SWITCH_ATTR_INIT_SWITCH;
    attr.value.booldata = true;
    attrs.push_back(attr);
    attr.id = SAI_SWITCH_ATTR_FDB_EVENT_NOTIFY;
    attr.value.ptr = (void *)on_fdb_event;
    attrs.push_back(attr);

    /* Disable/enable SwSS recording */
    if (gSwssRecord)
    {
        gRecordFile = record_location + "/" + swss_rec_filename;
        gRecordOfs.open(gRecordFile, std::ofstream::out | std::ofstream::app);
        if (!gRecordOfs.is_open())
        {
            SWSS_LOG_ERROR("Failed to open SwSS recording file %s", gRecordFile.c_str());
            exit(EXIT_FAILURE);
        }
        gRecordOfs << getTimestamp() << "|recording started" << endl;
    }

    attr.id = SAI_SWITCH_ATTR_PORT_STATE_CHANGE_NOTIFY;
    attr.value.ptr = (void *)on_port_state_change;
    attrs.push_back(attr);

    attr.id = SAI_SWITCH_ATTR_BFD_SESSION_STATE_CHANGE_NOTIFY;
    attr.value.ptr = (void *)on_bfd_session_state_change;
    attrs.push_back(attr);

    attr.id = SAI_SWITCH_ATTR_SHUTDOWN_REQUEST_NOTIFY;
    attr.value.ptr = (void *)on_switch_shutdown_request;
    attrs.push_back(attr);

    if (gMacAddress)
    {
        attr.id = SAI_SWITCH_ATTR_SRC_MAC_ADDRESS;
        memcpy(attr.value.mac, gMacAddress.getMac(), 6);
        attrs.push_back(attr);
    }

    // SAI_REDIS_SWITCH_ATTR_SYNC_MODE attribute only setBuffer and g_syncMode to true
    // since it is not using ASIC_DB, we can execute it before create_switch
    // when g_syncMode is set to true here, create_switch will wait the response from syncd
    if (gSyncMode)
    {
        SWSS_LOG_WARN("sync mode is depreacated, use -z param");

        gRedisCommunicationMode = SAI_REDIS_COMMUNICATION_MODE_REDIS_SYNC;
    }

    attr.id = SAI_REDIS_SWITCH_ATTR_REDIS_COMMUNICATION_MODE;
    attr.value.s32 = gRedisCommunicationMode;

    sai_switch_api->set_switch_attribute(gSwitchId, &attr);

    if (!gAsicInstance.empty())
    {
        attr.id = SAI_SWITCH_ATTR_SWITCH_HARDWARE_INFO;
        attr.value.s8list.count = (uint32_t)gAsicInstance.size();
        // TODO: change SAI definition of `list` to `const char *`
        attr.value.s8list.list = (int8_t *)const_cast<char *>(gAsicInstance.c_str());
        attrs.push_back(attr);
    }

    /* Must be last Attribute */
    attr.id = SAI_REDIS_SWITCH_ATTR_CONTEXT;
    attr.value.u64 = gSwitchId;
    attrs.push_back(attr);

    status = sai_switch_api->create_switch(&gSwitchId, (uint32_t)attrs.size(), attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create a switch, rv:%d", status);
        exit(EXIT_FAILURE);
    }
    SWSS_LOG_NOTICE("Create a switch, id:%" PRIu64, gSwitchId);

    /* Get switch source MAC address if not provided */
    if (!gMacAddress)
    {
        attr.id = SAI_SWITCH_ATTR_SRC_MAC_ADDRESS;
        status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to get MAC address from switch, rv:%d", status);
            exit(EXIT_FAILURE);
        }
        else
        {
            gMacAddress = attr.value.mac;
        }
    }

    /* Get the default virtual router ID */
    attr.id = SAI_SWITCH_ATTR_DEFAULT_VIRTUAL_ROUTER_ID;

    status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Fail to get switch virtual router ID %d", status);
        exit(EXIT_FAILURE);
    }

    gVirtualRouterId = attr.value.oid;
    SWSS_LOG_NOTICE("Get switch virtual router ID %" PRIx64, gVirtualRouterId);

    /* Get the NAT supported info */
    attr.id = SAI_SWITCH_ATTR_AVAILABLE_SNAT_ENTRY;

    status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_NOTICE("Failed to get the SNAT available entry count, rv:%d", status);
    }
    else
    {
        if (attr.value.u32 != 0)
        {
            gIsNatSupported = true;
        }
    }

    /* Create a loopback underlay router interface */
    vector<sai_attribute_t> underlay_intf_attrs;

    sai_attribute_t underlay_intf_attr;
    underlay_intf_attr.id = SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID;
    underlay_intf_attr.value.oid = gVirtualRouterId;
    underlay_intf_attrs.push_back(underlay_intf_attr);

    underlay_intf_attr.id = SAI_ROUTER_INTERFACE_ATTR_TYPE;
    underlay_intf_attr.value.s32 = SAI_ROUTER_INTERFACE_TYPE_LOOPBACK;
    underlay_intf_attrs.push_back(underlay_intf_attr);

    underlay_intf_attr.id = SAI_ROUTER_INTERFACE_ATTR_MTU;
    underlay_intf_attr.value.u32 = UNDERLAY_RIF_DEFAULT_MTU;
    underlay_intf_attrs.push_back(underlay_intf_attr);

    status = sai_router_intfs_api->create_router_interface(&gUnderlayIfId, gSwitchId, (uint32_t)underlay_intf_attrs.size(), underlay_intf_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create underlay router interface %d", status);
        exit(EXIT_FAILURE);
    }

    SWSS_LOG_NOTICE("Created underlay router interface ID %" PRIx64, gUnderlayIfId);

    /* Initialize orchestration components */
    DBConnector appl_db("APPL_DB", 0);
    DBConnector config_db("CONFIG_DB", 0);
    DBConnector state_db("STATE_DB", 0);

    init_gearbox_phys(&appl_db);

    auto orchDaemon = make_shared<OrchDaemon>(&appl_db, &config_db, &state_db);

    if (!orchDaemon->init())
    {
        SWSS_LOG_ERROR("Failed to initialize orchestration daemon");
        exit(EXIT_FAILURE);
    }

    /*
    * In syncd view comparison solution, apply view has been sent
    * immediately after restore is done
    */
    if (!WarmStart::isWarmStart())
    {
        syncd_apply_view();
    }

    orchDaemon->start();

    return 0;
}
