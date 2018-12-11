// Copyright (c) 2014-2017 The Maza Network developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "activemazanode.h"
#include "addrman.h"
#include "alert.h"
#include "clientversion.h"
#include "governance.h"
#include "mazanode-payments.h"
#include "mazanode-sync.h"
#include "mazanodeman.h"
#include "messagesigner.h"
#include "netfulfilledman.h"
#include "netmessagemaker.h"
#ifdef ENABLE_WALLET
#include "privatesend-client.h"
#endif // ENABLE_WALLET
#include "script/standard.h"
#include "ui_interface.h"
#include "util.h"
#include "warnings.h"

/** Mazanode manager */
CMazanodeMan mnodeman;

const std::string CMazanodeMan::SERIALIZATION_VERSION_STRING = "CMazanodeMan-Version-8";
const int CMazanodeMan::LAST_PAID_SCAN_BLOCKS = 100;

struct CompareLastPaidBlock
{
    bool operator()(const std::pair<int, const CMazanode*>& t1,
                    const std::pair<int, const CMazanode*>& t2) const
    {
        return (t1.first != t2.first) ? (t1.first < t2.first) : (t1.second->outpoint < t2.second->outpoint);
    }
};

struct CompareScoreMN
{
    bool operator()(const std::pair<arith_uint256, const CMazanode*>& t1,
                    const std::pair<arith_uint256, const CMazanode*>& t2) const
    {
        return (t1.first != t2.first) ? (t1.first < t2.first) : (t1.second->outpoint < t2.second->outpoint);
    }
};

struct CompareByAddr

{
    bool operator()(const CMazanode* t1,
                    const CMazanode* t2) const
    {
        return t1->addr < t2->addr;
    }
};

CMazanodeMan::CMazanodeMan():
    cs(),
    mapMazanodes(),
    mAskedUsForMazanodeList(),
    mWeAskedForMazanodeList(),
    mWeAskedForMazanodeListEntry(),
    mWeAskedForVerification(),
    mMnbRecoveryRequests(),
    mMnbRecoveryGoodReplies(),
    listScheduledMnbRequestConnections(),
    fMazanodesAdded(false),
    fMazanodesRemoved(false),
    vecDirtyGovernanceObjectHashes(),
    nLastSentinelPingTime(0),
    mapSeenMazanodeBroadcast(),
    mapSeenMazanodePing(),
    nDsqCount(0)
{}

bool CMazanodeMan::Add(CMazanode &mn)
{
    LOCK(cs);

    if (Has(mn.outpoint)) return false;

    LogPrint("mazanode", "CMazanodeMan::Add -- Adding new Mazanode: addr=%s, %i now\n", mn.addr.ToString(), size() + 1);
    mapMazanodes[mn.outpoint] = mn;
    fMazanodesAdded = true;
    return true;
}

void CMazanodeMan::AskForMN(CNode* pnode, const COutPoint& outpoint, CConnman& connman)
{
    if(!pnode) return;

    CNetMsgMaker msgMaker(pnode->GetSendVersion());
    LOCK(cs);

    CService addrSquashed = Params().AllowMultiplePorts() ? (CService)pnode->addr : CService(pnode->addr, 0);
    auto it1 = mWeAskedForMazanodeListEntry.find(outpoint);
    if (it1 != mWeAskedForMazanodeListEntry.end()) {
        auto it2 = it1->second.find(addrSquashed);
        if (it2 != it1->second.end()) {
            if (GetTime() < it2->second) {
                // we've asked recently, should not repeat too often or we could get banned
                return;
            }
            // we asked this node for this outpoint but it's ok to ask again already
            LogPrintf("CMazanodeMan::AskForMN -- Asking same peer %s for missing mazanode entry again: %s\n", addrSquashed.ToString(), outpoint.ToStringShort());
        } else {
            // we already asked for this outpoint but not this node
            LogPrintf("CMazanodeMan::AskForMN -- Asking new peer %s for missing mazanode entry: %s\n", addrSquashed.ToString(), outpoint.ToStringShort());
        }
    } else {
        // we never asked any node for this outpoint
        LogPrintf("CMazanodeMan::AskForMN -- Asking peer %s for missing mazanode entry for the first time: %s\n", addrSquashed.ToString(), outpoint.ToStringShort());
    }
    mWeAskedForMazanodeListEntry[outpoint][addrSquashed] = GetTime() + DSEG_UPDATE_SECONDS;

    if (pnode->GetSendVersion() == 70208) {
        connman.PushMessage(pnode, msgMaker.Make(NetMsgType::DSEG, CTxIn(outpoint)));
    } else {
        connman.PushMessage(pnode, msgMaker.Make(NetMsgType::DSEG, outpoint));
    }
}

bool CMazanodeMan::AllowMixing(const COutPoint &outpoint)
{
    LOCK(cs);
    CMazanode* pmn = Find(outpoint);
    if (!pmn) {
        return false;
    }
    nDsqCount++;
    pmn->nLastDsq = nDsqCount;
    pmn->fAllowMixingTx = true;

    return true;
}

bool CMazanodeMan::DisallowMixing(const COutPoint &outpoint)
{
    LOCK(cs);
    CMazanode* pmn = Find(outpoint);
    if (!pmn) {
        return false;
    }
    pmn->fAllowMixingTx = false;

    return true;
}

bool CMazanodeMan::PoSeBan(const COutPoint &outpoint)
{
    LOCK(cs);
    CMazanode* pmn = Find(outpoint);
    if (!pmn) {
        return false;
    }
    pmn->PoSeBan();

    return true;
}

void CMazanodeMan::Check()
{
    LOCK2(cs_main, cs);

    LogPrint("mazanode", "CMazanodeMan::Check -- nLastSentinelPingTime=%d, IsSentinelPingActive()=%d\n", nLastSentinelPingTime, IsSentinelPingActive());

    for (auto& mnpair : mapMazanodes) {
        // NOTE: internally it checks only every MAZANODE_CHECK_SECONDS seconds
        // since the last time, so expect some MNs to skip this
        mnpair.second.Check();
    }
}

