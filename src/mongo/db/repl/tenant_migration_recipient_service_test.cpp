/**
 *    Copyright (C) 2020-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include <boost/optional/optional_io.hpp>
#include <memory>

#include "mongo/client/connpool.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/client/replica_set_monitor_protocol_test_util.h"
#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/op_observer_impl.h"
#include "mongo/db/op_observer_registry.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/repl/primary_only_service_op_observer.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/tenant_migration_recipient_entry_helpers.h"
#include "mongo/db/repl/tenant_migration_recipient_service.h"
#include "mongo/db/repl/tenant_migration_state_machine_gen.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/session_txn_record_gen.h"
#include "mongo/dbtests/mock/mock_conn_registry.h"
#include "mongo/dbtests/mock/mock_replica_set.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"

namespace mongo {
namespace repl {

namespace {
OplogEntry makeOplogEntry(OpTime opTime,
                          OpTypeEnum opType,
                          NamespaceString nss,
                          OptionalCollectionUUID uuid,
                          BSONObj o,
                          boost::optional<BSONObj> o2) {
    return OplogEntry(opTime,                     // optime
                      boost::none,                // hash
                      opType,                     // opType
                      nss,                        // namespace
                      uuid,                       // uuid
                      boost::none,                // fromMigrate
                      OplogEntry::kOplogVersion,  // version
                      o,                          // o
                      o2,                         // o2
                      {},                         // sessionInfo
                      boost::none,                // upsert
                      Date_t(),                   // wall clock time
                      boost::none,                // statement id
                      boost::none,   // optime of previous write within same transaction
                      boost::none,   // pre-image optime
                      boost::none,   // post-image optime
                      boost::none);  // ShardId of resharding recipient
}

}  // namespace

class TenantMigrationRecipientServiceTest : public ServiceContextMongoDTest {
public:
    void setUp() override {
        ServiceContextMongoDTest::setUp();
        auto serviceContext = getServiceContext();
        // Only the ReplicaSetMonitor scanning protocol supports mock connections.
        ReplicaSetMonitorProtocolTestUtil::setRSMProtocol(ReplicaSetMonitorProtocol::kScanning);
        ConnectionString::setConnectionHook(mongo::MockConnRegistry::get()->getConnStrHook());

        WaitForMajorityService::get(getServiceContext()).setUp(getServiceContext());

        {
            auto opCtx = cc().makeOperationContext();
            auto replCoord = std::make_unique<ReplicationCoordinatorMock>(serviceContext);
            ReplicationCoordinator::set(serviceContext, std::move(replCoord));

            repl::setOplogCollectionName(serviceContext);
            repl::createOplog(opCtx.get());
            // Set up OpObserver so that repl::logOp() will store the oplog entry's optime in
            // ReplClientInfo.
            OpObserverRegistry* opObserverRegistry =
                dynamic_cast<OpObserverRegistry*>(serviceContext->getOpObserver());
            opObserverRegistry->addObserver(std::make_unique<OpObserverImpl>());
            opObserverRegistry->addObserver(
                std::make_unique<PrimaryOnlyServiceOpObserver>(serviceContext));

            _registry = repl::PrimaryOnlyServiceRegistry::get(getServiceContext());
            std::unique_ptr<TenantMigrationRecipientService> service =
                std::make_unique<TenantMigrationRecipientService>(getServiceContext());
            _registry->registerService(std::move(service));
            _registry->onStartup(opCtx.get());
        }
        stepUp();

        _service = _registry->lookupServiceByName(
            TenantMigrationRecipientService::kTenantMigrationRecipientServiceName);
        ASSERT(_service);
    }

    void tearDown() override {
        WaitForMajorityService::get(getServiceContext()).shutDown();

        _registry->onShutdown();
        _service = nullptr;

        // Clearing the connection pool is necessary when doing tests which use the
        // ReplicaSetMonitor.  See src/mongo/dbtests/mock/mock_replica_set.h for details.
        ScopedDbConnection::clearPool();
        ReplicaSetMonitorProtocolTestUtil::resetRSMProtocol();
        ServiceContextMongoDTest::tearDown();
    }

    void stepDown() {
        ASSERT_OK(ReplicationCoordinator::get(getServiceContext())
                      ->setFollowerMode(MemberState::RS_SECONDARY));
        _registry->onStepDown();
    }

    void stepUp() {
        auto opCtx = cc().makeOperationContext();
        auto replCoord = ReplicationCoordinator::get(getServiceContext());

        // Advance term
        _term++;

        ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_PRIMARY));
        ASSERT_OK(replCoord->updateTerm(opCtx.get(), _term));
        replCoord->setMyLastAppliedOpTimeAndWallTime(
            OpTimeAndWallTime(OpTime(Timestamp(1, 1), _term), Date_t()));

        _registry->onStepUpComplete(opCtx.get(), _term);
    }

protected:
    PrimaryOnlyServiceRegistry* _registry;
    PrimaryOnlyService* _service;
    long long _term = 0;

    void checkStateDocPersisted(const TenantMigrationRecipientService::Instance* instance) {
        auto opCtx = cc().makeOperationContext();
        auto memoryStateDoc = getStateDoc(instance);
        auto persistedStateDocWithStatus =
            tenantMigrationRecipientEntryHelpers::getStateDoc(opCtx.get(), memoryStateDoc.getId());
        ASSERT_OK(persistedStateDocWithStatus.getStatus());
        ASSERT_BSONOBJ_EQ(memoryStateDoc.toBSON(), persistedStateDocWithStatus.getValue().toBSON());
    }
    void insertToAllNodes(MockReplicaSet* replSet, const std::string& nss, BSONObj obj) {
        for (const auto& host : replSet->getHosts()) {
            replSet->getNode(host.toString())->insert(nss, obj);
        }
    }

    void clearCollectionAllNodes(MockReplicaSet* replSet, const std::string& nss) {
        for (const auto& host : replSet->getHosts()) {
            replSet->getNode(host.toString())->remove(nss, Query());
        }
    }

    void insertTopOfOplog(MockReplicaSet* replSet, const OpTime& topOfOplogOpTime) {
        // The MockRemoteDBService does not actually implement the database, so to make our
        // find work correctly we must make sure there's only one document to find.
        clearCollectionAllNodes(replSet, NamespaceString::kRsOplogNamespace.ns());
        insertToAllNodes(replSet,
                         NamespaceString::kRsOplogNamespace.ns(),
                         makeOplogEntry(topOfOplogOpTime,
                                        OpTypeEnum::kNoop,
                                        {} /* namespace */,
                                        boost::none /* uuid */,
                                        BSONObj() /* o */,
                                        boost::none /* o2 */)
                             .toBSON());
    }

    // Accessors to class private members
    DBClientConnection* getClient(const TenantMigrationRecipientService::Instance* instance) const {
        return instance->_client.get();
    }

    DBClientConnection* getOplogFetcherClient(
        const TenantMigrationRecipientService::Instance* instance) const {
        return instance->_oplogFetcherClient.get();
    }

    const TenantMigrationRecipientDocument& getStateDoc(
        const TenantMigrationRecipientService::Instance* instance) const {
        return instance->_stateDoc;
    }

