/**
* Copyright (c) 2018 Zilliqa 
* This source code is being disclosed to you solely for the purpose of your participation in 
* testing Zilliqa. You may view, compile and run the code for that purpose and pursuant to 
* the protocols and algorithms that are programmed into, and intended by, the code. You may 
* not do anything else with the code without express permission from Zilliqa Research Pte. Ltd., 
* including modifying or publishing the code (or any part of it), and developing or forming 
* another public or private blockchain network. This source code is provided ‘as is’ and no 
* warranties are given as to title or non-infringement, merchantability or fitness for purpose 
* and, to the extent permitted by law, all liability for your use of the code is disclaimed. 
* Some programs in this code are governed by the GNU General Public License v3.0 (available at 
* https://www.gnu.org/licenses/gpl-3.0.en.html) (‘GPLv3’). The programs that are governed by 
* GPLv3.0 are those programs that are located in the folders src/depends and tests/depends 
* and which include a reference to GPLv3 in their program files.
**/

#include <algorithm>
#include <chrono>
#include <thread>

#include "DirectoryService.h"
#include "common/Constants.h"
#include "common/Messages.h"
#include "common/Serializable.h"
#include "depends/common/RLP.h"
#include "depends/libDatabase/MemoryDB.h"
#include "depends/libTrie/TrieDB.h"
#include "depends/libTrie/TrieHash.h"
#include "libCrypto/Sha2.h"
#include "libMediator/Mediator.h"
#include "libNetwork/P2PComm.h"
#include "libNetwork/Whitelist.h"
#include "libUtils/DataConversion.h"
#include "libUtils/DetachedFunction.h"
#include "libUtils/Logger.h"
#include "libUtils/SanityChecks.h"

using namespace std;
using namespace boost::multiprecision;

#ifndef IS_LOOKUP_NODE
void DirectoryService::StoreDSBlockToStorage()
{
    LOG_MARKER();
    lock_guard<mutex> g(m_mutexPendingDSBlock);
    int result = m_mediator.m_dsBlockChain.AddBlock(*m_pendingDSBlock);
    LOG_EPOCH(
        INFO, to_string(m_mediator.m_currentEpochNum).c_str(),
        "Storing DS Block Number: "
            << m_pendingDSBlock->GetHeader().GetBlockNum()
            << " with Nonce: " << m_pendingDSBlock->GetHeader().GetNonce()
            << ", Difficulty: " << m_pendingDSBlock->GetHeader().GetDifficulty()
            << ", Timestamp: " << m_pendingDSBlock->GetHeader().GetTimestamp());

    if (result == -1)
    {
        LOG_EPOCH(WARNING, to_string(m_mediator.m_currentEpochNum).c_str(),
                  "We failed to add pendingdsblock to dsblockchain.");
        // throw exception();
    }

    // Store DS Block to disk
    vector<unsigned char> serializedDSBlock;
    m_pendingDSBlock->Serialize(serializedDSBlock, 0);
    BlockStorage::GetBlockStorage().PutDSBlock(
        m_pendingDSBlock->GetHeader().GetBlockNum(), serializedDSBlock);
    BlockStorage::GetBlockStorage().PushBackTxBodyDB(
        m_pendingDSBlock->GetHeader().GetBlockNum());
    m_latestActiveDSBlockNum
        = m_pendingDSBlock->GetHeader().GetBlockNum().convert_to<uint64_t>();
    BlockStorage::GetBlockStorage().PutMetadata(
        LATESTACTIVEDSBLOCKNUM,
        DataConversion::StringToCharArray(to_string(m_latestActiveDSBlockNum)));
}

bool DirectoryService::SendDSBlockToLookupNodes()
{
    // Message = [32-byte DS block hash / rand1] [The raw DSBlock consensus message]

    vector<unsigned char> dsblock_message
        = {MessageType::NODE, NodeInstructionType::DSBLOCK};
    unsigned int curr_offset = MessageOffset::BODY;

    dsblock_message.resize(dsblock_message.size() + BLOCK_HASH_SIZE
                           + m_DSBlockConsensusRawMessage.size());

    // 32-byte DS block hash / rand1
    copy(m_mediator.m_dsBlockRand.begin(), m_mediator.m_dsBlockRand.end(),
         dsblock_message.begin() + curr_offset);
    curr_offset += BLOCK_HASH_SIZE;

    // The raw DS Block consensus message (DS block + Sharding structure + Txn sharing assignments)
    copy(m_DSBlockConsensusRawMessage.begin(),
         m_DSBlockConsensusRawMessage.end(),
         dsblock_message.begin() + curr_offset);

    m_mediator.m_lookup->SendMessageToLookupNodes(dsblock_message);
    LOG_EPOCH(INFO, to_string(m_mediator.m_currentEpochNum).c_str(),
              "I'm part of the subset of the DS committee that will send the "
              "DSBlock to the lookup nodes");

    return true;
}

