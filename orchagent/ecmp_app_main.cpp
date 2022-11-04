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
#include <sstream>
#include <stdexcept>
#include <stdlib.h>
#include <string.h>

#include <sys/time.h>
#include "timestamp.h"

#include <sairedis.h>
#include <logger.h>

#include "ecmpapp_orchdaemon.h"
#include "sai_serialize.h"
#include "saihelper.h"
#include "notifications.h"
#include <signal.h>

using namespace std;
using namespace swss;
ofstream gRecordOfs;
string gRecordFile;
ofstream gResponsePublisherRecordOfs;
string gResponsePublisherRecordFile;

extern sai_switch_api_t *sai_switch_api;
extern sai_router_interface_api_t *sai_router_intfs_api;

bool gSairedisRecord = false;
bool gSwssRecord = false;
bool gResponsePublisherRecord = false;
bool gLogRotate = false;
bool gResponsePublisherLogRotate = false;
bool gSyncMode = false;
sai_redis_communication_mode_t gRedisCommunicationMode = SAI_REDIS_COMMUNICATION_MODE_REDIS_ASYNC;
string gAsicInstance;
sai_object_id_t gSwitchId = SAI_NULL_OBJECT_ID;

#define DEFAULT_BATCH_SIZE  128
int gBatchSize = DEFAULT_BATCH_SIZE;

int main(int argc, char **argv)
{
    swss::Logger::linkToDbNative("ecmpAppOrch");

    SWSS_LOG_ENTER();

    DBConnector appl_db("APPL_DB", 0);
    DBConnector asic_db("ASIC_DB", 0);
    DBConnector state_db("STATE_DB", 0);

    EcmpAppOrchDaemon *ecmpAppOrchDaemon = new EcmpAppOrchDaemon(&appl_db, &asic_db, &state_db);
    SWSS_LOG_NOTICE("--- Starting ECMP Application Orchestration Agent ---");

    
    initSaiApi(true);


    if (!ecmpAppOrchDaemon->init())
    {
        SWSS_LOG_ERROR("Failed to initialize orchestration daemon");
        exit(EXIT_FAILURE);
    }

    ecmpAppOrchDaemon->start();

    return 0;
}
