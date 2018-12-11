// Copyright (c) 2014-2017 The Maza Network developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef MAZANODEMAN_H
#define MAZANODEMAN_H

#include "mazanode.h"
#include "sync.h"

class CMazanodeMan;
class CConnman;

extern CMazanodeMan mnodeman;

class CMazanodeMan
{
public:
    typedef std::pair<arith_uint256, const CMazanode*> score_pair_t;
    typedef std::vector<score_pair_t> score_pair_vec_t;
    typedef std::pair<int, const CMazanode> rank_pair_t;
    typedef std::vector<rank_pair_t> rank_pair_vec_t;

private:
    static const std::string SERIALIZATION_VERSION_STRING;

    static const int DSEG_UPDATE_SECONDS        = 3 * 60 * 60;

    static const int LAST_PAID_SCAN_BLOCKS;

    static const int MIN_POSE_PROTO_VERSION     = 70203;
    static const int MAX_POSE_CONNECTIONS       = 10;
    static const int MAX_POSE_RANK              = 10;
    static const int MAX_POSE_BLOCKS            = 10;

    static const int MNB_RECOVERY_QUORUM_TOTAL      = 10;
    static const int MNB_RECOVERY_QUORUM_REQUIRED   = 6;
    static const int MNB_RECOVERY_MAX_ASK_ENTRIES   = 10;
    static const int MNB_RECOVERY_WAIT_SECONDS      = 60;
    static const int MNB_RECOVERY_RETRY_SECONDS     = 3 * 60 * 60;


    // critical section to protect the inner data structures
    mutable CCriticalSection cs;

    // Keep track of current block height
    int nCachedBlockHeight;

    // map to hold all MNs
    std::map<COutPoint, CMazanode> mapMazanodes;
    // who's asked for the Mazanode list and the last time
    std::map<CService, int64_t> mAskedUsForMazanodeList;
    // who we asked for the Mazanode list and the last time
    std::map<CService, int64_t> mWeAskedForMazanodeList;
    // which Mazanodes we've asked for
    std::map<COutPoint, std::map<CService, int64_t> > mWeAskedForMazanodeListEntry;

    // who we asked for the mazanode verification
    std::map<CService, CMazanodeVerification> mWeAskedForVerification;

    // these maps are used for mazanode recovery from MAZANODE_NEW_START_REQUIRED state
    std::map<uint256, std::pair< int64_t, std::set<CService> > > mMnbRecoveryRequests;
    std::map<uint256, std::vector<CMazanodeBroadcast> > mMnbRecoveryGoodReplies;
    std::list< std::pair<CService, uint256> > listScheduledMnbRequestConnections;
    std::map<CService, std::pair<int64_t, std::set<uint256> > > mapPendingMNB;
    std::map<CService, std::pair<int64_t, CMazanodeVerification> > mapPendingMNV;
    CCriticalSection cs_mapPendingMNV;

    /// Set when mazanodes are added, cleared when CGovernanceManager is notified
    bool fMazanodesAdded;

    /// Set when mazanodes are removed, cleared when CGovernanceManager is notified
    bool fMazanodesRemoved;

    std::vector<uint256> vecDirtyGovernanceObjectHashes;

    int64_t nLastSentinelPingTime;

    friend class CMazanodeSync;
    /// Find an entry
    CMazanode* Find(const COutPoint& outpoint);

    bool GetMazanodeScores(const uint256& nBlockHash, score_pair_vec_t& vecMazanodeScoresRet, int nMinProtocol = 0);

    void SyncSingle(CNode* pnode, const COutPoint& outpoint, CConnman& connman);
    void SyncAll(CNode* pnode, CConnman& connman);

    void PushDsegInvs(CNode* pnode, const CMazanode& mn);

public:
    // Keep track of all broadcasts I've seen
    std::map<uint256, std::pair<int64_t, CMazanodeBroadcast> > mapSeenMazanodeBroadcast;
    // Keep track of all pings I've seen
    std::map<uint256, CMazanodePing> mapSeenMazanodePing;
    // Keep track of all verifications I've seen
    std::map<uint256, CMazanodeVerification> mapSeenMazanodeVerification;
    // keep track of dsq count to prevent mazanodes from gaming darksend queue
    int64_t nDsqCount;


    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        LOCK(cs);
        std::string strVersion;
        if(ser_action.ForRead()) {
            READWRITE(strVersion);
        }
        else {
            strVersion = SERIALIZATION_VERSION_STRING; 
            READWRITE(strVersion);
        }

        READWRITE(mapMazanodes);
        READWRITE(mAskedUsForMazanodeList);
        READWRITE(mWeAskedForMazanodeList);
        READWRITE(mWeAskedForMazanodeListEntry);
        READWRITE(mMnbRecoveryRequests);
        READWRITE(mMnbRecoveryGoodReplies);
        READWRITE(nLastSentinelPingTime);
        READWRITE(nDsqCount);