private:
    unittest::MinimumLoggedSeverityGuard _replicationSeverityGuard{
        logv2::LogComponent::kReplication, logv2::LogSeverity::Debug(1)};
};


TEST_F(TenantMigrationRecipientServiceTest, BasicTenantMigrationRecipientServiceInstanceCreation) {
    FailPointEnableBlock fp("stopAfterPersistingTenantMigrationRecipientInstanceStateDoc");

    const UUID migrationUUID = UUID::gen();

    TenantMigrationRecipientDocument TenantMigrationRecipientInstance(
        migrationUUID,
        "DonorHost:12345",
        "tenantA",
        ReadPreferenceSetting(ReadPreference::PrimaryOnly, TagSet::primaryOnly()));

    // Create and start the instance.
    auto instance = TenantMigrationRecipientService::Instance::getOrCreate(
        _service, TenantMigrationRecipientInstance.toBSON());
    ASSERT(instance.get());
    ASSERT_EQ(migrationUUID, instance->getMigrationUUID());

    // Wait for task completion success.
    ASSERT_OK(instance->getCompletionFuture().getNoThrow());
}


TEST_F(TenantMigrationRecipientServiceTest, InstanceReportsErrorOnFailureWhilePersisitingStateDoc) {
    FailPointEnableBlock failPoint("failWhilePersistingTenantMigrationRecipientInstanceStateDoc");

    const UUID migrationUUID = UUID::gen();

    TenantMigrationRecipientDocument TenantMigrationRecipientInstance(
        migrationUUID,
        "DonorHost:12345",
        "tenantA",
        ReadPreferenceSetting(ReadPreference::PrimaryOnly, TagSet::primaryOnly()));

    // Create and start the instance.
    auto instance = TenantMigrationRecipientService::Instance::getOrCreate(
        _service, TenantMigrationRecipientInstance.toBSON());
    ASSERT(instance.get());
    ASSERT_EQ(migrationUUID, instance->getMigrationUUID());

    // Should be able to see the instance task failure error.
    auto status = instance->getCompletionFuture().getNoThrow();
    ASSERT_EQ(ErrorCodes::NotWritablePrimary, status.code());
}

