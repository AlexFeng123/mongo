/**
 *    Copyright (C) 2018 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/session_catalog.h"
#include "mongo/db/transaction_coordinator_service.h"
#include "mongo/s/catalog/sharding_catalog_client_mock.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/shard_id.h"
#include "mongo/s/shard_server_test_fixture.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

const std::vector<ShardId> kTwoShardIdList{{"s1"}, {"s2"}};
const std::set<ShardId> kTwoShardIdSet{{"s1"}, {"s2"}};
const std::vector<ShardId> kThreeShardIdList{{"s1"}, {"s2"}, {"s3"}};
const std::set<ShardId> kThreeShardIdSet{{"s1"}, {"s2"}, {"s3"}};
const Timestamp kDummyTimestamp = Timestamp::min();
const Date_t kCommitDeadline = Date_t::max();
const StatusWith<BSONObj> kRetryableError = {ErrorCodes::HostUnreachable, ""};
const StatusWith<BSONObj> kOk = BSON("ok" << 1);

HostAndPort makeHostAndPort(const ShardId& shardId) {
    return HostAndPort(str::stream() << shardId << ":123");
}

class TransactionCoordinatorServiceTest : public ShardServerTestFixture {
public:
    void setUp() override {
        ShardServerTestFixture::setUp();

        operationContext()->setLogicalSessionId(_lsid);
        operationContext()->setTxnNumber(_txnNumber);

        for (const auto& shardId : kThreeShardIdList) {
            auto shardTargeter = RemoteCommandTargeterMock::get(
                uassertStatusOK(shardRegistry()->getShard(operationContext(), shardId))
                    ->getTargeter());
            shardTargeter->setFindHostReturnValue(makeHostAndPort(shardId));
        }
    }

    void tearDown() override {
        ShardServerTestFixture::tearDown();
    }

    void assertCommandSentAndRespondWith(const StringData& commandName,
                                         const StatusWith<BSONObj>& response) {
        onCommand([commandName, response](const executor::RemoteCommandRequest& request) {
            ASSERT_EQ(commandName, request.cmdObj.firstElement().fieldNameStringData());
            return response;
        });
    }

    void assertAbortSentAndRespondWithSuccess() {
        assertCommandSentAndRespondWith("abortTransaction", kOk);
    }

    void assertCommitSentAndRespondWithSuccess() {
        assertCommandSentAndRespondWith(CommitTransaction::kCommandName, kOk);
    }

    void assertCommitSentAndRespondWithRetryableError() {
        assertCommandSentAndRespondWith(CommitTransaction::kCommandName, kRetryableError);
    }

    void assertNoMessageSent() {
        executor::NetworkInterfaceMock::InNetworkGuard networkGuard(network());
        ASSERT_FALSE(network()->hasReadyRequests());
    }

    LogicalSessionId lsid() {
        return _lsid;
    }

    TxnNumber txnNumber() {
        return _txnNumber;
    }

    /**
     * Goes through the steps to commit a transaction through the coordinator service  for a given
     * lsid and txnNumber. Useful when not explictly testing the commit protocol.
     */
    void commitTransaction(TransactionCoordinatorService& coordinatorService,
                           const LogicalSessionId& lsid,
                           const TxnNumber& txnNumber,
                           const std::set<ShardId>& transactionParticipantShards) {
        auto commitDecisionFuture = coordinatorService.coordinateCommit(
            operationContext(), lsid, txnNumber, transactionParticipantShards);

        for (const auto& shardId : transactionParticipantShards) {
            coordinatorService.voteCommit(
                operationContext(), lsid, txnNumber, shardId, kDummyTimestamp);
        }

        for (size_t i = 0; i < transactionParticipantShards.size(); ++i) {
            assertCommitSentAndRespondWithSuccess();
        }

        // Wait for commit to complete.
        commitDecisionFuture.get();
    }

    /**
     * Goes through the steps to abort a transaction through the coordinator service for a given
     * lsid and txnNumber. Useful when not explictly testing the abort protocol.
     */
    void abortTransaction(TransactionCoordinatorService& coordinatorService,
                          const LogicalSessionId& lsid,
                          const TxnNumber& txnNumber,
                          const std::set<ShardId>& shardIdSet,
                          const ShardId& abortingShard) {
        auto commitDecisionFuture =
            coordinatorService.coordinateCommit(operationContext(), lsid, txnNumber, shardIdSet);

        coordinatorService.voteAbort(operationContext(), lsid, txnNumber, abortingShard);

        // Wait for abort to complete.
        commitDecisionFuture.get();
    }


