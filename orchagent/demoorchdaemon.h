#ifndef SWSS_ORCHDAEMON_H
#define SWSS_ORCHDAEMON_H

#include "dbconnector.h"
#include "producerstatetable.h"
#include "consumertable.h"
#include "select.h"

#include "demoorch.h"
#include <sairedis.h>

using namespace swss;

class DemoOrchDaemon
{
public:
    DemoOrchDaemon(DBConnector *, DBConnector *, DBConnector *);
    ~DemoOrchDaemon();

    virtual bool init();
    void start();
private:
    DBConnector *m_applDb;
    DBConnector *m_asicDb;
    DBConnector *m_stateDb;

    std::vector<Orch *> m_orchList;
    Select *m_select;
    bool isPortInitDone();
    void initSwitchId();

};

#endif /* SWSS_ORCHDAEMON_H */