void CMazanodeMan::CheckAndRemove(CConnman& connman)
{
    if(!mazanodeSync.IsMazanodeListSynced()) return;

    LogPrintf("CMazanodeMan::CheckAndRemove\n");

    {
        // Need LOCK2 here to ensure consistent locking order because code below locks cs_main
        // in CheckMnbAndUpdateMazanodeList()
        LOCK2(cs_main, cs);

        Check();

        // Remove spent mazanodes, prepare structures and make requests to reasure the state of inactive ones
        rank_pair_vec_t vecMazanodeRanks;
        // ask for up to MNB_RECOVERY_MAX_ASK_ENTRIES mazanode entries at a time
        int nAskForMnbRecovery = MNB_RECOVERY_MAX_ASK_ENTRIES;
        std::map<COutPoint, CMazanode>::iterator it = mapMazanodes.begin();
        while (it != mapMazanodes.end()) {
            CMazanodeBroadcast mnb = CMazanodeBroadcast(it->second);
            uint256 hash = mnb.GetHash();
            // If collateral was spent ...
            if (it->second.IsOutpointSpent()) {
                LogPrint("mazanode", "CMazanodeMan::CheckAndRemove -- Removing Mazanode: %s  addr=%s  %i now\n", it->second.GetStateString(), it->second.addr.ToString(), size() - 1);

                // erase all of the broadcasts we've seen from this txin, ...
                mapSeenMazanodeBroadcast.erase(hash);
                mWeAskedForMazanodeListEntry.erase(it->first);

                // and finally remove it from the list
                it->second.FlagGovernanceItemsAsDirty();
                mapMazanodes.erase(it++);
                fMazanodesRemoved = true;
            } else {
                bool fAsk = (nAskForMnbRecovery > 0) &&
                            mazanodeSync.IsSynced() &&
                            it->second.IsNewStartRequired() &&
                            !IsMnbRecoveryRequested(hash) &&
                            !IsArgSet("-connect");
                if(fAsk) {
                    // this mn is in a non-recoverable state and we haven't asked other nodes yet
                    std::set<CService> setRequested;
                    // calulate only once and only when it's needed
                    if(vecMazanodeRanks.empty()) {
                        int nRandomBlockHeight = GetRandInt(nCachedBlockHeight);
                        GetMazanodeRanks(vecMazanodeRanks, nRandomBlockHeight);
                    }
                    bool fAskedForMnbRecovery = false;
                    // ask first MNB_RECOVERY_QUORUM_TOTAL mazanodes we can connect to and we haven't asked recently
                    for(int i = 0; setRequested.size() < MNB_RECOVERY_QUORUM_TOTAL && i < (int)vecMazanodeRanks.size(); i++) {
                        // avoid banning
                        if(mWeAskedForMazanodeListEntry.count(it->first) && mWeAskedForMazanodeListEntry[it->first].count(vecMazanodeRanks[i].second.addr)) continue;
                        // didn't ask recently, ok to ask now
                        CService addr = vecMazanodeRanks[i].second.addr;
                        setRequested.insert(addr);
                        listScheduledMnbRequestConnections.push_back(std::make_pair(addr, hash));
                        fAskedForMnbRecovery = true;
                    }
                    if(fAskedForMnbRecovery) {
                        LogPrint("mazanode", "CMazanodeMan::CheckAndRemove -- Recovery initiated, mazanode=%s\n", it->first.ToStringShort());
                        nAskForMnbRecovery--;
                    }
                    // wait for mnb recovery replies for MNB_RECOVERY_WAIT_SECONDS seconds
                    mMnbRecoveryRequests[hash] = std::make_pair(GetTime() + MNB_RECOVERY_WAIT_SECONDS, setRequested);
                }
                ++it;
            }
        }

        // proces replies for MAZANODE_NEW_START_REQUIRED mazanodes
        LogPrint("mazanode", "CMazanodeMan::CheckAndRemove -- mMnbRecoveryGoodReplies size=%d\n", (int)mMnbRecoveryGoodReplies.size());
        std::map<uint256, std::vector<CMazanodeBroadcast> >::iterator itMnbReplies = mMnbRecoveryGoodReplies.begin();
        while(itMnbReplies != mMnbRecoveryGoodReplies.end()){
            if(mMnbRecoveryRequests[itMnbReplies->first].first < GetTime()) {
                // all nodes we asked should have replied now
                if(itMnbReplies->second.size() >= MNB_RECOVERY_QUORUM_REQUIRED) {
                    // majority of nodes we asked agrees that this mn doesn't require new mnb, reprocess one of new mnbs
                    LogPrint("mazanode", "CMazanodeMan::CheckAndRemove -- reprocessing mnb, mazanode=%s\n", itMnbReplies->second[0].outpoint.ToStringShort());
                    // mapSeenMazanodeBroadcast.erase(itMnbReplies->first);
                    int nDos;
                    itMnbReplies->second[0].fRecovery = true;
                    CheckMnbAndUpdateMazanodeList(NULL, itMnbReplies->second[0], nDos, connman);
                }
                LogPrint("mazanode", "CMazanodeMan::CheckAndRemove -- removing mnb recovery reply, mazanode=%s, size=%d\n", itMnbReplies->second[0].outpoint.ToStringShort(), (int)itMnbReplies->second.size());
                mMnbRecoveryGoodReplies.erase(itMnbReplies++);
            } else {
                ++itMnbReplies;
            }
        }
    }
    {
        // no need for cm_main below
        LOCK(cs);

        auto itMnbRequest = mMnbRecoveryRequests.begin();
        while(itMnbRequest != mMnbRecoveryRequests.end()){
            // Allow this mnb to be re-verified again after MNB_RECOVERY_RETRY_SECONDS seconds
            // if mn is still in MAZANODE_NEW_START_REQUIRED state.
            if(GetTime() - itMnbRequest->second.first > MNB_RECOVERY_RETRY_SECONDS) {
                mMnbRecoveryRequests.erase(itMnbRequest++);
            } else {
                ++itMnbRequest;
            }
        }

        // check who's asked for the Mazanode list
        auto it1 = mAskedUsForMazanodeList.begin();
        while(it1 != mAskedUsForMazanodeList.end()){
            if((*it1).second < GetTime()) {
                mAskedUsForMazanodeList.erase(it1++);
            } else {
                ++it1;
            }
        }

        // check who we asked for the Mazanode list
        it1 = mWeAskedForMazanodeList.begin();
        while(it1 != mWeAskedForMazanodeList.end()){
            if((*it1).second < GetTime()){
                mWeAskedForMazanodeList.erase(it1++);
            } else {
                ++it1;
            }
        }

        // check which Mazanodes we've asked for
        auto it2 = mWeAskedForMazanodeListEntry.begin();
        while(it2 != mWeAskedForMazanodeListEntry.end()){
            auto it3 = it2->second.begin();
            while(it3 != it2->second.end()){
                if(it3->second < GetTime()){
                    it2->second.erase(it3++);
                } else {
                    ++it3;
                }
            }
            if(it2->second.empty()) {
                mWeAskedForMazanodeListEntry.erase(it2++);
            } else {
                ++it2;
            }
        }

        auto it3 = mWeAskedForVerification.begin();
        while(it3 != mWeAskedForVerification.end()){
            if(it3->second.nBlockHeight < nCachedBlockHeight - MAX_POSE_BLOCKS) {
                mWeAskedForVerification.erase(it3++);
            } else {
                ++it3;
            }
        }

        // NOTE: do not expire mapSeenMazanodeBroadcast entries here, clean them on mnb updates!

        // remove expired mapSeenMazanodePing
        std::map<uint256, CMazanodePing>::iterator it4 = mapSeenMazanodePing.begin();
        while(it4 != mapSeenMazanodePing.end()){
            if((*it4).second.IsExpired()) {
                LogPrint("mazanode", "CMazanodeMan::CheckAndRemove -- Removing expired Mazanode ping: hash=%s\n", (*it4).second.GetHash().ToString());
                mapSeenMazanodePing.erase(it4++);
            } else {
                ++it4;
            }
        }

        // remove expired mapSeenMazanodeVerification
        std::map<uint256, CMazanodeVerification>::iterator itv2 = mapSeenMazanodeVerification.begin();
        while(itv2 != mapSeenMazanodeVerification.end()){
            if((*itv2).second.nBlockHeight < nCachedBlockHeight - MAX_POSE_BLOCKS){
                LogPrint("mazanode", "CMazanodeMan::CheckAndRemove -- Removing expired Mazanode verification: hash=%s\n", (*itv2).first.ToString());
                mapSeenMazanodeVerification.erase(itv2++);
            } else {
                ++itv2;
            }
        }

        LogPrintf("CMazanodeMan::CheckAndRemove -- %s\n", ToString());
    }

    if(fMazanodesRemoved) {
        NotifyMazanodeUpdates(connman);
    }
}

void CMazanodeMan::Clear()
{
    LOCK(cs);
    mapMazanodes.clear();
    mAskedUsForMazanodeList.clear();
    mWeAskedForMazanodeList.clear();
    mWeAskedForMazanodeListEntry.clear();
    mapSeenMazanodeBroadcast.clear();
    mapSeenMazanodePing.clear();
    nDsqCount = 0;
    nLastSentinelPingTime = 0;
}

int CMazanodeMan::CountMazanodes(int nProtocolVersion)
{
    LOCK(cs);
    int nCount = 0;
    nProtocolVersion = nProtocolVersion == -1 ? mnpayments.GetMinMazanodePaymentsProto() : nProtocolVersion;

    for (const auto& mnpair : mapMazanodes) {
        if(mnpair.second.nProtocolVersion < nProtocolVersion) continue;
        nCount++;
    }

    return nCount;
}

int CMazanodeMan::CountEnabled(int nProtocolVersion)
{
    LOCK(cs);
    int nCount = 0;
    nProtocolVersion = nProtocolVersion == -1 ? mnpayments.GetMinMazanodePaymentsProto() : nProtocolVersion;

    for (const auto& mnpair : mapMazanodes) {
        if(mnpair.second.nProtocolVersion < nProtocolVersion || !mnpair.second.IsEnabled()) continue;
        nCount++;
    }

    return nCount;
}

/* Only IPv4 mazanodes are allowed in 12.1, saving this for later
int CMazanodeMan::CountByIP(int nNetworkType)
{
    LOCK(cs);
    int nNodeCount = 0;

    for (const auto& mnpair : mapMazanodes)
        if ((nNetworkType == NET_IPV4 && mnpair.second.addr.IsIPv4()) ||
            (nNetworkType == NET_TOR  && mnpair.second.addr.IsTor())  ||
            (nNetworkType == NET_IPV6 && mnpair.second.addr.IsIPv6())) {
                nNodeCount++;
        }

    return nNodeCount;
}
*/

void CMazanodeMan::DsegUpdate(CNode* pnode, CConnman& connman)
{
    CNetMsgMaker msgMaker(pnode->GetSendVersion());
    LOCK(cs);

    CService addrSquashed = Params().AllowMultiplePorts() ? (CService)pnode->addr : CService(pnode->addr, 0);
    if(Params().NetworkIDString() == CBaseChainParams::MAIN) {
        if(!(pnode->addr.IsRFC1918() || pnode->addr.IsLocal())) {
            auto it = mWeAskedForMazanodeList.find(addrSquashed);
            if(it != mWeAskedForMazanodeList.end() && GetTime() < (*it).second) {
                LogPrintf("CMazanodeMan::DsegUpdate -- we already asked %s for the list; skipping...\n", addrSquashed.ToString());
                return;
            }
        }
    }

    if (pnode->GetSendVersion() == 70208) {
        connman.PushMessage(pnode, msgMaker.Make(NetMsgType::DSEG, CTxIn()));
    } else {
        connman.PushMessage(pnode, msgMaker.Make(NetMsgType::DSEG, COutPoint()));
    }
    int64_t askAgain = GetTime() + DSEG_UPDATE_SECONDS;
    mWeAskedForMazanodeList[addrSquashed] = askAgain;

    LogPrint("mazanode", "CMazanodeMan::DsegUpdate -- asked %s for the list\n", pnode->addr.ToString());
}

CMazanode* CMazanodeMan::Find(const COutPoint &outpoint)
{
    LOCK(cs);
    auto it = mapMazanodes.find(outpoint);
    return it == mapMazanodes.end() ? NULL : &(it->second);
}

bool CMazanodeMan::Get(const COutPoint& outpoint, CMazanode& mazanodeRet)
{
    // Theses mutexes are recursive so double locking by the same thread is safe.
    LOCK(cs);
    auto it = mapMazanodes.find(outpoint);
    if (it == mapMazanodes.end()) {
        return false;
    }

    mazanodeRet = it->second;
    return true;
}

bool CMazanodeMan::GetMazanodeInfo(const COutPoint& outpoint, mazanode_info_t& mnInfoRet)
{
    LOCK(cs);
    auto it = mapMazanodes.find(outpoint);
    if (it == mapMazanodes.end()) {
        return false;
    }
    mnInfoRet = it->second.GetInfo();
    return true;
}

bool CMazanodeMan::GetMazanodeInfo(const CPubKey& pubKeyMazanode, mazanode_info_t& mnInfoRet)
{
    LOCK(cs);
    for (const auto& mnpair : mapMazanodes) {
        if (mnpair.second.pubKeyMazanode == pubKeyMazanode) {
            mnInfoRet = mnpair.second.GetInfo();
            return true;
        }
    }
    return false;
}