private:
    // Override the CatalogClient to make CatalogClient::getAllShards automatically return the
    // expected shards. We cannot mock the network responses for the ShardRegistry reload, since the
    // ShardRegistry reload is done over DBClient, not the NetworkInterface, and there is no
    // DBClientMock analogous to the NetworkInterfaceMock.
    std::unique_ptr<ShardingCatalogClient> makeShardingCatalogClient(
        std::unique_ptr<DistLockManager> distLockManager) override {

        class StaticCatalogClient final : public ShardingCatalogClientMock {
        public:
            StaticCatalogClient() : ShardingCatalogClientMock(nullptr) {}

            StatusWith<repl::OpTimeWith<std::vector<ShardType>>> getAllShards(
                OperationContext* opCtx, repl::ReadConcernLevel readConcern) override {
                std::vector<ShardType> shardTypes;
                for (const auto& shardId : kThreeShardIdList) {
                    const ConnectionString cs = ConnectionString::forReplicaSet(
                        shardId.toString(), {makeHostAndPort(shardId)});
                    ShardType sType;
                    sType.setName(cs.getSetName());
                    sType.setHost(cs.toString());
                    shardTypes.push_back(std::move(sType));
                };
                return repl::OpTimeWith<std::vector<ShardType>>(shardTypes);
            }
        };

        return stdx::make_unique<StaticCatalogClient>();
    }

    LogicalSessionId _lsid{makeLogicalSessionIdForTest()};
    TxnNumber _txnNumber{1};
};

/**
 * Fixture that during setUp automatically creates a coordinator service and then creates a
 * coordinator on the service for a default lsid/txnNumber pair.
 */
class TransactionCoordinatorServiceTestSingleTxn : public TransactionCoordinatorServiceTest {
public:
    void setUp() final {
        TransactionCoordinatorServiceTest::setUp();

        _coordinatorService = std::make_unique<TransactionCoordinatorService>();
        _coordinatorService->createCoordinator(
            operationContext(), lsid(), txnNumber(), kCommitDeadline);
    }

    void tearDown() final {
        _coordinatorService.reset();
        TransactionCoordinatorServiceTest::tearDown();
    }

    TransactionCoordinatorService* coordinatorService() {
        return _coordinatorService.get();
    }

private:
    std::unique_ptr<TransactionCoordinatorService> _coordinatorService;
};


}  // namespace

TEST_F(TransactionCoordinatorServiceTest, CreateCoordinatorOnNewSessionSucceeds) {
    TransactionCoordinatorService coordinatorService;
    coordinatorService.createCoordinator(operationContext(), lsid(), txnNumber(), kCommitDeadline);
    commitTransaction(coordinatorService, lsid(), txnNumber(), kTwoShardIdSet);
}

TEST_F(TransactionCoordinatorServiceTest,
       CreateCoordinatorForExistingSessionWithPreviouslyCommittedTxnSucceeds) {

    TransactionCoordinatorService coordinatorService;

    coordinatorService.createCoordinator(operationContext(), lsid(), txnNumber(), kCommitDeadline);
    commitTransaction(coordinatorService, lsid(), txnNumber(), kTwoShardIdSet);

    coordinatorService.createCoordinator(
        operationContext(), lsid(), txnNumber() + 1, kCommitDeadline);
    commitTransaction(coordinatorService, lsid(), txnNumber() + 1, kTwoShardIdSet);
}

TEST_F(TransactionCoordinatorServiceTest,
       CreateCoordinatorWithHigherTxnNumberThanOngoingUncommittedTxnAbortsPreviousTxnAndSucceeds) {
    // TODO (SERVER-37021): Implement once more validation is implemented for coordinator creation.
}

TEST_F(
    TransactionCoordinatorServiceTest,
    CreateCoordinatorWithHigherTxnNumberThanOngoingCommittingTxnWaitsForPreviousTxnToCommitAndSucceeds) {
    // TODO (SERVER-37021): Implement once more validation is implemented for coordinator creation.
}