TEST_F(TenantMigrationRecipientServiceTest, TenantMigrationRecipientConnection_Primary) {
    FailPointEnableBlock fp("stopAfterConnectingTenantMigrationRecipientInstance");

    const UUID migrationUUID = UUID::gen();

    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /*dollarPrefixHosts */);

    TenantMigrationRecipientDocument TenantMigrationRecipientInstance(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));

    // Create and start the instance.
    auto instance = TenantMigrationRecipientService::Instance::getOrCreate(
        _service, TenantMigrationRecipientInstance.toBSON());
    ASSERT(instance.get());

    // Wait for task completion success.
    ASSERT_OK(instance->getCompletionFuture().getNoThrow());

    auto* client = getClient(instance.get());
    auto* oplogFetcherClient = getOplogFetcherClient(instance.get());
    // Both clients should be populated.
    ASSERT(client);
    ASSERT(oplogFetcherClient);

    // Clients should be distinct.
    ASSERT(client != oplogFetcherClient);

    // Clients should be connected to primary.
    auto primary = replSet.getHosts()[0].toString();
    ASSERT_EQ(primary, client->getServerAddress());
    ASSERT(client->isStillConnected());
    ASSERT_EQ(primary, oplogFetcherClient->getServerAddress());
    ASSERT(oplogFetcherClient->isStillConnected());
}

TEST_F(TenantMigrationRecipientServiceTest, TenantMigrationRecipientConnection_Secondary) {
    FailPointEnableBlock fp("stopAfterConnectingTenantMigrationRecipientInstance");

    const UUID migrationUUID = UUID::gen();

    MockReplicaSet replSet("donorSet", 2, true /* hasPrimary */, true /*dollarPrefixHosts */);

    TenantMigrationRecipientDocument TenantMigrationRecipientInstance(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        ReadPreferenceSetting(ReadPreference::SecondaryOnly));

    // Create and start the instance.
    auto instance = TenantMigrationRecipientService::Instance::getOrCreate(
        _service, TenantMigrationRecipientInstance.toBSON());
    ASSERT(instance.get());

    // Wait for task completion success.
    ASSERT_OK(instance->getCompletionFuture().getNoThrow());

    auto* client = getClient(instance.get());
    auto* oplogFetcherClient = getOplogFetcherClient(instance.get());
    // Both clients should be populated.
    ASSERT(client);
    ASSERT(oplogFetcherClient);

    // Clients should be distinct.
    ASSERT(client != oplogFetcherClient);

    // Clients should be connected to secondary.
    auto secondary = replSet.getHosts()[1].toString();
    ASSERT_EQ(secondary, client->getServerAddress());
    ASSERT(client->isStillConnected());
    ASSERT_EQ(secondary, oplogFetcherClient->getServerAddress());
    ASSERT(oplogFetcherClient->isStillConnected());
}

