/*
 *
 *  Copyright (C) 2012-2026, OFFIS e.V.
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
 *  Author:  Michael Onken
 *
 *  Purpose: Class listening for association requests and managing a pool of
 *  worker threads that each are waiting to take over a single incoming
 *  association. Thus, the pool can serve as many associations
 *  simultaneously as the number of threads it is configured to create.
 *
 */

#include "dcmtk/config/osconfig.h" /* make sure OS specific configuration is included first */

#ifdef WITH_THREADS // Without threads pool does not make sense...

#include "dcmtk/dcmnet/scppool.h"
#include "dcmtk/dcmnet/diutil.h"
#include "dcmtk/dcmtls/tlslayer.h"

// ----------------------------------------------------------------------------

DcmBaseSCPPool::DcmBaseSCPPool()
  : m_criticalSection(),
    m_workersBusy(),
    m_workersIdle(),
    m_cfg(),
    m_maxWorkers(5),
    m_runMode( SHUTDOWN ) // LISTEN mode will be set, once actual listening will be started.
    // not implemented yet: m_workersBusyTimeout(60),
    // not implemented yet: m_waiting(),
{
}

// ----------------------------------------------------------------------------

DcmBaseSCPPool::~DcmBaseSCPPool()
{
  // Wait that we are in SHUTDOWN mode
  // Let busy threads finish their work and get moved from the busy list to the idle list.
  while (getRunMode() != SHUTDOWN || DcmBaseSCPPool::numThreads(OFTrue) != 0)
  {
    DCMNET_DEBUG("DcmBaseSCPPool: Destructor called, waiting for runMode to become SHUTDOWN (currently " << getRunMode() << ")");
    OFStandard::forceSleep(1);
  }
  DCMNET_DEBUG("DcmBaseSCPPool: Destructor called, cleaning up " << m_workersIdle.size() << " finished worker threads");
  // Now all workers must be in the list of finished workers which will be
  // joined and deleted now. Since we are in SHUTDOWN mode, no other thread
  // can modify the lists anymore.
  reapFinishedWorkers();
  DCMNET_DEBUG("DcmBaseSCPPool: Destructor finished");
}

// ----------------------------------------------------------------------------

OFCondition DcmBaseSCPPool::listen()
{
  setRunMode(LISTEN);

  /* Copy the config to a shared config that is shared by all workers. */
  DcmSharedSCPConfig sharedConfig(m_cfg);

  /* Initialize network, i.e. create an instance of T_ASC_Network*. */
  T_ASC_Network *network = NULL;
  OFCondition cond = initializeNetwork(&network);
  if(cond.bad())
  {
    finishListening();
    return cond;
  }

  /* Keep listening until we are asked to stop. Errors while handling a
   * single connection request are considered transient, i.e. the affected
   * association is refused but the server keeps running. */
  while ( getRunMode() == LISTEN )
  {
    // Join and delete worker threads that have finished their association
    reapFinishedWorkers();
    // Reset status
    cond = EC_Normal;
    // Every incoming connection is handled in a new association object
    T_ASC_Association *assoc = NULL;
    OFBool useSecureLayer = m_cfg.transportLayerEnabled();

    // Listen to a socket for timeout seconds for an association request, accepts TCP connection.
    cond = ASC_receiveAssociation(
        network,
        &assoc,
        m_cfg.getMaxReceivePDULength(),
        NULL,
        NULL,
        useSecureLayer,
        m_cfg.getConnectionBlockingMode(),
        OFstatic_cast(int, m_cfg.getConnectionTimeout()),
        m_cfg.getImplementationIdentification());

    /* If we have a connection request, try to find/create a worker to handle it */
    if (cond.good())
    {
      cond = runAssociation(assoc, sharedConfig);

      /* If anything goes wrong running the association: Refuse it and keep
       * listening. All errors that can occur here (all worker slots busy,
       * memory exhaustion, thread creation failure) are transient and must
       * not bring down the server. */
      if (cond.bad())
      {
        if (cond == NET_EC_SCPBusy)
        {
          rejectAssociation(assoc, ASC_REASON_SP_PRES_LOCALLIMITEXCEEDED);
        }
        else
        {
          DCMNET_WARN("DcmBaseSCPPool: Cannot start worker thread for incoming association ("
              << cond.text() << "), refusing association");
          rejectAssociation(assoc, ASC_REASON_SP_PRES_TEMPORARYCONGESTION);
        }
        dropAndDestroyAssociation(assoc);
      }
    }

    /* If error occurred while receiving association, clean up */
    else
    {
      /* Handle timeout and errors differently */
      if ( cond == DUL_NOASSOCIATIONREQUEST )
      {
        ASC_destroyAssociation( &assoc );
      }
      else
      {
        dropAndDestroyAssociation(assoc);
        DCMNET_ERROR("DcmBaseSCPPool: Error receiving association: " << cond.text());
      }
      // ... and keep listening ...
      cond = EC_Normal;
    }
  }
  // Log why we left the listen loop
  const runmode mode = getRunMode();
  if (mode == STOP)
    DCMNET_DEBUG("DcmBaseSCPPool: Leaving listen loop due to stop request.");
  else
    DCMNET_DEBUG("DcmBaseSCPPool: Leaving listen loop (runMode: " << mode << ")");

  finishListening();

  /* In the end, clean up the rest of the memory and drop network */
  ASC_dropNetwork(&network);

  return EC_Normal;
}