void DirectoryService::DetermineNodesToSendDSBlockTo(
    const Peer& winnerpeer, unsigned int& my_DS_cluster_num,
    unsigned int& my_pow1nodes_cluster_lo,
    unsigned int& my_pow1nodes_cluster_hi) const
{
    // Multicast DSBLOCK message to everyone in the network
    // Multicast assignments:
    // 1. Divide DS committee into clusters of size 20
    // 2. Divide the sorted PoW1 submitters into (DS committee / 20) clusters
    LOG_MARKER();

    LOG_EPOCH(
        INFO, to_string(m_mediator.m_currentEpochNum).c_str(),
        "New DSBlock created with chosen nonce   = 0x"
            << hex << "\n"
            << m_mediator.m_dsBlockChain.GetLastBlock().GetHeader().GetNonce()
            << "\n"
            << "New DSBlock hash is                     = 0x"
            << DataConversion::charArrToHexStr(m_mediator.m_dsBlockRand) << "\n"
            << "New DS leader (PoW1 winner)          = " << winnerpeer);

    unsigned int num_DS_clusters
        = m_mediator.m_DSCommittee.size() / DS_MULTICAST_CLUSTER_SIZE;
    if ((m_mediator.m_DSCommittee.size() % DS_MULTICAST_CLUSTER_SIZE) > 0)
    {
        num_DS_clusters++;
    }

    unsigned int pow1nodes_cluster_size
        = m_allPoWConns.size() / num_DS_clusters;
    if ((m_allPoWConns.size() % num_DS_clusters) > 0)
    {
        pow1nodes_cluster_size++;
    }

    my_DS_cluster_num = m_consensusMyID / DS_MULTICAST_CLUSTER_SIZE;
    my_pow1nodes_cluster_lo = my_DS_cluster_num * pow1nodes_cluster_size;
    my_pow1nodes_cluster_hi
        = my_pow1nodes_cluster_lo + pow1nodes_cluster_size - 1;

    if (my_pow1nodes_cluster_hi >= m_allPoWConns.size())
    {
        my_pow1nodes_cluster_hi = m_allPoWConns.size() - 1;
    }
}

void DirectoryService::SendDSBlockToCluster(
    unsigned int my_pow1nodes_cluster_lo, unsigned int my_pow1nodes_cluster_hi)
{
    // Message = [32-byte DS block hash / rand1] [The raw DSBlock consensus message]

    vector<unsigned char> dsblock_message
        = {MessageType::NODE, NodeInstructionType::DSBLOCK};
    unsigned int curr_offset = MessageOffset::BODY;

    dsblock_message.resize(dsblock_message.size() + BLOCK_HASH_SIZE
                           + m_DSBlockConsensusRawMessage.size());

    // 32-byte DS block hash / rand1
    copy(m_mediator.m_dsBlockRand.begin(), m_mediator.m_dsBlockRand.end(),
         dsblock_message.begin() + curr_offset);
    curr_offset += BLOCK_HASH_SIZE;

    // The raw DS Block consensus message (DS block + Sharding structure + Txn sharing assignments)
    copy(m_DSBlockConsensusRawMessage.begin(),
         m_DSBlockConsensusRawMessage.end(),
         dsblock_message.begin() + curr_offset);

    vector<Peer> pow1nodes_cluster;

    auto p = m_allPoWConns.begin();
    advance(p, my_pow1nodes_cluster_lo);

    for (unsigned int i = my_pow1nodes_cluster_lo; i <= my_pow1nodes_cluster_hi;
         i++)
    {
        pow1nodes_cluster.push_back(p->second);
        p++;
    }

    LOG_EPOCH(INFO, to_string(m_mediator.m_currentEpochNum).c_str(),
              "Multicasting DSBLOCK message to PoW1 nodes "
                  << my_pow1nodes_cluster_lo << " to "
                  << my_pow1nodes_cluster_hi);

    SHA2<HASH_TYPE::HASH_VARIANT_256> sha256;
    sha256.Update(dsblock_message);
    vector<unsigned char> this_msg_hash = sha256.Finalize();
    LOG_STATE(
        "[INFOR]["
        << std::setw(15) << std::left
        << m_mediator.m_selfPeer.GetPrintableIPAddress() << "]["
        << DataConversion::Uint8VecToHexStr(this_msg_hash).substr(0, 6) << "]["
        << DataConversion::charArrToHexStr(m_mediator.m_dsBlockRand)
               .substr(0, 6)
        << "]["
        << m_mediator.m_dsBlockChain.GetLastBlock().GetHeader().GetBlockNum()
        << "] DSBLOCKGEN");

    P2PComm::GetInstance().SendBroadcastMessage(pow1nodes_cluster,
                                                dsblock_message);
}