bool CMazanodeMan::GetMazanodeInfo(const CScript& payee, mazanode_info_t& mnInfoRet)
{
    LOCK(cs);
    for (const auto& mnpair : mapMazanodes) {
        CScript scriptCollateralAddress = GetScriptForDestination(mnpair.second.pubKeyCollateralAddress.GetID());
        if (scriptCollateralAddress == payee) {
            mnInfoRet = mnpair.second.GetInfo();
            return true;
        }
    }
    return false;
}

bool CMazanodeMan::Has(const COutPoint& outpoint)
{
    LOCK(cs);
    return mapMazanodes.find(outpoint) != mapMazanodes.end();
}

//
// Deterministically select the oldest/best mazanode to pay on the network
//
bool CMazanodeMan::GetNextMazanodeInQueueForPayment(bool fFilterSigTime, int& nCountRet, mazanode_info_t& mnInfoRet)
{
    return GetNextMazanodeInQueueForPayment(nCachedBlockHeight, fFilterSigTime, nCountRet, mnInfoRet);
}

bool CMazanodeMan::GetNextMazanodeInQueueForPayment(int nBlockHeight, bool fFilterSigTime, int& nCountRet, mazanode_info_t& mnInfoRet)
{
    mnInfoRet = mazanode_info_t();
    nCountRet = 0;

    if (!mazanodeSync.IsWinnersListSynced()) {
        // without winner list we can't reliably find the next winner anyway
        return false;
    }

    // Need LOCK2 here to ensure consistent locking order because the GetBlockHash call below locks cs_main
    LOCK2(cs_main,cs);

    std::vector<std::pair<int, const CMazanode*> > vecMazanodeLastPaid;

    /*
        Make a vector with all of the last paid times
    */

    int nMnCount = CountMazanodes();

    for (const auto& mnpair : mapMazanodes) {
        if(!mnpair.second.IsValidForPayment()) continue;

        //check protocol version
        if(mnpair.second.nProtocolVersion < mnpayments.GetMinMazanodePaymentsProto()) continue;

        //it's in the list (up to 8 entries ahead of current block to allow propagation) -- so let's skip it
        if(mnpayments.IsScheduled(mnpair.second, nBlockHeight)) continue;

        //it's too new, wait for a cycle
        if(fFilterSigTime && mnpair.second.sigTime + (nMnCount*2.6*60) > GetAdjustedTime()) continue;

        //make sure it has at least as many confirmations as there are mazanodes
        if(GetUTXOConfirmations(mnpair.first) < nMnCount) continue;

        vecMazanodeLastPaid.push_back(std::make_pair(mnpair.second.GetLastPaidBlock(), &mnpair.second));
    }

    nCountRet = (int)vecMazanodeLastPaid.size();

    //when the network is in the process of upgrading, don't penalize nodes that recently restarted
    if(fFilterSigTime && nCountRet < nMnCount/3)
        return GetNextMazanodeInQueueForPayment(nBlockHeight, false, nCountRet, mnInfoRet);

    // Sort them low to high
    sort(vecMazanodeLastPaid.begin(), vecMazanodeLastPaid.end(), CompareLastPaidBlock());

    uint256 blockHash;
    if(!GetBlockHash(blockHash, nBlockHeight - 101)) {
        LogPrintf("CMazanode::GetNextMazanodeInQueueForPayment -- ERROR: GetBlockHash() failed at nBlockHeight %d\n", nBlockHeight - 101);
        return false;
    }
    // Look at 1/10 of the oldest nodes (by last payment), calculate their scores and pay the best one
    //  -- This doesn't look at who is being paid in the +8-10 blocks, allowing for double payments very rarely
    //  -- 1/100 payments should be a double payment on mainnet - (1/(3000/10))*2
    //  -- (chance per block * chances before IsScheduled will fire)
    int nTenthNetwork = nMnCount/10;
    int nCountTenth = 0;
    arith_uint256 nHighest = 0;
    const CMazanode *pBestMazanode = NULL;
    for (const auto& s : vecMazanodeLastPaid) {
        arith_uint256 nScore = s.second->CalculateScore(blockHash);
        if(nScore > nHighest){
            nHighest = nScore;
            pBestMazanode = s.second;
        }
        nCountTenth++;
        if(nCountTenth >= nTenthNetwork) break;
    }
    if (pBestMazanode) {
        mnInfoRet = pBestMazanode->GetInfo();
    }
    return mnInfoRet.fInfoValid;
}

mazanode_info_t CMazanodeMan::FindRandomNotInVec(const std::vector<COutPoint> &vecToExclude, int nProtocolVersion)
{
    LOCK(cs);

    nProtocolVersion = nProtocolVersion == -1 ? mnpayments.GetMinMazanodePaymentsProto() : nProtocolVersion;

    int nCountEnabled = CountEnabled(nProtocolVersion);
    int nCountNotExcluded = nCountEnabled - vecToExclude.size();

    LogPrintf("CMazanodeMan::FindRandomNotInVec -- %d enabled mazanodes, %d mazanodes to choose from\n", nCountEnabled, nCountNotExcluded);
    if(nCountNotExcluded < 1) return mazanode_info_t();

    // fill a vector of pointers
    std::vector<const CMazanode*> vpMazanodesShuffled;
    for (const auto& mnpair : mapMazanodes) {
        vpMazanodesShuffled.push_back(&mnpair.second);
    }

    FastRandomContext insecure_rand;
    // shuffle pointers
    std::random_shuffle(vpMazanodesShuffled.begin(), vpMazanodesShuffled.end(), insecure_rand);
    bool fExclude;

    // loop through
    for (const auto& pmn : vpMazanodesShuffled) {
        if(pmn->nProtocolVersion < nProtocolVersion || !pmn->IsEnabled()) continue;
        fExclude = false;
        for (const auto& outpointToExclude : vecToExclude) {
            if(pmn->outpoint == outpointToExclude) {
                fExclude = true;
                break;
            }
        }
        if(fExclude) continue;
        // found the one not in vecToExclude
        LogPrint("mazanode", "CMazanodeMan::FindRandomNotInVec -- found, mazanode=%s\n", pmn->outpoint.ToStringShort());
        return pmn->GetInfo();
    }

    LogPrint("mazanode", "CMazanodeMan::FindRandomNotInVec -- failed\n");
    return mazanode_info_t();
}

bool CMazanodeMan::GetMazanodeScores(const uint256& nBlockHash, CMazanodeMan::score_pair_vec_t& vecMazanodeScoresRet, int nMinProtocol)
{
    vecMazanodeScoresRet.clear();

    if (!mazanodeSync.IsMazanodeListSynced())
        return false;

    AssertLockHeld(cs);

    if (mapMazanodes.empty())
        return false;

    // calculate scores
    for (const auto& mnpair : mapMazanodes) {
        if (mnpair.second.nProtocolVersion >= nMinProtocol) {
            vecMazanodeScoresRet.push_back(std::make_pair(mnpair.second.CalculateScore(nBlockHash), &mnpair.second));
        }
    }

    sort(vecMazanodeScoresRet.rbegin(), vecMazanodeScoresRet.rend(), CompareScoreMN());
    return !vecMazanodeScoresRet.empty();
}

bool CMazanodeMan::GetMazanodeRank(const COutPoint& outpoint, int& nRankRet, int nBlockHeight, int nMinProtocol)
{
    nRankRet = -1;

    if (!mazanodeSync.IsMazanodeListSynced())
        return false;

    // make sure we know about this block
    uint256 nBlockHash = uint256();
    if (!GetBlockHash(nBlockHash, nBlockHeight)) {
        LogPrintf("CMazanodeMan::%s -- ERROR: GetBlockHash() failed at nBlockHeight %d\n", __func__, nBlockHeight);
        return false;
    }

    LOCK(cs);

    score_pair_vec_t vecMazanodeScores;
    if (!GetMazanodeScores(nBlockHash, vecMazanodeScores, nMinProtocol))
        return false;

    int nRank = 0;
    for (const auto& scorePair : vecMazanodeScores) {
        nRank++;
        if(scorePair.second->outpoint == outpoint) {
            nRankRet = nRank;
            return true;
        }
    }

    return false;
}

bool CMazanodeMan::GetMazanodeRanks(CMazanodeMan::rank_pair_vec_t& vecMazanodeRanksRet, int nBlockHeight, int nMinProtocol)
{
    vecMazanodeRanksRet.clear();

    if (!mazanodeSync.IsMazanodeListSynced())
        return false;

    // make sure we know about this block
    uint256 nBlockHash = uint256();
    if (!GetBlockHash(nBlockHash, nBlockHeight)) {
        LogPrintf("CMazanodeMan::%s -- ERROR: GetBlockHash() failed at nBlockHeight %d\n", __func__, nBlockHeight);
        return false;
    }

    LOCK(cs);

    score_pair_vec_t vecMazanodeScores;
    if (!GetMazanodeScores(nBlockHash, vecMazanodeScores, nMinProtocol))
        return false;

    int nRank = 0;
    for (const auto& scorePair : vecMazanodeScores) {
        nRank++;
        vecMazanodeRanksRet.push_back(std::make_pair(nRank, *scorePair.second));
    }

    return true;
}