TEST_F(
    TransactionCoordinatorServiceTest,
    CreateCoordinatorWithSameTxnNumberAsOngoingUncommittedTxnThrowsIfPreviousCoordinatorHasReceivedEvents) {
    // TODO (SERVER-37021): Implement once more validation is implemented for coordinator creation.
}

TEST_F(
    TransactionCoordinatorServiceTest,
    CreateCoordinatorWithSameTxnNumberAsOngoingUncommittedTxnSucceedsIfPreviousCoordinatorHasNotReceivedEvents) {
    // TODO (SERVER-37021): Implement once more validation is implemented for coordinator creation.
}

TEST_F(TransactionCoordinatorServiceTestSingleTxn,
       CoordinateCommitWithNoVotesReturnsNotReadyFuture) {

    auto commitDecisionFuture = coordinatorService()->coordinateCommit(
        operationContext(), lsid(), txnNumber(), kTwoShardIdSet);

    ASSERT_FALSE(commitDecisionFuture.isReady());
    // To prevent invariant failure in TransactionCoordinator that all futures have been completed.
    abortTransaction(
        *coordinatorService(), lsid(), txnNumber(), kTwoShardIdSet, kTwoShardIdList[0]);
}

TEST_F(TransactionCoordinatorServiceTestSingleTxn,
       CoordinateCommitReturnsCorrectCommitDecisionOnAbort) {

    auto commitDecisionFuture = coordinatorService()->coordinateCommit(
        operationContext(), lsid(), txnNumber(), kTwoShardIdSet);

    coordinatorService()->voteAbort(operationContext(), lsid(), txnNumber(), kTwoShardIdList[0]);

    auto commitDecision = commitDecisionFuture.get();
    ASSERT_EQ(static_cast<int>(commitDecision),
              static_cast<int>(TransactionCoordinatorService::CommitDecision::kAbort));
}

TEST_F(TransactionCoordinatorServiceTestSingleTxn,
       CoordinateCommitReturnsCorrectCommitDecisionOnCommit) {

    auto commitDecisionFuture = coordinatorService()->coordinateCommit(
        operationContext(), lsid(), txnNumber(), kTwoShardIdSet);

    coordinatorService()->voteCommit(
        operationContext(), lsid(), txnNumber(), kTwoShardIdList[0], kDummyTimestamp);

    coordinatorService()->voteCommit(
        operationContext(), lsid(), txnNumber(), kTwoShardIdList[1], kDummyTimestamp);

    assertCommitSentAndRespondWithSuccess();
    assertCommitSentAndRespondWithSuccess();

    auto commitDecision = commitDecisionFuture.get();
    ASSERT_EQ(static_cast<int>(commitDecision),
              static_cast<int>(TransactionCoordinatorService::CommitDecision::kCommit));
}

TEST_F(TransactionCoordinatorServiceTest,
       CoordinateCommitReturnsAbortDecisionWhenCoordinatorDoesNotExist) {

    TransactionCoordinatorService coordinatorService;
    auto commitDecisionFuture = coordinatorService.coordinateCommit(
        operationContext(), lsid(), txnNumber(), kTwoShardIdSet);
    ASSERT_TRUE(commitDecisionFuture.isReady());

    auto commitDecision = commitDecisionFuture.get();
    ASSERT_EQ(static_cast<int>(commitDecision),
              static_cast<int>(TransactionCoordinatorService::CommitDecision::kAbort));
}

TEST_F(TransactionCoordinatorServiceTest,
       CoordinateCommitRecoversCorrectCommitDecisionForTransactionThatAlreadyCommitted) {
    // TODO (SERVER-37440): Implement test when coordinateCommit is made to work correctly on
    // retries.
}

TEST_F(TransactionCoordinatorServiceTest,
       CoordinateCommitRecoversCorrectCommitDecisionForTransactionThatAlreadyAborted) {
    // TODO (SERVER-37440): Implement test when coordinateCommit is made to work correctly on
    // retries.
}