TEST_F(TenantMigrationRecipientServiceTest, TenantMigrationRecipientConnection_PrimaryFails) {
    FailPointEnableBlock fp("stopAfterConnectingTenantMigrationRecipientInstance");
    FailPointEnableBlock timeoutFp("setTenantMigrationRecipientInstanceHostTimeout",
                                   BSON("findHostTimeoutMillis" << 100));

    const UUID migrationUUID = UUID::gen();

    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /*dollarPrefixHosts */);
    // Primary is unavailable.
    replSet.kill(replSet.getHosts()[0].toString());

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));

    // Create and start the instance.
    auto instance = TenantMigrationRecipientService::Instance::getOrCreate(
        _service, initialStateDocument.toBSON());
    ASSERT(instance.get());

    // Keep scanning the replica set while waiting for task completion.  This would normally
    // be automatic but that doesn't work with mock replica sets.
    while (!instance->getCompletionFuture().isReady()) {
        auto monitor = ReplicaSetMonitor::get(replSet.getSetName());
        // Monitor may not have been created yet.
        if (monitor) {
            monitor->runScanForMockReplicaSet();
        }
        mongo::sleepmillis(50);
    }
    // Wait for task completion failure.
    ASSERT_EQUALS(ErrorCodes::FailedToSatisfyReadPreference,
                  instance->getCompletionFuture().getNoThrow().code());

    auto* client = getClient(instance.get());
    auto* oplogFetcherClient = getOplogFetcherClient(instance.get());
    // Neither client should be populated.
    ASSERT_FALSE(client);
    ASSERT_FALSE(oplogFetcherClient);
}

TEST_F(TenantMigrationRecipientServiceTest, TenantMigrationRecipientConnection_PrimaryFailsOver) {
    FailPointEnableBlock fp("stopAfterConnectingTenantMigrationRecipientInstance");

    const UUID migrationUUID = UUID::gen();

    MockReplicaSet replSet("donorSet", 2, true /* hasPrimary */, true /*dollarPrefixHosts */);

    // Primary is unavailable.
    replSet.kill(replSet.getHosts()[0].toString());

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        ReadPreferenceSetting(ReadPreference::PrimaryPreferred));

    // Create and start the instance.
    auto instance = TenantMigrationRecipientService::Instance::getOrCreate(
        _service, initialStateDocument.toBSON());
    ASSERT(instance.get());

    // Wait for task completion success.
    ASSERT_OK(instance->getCompletionFuture().getNoThrow());

    auto* client = getClient(instance.get());
    auto* oplogFetcherClient = getOplogFetcherClient(instance.get());
    // Both clients should be populated.
    ASSERT(client);
    ASSERT(oplogFetcherClient);

    // Clients should be distinct.
    ASSERT(client != oplogFetcherClient);

    // Clients should be connected to secondary.
    auto secondary = replSet.getHosts()[1].toString();
    ASSERT_EQ(secondary, client->getServerAddress());
    ASSERT(client->isStillConnected());
    ASSERT_EQ(secondary, oplogFetcherClient->getServerAddress());
    ASSERT(oplogFetcherClient->isStillConnected());
}

TEST_F(TenantMigrationRecipientServiceTest, TenantMigrationRecipientConnection_BadConnectString) {
    FailPointEnableBlock fp("stopAfterConnectingTenantMigrationRecipientInstance");

    const UUID migrationUUID = UUID::gen();

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        "broken,connect,string,no,set,name",
        "tenantA",
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));

    // Create and start the instance.
    auto instance = TenantMigrationRecipientService::Instance::getOrCreate(
        _service, initialStateDocument.toBSON());
    ASSERT(instance.get());

    // Wait for task completion failure.
    ASSERT_EQUALS(ErrorCodes::FailedToParse, instance->getCompletionFuture().getNoThrow().code());
}