void CMazanodeMan::ProcessMazanodeConnections(CConnman& connman)
{
    //we don't care about this for regtest
    if(Params().NetworkIDString() == CBaseChainParams::REGTEST) return;

    connman.ForEachNode(CConnman::AllNodes, [](CNode* pnode) {
#ifdef ENABLE_WALLET
        if(pnode->fMazanode && !privateSendClient.IsMixingMazanode(pnode)) {
#else
        if(pnode->fMazanode) {
#endif // ENABLE_WALLET
            LogPrintf("Closing Mazanode connection: peer=%d, addr=%s\n", pnode->id, pnode->addr.ToString());
            pnode->fDisconnect = true;
        }
    });
}

std::pair<CService, std::set<uint256> > CMazanodeMan::PopScheduledMnbRequestConnection()
{
    LOCK(cs);
    if(listScheduledMnbRequestConnections.empty()) {
        return std::make_pair(CService(), std::set<uint256>());
    }

    std::set<uint256> setResult;

    listScheduledMnbRequestConnections.sort();
    std::pair<CService, uint256> pairFront = listScheduledMnbRequestConnections.front();

    // squash hashes from requests with the same CService as the first one into setResult
    std::list< std::pair<CService, uint256> >::iterator it = listScheduledMnbRequestConnections.begin();
    while(it != listScheduledMnbRequestConnections.end()) {
        if(pairFront.first == it->first) {
            setResult.insert(it->second);
            it = listScheduledMnbRequestConnections.erase(it);
        } else {
            // since list is sorted now, we can be sure that there is no more hashes left
            // to ask for from this addr
            break;
        }
    }
    return std::make_pair(pairFront.first, setResult);
}

void CMazanodeMan::ProcessPendingMnbRequests(CConnman& connman)
{
    std::pair<CService, std::set<uint256> > p = PopScheduledMnbRequestConnection();
    if (!(p.first == CService() || p.second.empty())) {
        if (connman.IsMazanodeOrDisconnectRequested(p.first)) return;
        mapPendingMNB.insert(std::make_pair(p.first, std::make_pair(GetTime(), p.second)));
        connman.AddPendingMazanode(p.first);
    }

    std::map<CService, std::pair<int64_t, std::set<uint256> > >::iterator itPendingMNB = mapPendingMNB.begin();
    while (itPendingMNB != mapPendingMNB.end()) {
        bool fDone = connman.ForNode(itPendingMNB->first, [&](CNode* pnode) {
            // compile request vector
            std::vector<CInv> vToFetch;
            std::set<uint256>& setHashes = itPendingMNB->second.second;
            std::set<uint256>::iterator it = setHashes.begin();
            while(it != setHashes.end()) {
                if(*it != uint256()) {
                    vToFetch.push_back(CInv(MSG_MAZANODE_ANNOUNCE, *it));
                    LogPrint("mazanode", "-- asking for mnb %s from addr=%s\n", it->ToString(), pnode->addr.ToString());
                }
                ++it;
            }

            // ask for data
            CNetMsgMaker msgMaker(pnode->GetSendVersion());
            connman.PushMessage(pnode, msgMaker.Make(NetMsgType::GETDATA, vToFetch));
            return true;
        });

        int64_t nTimeAdded = itPendingMNB->second.first;
        if (fDone || (GetTime() - nTimeAdded > 15)) {
            if (!fDone) {
                LogPrint("mazanode", "CMazanodeMan::%s -- failed to connect to %s\n", __func__, itPendingMNB->first.ToString());
            }
            mapPendingMNB.erase(itPendingMNB++);
        } else {
            ++itPendingMNB;
        }
    }
    LogPrint("mazanode", "%s -- mapPendingMNB size: %d\n", __func__, mapPendingMNB.size());
}

void CMazanodeMan::ProcessMessage(CNode* pfrom, const std::string& strCommand, CDataStream& vRecv, CConnman& connman)
{
    if(fLiteMode) return; // disable all Maza specific functionality

    if (strCommand == NetMsgType::MNANNOUNCE) { //Mazanode Broadcast

        CMazanodeBroadcast mnb;
        vRecv >> mnb;

        pfrom->setAskFor.erase(mnb.GetHash());

        if(!mazanodeSync.IsBlockchainSynced()) return;

        LogPrint("mazanode", "MNANNOUNCE -- Mazanode announce, mazanode=%s\n", mnb.outpoint.ToStringShort());

        int nDos = 0;

        if (CheckMnbAndUpdateMazanodeList(pfrom, mnb, nDos, connman)) {
            // use announced Mazanode as a peer
            connman.AddNewAddress(CAddress(mnb.addr, NODE_NETWORK), pfrom->addr, 2*60*60);
        } else if(nDos > 0) {
            LOCK(cs_main);
            Misbehaving(pfrom->GetId(), nDos);
        }

        if(fMazanodesAdded) {
            NotifyMazanodeUpdates(connman);
        }
    } else if (strCommand == NetMsgType::MNPING) { //Mazanode Ping

        CMazanodePing mnp;
        vRecv >> mnp;

        uint256 nHash = mnp.GetHash();

        pfrom->setAskFor.erase(nHash);

        if(!mazanodeSync.IsBlockchainSynced()) return;

        LogPrint("mazanode", "MNPING -- Mazanode ping, mazanode=%s\n", mnp.mazanodeOutpoint.ToStringShort());

        // Need LOCK2 here to ensure consistent locking order because the CheckAndUpdate call below locks cs_main
        LOCK2(cs_main, cs);

        if(mapSeenMazanodePing.count(nHash)) return; //seen
        mapSeenMazanodePing.insert(std::make_pair(nHash, mnp));

        LogPrint("mazanode", "MNPING -- Mazanode ping, mazanode=%s new\n", mnp.mazanodeOutpoint.ToStringShort());

        // see if we have this Mazanode
        CMazanode* pmn = Find(mnp.mazanodeOutpoint);

        if(pmn && mnp.fSentinelIsCurrent)
            UpdateLastSentinelPingTime();

        // too late, new MNANNOUNCE is required
        if(pmn && pmn->IsNewStartRequired()) return;

        int nDos = 0;
        if(mnp.CheckAndUpdate(pmn, false, nDos, connman)) return;

        if(nDos > 0) {
            // if anything significant failed, mark that node
            Misbehaving(pfrom->GetId(), nDos);
        } else if(pmn != NULL) {
            // nothing significant failed, mn is a known one too
            return;
        }

        // something significant is broken or mn is unknown,
        // we might have to ask for a mazanode entry once
        AskForMN(pfrom, mnp.mazanodeOutpoint, connman);

    } else if (strCommand == NetMsgType::DSEG) { //Get Mazanode list or specific entry
        // Ignore such requests until we are fully synced.
        // We could start processing this after mazanode list is synced
        // but this is a heavy one so it's better to finish sync first.
        if (!mazanodeSync.IsSynced()) return;

        COutPoint mazanodeOutpoint;

        if (pfrom->nVersion == 70208) {
            CTxIn vin;
            vRecv >> vin;
            mazanodeOutpoint = vin.prevout;
        } else {
            vRecv >> mazanodeOutpoint;
        }

        LogPrint("mazanode", "DSEG -- Mazanode list, mazanode=%s\n", mazanodeOutpoint.ToStringShort());

        if(mazanodeOutpoint.IsNull()) {
            SyncAll(pfrom, connman);
        } else {
            SyncSingle(pfrom, mazanodeOutpoint, connman);
        }

    } else if (strCommand == NetMsgType::MNVERIFY) { // Mazanode Verify

        // Need LOCK2 here to ensure consistent locking order because all functions below call GetBlockHash which locks cs_main
        LOCK2(cs_main, cs);

        CMazanodeVerification mnv;
        vRecv >> mnv;

        pfrom->setAskFor.erase(mnv.GetHash());

        if(!mazanodeSync.IsMazanodeListSynced()) return;

        if(mnv.vchSig1.empty()) {
            // CASE 1: someone asked me to verify myself /IP we are using/
            SendVerifyReply(pfrom, mnv, connman);
        } else if (mnv.vchSig2.empty()) {
            // CASE 2: we _probably_ got verification we requested from some mazanode
            ProcessVerifyReply(pfrom, mnv);
        } else {
            // CASE 3: we _probably_ got verification broadcast signed by some mazanode which verified another one
            ProcessVerifyBroadcast(pfrom, mnv);
        }
    }
}

void CMazanodeMan::SyncSingle(CNode* pnode, const COutPoint& outpoint, CConnman& connman)
{
    // do not provide any data until our node is synced
    if (!mazanodeSync.IsSynced()) return;

    LOCK(cs);

    auto it = mapMazanodes.find(outpoint);

    if(it != mapMazanodes.end()) {
        if (it->second.addr.IsRFC1918() || it->second.addr.IsLocal()) return; // do not send local network mazanode
        // NOTE: send mazanode regardless of its current state, the other node will need it to verify old votes.
        LogPrint("mazanode", "CMazanodeMan::%s -- Sending Mazanode entry: mazanode=%s  addr=%s\n", __func__, outpoint.ToStringShort(), it->second.addr.ToString());
        PushDsegInvs(pnode, it->second);
        LogPrintf("CMazanodeMan::%s -- Sent 1 Mazanode inv to peer=%d\n", __func__, pnode->id);
    }
}

void CMazanodeMan::SyncAll(CNode* pnode, CConnman& connman)
{
    // do not provide any data until our node is synced
    if (!mazanodeSync.IsSynced()) return;

    // local network
    bool isLocal = (pnode->addr.IsRFC1918() || pnode->addr.IsLocal());

    CService addrSquashed = Params().AllowMultiplePorts() ? (CService)pnode->addr : CService(pnode->addr, 0);
    // should only ask for this once
    if(!isLocal && Params().NetworkIDString() == CBaseChainParams::MAIN) {
        LOCK2(cs_main, cs);
        auto it = mAskedUsForMazanodeList.find(addrSquashed);
        if (it != mAskedUsForMazanodeList.end() && it->second > GetTime()) {
            Misbehaving(pnode->GetId(), 34);
            LogPrintf("CMazanodeMan::%s -- peer already asked me for the list, peer=%d\n", __func__, pnode->id);
            return;
        }
        int64_t askAgain = GetTime() + DSEG_UPDATE_SECONDS;
        mAskedUsForMazanodeList[addrSquashed] = askAgain;
    }

    int nInvCount = 0;

    LOCK(cs);

    for (const auto& mnpair : mapMazanodes) {
        if (mnpair.second.addr.IsRFC1918() || mnpair.second.addr.IsLocal()) continue; // do not send local network mazanode
        // NOTE: send mazanode regardless of its current state, the other node will need it to verify old votes.
        LogPrint("mazanode", "CMazanodeMan::%s -- Sending Mazanode entry: mazanode=%s  addr=%s\n", __func__, mnpair.first.ToStringShort(), mnpair.second.addr.ToString());
        PushDsegInvs(pnode, mnpair.second);
        nInvCount++;
    }

    connman.PushMessage(pnode, CNetMsgMaker(pnode->GetSendVersion()).Make(NetMsgType::SYNCSTATUSCOUNT, MAZANODE_SYNC_LIST, nInvCount));
    LogPrintf("CMazanodeMan::%s -- Sent %d Mazanode invs to peer=%d\n", __func__, nInvCount, pnode->id);
}

void CMazanodeMan::PushDsegInvs(CNode* pnode, const CMazanode& mn)
{
    AssertLockHeld(cs);

    CMazanodeBroadcast mnb(mn);
    CMazanodePing mnp = mnb.lastPing;
    uint256 hashMNB = mnb.GetHash();
    uint256 hashMNP = mnp.GetHash();
    pnode->PushInventory(CInv(MSG_MAZANODE_ANNOUNCE, hashMNB));
    pnode->PushInventory(CInv(MSG_MAZANODE_PING, hashMNP));
    mapSeenMazanodeBroadcast.insert(std::make_pair(hashMNB, std::make_pair(GetTime(), mnb)));
    mapSeenMazanodePing.insert(std::make_pair(hashMNP, mnp));
}

// Verification of mazanodes via unique direct requests.

void CMazanodeMan::DoFullVerificationStep(CConnman& connman)
{
    if(activeMazanode.outpoint.IsNull()) return;
    if(!mazanodeSync.IsSynced()) return;

    rank_pair_vec_t vecMazanodeRanks;
    GetMazanodeRanks(vecMazanodeRanks, nCachedBlockHeight - 1, MIN_POSE_PROTO_VERSION);

    LOCK(cs);

    int nCount = 0;

    int nMyRank = -1;
    int nRanksTotal = (int)vecMazanodeRanks.size();

    // send verify requests only if we are in top MAX_POSE_RANK
    rank_pair_vec_t::iterator it = vecMazanodeRanks.begin();
    while(it != vecMazanodeRanks.end()) {
        if(it->first > MAX_POSE_RANK) {
            LogPrint("mazanode", "CMazanodeMan::DoFullVerificationStep -- Must be in top %d to send verify request\n",
                        (int)MAX_POSE_RANK);
            return;
        }
        if(it->second.outpoint == activeMazanode.outpoint) {
            nMyRank = it->first;
            LogPrint("mazanode", "CMazanodeMan::DoFullVerificationStep -- Found self at rank %d/%d, verifying up to %d mazanodes\n",
                        nMyRank, nRanksTotal, (int)MAX_POSE_CONNECTIONS);
            break;
        }
        ++it;
    }

    // edge case: list is too short and this mazanode is not enabled
    if(nMyRank == -1) return;

    // send verify requests to up to MAX_POSE_CONNECTIONS mazanodes
    // starting from MAX_POSE_RANK + nMyRank and using MAX_POSE_CONNECTIONS as a step
    int nOffset = MAX_POSE_RANK + nMyRank - 1;
    if(nOffset >= (int)vecMazanodeRanks.size()) return;

    std::vector<const CMazanode*> vSortedByAddr;
    for (const auto& mnpair : mapMazanodes) {
        vSortedByAddr.push_back(&mnpair.second);
    }

    sort(vSortedByAddr.begin(), vSortedByAddr.end(), CompareByAddr());

    it = vecMazanodeRanks.begin() + nOffset;
    while(it != vecMazanodeRanks.end()) {
        if(it->second.IsPoSeVerified() || it->second.IsPoSeBanned()) {
            LogPrint("mazanode", "CMazanodeMan::DoFullVerificationStep -- Already %s%s%s mazanode %s address %s, skipping...\n",
                        it->second.IsPoSeVerified() ? "verified" : "",
                        it->second.IsPoSeVerified() && it->second.IsPoSeBanned() ? " and " : "",
                        it->second.IsPoSeBanned() ? "banned" : "",
                        it->second.outpoint.ToStringShort(), it->second.addr.ToString());
            nOffset += MAX_POSE_CONNECTIONS;
            if(nOffset >= (int)vecMazanodeRanks.size()) break;
            it += MAX_POSE_CONNECTIONS;
            continue;
        }
        LogPrint("mazanode", "CMazanodeMan::DoFullVerificationStep -- Verifying mazanode %s rank %d/%d address %s\n",
                    it->second.outpoint.ToStringShort(), it->first, nRanksTotal, it->second.addr.ToString());
        if(SendVerifyRequest(CAddress(it->second.addr, NODE_NETWORK), vSortedByAddr, connman)) {
            nCount++;
            if(nCount >= MAX_POSE_CONNECTIONS) break;
        }
        nOffset += MAX_POSE_CONNECTIONS;
        if(nOffset >= (int)vecMazanodeRanks.size()) break;
        it += MAX_POSE_CONNECTIONS;
    }

    LogPrint("mazanode", "CMazanodeMan::DoFullVerificationStep -- Sent verification requests to %d mazanodes\n", nCount);
}

// This function tries to find mazanodes with the same addr,
// find a verified one and ban all the other. If there are many nodes
// with the same addr but none of them is verified yet, then none of them are banned.
// It could take many times to run this before most of the duplicate nodes are banned.

void CMazanodeMan::CheckSameAddr()
{
    if(!mazanodeSync.IsSynced() || mapMazanodes.empty()) return;

    std::vector<CMazanode*> vBan;
    std::vector<CMazanode*> vSortedByAddr;

    {
        LOCK(cs);

        CMazanode* pprevMazanode = NULL;
        CMazanode* pverifiedMazanode = NULL;

        for (auto& mnpair : mapMazanodes) {
            vSortedByAddr.push_back(&mnpair.second);
        }

        sort(vSortedByAddr.begin(), vSortedByAddr.end(), CompareByAddr());

        for (const auto& pmn : vSortedByAddr) {
            // check only (pre)enabled mazanodes
            if(!pmn->IsEnabled() && !pmn->IsPreEnabled()) continue;
            // initial step
            if(!pprevMazanode) {
                pprevMazanode = pmn;
                pverifiedMazanode = pmn->IsPoSeVerified() ? pmn : NULL;
                continue;
            }
            // second+ step
            if(pmn->addr == pprevMazanode->addr) {
                if(pverifiedMazanode) {
                    // another mazanode with the same ip is verified, ban this one
                    vBan.push_back(pmn);
                } else if(pmn->IsPoSeVerified()) {
                    // this mazanode with the same ip is verified, ban previous one
                    vBan.push_back(pprevMazanode);
                    // and keep a reference to be able to ban following mazanodes with the same ip
                    pverifiedMazanode = pmn;
                }
            } else {
                pverifiedMazanode = pmn->IsPoSeVerified() ? pmn : NULL;
            }
            pprevMazanode = pmn;
        }
    }

    // ban duplicates
    for (auto& pmn : vBan) {
        LogPrintf("CMazanodeMan::CheckSameAddr -- increasing PoSe ban score for mazanode %s\n", pmn->outpoint.ToStringShort());
        pmn->IncreasePoSeBanScore();
    }
}

bool CMazanodeMan::SendVerifyRequest(const CAddress& addr, const std::vector<const CMazanode*>& vSortedByAddr, CConnman& connman)
{
    if(netfulfilledman.HasFulfilledRequest(addr, strprintf("%s", NetMsgType::MNVERIFY)+"-request")) {
        // we already asked for verification, not a good idea to do this too often, skip it
        LogPrint("mazanode", "CMazanodeMan::SendVerifyRequest -- too many requests, skipping... addr=%s\n", addr.ToString());
        return false;
    }

    if (connman.IsMazanodeOrDisconnectRequested(addr)) return false;

    connman.AddPendingMazanode(addr);
    // use random nonce, store it and require node to reply with correct one later
    CMazanodeVerification mnv(addr, GetRandInt(999999), nCachedBlockHeight - 1);
    LOCK(cs_mapPendingMNV);
    mapPendingMNV.insert(std::make_pair(addr, std::make_pair(GetTime(), mnv)));
    LogPrintf("CMazanodeMan::SendVerifyRequest -- verifying node using nonce %d addr=%s\n", mnv.nonce, addr.ToString());
    return true;
}

void CMazanodeMan::ProcessPendingMnvRequests(CConnman& connman)
{
    LOCK(cs_mapPendingMNV);

    std::map<CService, std::pair<int64_t, CMazanodeVerification> >::iterator itPendingMNV = mapPendingMNV.begin();

    while (itPendingMNV != mapPendingMNV.end()) {
        bool fDone = connman.ForNode(itPendingMNV->first, [&](CNode* pnode) {
            netfulfilledman.AddFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::MNVERIFY)+"-request");
            // use random nonce, store it and require node to reply with correct one later
            mWeAskedForVerification[pnode->addr] = itPendingMNV->second.second;
            LogPrint("mazanode", "-- verifying node using nonce %d addr=%s\n", itPendingMNV->second.second.nonce, pnode->addr.ToString());
            CNetMsgMaker msgMaker(pnode->GetSendVersion()); // TODO this gives a warning about version not being set (we should wait for VERSION exchange)
            connman.PushMessage(pnode, msgMaker.Make(NetMsgType::MNVERIFY, itPendingMNV->second.second));
            return true;
        });

        int64_t nTimeAdded = itPendingMNV->second.first;
        if (fDone || (GetTime() - nTimeAdded > 15)) {
            if (!fDone) {
                LogPrint("mazanode", "CMazanodeMan::%s -- failed to connect to %s\n", __func__, itPendingMNV->first.ToString());
            }
            mapPendingMNV.erase(itPendingMNV++);
        } else {
            ++itPendingMNV;
        }
    }
    LogPrint("mazanode", "%s -- mapPendingMNV size: %d\n", __func__, mapPendingMNV.size());
}