TEST_F(TransactionCoordinatorServiceTestSingleTxn,
       ConcurrentCallsToCoordinateCommitReturnSameDecisionOnCommit) {

    auto commitDecisionFuture1 = coordinatorService()->coordinateCommit(
        operationContext(), lsid(), txnNumber(), kTwoShardIdSet);
    auto commitDecisionFuture2 = coordinatorService()->coordinateCommit(
        operationContext(), lsid(), txnNumber(), kTwoShardIdSet);

    commitTransaction(*coordinatorService(), lsid(), txnNumber(), kTwoShardIdSet);

    ASSERT_EQ(static_cast<int>(commitDecisionFuture1.get()),
              static_cast<int>(commitDecisionFuture2.get()));
}

TEST_F(TransactionCoordinatorServiceTestSingleTxn,
       ConcurrentCallsToCoordinateCommitReturnSameDecisionOnAbort) {

    auto commitDecisionFuture1 = coordinatorService()->coordinateCommit(
        operationContext(), lsid(), txnNumber(), kTwoShardIdSet);
    auto commitDecisionFuture2 = coordinatorService()->coordinateCommit(
        operationContext(), lsid(), txnNumber(), kTwoShardIdSet);

    abortTransaction(
        *coordinatorService(), lsid(), txnNumber(), kTwoShardIdSet, kTwoShardIdList[0]);

    ASSERT_EQ(static_cast<int>(commitDecisionFuture1.get()),
              static_cast<int>(commitDecisionFuture2.get()));
}

TEST_F(TransactionCoordinatorServiceTestSingleTxn,
       VoteCommitDoesNotSendCommitIfParticipantListNotYetReceived) {

    coordinatorService()->voteCommit(
        operationContext(), lsid(), txnNumber(), kTwoShardIdList[0], kDummyTimestamp);

    assertNoMessageSent();
    // To prevent invariant failure in TransactionCoordinator that all futures have been completed.
    abortTransaction(
        *coordinatorService(), lsid(), txnNumber(), kTwoShardIdSet, kTwoShardIdList[1]);
}

TEST_F(TransactionCoordinatorServiceTestSingleTxn,
       ResentVoteCommitDoesNotSendCommitIfParticipantListNotYetReceived) {

    coordinatorService()->voteCommit(
        operationContext(), lsid(), txnNumber(), kTwoShardIdList[0], kDummyTimestamp);
    coordinatorService()->voteCommit(
        operationContext(), lsid(), txnNumber(), kTwoShardIdList[0], kDummyTimestamp);

    assertNoMessageSent();

    // To prevent invariant failure in TransactionCoordinator that all futures have been completed.
    abortTransaction(
        *coordinatorService(), lsid(), txnNumber(), kTwoShardIdSet, kTwoShardIdList[1]);
}

TEST_F(TransactionCoordinatorServiceTestSingleTxn,
       ResentVoteCommitDoesNotSendCommitIfParticipantListHasBeenReceived) {

    auto commitDecisionFuture = coordinatorService()->coordinateCommit(
        operationContext(), lsid(), txnNumber(), kTwoShardIdSet);

    coordinatorService()->voteCommit(
        operationContext(), lsid(), txnNumber(), kTwoShardIdList[0], kDummyTimestamp);
    coordinatorService()->voteCommit(
        operationContext(), lsid(), txnNumber(), kTwoShardIdList[0], kDummyTimestamp);

    assertNoMessageSent();

    // To prevent invariant failure in TransactionCoordinator that all futures have been completed.
    abortTransaction(
        *coordinatorService(), lsid(), txnNumber(), kTwoShardIdSet, kTwoShardIdList[1]);
    commitDecisionFuture.get();
}

TEST_F(TransactionCoordinatorServiceTestSingleTxn, FinalVoteCommitSendsCommit) {
    auto commitDecisionFuture = coordinatorService()->coordinateCommit(
        operationContext(), lsid(), txnNumber(), kTwoShardIdSet);

    coordinatorService()->voteCommit(
        operationContext(), lsid(), txnNumber(), kTwoShardIdList[0], kDummyTimestamp);

    coordinatorService()->voteCommit(
        operationContext(), lsid(), txnNumber(), kTwoShardIdList[1], kDummyTimestamp);

    assertCommitSentAndRespondWithSuccess();
    assertCommitSentAndRespondWithSuccess();
}