void DcmBaseSCPPool::stopAfterCurrentAssociations()
{
  m_criticalSection.lock();
  if (m_runMode == LISTEN )
    m_runMode = STOP;
  m_criticalSection.unlock();
}

DcmBaseSCPPool::runmode DcmBaseSCPPool::getRunMode()
{
  m_criticalSection.lock();
  const runmode mode = m_runMode;
  m_criticalSection.unlock();
  return mode;
}

void DcmBaseSCPPool::setRunMode(const runmode mode)
{
  m_criticalSection.lock();
  m_runMode = mode;
  m_criticalSection.unlock();
}

// ----------------------------------------------------------------------------

Uint16 DcmBaseSCPPool::getMaxThreads()
{
  return m_maxWorkers;
}

// ----------------------------------------------------------------------------

size_t DcmBaseSCPPool::numThreads(const OFBool onlyBusy)
{
  size_t result = 0;
  m_criticalSection.lock();
  if (!onlyBusy)
  {
    result = m_workersBusy.size() + m_workersIdle.size();
  }
  else
    result = m_workersBusy.size();
  m_criticalSection.unlock();
  return result;
}

// ----------------------------------------------------------------------------

void DcmBaseSCPPool::setMaxThreads(const Uint16 maxWorkers)
{
  m_maxWorkers = maxWorkers;
}

// ----------------------------------------------------------------------------

OFCondition DcmBaseSCPPool::runAssociation(T_ASC_Association *assoc,
                                           const DcmSharedSCPConfig& sharedConfig)
{
  OFCondition result = EC_Normal;
  DcmBaseSCPWorker *chosen = NULL;

  /* Check whether there is a free slot for another worker thread. Each
   * worker thread handles a single association and terminates afterwards;
   * finished workers are joined and deleted by the listen loop, so the
   * busy list alone reflects the number of active connections. */
  m_criticalSection.lock();
  if (m_workersBusy.size() >= m_maxWorkers)
  {
    DCMNET_DEBUG("DcmBaseSCPPool: Maximum number of busy worker threads reached (" << m_maxWorkers << "), cannot handle incoming association");
    result = NET_EC_SCPBusy;
  }
  else /* Else we can produce another worker */
  {
    DCMNET_DEBUG("DcmBaseSCPPool: Starting new DcmSCP worker thread");
    DcmBaseSCPWorker* const worker = createSCPWorker();
    if (!worker) /* Oops, we cannot allocate a new worker thread */
    {
      result = EC_MemoryExhausted;
    }
    else /* Configure worker thread */
    {
      m_workersBusy.push_back(worker);
      worker->setSharedConfig(sharedConfig);
      chosen = worker;
      DCMNET_DEBUG("DcmBaseSCPPool: Created new worker thread, now " << m_workersBusy.size() << " busy threads total");
    }
  }
  m_criticalSection.unlock();

  /* Hand association to worker and start its thread */
  if (result.good())
  {
    result = chosen->setAssociation(assoc);
    if (result.good() && (chosen->start() != 0))
    {
      result = NET_EC_CannotStartSCPThread;
    }
    if (result.bad())
    {
      /* The worker thread never ran: Remove the worker from the busy list
       * and delete it so that its pool slot does not leak and the
       * destructor does not wait for it forever. The caller refuses and
       * destroys the association. */
      m_criticalSection.lock();
      m_workersBusy.remove(chosen);
      m_criticalSection.unlock();
      delete chosen;
    }
  }
  /* Return to listen loop */
  return result;
}

// ----------------------------------------------------------------------------

void DcmBaseSCPPool::rejectAssociation(T_ASC_Association *assoc,
                                       const T_ASC_RejectParametersReason& reason)
{
  T_ASC_RejectParameters rej;
  rej.result = ASC_RESULT_REJECTEDTRANSIENT;
  rej.source = ASC_SOURCE_SERVICEPROVIDER_PRESENTATION_RELATED;
  rej.reason = reason;
  ASC_rejectAssociation( assoc, &rej );
}

// ----------------------------------------------------------------------------

void DcmBaseSCPPool::dropAndDestroyAssociation(T_ASC_Association *assoc)
{
  if (assoc)
  {
    ASC_dropAssociation( assoc );
    ASC_destroyAssociation( &assoc );
  }
  assoc = NULL;
}

// ----------------------------------------------------------------------------

DcmSCPConfig& DcmBaseSCPPool::getConfig()
{
  return m_cfg;
}

// ----------------------------------------------------------------------------