void CMazanodeMan::SendVerifyReply(CNode* pnode, CMazanodeVerification& mnv, CConnman& connman)
{
    AssertLockHeld(cs_main);

    // only mazanodes can sign this, why would someone ask regular node?
    if(!fMazanodeMode) {
        // do not ban, malicious node might be using my IP
        // and trying to confuse the node which tries to verify it
        return;
    }

    if(netfulfilledman.HasFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::MNVERIFY)+"-reply")) {
        // peer should not ask us that often
        LogPrintf("MazanodeMan::SendVerifyReply -- ERROR: peer already asked me recently, peer=%d\n", pnode->id);
        Misbehaving(pnode->id, 20);
        return;
    }

    uint256 blockHash;
    if(!GetBlockHash(blockHash, mnv.nBlockHeight)) {
        LogPrintf("MazanodeMan::SendVerifyReply -- can't get block hash for unknown block height %d, peer=%d\n", mnv.nBlockHeight, pnode->id);
        return;
    }

    std::string strError;

    if (sporkManager.IsSporkActive(SPORK_6_NEW_SIGS)) {
        uint256 hash = mnv.GetSignatureHash1(blockHash);

        if(!CHashSigner::SignHash(hash, activeMazanode.keyMazanode, mnv.vchSig1)) {
            LogPrintf("CMazanodeMan::SendVerifyReply -- SignHash() failed\n");
            return;
        }

        if (!CHashSigner::VerifyHash(hash, activeMazanode.pubKeyMazanode, mnv.vchSig1, strError)) {
            LogPrintf("CMazanodeMan::SendVerifyReply -- VerifyHash() failed, error: %s\n", strError);
            return;
        }
    } else {
        std::string strMessage = strprintf("%s%d%s", activeMazanode.service.ToString(false), mnv.nonce, blockHash.ToString());

        if(!CMessageSigner::SignMessage(strMessage, mnv.vchSig1, activeMazanode.keyMazanode)) {
            LogPrintf("MazanodeMan::SendVerifyReply -- SignMessage() failed\n");
            return;
        }

        if(!CMessageSigner::VerifyMessage(activeMazanode.pubKeyMazanode, mnv.vchSig1, strMessage, strError)) {
            LogPrintf("MazanodeMan::SendVerifyReply -- VerifyMessage() failed, error: %s\n", strError);
            return;
        }
    }

    CNetMsgMaker msgMaker(pnode->GetSendVersion());
    connman.PushMessage(pnode, msgMaker.Make(NetMsgType::MNVERIFY, mnv));
    netfulfilledman.AddFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::MNVERIFY)+"-reply");
}

