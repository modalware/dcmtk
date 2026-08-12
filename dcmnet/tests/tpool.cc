/*
 *
 *  Copyright (C) 2013-2025, OFFIS e.V.
 *  All rights reserved.  See COPYRIGHT file for details.
 *
 *  This software and supporting documentation were developed by
 *
 *    OFFIS e.V.
 *    R&D Division Health
 *    Escherweg 2
 *    D-26121 Oldenburg, Germany
 *
 *
 *  Module:  dcmnet
 *
 *  Author:  Jan Schlamelcher
 *
 *  Purpose: Test DcmSCPPool class, including DcmSCP and DcmSCU interaction
 *
 */


#include "dcmtk/config/osconfig.h"    /* make sure OS specific configuration is included first */

#ifdef WITH_THREADS

#include "dcmtk/ofstd/oftest.h"
#include "dcmtk/dcmnet/scppool.h"
#include "dcmtk/dcmnet/scu.h"
#include "dcmtk/ofstd/ofthread.h"
#include "dcmtk/ofstd/ofstd.h"


const size_t NUM_THREADS = 20;

/** Configure the given SCU for connecting to the test pool SCP.
 *  @param scu  The SCU to configure
 *  @param port The port the pool is listening on
 */
static void configurePoolSCU(DcmSCU& scu, const Uint16 port)
{
    OFList<OFString> xfers;
    xfers.push_back(UID_LittleEndianExplicitTransferSyntax);
    xfers.push_back(UID_LittleEndianImplicitTransferSyntax);
    scu.setAETitle("PoolTestSCU");
    scu.setPeerAETitle("PoolTestSCP");
    scu.setPeerHostName("localhost");
    scu.setPeerPort(port);
    scu.addPresentationContext(UID_VerificationSOPClass, xfers);
    scu.initNetwork();
}

/** Perform a complete ECHO association (negotiate, C-ECHO, release)
 *  synchronously in the calling thread.
 *  @param port The port the pool is listening on
 *  @return EC_Normal if the whole association worked, an error otherwise
 */
static OFCondition syncEcho(const Uint16 port)
{
    DcmSCU scu;
    configurePoolSCU(scu, port);
    OFCondition result = scu.negotiateAssociation();
    if (result.good())
        result = scu.sendECHORequest(0);
    scu.releaseAssociation();
    return result;
}

struct TestSCU : DcmSCU, OFThread
{

    TestSCU() : m_result()
    {
        m_resultMutex.lock();
        m_result = EC_IllegalCall;
        m_resultMutex.unlock();
    }

    void getResult(OFCondition& result)
    {
        m_resultMutex.lock();
        result = m_result;
        m_resultMutex.unlock();
    }

protected:

    void run()
    {
        negotiateAssociation();
        m_resultMutex.lock();
        m_result = sendECHORequest(0);
        m_resultMutex.unlock();
        releaseAssociation();
    }

private:

    OFCondition m_result;
    OFMutex m_resultMutex;

};

struct TestPool : DcmSCPPool<>, OFThread
{
    OFCondition result;
protected:
    void run()
    {
        result = listen();
    }
};


/* Test starts pool with a maximum of 20 SCP workers. All workers are
 * configured to respond to C-ECHO (Verification SOP Class). 20 SCU
 * threads are created and connect simultaneously to the pool, send
 * C-ECHO messages and release the association.
 */