// This logic is obviously correct for a transaction which has been aborted prior to receiving
// coordinateCommit, when the coordinator does not yet know all participants and so cannot send
// abortTransaction to all participants. In this case, it can potentially receive voteCommit
// messages from some participants even after the local TransactionCoordinator object has
// transitioned to the aborted state and then removed from the service. We then must tell
// the participant that sent the voteCommit message that it should abort.
//
// More subtly, it also works for voteCommit retries for transactions that have already committed,
// because we'll send abort to the participant, and the abort command will just receive
// NoSuchTransaction or TransactionTooOld (because the participant must have already committed if
// the transaction coordinator finished committing).
TEST_F(TransactionCoordinatorServiceTest,
       VoteCommitForCoordinatorThatDoesNotExistSendsVoteAbortToCallingParticipant) {

    TransactionCoordinatorService coordinatorService;
    coordinatorService.voteCommit(
        operationContext(), lsid(), txnNumber(), kTwoShardIdList[0], kDummyTimestamp);

    assertAbortSentAndRespondWithSuccess();
}

TEST_F(TransactionCoordinatorServiceTestSingleTxn,
       ResentFinalVoteCommitOnlySendsCommitToNonAckedParticipants) {

    auto commitDecisionFuture = coordinatorService()->coordinateCommit(
        operationContext(), lsid(), txnNumber(), kTwoShardIdSet);

    coordinatorService()->voteCommit(
        operationContext(), lsid(), txnNumber(), kTwoShardIdList[0], kDummyTimestamp);

    coordinatorService()->voteCommit(
        operationContext(), lsid(), txnNumber(), kTwoShardIdList[1], kDummyTimestamp);

    assertCommitSentAndRespondWithSuccess();
    assertCommitSentAndRespondWithRetryableError();

    coordinatorService()->voteCommit(
        operationContext(), lsid(), txnNumber(), kTwoShardIdList[1], kDummyTimestamp);

    assertCommitSentAndRespondWithSuccess();
}

TEST_F(TransactionCoordinatorServiceTestSingleTxn,
       VoteAbortDoesNotSendAbortIfIsOnlyVoteReceivedSoFar) {

    coordinatorService()->voteAbort(operationContext(), lsid(), txnNumber(), kTwoShardIdList[0]);

    assertNoMessageSent();
}

TEST_F(TransactionCoordinatorServiceTestSingleTxn,
       VoteAbortForCoordinatorThatDoesNotExistDoesNotSendAbort) {

    coordinatorService()->voteAbort(operationContext(), lsid(), txnNumber(), kTwoShardIdList[0]);
    // Coordinator no longer exists.
    coordinatorService()->voteAbort(operationContext(), lsid(), txnNumber(), kTwoShardIdList[0]);

    assertNoMessageSent();
}

TEST_F(TransactionCoordinatorServiceTestSingleTxn,
       VoteAbortSendsAbortIfSomeParticipantsHaveVotedCommit) {

    coordinatorService()->voteCommit(
        operationContext(), lsid(), txnNumber(), kTwoShardIdList[0], kDummyTimestamp);

    coordinatorService()->voteAbort(operationContext(), lsid(), txnNumber(), kTwoShardIdList[1]);

    // This should be sent to the shard that voted commit (s1).
    assertAbortSentAndRespondWithSuccess();
}

TEST_F(TransactionCoordinatorServiceTestSingleTxn,
       VoteAbortAfterReceivingParticipantListSendsAbortToAllParticipantsWhoHaventVotedAbort) {

    auto commitDecisionFuture = coordinatorService()->coordinateCommit(
        operationContext(), lsid(), txnNumber(), kThreeShardIdSet);

    coordinatorService()->voteCommit(
        operationContext(), lsid(), txnNumber(), kThreeShardIdList[0], kDummyTimestamp);

    coordinatorService()->voteAbort(operationContext(), lsid(), txnNumber(), kThreeShardIdList[1]);

    // Should send abort to shards s1 and s3 (the ones that did not vote abort).
    assertAbortSentAndRespondWithSuccess();
    assertAbortSentAndRespondWithSuccess();
}

}  // namespace mongo