void DcmBaseSCPPool::notifyWorkerDone(DcmBaseSCPPool::DcmBaseSCPWorker* thread,
                                      OFCondition result)
{
  m_criticalSection.lock();
  if (result.bad())
  {
    DCMNET_DEBUG("DcmBaseSCPPool: Worker thread #" << thread->threadID() << " exited with error: " << result.text());
  }
  else
  {
    DCMNET_DEBUG("DcmBaseSCPPool: Worker thread #" << thread->threadID() << " finished successfully.");
  }
  // Move thread from busy list to the list of finished workers. Remove it
  // from the finished list first so that the worker can never end up in that
  // list twice (which would lead to a double delete when reaping).
  m_workersBusy.remove(thread);
  m_workersIdle.remove(thread);
  m_workersIdle.push_back(thread);
  DCMNET_DEBUG("DcmBaseSCPPool: Put worker thread #" << thread->threadID() << " into finished list; now "
              << m_workersBusy.size() << " busy and " << m_workersIdle.size() << " finished worker threads.");
  m_criticalSection.unlock();
}

// ----------------------------------------------------------------------------

void DcmBaseSCPPool::reapFinishedWorkers()
{
  // Take over the list of finished workers with the mutex held, but join
  // and delete the workers outside the critical section.
  OFList<DcmBaseSCPWorker*> finished;
  m_criticalSection.lock();
  finished.splice(finished.begin(), m_workersIdle);
  m_criticalSection.unlock();
  for (OFListIterator(DcmBaseSCPWorker*) it = finished.begin(); it != finished.end(); ++it)
  {
    DCMNET_DEBUG("DcmBaseSCPPool: Joining and deleting finished worker thread #" << (*it)->threadID());
    (*it)->join();
    delete (*it);
  }
}

// ----------------------------------------------------------------------------

OFCondition DcmBaseSCPPool::initializeNetwork(T_ASC_Network** network)
{
    OFCondition cond = ASC_initializeNetwork(NET_ACCEPTOR, OFstatic_cast(int, m_cfg.getPort()), m_cfg.getACSETimeout(), network);
    if (cond.good())
    {
      if (m_cfg.transportLayerEnabled())
      {
        cond = ASC_setTransportLayer(*network, m_cfg.getTransportLayer(), 0 /* Do not take over ownership */);
        if (cond.bad())
        {
          DCMNET_ERROR("DcmBaseSCPPool: Error setting secured transport layer: " << cond.text());
          ASC_dropNetwork(network);
        }
      }
    }
    return cond;
}


/* *********************************************************************** */
/*                        DcmBaseSCPPool::BaseSCPWorker class              */
/* *********************************************************************** */

DcmBaseSCPPool::DcmBaseSCPWorker::DcmBaseSCPWorker(DcmBaseSCPPool& pool)
  : m_pool(pool),
    m_assoc(NULL)
{
}

// ----------------------------------------------------------------------------

DcmBaseSCPPool::DcmBaseSCPWorker::~DcmBaseSCPWorker()
{
}

// ----------------------------------------------------------------------------

OFCondition DcmBaseSCPPool::DcmBaseSCPWorker::setAssociation(T_ASC_Association* assoc)
{
  if (busy())
  {
    DCMNET_DEBUG("DcmBaseSCPPool: Worker thread #" << threadID() << " is already busy, cannot set new association");
    return NET_EC_AlreadyConnected;
  }

  if ( (m_assoc != NULL) || (assoc == NULL) )
    return DIMSE_ILLEGALASSOCIATION;

  m_assoc = assoc;
  return EC_Normal;
}

// ----------------------------------------------------------------------------

void DcmBaseSCPPool::DcmBaseSCPWorker::run()
{
  OFCondition result;
  if(!m_assoc)
  {
    DCMNET_ERROR("DcmBaseSCPPool: Worker thread #" << threadID() << " received run command but has no association, exiting");
    m_pool.notifyWorkerDone(this, ASC_NULLKEY);
  }
  else
  {
    T_ASC_Association *param = m_assoc;
    m_assoc = NULL;
    result = workerListen(param);
    m_pool.notifyWorkerDone(this, result);
    DCMNET_DEBUG("DcmBaseSCPPool: Worker thread #" << threadID() << " finished handling association, dropping and destroying its association");
    DCMNET_DEBUG("DcmBaseSCPPool: Worker thread #" << threadID() << " returns with result: " << result.text() );
  }
  return;
}

// ----------------------------------------------------------------------------

void DcmBaseSCPPool::DcmBaseSCPWorker::exit()
{
  thread_exit();
}

// ----------------------------------------------------------------------------

void DcmBaseSCPPool::DcmBaseSCPWorker::rerun()
{
  // No longer called by the pool: each worker thread handles exactly one
  // association and terminates afterwards (see DcmBaseSCPPool::runAssociation).
  DcmBaseSCPPool::DcmBaseSCPWorker::run();
}

// ----------------------------------------------------------------------------

void DcmBaseSCPPool::finishListening()
{
  m_criticalSection.lock();
  // Set run mode to SHUTDOWN which signals destructor that its time to clean up
  m_runMode = SHUTDOWN;
  m_criticalSection.unlock();
}


#endif // WITH_THREADS