void DirectoryService::UpdateMyDSModeAndConsensusId()
{
    // If I was DS primary, now I will only be DS backup
    if (m_mode == PRIMARY_DS)
    {
        LOG_EPOCH(INFO, to_string(m_mediator.m_currentEpochNum).c_str(),
                  "I am now just a backup DS");
        LOG_EPOCHINFO(to_string(m_mediator.m_currentEpochNum).c_str(),
                      DS_BACKUP_MSG);
        m_mode = BACKUP_DS;
        m_consensusMyID++;

        LOG_STATE("[IDENT][" << setw(15) << left
                             << m_mediator.m_selfPeer.GetPrintableIPAddress()
                             << "][" << setw(6) << left << m_consensusMyID
                             << "] DSBK");
    }
    // Check if I am the oldest backup DS (I will no longer be part of the DS committee)
    else if ((uint32_t)(m_consensusMyID + 1) == m_mediator.m_DSCommittee.size())
    {
        LOG_EPOCH(
            INFO, to_string(m_mediator.m_currentEpochNum).c_str(),
            "I am the oldest backup DS -> now kicked out of DS committee :-("
                << "\n"
                << DS_KICKOUT_MSG);
        m_mode = IDLE;

        LOG_STATE("[IDENT][" << setw(15) << left
                             << m_mediator.m_selfPeer.GetPrintableIPAddress()
                             << "][      ] IDLE");
    }
    // Other DS nodes continue to remain DS backups
    else
    {
        m_consensusMyID++;

        LOG_STATE("[IDENT][" << setw(15) << left
                             << m_mediator.m_selfPeer.GetPrintableIPAddress()
                             << "][" << setw(6) << left << m_consensusMyID
                             << "] DSBK");
    }
}

void DirectoryService::UpdateDSCommiteeComposition(const Peer& winnerpeer)
{
    LOG_MARKER();

    m_mediator.m_DSCommittee.push_front(make_pair(
        m_mediator.m_dsBlockChain.GetLastBlock().GetHeader().GetMinerPubKey(),
        winnerpeer));
    m_mediator.m_DSCommittee.pop_back();

    // Remove the new winner of pow1 from m_allpowconn. He is the new ds leader and do not need to do pow anymore
    m_allPoWConns.erase(
        m_mediator.m_dsBlockChain.GetLastBlock().GetHeader().GetMinerPubKey());
}