TEST_F(TenantMigrationRecipientServiceTest,
       TenantMigrationRecipientConnection_NonSetConnectString) {
    FailPointEnableBlock fp("stopAfterConnectingTenantMigrationRecipientInstance");

    const UUID migrationUUID = UUID::gen();

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        "localhost:12345",
        "tenantA",
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));

    // Create and start the instance.
    auto instance = TenantMigrationRecipientService::Instance::getOrCreate(
        _service, initialStateDocument.toBSON());
    ASSERT(instance.get());

    // Wait for task completion failure.
    ASSERT_EQUALS(ErrorCodes::FailedToParse, instance->getCompletionFuture().getNoThrow().code());
}

TEST_F(TenantMigrationRecipientServiceTest, TenantMigrationRecipientGetStartOpTime_NoTransaction) {
    FailPointEnableBlock fp("stopAfterRetrievingStartOpTimesMigrationRecipientInstance");

    const UUID migrationUUID = UUID::gen();
    const OpTime topOfOplogOpTime(Timestamp(5, 1), 1);

    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /*dollarPrefixHosts */);
    insertTopOfOplog(&replSet, topOfOplogOpTime);

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));

    // Create and start the instance.
    auto instance = TenantMigrationRecipientService::Instance::getOrCreate(
        _service, initialStateDocument.toBSON());
    ASSERT(instance.get());

    // Wait for task completion success.
    ASSERT_OK(instance->getCompletionFuture().getNoThrow());

    ASSERT_EQ(topOfOplogOpTime, getStateDoc(instance.get()).getStartFetchingOpTime());
    ASSERT_EQ(topOfOplogOpTime, getStateDoc(instance.get()).getStartApplyingOpTime());
    checkStateDocPersisted(instance.get());
}

TEST_F(TenantMigrationRecipientServiceTest,
       TenantMigrationRecipientGetStartOpTime_Advances_NoTransaction) {
    FailPointEnableBlock fp("stopAfterRetrievingStartOpTimesMigrationRecipientInstance");
    auto pauseFailPoint =
        globalFailPointRegistry().find("pauseAfterRetrievingLastTxnMigrationRecipientInstance");
    auto timesEntered = pauseFailPoint->setMode(FailPoint::alwaysOn, 0);

    const UUID migrationUUID = UUID::gen();
    const OpTime topOfOplogOpTime(Timestamp(5, 1), 1);
    const OpTime newTopOfOplogOpTime(Timestamp(6, 1), 1);

    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /*dollarPrefixHosts */);
    insertTopOfOplog(&replSet, topOfOplogOpTime);

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));

    // Create and start the instance.
    auto instance = TenantMigrationRecipientService::Instance::getOrCreate(
        _service, initialStateDocument.toBSON());
    ASSERT(instance.get());

    pauseFailPoint->waitForTimesEntered(timesEntered + 1);
    insertTopOfOplog(&replSet, newTopOfOplogOpTime);
    pauseFailPoint->setMode(FailPoint::off, 0);

    // Wait for task completion success.
    ASSERT_OK(instance->getCompletionFuture().getNoThrow());

    ASSERT_EQ(topOfOplogOpTime, getStateDoc(instance.get()).getStartFetchingOpTime());
    ASSERT_EQ(newTopOfOplogOpTime, getStateDoc(instance.get()).getStartApplyingOpTime());
    checkStateDocPersisted(instance.get());
}

TEST_F(TenantMigrationRecipientServiceTest, TenantMigrationRecipientGetStartOpTime_Transaction) {
    FailPointEnableBlock fp("stopAfterRetrievingStartOpTimesMigrationRecipientInstance");

    const UUID migrationUUID = UUID::gen();
    const OpTime txnStartOpTime(Timestamp(3, 1), 1);
    const OpTime txnLastWriteOpTime(Timestamp(4, 1), 1);
    const OpTime topOfOplogOpTime(Timestamp(5, 1), 1);

    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /*dollarPrefixHosts */);
    insertTopOfOplog(&replSet, topOfOplogOpTime);
    SessionTxnRecord lastTxn(makeLogicalSessionIdForTest(), 100, txnLastWriteOpTime, Date_t());
    lastTxn.setStartOpTime(txnStartOpTime);
    lastTxn.setState(DurableTxnStateEnum::kInProgress);
    insertToAllNodes(
        &replSet, NamespaceString::kSessionTransactionsTableNamespace.ns(), lastTxn.toBSON());

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));

    // Create and start the instance.
    auto instance = TenantMigrationRecipientService::Instance::getOrCreate(
        _service, initialStateDocument.toBSON());
    ASSERT(instance.get());

    // Wait for task completion success.
    ASSERT_OK(instance->getCompletionFuture().getNoThrow());

    ASSERT_EQ(txnStartOpTime, getStateDoc(instance.get()).getStartFetchingOpTime());
    ASSERT_EQ(topOfOplogOpTime, getStateDoc(instance.get()).getStartApplyingOpTime());
    checkStateDocPersisted(instance.get());
}