void CMazanodeMan::ProcessVerifyReply(CNode* pnode, CMazanodeVerification& mnv)
{
    AssertLockHeld(cs_main);

    std::string strError;

    // did we even ask for it? if that's the case we should have matching fulfilled request
    if(!netfulfilledman.HasFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::MNVERIFY)+"-request")) {
        LogPrintf("CMazanodeMan::ProcessVerifyReply -- ERROR: we didn't ask for verification of %s, peer=%d\n", pnode->addr.ToString(), pnode->id);
        Misbehaving(pnode->id, 20);
        return;
    }

    // Received nonce for a known address must match the one we sent
    if(mWeAskedForVerification[pnode->addr].nonce != mnv.nonce) {
        LogPrintf("CMazanodeMan::ProcessVerifyReply -- ERROR: wrong nounce: requested=%d, received=%d, peer=%d\n",
                    mWeAskedForVerification[pnode->addr].nonce, mnv.nonce, pnode->id);
        Misbehaving(pnode->id, 20);
        return;
    }

    // Received nBlockHeight for a known address must match the one we sent
    if(mWeAskedForVerification[pnode->addr].nBlockHeight != mnv.nBlockHeight) {
        LogPrintf("CMazanodeMan::ProcessVerifyReply -- ERROR: wrong nBlockHeight: requested=%d, received=%d, peer=%d\n",
                    mWeAskedForVerification[pnode->addr].nBlockHeight, mnv.nBlockHeight, pnode->id);
        Misbehaving(pnode->id, 20);
        return;
    }

    uint256 blockHash;
    if(!GetBlockHash(blockHash, mnv.nBlockHeight)) {
        // this shouldn't happen...
        LogPrintf("MazanodeMan::ProcessVerifyReply -- can't get block hash for unknown block height %d, peer=%d\n", mnv.nBlockHeight, pnode->id);
        return;
    }

    // we already verified this address, why node is spamming?
    if(netfulfilledman.HasFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::MNVERIFY)+"-done")) {
        LogPrintf("CMazanodeMan::ProcessVerifyReply -- ERROR: already verified %s recently\n", pnode->addr.ToString());
        Misbehaving(pnode->id, 20);
        return;
    }

    {
        LOCK(cs);

        CMazanode* prealMazanode = NULL;
        std::vector<CMazanode*> vpMazanodesToBan;

        uint256 hash1 = mnv.GetSignatureHash1(blockHash);
        std::string strMessage1 = strprintf("%s%d%s", pnode->addr.ToString(false), mnv.nonce, blockHash.ToString());

        for (auto& mnpair : mapMazanodes) {
            if(CAddress(mnpair.second.addr, NODE_NETWORK) == pnode->addr) {
                bool fFound = false;
                if (sporkManager.IsSporkActive(SPORK_6_NEW_SIGS)) {
                    fFound = CHashSigner::VerifyHash(hash1, mnpair.second.pubKeyMazanode, mnv.vchSig1, strError);
                    // we don't care about mnv with signature in old format
                } else {
                    fFound = CMessageSigner::VerifyMessage(mnpair.second.pubKeyMazanode, mnv.vchSig1, strMessage1, strError);
                }
                if (fFound) {
                    // found it!
                    prealMazanode = &mnpair.second;
                    if(!mnpair.second.IsPoSeVerified()) {
                        mnpair.second.DecreasePoSeBanScore();
                    }
                    netfulfilledman.AddFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::MNVERIFY)+"-done");

                    // we can only broadcast it if we are an activated mazanode
                    if(activeMazanode.outpoint.IsNull()) continue;
                    // update ...
                    mnv.addr = mnpair.second.addr;
                    mnv.mazanodeOutpoint1 = mnpair.second.outpoint;
                    mnv.mazanodeOutpoint2 = activeMazanode.outpoint;
                    // ... and sign it
                    std::string strError;

                    if (sporkManager.IsSporkActive(SPORK_6_NEW_SIGS)) {
                        uint256 hash2 = mnv.GetSignatureHash2(blockHash);

                        if(!CHashSigner::SignHash(hash2, activeMazanode.keyMazanode, mnv.vchSig2)) {
                            LogPrintf("MazanodeMan::ProcessVerifyReply -- SignHash() failed\n");
                            return;
                        }

                        if(!CHashSigner::VerifyHash(hash2, activeMazanode.pubKeyMazanode, mnv.vchSig2, strError)) {
                            LogPrintf("MazanodeMan::ProcessVerifyReply -- VerifyHash() failed, error: %s\n", strError);
                            return;
                        }
                    } else {
                        std::string strMessage2 = strprintf("%s%d%s%s%s", mnv.addr.ToString(false), mnv.nonce, blockHash.ToString(),
                                                mnv.mazanodeOutpoint1.ToStringShort(), mnv.mazanodeOutpoint2.ToStringShort());

                        if(!CMessageSigner::SignMessage(strMessage2, mnv.vchSig2, activeMazanode.keyMazanode)) {
                            LogPrintf("MazanodeMan::ProcessVerifyReply -- SignMessage() failed\n");
                            return;
                        }

                        if(!CMessageSigner::VerifyMessage(activeMazanode.pubKeyMazanode, mnv.vchSig2, strMessage2, strError)) {
                            LogPrintf("MazanodeMan::ProcessVerifyReply -- VerifyMessage() failed, error: %s\n", strError);
                            return;
                        }
                    }

                    mWeAskedForVerification[pnode->addr] = mnv;
                    mapSeenMazanodeVerification.insert(std::make_pair(mnv.GetHash(), mnv));
                    mnv.Relay();

                } else {
                    vpMazanodesToBan.push_back(&mnpair.second);
                }
            }
        }
        // no real mazanode found?...
        if(!prealMazanode) {
            // this should never be the case normally,
            // only if someone is trying to game the system in some way or smth like that
            LogPrintf("CMazanodeMan::ProcessVerifyReply -- ERROR: no real mazanode found for addr %s\n", pnode->addr.ToString());
            Misbehaving(pnode->id, 20);
            return;
        }
        LogPrintf("CMazanodeMan::ProcessVerifyReply -- verified real mazanode %s for addr %s\n",
                    prealMazanode->outpoint.ToStringShort(), pnode->addr.ToString());
        // increase ban score for everyone else
        for (const auto& pmn : vpMazanodesToBan) {
            pmn->IncreasePoSeBanScore();
            LogPrint("mazanode", "CMazanodeMan::ProcessVerifyReply -- increased PoSe ban score for %s addr %s, new score %d\n",
                        prealMazanode->outpoint.ToStringShort(), pnode->addr.ToString(), pmn->nPoSeBanScore);
        }
        if(!vpMazanodesToBan.empty())
            LogPrintf("CMazanodeMan::ProcessVerifyReply -- PoSe score increased for %d fake mazanodes, addr %s\n",
                        (int)vpMazanodesToBan.size(), pnode->addr.ToString());
    }
}