void DirectoryService::ProcessDSBlockConsensusWhenDone(
    const vector<unsigned char>& message, unsigned int offset)
{
    LOG_EPOCH(INFO, to_string(m_mediator.m_currentEpochNum).c_str(),
              "DS block consensus is DONE!!!");

    if (m_mode == PRIMARY_DS)
    {
        LOG_STATE("[DSCON]["
                  << setw(15) << left
                  << m_mediator.m_selfPeer.GetPrintableIPAddress() << "]["
                  << m_mediator.m_txBlockChain.GetBlockCount() << "] DONE");
    }

    {
        lock_guard<mutex> g(m_mutexPendingDSBlock);

        if (m_pendingDSBlock == nullptr)
        {
            LOG_GENERAL(FATAL,
                        "assertion failed (" << __FILE__ << ":" << __LINE__
                                             << ": " << __FUNCTION__ << ")");
        }

        // Update the DS block with the co-signatures from the consensus

        // Make the update in the DS block object
        m_pendingDSBlock->SetCoSignatures(*m_consensusObject);

        // Make the update in the raw DS Block message buffer (which includes the sharding structure and txn sharing assignments)
        m_pendingDSBlock->Serialize(m_DSBlockConsensusRawMessage, 0);

        // Check for missing blocks
        if (m_pendingDSBlock->GetHeader().GetBlockNum()
            != m_mediator.m_dsBlockChain.GetBlockCount() + 1)
        {
            LOG_EPOCH(WARNING, to_string(m_mediator.m_currentEpochNum).c_str(),
                      "We are missing some blocks. What to do here?");
        }
    }

    // Add the DS block to the chain
    StoreDSBlockToStorage();
    DSBlock lastDSBlock = m_mediator.m_dsBlockChain.GetLastBlock();

    m_mediator.UpdateDSBlockRand();

    Peer winnerpeer;
    {
        lock_guard<mutex> g(m_mutexAllPoWConns);
        winnerpeer = m_allPoWConns.at(lastDSBlock.GetHeader().GetMinerPubKey());
    }

    LOG_EPOCH(INFO, to_string(m_mediator.m_currentEpochNum).c_str(),
              "DSBlock to be sent to the lookup nodes");

    // TODO: Refine this
    unsigned int nodeToSendToLookUpLo = COMM_SIZE / 4;
    unsigned int nodeToSendToLookUpHi
        = nodeToSendToLookUpLo + TX_SHARING_CLUSTER_SIZE;

    if (m_consensusMyID > nodeToSendToLookUpLo
        && m_consensusMyID < nodeToSendToLookUpHi)
    {
        SendDSBlockToLookupNodes();
    }

    unsigned int my_DS_cluster_num, my_pow1nodes_cluster_lo,
        my_pow1nodes_cluster_hi;
    DetermineNodesToSendDSBlockTo(winnerpeer, my_DS_cluster_num,
                                  my_pow1nodes_cluster_lo,
                                  my_pow1nodes_cluster_hi);

    LOG_STATE("[DSBLK][" << setw(15) << left
                         << m_mediator.m_selfPeer.GetPrintableIPAddress()
                         << "][" << m_mediator.m_txBlockChain.GetBlockCount()
                         << "] BEFORE SENDING DSBLOCK");

    // Too few target nodes - avoid asking all DS clusters to send
    if ((my_DS_cluster_num + 1) <= m_allPoWConns.size())
    {
        SendDSBlockToCluster(my_pow1nodes_cluster_lo, my_pow1nodes_cluster_hi);
    }

    LOG_STATE("[DSBLK][" << setw(15) << left
                         << m_mediator.m_selfPeer.GetPrintableIPAddress()
                         << "][" << m_mediator.m_txBlockChain.GetBlockCount()
                         << "] AFTER SENDING DSBLOCK");

    UpdateDSCommiteeComposition(winnerpeer);
    UpdateMyDSModeAndConsensusId();

    {
        lock_guard<mutex> g(m_mutexAllPOW2);
        m_allPoW2s.clear();
        m_sortedPoW2s.clear();
        m_viewChangeCounter = 0;
    }

    if (m_mode != IDLE)
    {
        if (TEST_NET_MODE)
        {
            LOG_GENERAL(INFO, "Updating shard whitelist");
            Whitelist::GetInstance().UpdateShardWhitelist();
        }

        // Start sharding work
        SetState(MICROBLOCK_SUBMISSION);

        // Check for state change. If it get stuck at microblock submission for too long,
        // Move on to finalblock without the microblock
        std::unique_lock<std::mutex> cv_lk(m_MutexScheduleFinalBlockConsensus);
        if (cv_scheduleFinalBlockConsensus.wait_for(
                cv_lk, std::chrono::seconds(SHARDING_TIMEOUT))
            == std::cv_status::timeout)
        {
            LOG_GENERAL(
                WARNING,
                "Timeout: Didn't receive all Microblock. Proceeds without it");

            auto func
                = [this]() mutable -> void { RunConsensusOnFinalBlock(); };

            DetachedFunction(1, func);
        }
    }
    else
    {
        // Tell my Node class to start Tx submission
        m_mediator.m_node->SetState(Node::NodeState::TX_SUBMISSION);
    }
}
#endif // IS_LOOKUP_NODE