TEST_F(TenantMigrationRecipientServiceTest,
       TenantMigrationRecipientGetStartOpTime_Advances_Transaction) {
    FailPointEnableBlock fp("stopAfterRetrievingStartOpTimesMigrationRecipientInstance");
    auto pauseFailPoint =
        globalFailPointRegistry().find("pauseAfterRetrievingLastTxnMigrationRecipientInstance");
    auto timesEntered = pauseFailPoint->setMode(FailPoint::alwaysOn, 0);

    const UUID migrationUUID = UUID::gen();
    const OpTime txnStartOpTime(Timestamp(3, 1), 1);
    const OpTime txnLastWriteOpTime(Timestamp(4, 1), 1);
    const OpTime topOfOplogOpTime(Timestamp(5, 1), 1);
    const OpTime newTopOfOplogOpTime(Timestamp(6, 1), 1);

    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /*dollarPrefixHosts */);
    insertTopOfOplog(&replSet, topOfOplogOpTime);
    SessionTxnRecord lastTxn(makeLogicalSessionIdForTest(), 100, txnLastWriteOpTime, Date_t());
    lastTxn.setStartOpTime(txnStartOpTime);
    lastTxn.setState(DurableTxnStateEnum::kInProgress);
    insertToAllNodes(
        &replSet, NamespaceString::kSessionTransactionsTableNamespace.ns(), lastTxn.toBSON());

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));

    // Create and start the instance.
    auto instance = TenantMigrationRecipientService::Instance::getOrCreate(
        _service, initialStateDocument.toBSON());
    ASSERT(instance.get());

    pauseFailPoint->waitForTimesEntered(timesEntered + 1);
    insertTopOfOplog(&replSet, newTopOfOplogOpTime);
    pauseFailPoint->setMode(FailPoint::off, 0);

    // Wait for task completion success.
    ASSERT_OK(instance->getCompletionFuture().getNoThrow());

    ASSERT_EQ(txnStartOpTime, getStateDoc(instance.get()).getStartFetchingOpTime());
    ASSERT_EQ(newTopOfOplogOpTime, getStateDoc(instance.get()).getStartApplyingOpTime());
    checkStateDocPersisted(instance.get());
}

TEST_F(TenantMigrationRecipientServiceTest,
       TenantMigrationRecipientGetStartOpTimes_RemoteOplogQueryFails) {
    FailPointEnableBlock fp("stopAfterRetrievingStartOpTimesMigrationRecipientInstance");

    const UUID migrationUUID = UUID::gen();

    MockReplicaSet replSet("donorSet", 3, true /* hasPrimary */, true /*dollarPrefixHosts */);

    TenantMigrationRecipientDocument initialStateDocument(
        migrationUUID,
        replSet.getConnectionString(),
        "tenantA",
        ReadPreferenceSetting(ReadPreference::PrimaryOnly));

    // Create and start the instance.  Fail to populate the remote oplog mock.
    auto instance = TenantMigrationRecipientService::Instance::getOrCreate(
        _service, initialStateDocument.toBSON());
    ASSERT(instance.get());

    // Wait for task completion success.
    ASSERT_NOT_OK(instance->getCompletionFuture().getNoThrow());

    // Even though we failed, the memory state should still match the on-disk state.
    checkStateDocPersisted(instance.get());
}

}  // namespace repl
}  // namespace mongo