void CMazanodeMan::ProcessVerifyBroadcast(CNode* pnode, const CMazanodeVerification& mnv)
{
    AssertLockHeld(cs_main);

    std::string strError;

    if(mapSeenMazanodeVerification.find(mnv.GetHash()) != mapSeenMazanodeVerification.end()) {
        // we already have one
        return;
    }
    mapSeenMazanodeVerification[mnv.GetHash()] = mnv;

    // we don't care about history
    if(mnv.nBlockHeight < nCachedBlockHeight - MAX_POSE_BLOCKS) {
        LogPrint("mazanode", "CMazanodeMan::ProcessVerifyBroadcast -- Outdated: current block %d, verification block %d, peer=%d\n",
                    nCachedBlockHeight, mnv.nBlockHeight, pnode->id);
        return;
    }

    if(mnv.mazanodeOutpoint1 == mnv.mazanodeOutpoint2) {
        LogPrint("mazanode", "CMazanodeMan::ProcessVerifyBroadcast -- ERROR: same outpoints %s, peer=%d\n",
                    mnv.mazanodeOutpoint1.ToStringShort(), pnode->id);
        // that was NOT a good idea to cheat and verify itself,
        // ban the node we received such message from
        Misbehaving(pnode->id, 100);
        return;
    }

    uint256 blockHash;
    if(!GetBlockHash(blockHash, mnv.nBlockHeight)) {
        // this shouldn't happen...
        LogPrintf("CMazanodeMan::ProcessVerifyBroadcast -- Can't get block hash for unknown block height %d, peer=%d\n", mnv.nBlockHeight, pnode->id);
        return;
    }

    int nRank;

    if (!GetMazanodeRank(mnv.mazanodeOutpoint2, nRank, mnv.nBlockHeight, MIN_POSE_PROTO_VERSION)) {
        LogPrint("mazanode", "CMazanodeMan::ProcessVerifyBroadcast -- Can't calculate rank for mazanode %s\n",
                    mnv.mazanodeOutpoint2.ToStringShort());
        return;
    }

    if(nRank > MAX_POSE_RANK) {
        LogPrint("mazanode", "CMazanodeMan::ProcessVerifyBroadcast -- Mazanode %s is not in top %d, current rank %d, peer=%d\n",
                    mnv.mazanodeOutpoint2.ToStringShort(), (int)MAX_POSE_RANK, nRank, pnode->id);
        return;
    }

    {
        LOCK(cs);

        CMazanode* pmn1 = Find(mnv.mazanodeOutpoint1);
        if(!pmn1) {
            LogPrintf("CMazanodeMan::ProcessVerifyBroadcast -- can't find mazanode1 %s\n", mnv.mazanodeOutpoint1.ToStringShort());
            return;
        }

        CMazanode* pmn2 = Find(mnv.mazanodeOutpoint2);
        if(!pmn2) {
            LogPrintf("CMazanodeMan::ProcessVerifyBroadcast -- can't find mazanode2 %s\n", mnv.mazanodeOutpoint2.ToStringShort());
            return;
        }

        if(pmn1->addr != mnv.addr) {
            LogPrintf("CMazanodeMan::ProcessVerifyBroadcast -- addr %s does not match %s\n", mnv.addr.ToString(), pmn1->addr.ToString());
            return;
        }

        if (sporkManager.IsSporkActive(SPORK_6_NEW_SIGS)) {
            uint256 hash1 = mnv.GetSignatureHash1(blockHash);
            uint256 hash2 = mnv.GetSignatureHash2(blockHash);

            if(!CHashSigner::VerifyHash(hash1, pmn1->pubKeyMazanode, mnv.vchSig1, strError)) {
                LogPrintf("MazanodeMan::ProcessVerifyBroadcast -- VerifyHash() failed, error: %s\n", strError);
                return;
            }

            if(!CHashSigner::VerifyHash(hash2, pmn2->pubKeyMazanode, mnv.vchSig2, strError)) {
                LogPrintf("MazanodeMan::ProcessVerifyBroadcast -- VerifyHash() failed, error: %s\n", strError);
                return;
            }
        } else {
            std::string strMessage1 = strprintf("%s%d%s", mnv.addr.ToString(false), mnv.nonce, blockHash.ToString());
            std::string strMessage2 = strprintf("%s%d%s%s%s", mnv.addr.ToString(false), mnv.nonce, blockHash.ToString(),
                                    mnv.mazanodeOutpoint1.ToStringShort(), mnv.mazanodeOutpoint2.ToStringShort());

            if(!CMessageSigner::VerifyMessage(pmn1->pubKeyMazanode, mnv.vchSig1, strMessage1, strError)) {
                LogPrintf("CMazanodeMan::ProcessVerifyBroadcast -- VerifyMessage() for mazanode1 failed, error: %s\n", strError);
                return;
            }

            if(!CMessageSigner::VerifyMessage(pmn2->pubKeyMazanode, mnv.vchSig2, strMessage2, strError)) {
                LogPrintf("CMazanodeMan::ProcessVerifyBroadcast -- VerifyMessage() for mazanode2 failed, error: %s\n", strError);
                return;
            }
        }

        if(!pmn1->IsPoSeVerified()) {
            pmn1->DecreasePoSeBanScore();
        }
        mnv.Relay();

        LogPrintf("CMazanodeMan::ProcessVerifyBroadcast -- verified mazanode %s for addr %s\n",
                    pmn1->outpoint.ToStringShort(), pmn1->addr.ToString());

        // increase ban score for everyone else with the same addr
        int nCount = 0;
        for (auto& mnpair : mapMazanodes) {
            if(mnpair.second.addr != mnv.addr || mnpair.first == mnv.mazanodeOutpoint1) continue;
            mnpair.second.IncreasePoSeBanScore();
            nCount++;
            LogPrint("mazanode", "CMazanodeMan::ProcessVerifyBroadcast -- increased PoSe ban score for %s addr %s, new score %d\n",
                        mnpair.first.ToStringShort(), mnpair.second.addr.ToString(), mnpair.second.nPoSeBanScore);
        }
        if(nCount)
            LogPrintf("CMazanodeMan::ProcessVerifyBroadcast -- PoSe score increased for %d fake mazanodes, addr %s\n",
                        nCount, pmn1->addr.ToString());
    }
}

std::string CMazanodeMan::ToString() const
{
    std::ostringstream info;

    info << "Mazanodes: " << (int)mapMazanodes.size() <<
            ", peers who asked us for Mazanode list: " << (int)mAskedUsForMazanodeList.size() <<
            ", peers we asked for Mazanode list: " << (int)mWeAskedForMazanodeList.size() <<
            ", entries in Mazanode list we asked for: " << (int)mWeAskedForMazanodeListEntry.size() <<
            ", nDsqCount: " << (int)nDsqCount;

    return info.str();
}