OFTEST_FLAGS(dcmnet_scp_pool, EF_Slow)
{
    TestPool pool;
    DcmSCPConfig& config = pool.getConfig();

    config.setAETitle("PoolTestSCP");
    config.setPort(11112);
    config.setConnectionBlockingMode(DUL_NOBLOCK);

    // Dead time during which the pool is unable to respond to
    // stopAfterCurrentAssociations().
    config.setConnectionTimeout(1);

    pool.setMaxThreads(NUM_THREADS);
    OFList<OFString> xfers;
    xfers.push_back(UID_LittleEndianExplicitTransferSyntax);
    xfers.push_back(UID_LittleEndianImplicitTransferSyntax);
    config.addPresentationContext(UID_VerificationSOPClass, xfers);

    pool.start();

    OFVector<TestSCU*> scus(NUM_THREADS, NULL);
    for (OFVector<TestSCU*>::iterator it1 = scus.begin(); it1 != scus.end(); ++it1)
    {
        *it1 = new TestSCU;
        (*it1)->setAETitle("PoolTestSCU");
        (*it1)->setPeerAETitle("PoolTestSCP");
        (*it1)->setPeerHostName("localhost");
        (*it1)->setPeerPort(11112);
        (*it1)->addPresentationContext(UID_VerificationSOPClass, xfers);
        (*it1)->initNetwork();
    }

    // "ensure" the pool is initialized before any SCU starts connecting to it. The initialization
    // can take a couple of seconds on older systems, e.g. debian i368.
    OFStandard::forceSleep(5);

    for (OFVector<TestSCU*>::const_iterator it2 = scus.begin(); it2 != scus.end(); ++it2)
        (*it2)->start();
    // Ensure the SCUs have time to connect and send requests also on slow systems
    OFStandard::forceSleep(5);

    for (OFVector<TestSCU*>::iterator it3 = scus.begin(); it3 != scus.end(); ++it3)
    {
        OFCondition scuResult;
        (*it3)->getResult(scuResult);
        (*it3)->join();
        delete *it3;
        (*it3) = NULL;
        OFCHECK(scuResult.good());
    }
    scus.clear();

    // Second round to check whether thread re-use works inside the pool
    for (OFVector<TestSCU*>::iterator it4 = scus.begin(); it4 != scus.end(); ++it4)
    {
        *it4 = new TestSCU;
        (*it4)->setAETitle("PoolTestSCU");
        (*it4)->setPeerAETitle("PoolTestSCP");
        (*it4)->setPeerHostName("localhost");
        (*it4)->setPeerPort(11112);
        (*it4)->addPresentationContext(UID_VerificationSOPClass, xfers);
        (*it4)->initNetwork();
    }

    for (OFVector<TestSCU*>::const_iterator it2 = scus.begin(); it2 != scus.end(); ++it2)
        (*it2)->start();

    for (OFVector<TestSCU*>::iterator it3 = scus.begin(); it3 != scus.end(); ++it3)
    {
        OFCondition scuResult;
        (*it3)->getResult(scuResult);
        OFCHECK(scuResult.good());
        (*it3)->join();
        delete *it3;
    }


    // Request shutdown.
    pool.stopAfterCurrentAssociations();
    pool.join();

    OFCHECK(pool.result.good());
}


/** SCU thread that connects to the pool, keeps the association open for the
 *  given number of seconds and only then sends C-ECHO and releases. Used to
 *  occupy one worker of the pool for a defined amount of time.
 */
struct HoldSCU : DcmSCU, OFThread
{
    HoldSCU(const Uint16 port, const Uint32 holdSeconds)
      : m_port(port), m_holdSeconds(holdSeconds), m_released(OFFalse),
        m_result(EC_IllegalCall), m_mutex()
    {
    }

    OFBool released()
    {
        m_mutex.lock();
        const OFBool result = m_released;
        m_mutex.unlock();
        return result;
    }

    void getResult(OFCondition& result)
    {
        m_mutex.lock();
        result = m_result;
        m_mutex.unlock();
    }

protected:

    void run()
    {
        configurePoolSCU(*this, m_port);
        OFCondition result = negotiateAssociation();
        if (result.good())
        {
            OFStandard::forceSleep(m_holdSeconds);
            result = sendECHORequest(0);
        }
        releaseAssociation();
        m_mutex.lock();
        m_released = OFTrue;
        m_result = result;
        m_mutex.unlock();
    }

private:

    Uint16 m_port;
    Uint32 m_holdSeconds;
    OFBool m_released;
    OFCondition m_result;
    OFMutex m_mutex;
};


/* Regression test: the pool must stay responsive while a worker that is not
 * the first one ever created is busy with a long-running association. In
 * earlier versions, a worker taken from the idle list executed the whole
 * association synchronously in the pool's listener thread, so that a single
 * slow client blocked the complete server (no new TCP/ACSE handshake could
 * complete until that client disconnected).
 */
OFTEST_FLAGS(dcmnet_scp_pool_concurrency, EF_Slow)
{
    const Uint16 port = 11113;
    TestPool pool;
    DcmSCPConfig& config = pool.getConfig();

    config.setAETitle("PoolTestSCP");
    config.setPort(port);
    config.setConnectionBlockingMode(DUL_NOBLOCK);
    config.setConnectionTimeout(1);
    pool.setMaxThreads(2);
    OFList<OFString> xfers;
    xfers.push_back(UID_LittleEndianExplicitTransferSyntax);
    xfers.push_back(UID_LittleEndianImplicitTransferSyntax);
    config.addPresentationContext(UID_VerificationSOPClass, xfers);

    pool.start();
    // "ensure" the pool is initialized before any SCU starts connecting
    OFStandard::forceSleep(5);

    // Let a first association complete, so that one worker has already
    // finished when the long-running association arrives
    OFCHECK(syncEcho(port).good());

    // Occupy one worker with an association that stays open for 10 seconds
    HoldSCU hold(port, 10);
    hold.start();
    OFStandard::forceSleep(3); // let the HoldSCU connect

    // The pool must serve another SCU *while* the first association is
    // still open, i.e. long before the HoldSCU releases
    OFCHECK(syncEcho(port).good());
    OFCHECK(!hold.released());

    hold.join();
    OFCondition holdResult;
    hold.getResult(holdResult);
    OFCHECK(holdResult.good());

    pool.stopAfterCurrentAssociations();
    pool.join();
    OFCHECK(pool.result.good());
}

#endif // WITH_THREADS