bool DirectoryService::ProcessDSBlockConsensus(
    const vector<unsigned char>& message, unsigned int offset, const Peer& from)
{
#ifndef IS_LOOKUP_NODE
    LOG_MARKER();
    // Consensus messages must be processed in correct sequence as they come in
    // It is possible for ANNOUNCE to arrive before correct DS state
    // In that case, ANNOUNCE will sleep for a second below
    // If COLLECTIVESIG also comes in, it's then possible COLLECTIVESIG will be processed before ANNOUNCE!
    // So, ANNOUNCE should acquire a lock here

    std::unique_lock<mutex> cv_lk(m_mutexProcessConsensusMessage);
    if (cv_processConsensusMessage.wait_for(
            cv_lk, std::chrono::seconds(CONSENSUS_MSG_ORDER_BLOCK_WINDOW),
            [this, message, offset]() -> bool {
                lock_guard<mutex> g(m_mutexConsensus);
                if (m_mediator.m_lookup->m_syncType != SyncType::NO_SYNC)
                {
                    LOG_GENERAL(WARNING,
                                "The node started the process of rejoining, "
                                "Ignore rest of "
                                "consensus msg.")
                    return false;
                }

                if (m_consensusObject == nullptr)
                {
                    LOG_GENERAL(WARNING,
                                "m_consensusObject is a nullptr. It has not "
                                "been initialized.")
                    return false;
                }
                return m_consensusObject->CanProcessMessage(message, offset);
            }))
    {
        // Correct order preserved
    }
    else
    {
        LOG_GENERAL(
            WARNING,
            "Timeout while waiting for correct order of DS Block consensus "
            "messages");
        return false;
    }

    lock_guard<mutex> g(m_mutexConsensus);

    // Wait until ProcessDSBlock in the case that primary sent announcement pretty early
    if ((m_state == POW1_SUBMISSION) || (m_state == DSBLOCK_CONSENSUS_PREP))
    {
        cv_DSBlockConsensus.notify_all();

        std::unique_lock<std::mutex> cv_lk(m_MutexCVDSBlockConsensusObject);

        if (cv_DSBlockConsensusObject.wait_for(
                cv_lk, std::chrono::seconds(CONSENSUS_OBJECT_TIMEOUT))
            == std::cv_status::timeout)
        {
            LOG_EPOCH(WARNING, to_string(m_mediator.m_currentEpochNum).c_str(),
                      "Time out while waiting for state transition and "
                      "consensus object creation ");
        }

        LOG_EPOCH(INFO, to_string(m_mediator.m_currentEpochNum).c_str(),
                  "State transition is completed and consensus object "
                  "creation. (check for timeout)");
    }

    if (!CheckState(PROCESS_DSBLOCKCONSENSUS))
    {
        LOG_EPOCH(INFO, to_string(m_mediator.m_currentEpochNum).c_str(),
                  "Ignoring consensus message");
        return false;
    }

    bool result = m_consensusObject->ProcessMessage(message, offset, from);
    ConsensusCommon::State state = m_consensusObject->GetState();

    if (state == ConsensusCommon::State::DONE)
    {
        m_viewChangeCounter = 0;
        cv_viewChangeDSBlock.notify_all();
        ProcessDSBlockConsensusWhenDone(message, offset);
    }
    else if (state == ConsensusCommon::State::ERROR)
    {
        LOG_EPOCH(INFO, to_string(m_mediator.m_currentEpochNum).c_str(),
                  "Oops, no consensus reached - what to do now???");
        LOG_EPOCH(
            INFO, to_string(m_mediator.m_currentEpochNum).c_str(),
            "DEBUG for verify sig m_allPoWConns  size is "
                << m_allPoWConns.size()
                << ". Please check numbers of pow1 receivied by this node");

        // Wait for view change to happen
        //throw exception();
        // if (m_mode != PRIMARY_DS)
        // {
        //     RejoinAsDS();
        // }
    }
    else
    {
        LOG_EPOCH(INFO, to_string(m_mediator.m_currentEpochNum).c_str(),
                  "Consensus state = " << m_consensusObject->GetStateString());
        cv_processConsensusMessage.notify_all();
    }

    return result;
#else // IS_LOOKUP_NODE
    return true;
#endif // IS_LOOKUP_NODE
}