bool CMazanodeMan::CheckMnbAndUpdateMazanodeList(CNode* pfrom, CMazanodeBroadcast mnb, int& nDos, CConnman& connman)
{
    // Need to lock cs_main here to ensure consistent locking order because the SimpleCheck call below locks cs_main
    LOCK(cs_main);

    {
        LOCK(cs);
        nDos = 0;
        LogPrint("mazanode", "CMazanodeMan::CheckMnbAndUpdateMazanodeList -- mazanode=%s\n", mnb.outpoint.ToStringShort());

        uint256 hash = mnb.GetHash();
        if(mapSeenMazanodeBroadcast.count(hash) && !mnb.fRecovery) { //seen
            LogPrint("mazanode", "CMazanodeMan::CheckMnbAndUpdateMazanodeList -- mazanode=%s seen\n", mnb.outpoint.ToStringShort());
            // less then 2 pings left before this MN goes into non-recoverable state, bump sync timeout
            if(GetTime() - mapSeenMazanodeBroadcast[hash].first > MAZANODE_NEW_START_REQUIRED_SECONDS - MAZANODE_MIN_MNP_SECONDS * 2) {
                LogPrint("mazanode", "CMazanodeMan::CheckMnbAndUpdateMazanodeList -- mazanode=%s seen update\n", mnb.outpoint.ToStringShort());
                mapSeenMazanodeBroadcast[hash].first = GetTime();
                mazanodeSync.BumpAssetLastTime("CMazanodeMan::CheckMnbAndUpdateMazanodeList - seen");
            }
            // did we ask this node for it?
            if(pfrom && IsMnbRecoveryRequested(hash) && GetTime() < mMnbRecoveryRequests[hash].first) {
                LogPrint("mazanode", "CMazanodeMan::CheckMnbAndUpdateMazanodeList -- mnb=%s seen request\n", hash.ToString());
                if(mMnbRecoveryRequests[hash].second.count(pfrom->addr)) {
                    LogPrint("mazanode", "CMazanodeMan::CheckMnbAndUpdateMazanodeList -- mnb=%s seen request, addr=%s\n", hash.ToString(), pfrom->addr.ToString());
                    // do not allow node to send same mnb multiple times in recovery mode
                    mMnbRecoveryRequests[hash].second.erase(pfrom->addr);
                    // does it have newer lastPing?
                    if(mnb.lastPing.sigTime > mapSeenMazanodeBroadcast[hash].second.lastPing.sigTime) {
                        // simulate Check
                        CMazanode mnTemp = CMazanode(mnb);
                        mnTemp.Check();
                        LogPrint("mazanode", "CMazanodeMan::CheckMnbAndUpdateMazanodeList -- mnb=%s seen request, addr=%s, better lastPing: %d min ago, projected mn state: %s\n", hash.ToString(), pfrom->addr.ToString(), (GetAdjustedTime() - mnb.lastPing.sigTime)/60, mnTemp.GetStateString());
                        if(mnTemp.IsValidStateForAutoStart(mnTemp.nActiveState)) {
                            // this node thinks it's a good one
                            LogPrint("mazanode", "CMazanodeMan::CheckMnbAndUpdateMazanodeList -- mazanode=%s seen good\n", mnb.outpoint.ToStringShort());
                            mMnbRecoveryGoodReplies[hash].push_back(mnb);
                        }
                    }
                }
            }
            return true;
        }
        mapSeenMazanodeBroadcast.insert(std::make_pair(hash, std::make_pair(GetTime(), mnb)));

        LogPrint("mazanode", "CMazanodeMan::CheckMnbAndUpdateMazanodeList -- mazanode=%s new\n", mnb.outpoint.ToStringShort());

        if(!mnb.SimpleCheck(nDos)) {
            LogPrint("mazanode", "CMazanodeMan::CheckMnbAndUpdateMazanodeList -- SimpleCheck() failed, mazanode=%s\n", mnb.outpoint.ToStringShort());
            return false;
        }

        // search Mazanode list
        CMazanode* pmn = Find(mnb.outpoint);
        if(pmn) {
            CMazanodeBroadcast mnbOld = mapSeenMazanodeBroadcast[CMazanodeBroadcast(*pmn).GetHash()].second;
            if(!mnb.Update(pmn, nDos, connman)) {
                LogPrint("mazanode", "CMazanodeMan::CheckMnbAndUpdateMazanodeList -- Update() failed, mazanode=%s\n", mnb.outpoint.ToStringShort());
                return false;
            }
            if(hash != mnbOld.GetHash()) {
                mapSeenMazanodeBroadcast.erase(mnbOld.GetHash());
            }
            return true;
        }
    }

    if(mnb.CheckOutpoint(nDos)) {
        Add(mnb);
        mazanodeSync.BumpAssetLastTime("CMazanodeMan::CheckMnbAndUpdateMazanodeList - new");
        // if it matches our Mazanode privkey...
        if(fMazanodeMode && mnb.pubKeyMazanode == activeMazanode.pubKeyMazanode) {
            mnb.nPoSeBanScore = -MAZANODE_POSE_BAN_MAX_SCORE;
            if(mnb.nProtocolVersion == PROTOCOL_VERSION) {
                // ... and PROTOCOL_VERSION, then we've been remotely activated ...
                LogPrintf("CMazanodeMan::CheckMnbAndUpdateMazanodeList -- Got NEW Mazanode entry: mazanode=%s  sigTime=%lld  addr=%s\n",
                            mnb.outpoint.ToStringShort(), mnb.sigTime, mnb.addr.ToString());
                activeMazanode.ManageState(connman);
            } else {
                // ... otherwise we need to reactivate our node, do not add it to the list and do not relay
                // but also do not ban the node we get this message from
                LogPrintf("CMazanodeMan::CheckMnbAndUpdateMazanodeList -- wrong PROTOCOL_VERSION, re-activate your MN: message nProtocolVersion=%d  PROTOCOL_VERSION=%d\n", mnb.nProtocolVersion, PROTOCOL_VERSION);
                return false;
            }
        }
        mnb.Relay(connman);
    } else {
        LogPrintf("CMazanodeMan::CheckMnbAndUpdateMazanodeList -- Rejected Mazanode entry: %s  addr=%s\n", mnb.outpoint.ToStringShort(), mnb.addr.ToString());
        return false;
    }

    return true;
}

void CMazanodeMan::UpdateLastPaid(const CBlockIndex* pindex)
{
    LOCK(cs);

    if(fLiteMode || !mazanodeSync.IsWinnersListSynced() || mapMazanodes.empty()) return;

    static int nLastRunBlockHeight = 0;
    // Scan at least LAST_PAID_SCAN_BLOCKS but no more than mnpayments.GetStorageLimit()
    int nMaxBlocksToScanBack = std::max(LAST_PAID_SCAN_BLOCKS, nCachedBlockHeight - nLastRunBlockHeight);
    nMaxBlocksToScanBack = std::min(nMaxBlocksToScanBack, mnpayments.GetStorageLimit());

    LogPrint("mazanode", "CMazanodeMan::UpdateLastPaid -- nCachedBlockHeight=%d, nLastRunBlockHeight=%d, nMaxBlocksToScanBack=%d\n",
                            nCachedBlockHeight, nLastRunBlockHeight, nMaxBlocksToScanBack);

    for (auto& mnpair : mapMazanodes) {
        mnpair.second.UpdateLastPaid(pindex, nMaxBlocksToScanBack);
    }

    nLastRunBlockHeight = nCachedBlockHeight;
}

void CMazanodeMan::UpdateLastSentinelPingTime()
{
    LOCK(cs);
    nLastSentinelPingTime = GetTime();
}

bool CMazanodeMan::IsSentinelPingActive()
{
    LOCK(cs);
    // Check if any mazanodes have voted recently, otherwise return false
    return (GetTime() - nLastSentinelPingTime) <= MAZANODE_SENTINEL_PING_MAX_SECONDS;
}

bool CMazanodeMan::AddGovernanceVote(const COutPoint& outpoint, uint256 nGovernanceObjectHash)
{
    LOCK(cs);
    CMazanode* pmn = Find(outpoint);
    if(!pmn) {
        return false;
    }
    pmn->AddGovernanceVote(nGovernanceObjectHash);
    return true;
}

void CMazanodeMan::RemoveGovernanceObject(uint256 nGovernanceObjectHash)
{
    LOCK(cs);
    for(auto& mnpair : mapMazanodes) {
        mnpair.second.RemoveGovernanceObject(nGovernanceObjectHash);
    }
}

void CMazanodeMan::CheckMazanode(const CPubKey& pubKeyMazanode, bool fForce)
{
    LOCK2(cs_main, cs);
    for (auto& mnpair : mapMazanodes) {
        if (mnpair.second.pubKeyMazanode == pubKeyMazanode) {
            mnpair.second.Check(fForce);
            return;
        }
    }
}

bool CMazanodeMan::IsMazanodePingedWithin(const COutPoint& outpoint, int nSeconds, int64_t nTimeToCheckAt)
{
    LOCK(cs);
    CMazanode* pmn = Find(outpoint);
    return pmn ? pmn->IsPingedWithin(nSeconds, nTimeToCheckAt) : false;
}

void CMazanodeMan::SetMazanodeLastPing(const COutPoint& outpoint, const CMazanodePing& mnp)
{
    LOCK(cs);
    CMazanode* pmn = Find(outpoint);
    if(!pmn) {
        return;
    }
    pmn->lastPing = mnp;
    if(mnp.fSentinelIsCurrent) {
        UpdateLastSentinelPingTime();
    }
    mapSeenMazanodePing.insert(std::make_pair(mnp.GetHash(), mnp));

    CMazanodeBroadcast mnb(*pmn);
    uint256 hash = mnb.GetHash();
    if(mapSeenMazanodeBroadcast.count(hash)) {
        mapSeenMazanodeBroadcast[hash].second.lastPing = mnp;
    }
}

void CMazanodeMan::UpdatedBlockTip(const CBlockIndex *pindex)
{
    nCachedBlockHeight = pindex->nHeight;
    LogPrint("mazanode", "CMazanodeMan::UpdatedBlockTip -- nCachedBlockHeight=%d\n", nCachedBlockHeight);

    CheckSameAddr();

    if(fMazanodeMode) {
        // normal wallet does not need to update this every block, doing update on rpc call should be enough
        UpdateLastPaid(pindex);
    }
}

void CMazanodeMan::WarnMazanodeDaemonUpdates()
{
    LOCK(cs);

    static bool fWarned = false;

    if (fWarned || !size() || !mazanodeSync.IsMazanodeListSynced())
        return;

    int nUpdatedMazanodes{0};

    for (const auto& mnpair : mapMazanodes) {
        if (mnpair.second.lastPing.nDaemonVersion > CLIENT_VERSION) {
            ++nUpdatedMazanodes;
        }
    }

    // Warn only when at least half of known mazanodes already updated
    if (nUpdatedMazanodes < size() / 2)
        return;

    std::string strWarning;
    if (nUpdatedMazanodes != size()) {
        strWarning = strprintf(_("Warning: At least %d of %d mazanodes are running on a newer software version. Please check latest releases, you might need to update too."),
                    nUpdatedMazanodes, size());
    } else {
        // someone was postponing this update for way too long probably
        strWarning = strprintf(_("Warning: Every mazanode (out of %d known ones) is running on a newer software version. Please check latest releases, it's very likely that you missed a major/critical update."),
                    size());
    }

    // notify GetWarnings(), called by Qt and the JSON-RPC code to warn the user
    SetMiscWarning(strWarning);
    // trigger GUI update
    uiInterface.NotifyAlertChanged(SerializeHash(strWarning), CT_NEW);
    // trigger cmd-line notification
    CAlert::Notify(strWarning);

    fWarned = true;
}

void CMazanodeMan::NotifyMazanodeUpdates(CConnman& connman)
{
    // Avoid double locking
    bool fMazanodesAddedLocal = false;
    bool fMazanodesRemovedLocal = false;
    {
        LOCK(cs);
        fMazanodesAddedLocal = fMazanodesAdded;
        fMazanodesRemovedLocal = fMazanodesRemoved;
    }

    if(fMazanodesAddedLocal) {
        governance.CheckMazanodeOrphanObjects(connman);
        governance.CheckMazanodeOrphanVotes(connman);
    }
    if(fMazanodesRemovedLocal) {
        governance.UpdateCachesAndClean();
    }

    LOCK(cs);
    fMazanodesAdded = false;
    fMazanodesRemoved = false;
}