        READWRITE(mapSeenMazanodeBroadcast);
        READWRITE(mapSeenMazanodePing);
        if(ser_action.ForRead() && (strVersion != SERIALIZATION_VERSION_STRING)) {
            Clear();
        }
    }

    CMazanodeMan();

    /// Add an entry
    bool Add(CMazanode &mn);

    /// Ask (source) node for mnb
    void AskForMN(CNode *pnode, const COutPoint& outpoint, CConnman& connman);
    void AskForMnb(CNode *pnode, const uint256 &hash);

    bool PoSeBan(const COutPoint &outpoint);
    bool AllowMixing(const COutPoint &outpoint);
    bool DisallowMixing(const COutPoint &outpoint);

    /// Check all Mazanodes
    void Check();

    /// Check all Mazanodes and remove inactive
    void CheckAndRemove(CConnman& connman);
    /// This is dummy overload to be used for dumping/loading mncache.dat
    void CheckAndRemove() {}

    /// Clear Mazanode vector
    void Clear();

    /// Count Mazanodes filtered by nProtocolVersion.
    /// Mazanode nProtocolVersion should match or be above the one specified in param here.
    int CountMazanodes(int nProtocolVersion = -1);
    /// Count enabled Mazanodes filtered by nProtocolVersion.
    /// Mazanode nProtocolVersion should match or be above the one specified in param here.
    int CountEnabled(int nProtocolVersion = -1);

    /// Count Mazanodes by network type - NET_IPV4, NET_IPV6, NET_TOR
    // int CountByIP(int nNetworkType);

    void DsegUpdate(CNode* pnode, CConnman& connman);

    /// Versions of Find that are safe to use from outside the class
    bool Get(const COutPoint& outpoint, CMazanode& mazanodeRet);
    bool Has(const COutPoint& outpoint);

    bool GetMazanodeInfo(const COutPoint& outpoint, mazanode_info_t& mnInfoRet);
    bool GetMazanodeInfo(const CPubKey& pubKeyMazanode, mazanode_info_t& mnInfoRet);
    bool GetMazanodeInfo(const CScript& payee, mazanode_info_t& mnInfoRet);

    /// Find an entry in the mazanode list that is next to be paid
    bool GetNextMazanodeInQueueForPayment(int nBlockHeight, bool fFilterSigTime, int& nCountRet, mazanode_info_t& mnInfoRet);
    /// Same as above but use current block height
    bool GetNextMazanodeInQueueForPayment(bool fFilterSigTime, int& nCountRet, mazanode_info_t& mnInfoRet);

    /// Find a random entry
    mazanode_info_t FindRandomNotInVec(const std::vector<COutPoint> &vecToExclude, int nProtocolVersion = -1);

    std::map<COutPoint, CMazanode> GetFullMazanodeMap() { return mapMazanodes; }

    bool GetMazanodeRanks(rank_pair_vec_t& vecMazanodeRanksRet, int nBlockHeight = -1, int nMinProtocol = 0);
    bool GetMazanodeRank(const COutPoint &outpoint, int& nRankRet, int nBlockHeight = -1, int nMinProtocol = 0);

    void ProcessMazanodeConnections(CConnman& connman);
    std::pair<CService, std::set<uint256> > PopScheduledMnbRequestConnection();
    void ProcessPendingMnbRequests(CConnman& connman);

    void ProcessMessage(CNode* pfrom, const std::string& strCommand, CDataStream& vRecv, CConnman& connman);

    void DoFullVerificationStep(CConnman& connman);
    void CheckSameAddr();
    bool SendVerifyRequest(const CAddress& addr, const std::vector<const CMazanode*>& vSortedByAddr, CConnman& connman);
    void ProcessPendingMnvRequests(CConnman& connman);
    void SendVerifyReply(CNode* pnode, CMazanodeVerification& mnv, CConnman& connman);
    void ProcessVerifyReply(CNode* pnode, CMazanodeVerification& mnv);
    void ProcessVerifyBroadcast(CNode* pnode, const CMazanodeVerification& mnv);

    /// Return the number of (unique) Mazanodes
    int size() { return mapMazanodes.size(); }

    std::string ToString() const;

    /// Perform complete check and only then update mazanode list and maps using provided CMazanodeBroadcast
    bool CheckMnbAndUpdateMazanodeList(CNode* pfrom, CMazanodeBroadcast mnb, int& nDos, CConnman& connman);
    bool IsMnbRecoveryRequested(const uint256& hash) { return mMnbRecoveryRequests.count(hash); }

    void UpdateLastPaid(const CBlockIndex* pindex);

    void AddDirtyGovernanceObjectHash(const uint256& nHash)
    {
        LOCK(cs);
        vecDirtyGovernanceObjectHashes.push_back(nHash);
    }

    std::vector<uint256> GetAndClearDirtyGovernanceObjectHashes()
    {
        LOCK(cs);
        std::vector<uint256> vecTmp = vecDirtyGovernanceObjectHashes;
        vecDirtyGovernanceObjectHashes.clear();
        return vecTmp;;
    }

    bool IsSentinelPingActive();
    void UpdateLastSentinelPingTime();
    bool AddGovernanceVote(const COutPoint& outpoint, uint256 nGovernanceObjectHash);
    void RemoveGovernanceObject(uint256 nGovernanceObjectHash);

    void CheckMazanode(const CPubKey& pubKeyMazanode, bool fForce);

    bool IsMazanodePingedWithin(const COutPoint& outpoint, int nSeconds, int64_t nTimeToCheckAt = -1);
    void SetMazanodeLastPing(const COutPoint& outpoint, const CMazanodePing& mnp);

    void UpdatedBlockTip(const CBlockIndex *pindex);

    void WarnMazanodeDaemonUpdates();

    /**
     * Called to notify CGovernanceManager that the mazanode index has been updated.
     * Must be called while not holding the CMazanodeMan::cs mutex
     */
    void NotifyMazanodeUpdates(CConnman& connman);

};

#endif
