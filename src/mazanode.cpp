// Copyright (c) 2014-2017 The Maza Network developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "activemazanode.h"
#include "base58.h"
#include "clientversion.h"
#include "init.h"
#include "netbase.h"
#include "mazanode.h"
#include "mazanode-payments.h"
#include "mazanode-sync.h"
#include "mazanodeman.h"
#include "messagesigner.h"
#include "script/standard.h"
#include "util.h"
#ifdef ENABLE_WALLET
#include "wallet/wallet.h"
#endif // ENABLE_WALLET

#include <boost/lexical_cast.hpp>


CMazanode::CMazanode() :
    mazanode_info_t{ MAZANODE_ENABLED, PROTOCOL_VERSION, GetAdjustedTime()},
    fAllowMixingTx(true)
{}

CMazanode::CMazanode(CService addr, COutPoint outpoint, CPubKey pubKeyCollateralAddress, CPubKey pubKeyMazanode, int nProtocolVersionIn) :
    mazanode_info_t{ MAZANODE_ENABLED, nProtocolVersionIn, GetAdjustedTime(),
                       outpoint, addr, pubKeyCollateralAddress, pubKeyMazanode},
    fAllowMixingTx(true)
{}

CMazanode::CMazanode(const CMazanode& other) :
    mazanode_info_t{other},
    lastPing(other.lastPing),
    vchSig(other.vchSig),
    nCollateralMinConfBlockHash(other.nCollateralMinConfBlockHash),
    nBlockLastPaid(other.nBlockLastPaid),
    nPoSeBanScore(other.nPoSeBanScore),
    nPoSeBanHeight(other.nPoSeBanHeight),
    fAllowMixingTx(other.fAllowMixingTx),
    fUnitTest(other.fUnitTest)
{}

CMazanode::CMazanode(const CMazanodeBroadcast& mnb) :
    mazanode_info_t{ mnb.nActiveState, mnb.nProtocolVersion, mnb.sigTime,
                       mnb.outpoint, mnb.addr, mnb.pubKeyCollateralAddress, mnb.pubKeyMazanode},
    lastPing(mnb.lastPing),
    vchSig(mnb.vchSig),
    fAllowMixingTx(true)
{}

//
// When a new mazanode broadcast is sent, update our information
//
bool CMazanode::UpdateFromNewBroadcast(CMazanodeBroadcast& mnb, CConnman& connman)
{
    if(mnb.sigTime <= sigTime && !mnb.fRecovery) return false;

    pubKeyMazanode = mnb.pubKeyMazanode;
    sigTime = mnb.sigTime;
    vchSig = mnb.vchSig;
    nProtocolVersion = mnb.nProtocolVersion;
    addr = mnb.addr;
    nPoSeBanScore = 0;
    nPoSeBanHeight = 0;
    nTimeLastChecked = 0;
    int nDos = 0;
    if(!mnb.lastPing || (mnb.lastPing && mnb.lastPing.CheckAndUpdate(this, true, nDos, connman))) {
        lastPing = mnb.lastPing;
        mnodeman.mapSeenMazanodePing.insert(std::make_pair(lastPing.GetHash(), lastPing));
    }
    // if it matches our Mazanode privkey...
    if(fMazanodeMode && pubKeyMazanode == activeMazanode.pubKeyMazanode) {
        nPoSeBanScore = -MAZANODE_POSE_BAN_MAX_SCORE;
        if(nProtocolVersion == PROTOCOL_VERSION) {
            // ... and PROTOCOL_VERSION, then we've been remotely activated ...
            activeMazanode.ManageState(connman);
        } else {
            // ... otherwise we need to reactivate our node, do not add it to the list and do not relay
            // but also do not ban the node we get this message from
            LogPrintf("CMazanode::UpdateFromNewBroadcast -- wrong PROTOCOL_VERSION, re-activate your MN: message nProtocolVersion=%d  PROTOCOL_VERSION=%d\n", nProtocolVersion, PROTOCOL_VERSION);
            return false;
        }
    }
    return true;
}

//
// Deterministically calculate a given "score" for a Mazanode depending on how close it's hash is to
// the proof of work for that block. The further away they are the better, the furthest will win the election
// and get paid this block
//
arith_uint256 CMazanode::CalculateScore(const uint256& blockHash) const
{
    // Deterministically calculate a "score" for a Mazanode based on any given (block)hash
    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << outpoint << nCollateralMinConfBlockHash << blockHash;
    return UintToArith256(ss.GetHash());
}

CMazanode::CollateralStatus CMazanode::CheckCollateral(const COutPoint& outpoint, const CPubKey& pubkey)
{
    int nHeight;
    return CheckCollateral(outpoint, pubkey, nHeight);
}

CMazanode::CollateralStatus CMazanode::CheckCollateral(const COutPoint& outpoint, const CPubKey& pubkey, int& nHeightRet)
{
    AssertLockHeld(cs_main);

    Coin coin;
    if(!GetUTXOCoin(outpoint, coin)) {
        return COLLATERAL_UTXO_NOT_FOUND;
    }

    if(coin.out.nValue != 1000 * COIN) {
        return COLLATERAL_INVALID_AMOUNT;
    }

    if(pubkey == CPubKey() || coin.out.scriptPubKey != GetScriptForDestination(pubkey.GetID())) {
        return COLLATERAL_INVALID_PUBKEY;
    }

    nHeightRet = coin.nHeight;
    return COLLATERAL_OK;
}

void CMazanode::Check(bool fForce)
{
    AssertLockHeld(cs_main);
    LOCK(cs);

    if(ShutdownRequested()) return;

    if(!fForce && (GetTime() - nTimeLastChecked < MAZANODE_CHECK_SECONDS)) return;
    nTimeLastChecked = GetTime();

    LogPrint("mazanode", "CMazanode::Check -- Mazanode %s is in %s state\n", outpoint.ToStringShort(), GetStateString());

    //once spent, stop doing the checks
    if(IsOutpointSpent()) return;

    int nHeight = 0;
    if(!fUnitTest) {
        Coin coin;
        if(!GetUTXOCoin(outpoint, coin)) {
            nActiveState = MAZANODE_OUTPOINT_SPENT;
            LogPrint("mazanode", "CMazanode::Check -- Failed to find Mazanode UTXO, mazanode=%s\n", outpoint.ToStringShort());
            return;
        }

        nHeight = chainActive.Height();
    }

    if(IsPoSeBanned()) {
        if(nHeight < nPoSeBanHeight) return; // too early?
        // Otherwise give it a chance to proceed further to do all the usual checks and to change its state.
        // Mazanode still will be on the edge and can be banned back easily if it keeps ignoring mnverify
        // or connect attempts. Will require few mnverify messages to strengthen its position in mn list.
        LogPrintf("CMazanode::Check -- Mazanode %s is unbanned and back in list now\n", outpoint.ToStringShort());
        DecreasePoSeBanScore();
    } else if(nPoSeBanScore >= MAZANODE_POSE_BAN_MAX_SCORE) {
        nActiveState = MAZANODE_POSE_BAN;
        // ban for the whole payment cycle
        nPoSeBanHeight = nHeight + mnodeman.size();
        LogPrintf("CMazanode::Check -- Mazanode %s is banned till block %d now\n", outpoint.ToStringShort(), nPoSeBanHeight);
        return;
    }

    int nActiveStatePrev = nActiveState;
    bool fOurMazanode = fMazanodeMode && activeMazanode.pubKeyMazanode == pubKeyMazanode;

                   // mazanode doesn't meet payment protocol requirements ...
    bool fRequireUpdate = nProtocolVersion < mnpayments.GetMinMazanodePaymentsProto() ||
                   // or it's our own node and we just updated it to the new protocol but we are still waiting for activation ...
                   (fOurMazanode && nProtocolVersion < PROTOCOL_VERSION);

    if(fRequireUpdate) {
        nActiveState = MAZANODE_UPDATE_REQUIRED;
        if(nActiveStatePrev != nActiveState) {
            LogPrint("mazanode", "CMazanode::Check -- Mazanode %s is in %s state now\n", outpoint.ToStringShort(), GetStateString());
        }
        return;
    }

    // keep old mazanodes on start, give them a chance to receive updates...
    bool fWaitForPing = !mazanodeSync.IsMazanodeListSynced() && !IsPingedWithin(MAZANODE_MIN_MNP_SECONDS);

    if(fWaitForPing && !fOurMazanode) {
        // ...but if it was already expired before the initial check - return right away
        if(IsExpired() || IsSentinelPingExpired() || IsNewStartRequired()) {
            LogPrint("mazanode", "CMazanode::Check -- Mazanode %s is in %s state, waiting for ping\n", outpoint.ToStringShort(), GetStateString());
            return;
        }
    }

    // don't expire if we are still in "waiting for ping" mode unless it's our own mazanode
    if(!fWaitForPing || fOurMazanode) {

        if(!IsPingedWithin(MAZANODE_NEW_START_REQUIRED_SECONDS)) {
            nActiveState = MAZANODE_NEW_START_REQUIRED;
            if(nActiveStatePrev != nActiveState) {
                LogPrint("mazanode", "CMazanode::Check -- Mazanode %s is in %s state now\n", outpoint.ToStringShort(), GetStateString());
            }
            return;
        }

        if(!IsPingedWithin(MAZANODE_EXPIRATION_SECONDS)) {
            nActiveState = MAZANODE_EXPIRED;
            if(nActiveStatePrev != nActiveState) {
                LogPrint("mazanode", "CMazanode::Check -- Mazanode %s is in %s state now\n", outpoint.ToStringShort(), GetStateString());
            }
            return;
        }

        // part 1: expire based on mazad ping
        bool fSentinelPingActive = mazanodeSync.IsSynced() && mnodeman.IsSentinelPingActive();
        bool fSentinelPingExpired = fSentinelPingActive && !IsPingedWithin(MAZANODE_SENTINEL_PING_MAX_SECONDS);
        LogPrint("mazanode", "CMazanode::Check -- outpoint=%s, GetAdjustedTime()=%d, fSentinelPingExpired=%d\n",
                outpoint.ToStringShort(), GetAdjustedTime(), fSentinelPingExpired);

        if(fSentinelPingExpired) {
            nActiveState = MAZANODE_SENTINEL_PING_EXPIRED;
            if(nActiveStatePrev != nActiveState) {
                LogPrint("mazanode", "CMazanode::Check -- Mazanode %s is in %s state now\n", outpoint.ToStringShort(), GetStateString());
            }
            return;
        }
    }

    // We require MNs to be in PRE_ENABLED until they either start to expire or receive a ping and go into ENABLED state
    // Works on mainnet/testnet only and not the case on regtest/devnet.
    if (Params().NetworkIDString() != CBaseChainParams::REGTEST && Params().NetworkIDString() != CBaseChainParams::DEVNET) {
        if (lastPing.sigTime - sigTime < MAZANODE_MIN_MNP_SECONDS) {
            nActiveState = MAZANODE_PRE_ENABLED;
            if (nActiveStatePrev != nActiveState) {
                LogPrint("mazanode", "CMazanode::Check -- Mazanode %s is in %s state now\n", outpoint.ToStringShort(), GetStateString());
            }
            return;
        }
    }

    if(!fWaitForPing || fOurMazanode) {
        // part 2: expire based on sentinel info
        bool fSentinelPingActive = mazanodeSync.IsSynced() && mnodeman.IsSentinelPingActive();
        bool fSentinelPingExpired = fSentinelPingActive && !lastPing.fSentinelIsCurrent;

        LogPrint("mazanode", "CMazanode::Check -- outpoint=%s, GetAdjustedTime()=%d, fSentinelPingExpired=%d\n",
                outpoint.ToStringShort(), GetAdjustedTime(), fSentinelPingExpired);

        if(fSentinelPingExpired) {
            nActiveState = MAZANODE_SENTINEL_PING_EXPIRED;
            if(nActiveStatePrev != nActiveState) {
                LogPrint("mazanode", "CMazanode::Check -- Mazanode %s is in %s state now\n", outpoint.ToStringShort(), GetStateString());
            }
            return;
        }
    }

    nActiveState = MAZANODE_ENABLED; // OK
    if(nActiveStatePrev != nActiveState) {
        LogPrint("mazanode", "CMazanode::Check -- Mazanode %s is in %s state now\n", outpoint.ToStringShort(), GetStateString());
    }
}

bool CMazanode::IsValidNetAddr()
{
    return IsValidNetAddr(addr);
}

bool CMazanode::IsValidNetAddr(CService addrIn)
{
    // TODO: regtest is fine with any addresses for now,
    // should probably be a bit smarter if one day we start to implement tests for this
    return Params().NetworkIDString() == CBaseChainParams::REGTEST ||
            (addrIn.IsIPv4() && IsReachable(addrIn) && addrIn.IsRoutable());
}

mazanode_info_t CMazanode::GetInfo() const
{
    mazanode_info_t info{*this};
    info.nTimeLastPing = lastPing.sigTime;
    info.fInfoValid = true;
    return info;
}

std::string CMazanode::StateToString(int nStateIn)
{
    switch(nStateIn) {
        case MAZANODE_PRE_ENABLED:            return "PRE_ENABLED";
        case MAZANODE_ENABLED:                return "ENABLED";
        case MAZANODE_EXPIRED:                return "EXPIRED";
        case MAZANODE_OUTPOINT_SPENT:         return "OUTPOINT_SPENT";
        case MAZANODE_UPDATE_REQUIRED:        return "UPDATE_REQUIRED";
        case MAZANODE_SENTINEL_PING_EXPIRED:  return "SENTINEL_PING_EXPIRED";
        case MAZANODE_NEW_START_REQUIRED:     return "NEW_START_REQUIRED";
        case MAZANODE_POSE_BAN:               return "POSE_BAN";
        default:                                return "UNKNOWN";
    }
}

std::string CMazanode::GetStateString() const
{
    return StateToString(nActiveState);
}

std::string CMazanode::GetStatus() const
{
    // TODO: return smth a bit more human readable here
    return GetStateString();
}

void CMazanode::UpdateLastPaid(const CBlockIndex *pindex, int nMaxBlocksToScanBack)
{
    if(!pindex) return;

    const CBlockIndex *BlockReading = pindex;

    CScript mnpayee = GetScriptForDestination(pubKeyCollateralAddress.GetID());
    // LogPrint("mnpayments", "CMazanode::UpdateLastPaidBlock -- searching for block with payment to %s\n", outpoint.ToStringShort());

    LOCK(cs_mapMazanodeBlocks);

    for (int i = 0; BlockReading && BlockReading->nHeight > nBlockLastPaid && i < nMaxBlocksToScanBack; i++) {
        if(mnpayments.mapMazanodeBlocks.count(BlockReading->nHeight) &&
            mnpayments.mapMazanodeBlocks[BlockReading->nHeight].HasPayeeWithVotes(mnpayee, 2))
        {
            CBlock block;
            if(!ReadBlockFromDisk(block, BlockReading, Params().GetConsensus()))
                continue; // shouldn't really happen

            CAmount nMazanodePayment = GetMazanodePayment(BlockReading->nHeight, block.vtx[0]->GetValueOut());

            for (const auto& txout : block.vtx[0]->vout)
                if(mnpayee == txout.scriptPubKey && nMazanodePayment == txout.nValue) {
                    nBlockLastPaid = BlockReading->nHeight;
                    nTimeLastPaid = BlockReading->nTime;
                    LogPrint("mnpayments", "CMazanode::UpdateLastPaidBlock -- searching for block with payment to %s -- found new %d\n", outpoint.ToStringShort(), nBlockLastPaid);
                    return;
                }
        }

        if (BlockReading->pprev == NULL) { assert(BlockReading); break; }
        BlockReading = BlockReading->pprev;
    }

    // Last payment for this mazanode wasn't found in latest mnpayments blocks
    // or it was found in mnpayments blocks but wasn't found in the blockchain.
    // LogPrint("mnpayments", "CMazanode::UpdateLastPaidBlock -- searching for block with payment to %s -- keeping old %d\n", outpoint.ToStringShort(), nBlockLastPaid);
}

#ifdef ENABLE_WALLET
bool CMazanodeBroadcast::Create(const std::string& strService, const std::string& strKeyMazanode, const std::string& strTxHash, const std::string& strOutputIndex, std::string& strErrorRet, CMazanodeBroadcast &mnbRet, bool fOffline)
{
    COutPoint outpoint;
    CPubKey pubKeyCollateralAddressNew;
    CKey keyCollateralAddressNew;
    CPubKey pubKeyMazanodeNew;
    CKey keyMazanodeNew;

    auto Log = [&strErrorRet](std::string sErr)->bool
    {
        strErrorRet = sErr;
        LogPrintf("CMazanodeBroadcast::Create -- %s\n", strErrorRet);
        return false;
    };

    // Wait for sync to finish because mnb simply won't be relayed otherwise
    if (!fOffline && !mazanodeSync.IsSynced())
        return Log("Sync in progress. Must wait until sync is complete to start Mazanode");

    if (!CMessageSigner::GetKeysFromSecret(strKeyMazanode, keyMazanodeNew, pubKeyMazanodeNew))
        return Log(strprintf("Invalid mazanode key %s", strKeyMazanode));

    if (!pwalletMain->GetMazanodeOutpointAndKeys(outpoint, pubKeyCollateralAddressNew, keyCollateralAddressNew, strTxHash, strOutputIndex))
        return Log(strprintf("Could not allocate outpoint %s:%s for mazanode %s", strTxHash, strOutputIndex, strService));

    CService service;
    if (!Lookup(strService.c_str(), service, 0, false))
        return Log(strprintf("Invalid address %s for mazanode.", strService));
    int mainnetDefaultPort = Params(CBaseChainParams::MAIN).GetDefaultPort();
    if (Params().NetworkIDString() == CBaseChainParams::MAIN) {
        if (service.GetPort() != mainnetDefaultPort)
            return Log(strprintf("Invalid port %u for mazanode %s, only %d is supported on mainnet.", service.GetPort(), strService, mainnetDefaultPort));
    } else if (service.GetPort() == mainnetDefaultPort)
        return Log(strprintf("Invalid port %u for mazanode %s, %d is the only supported on mainnet.", service.GetPort(), strService, mainnetDefaultPort));

    return Create(outpoint, service, keyCollateralAddressNew, pubKeyCollateralAddressNew, keyMazanodeNew, pubKeyMazanodeNew, strErrorRet, mnbRet);
}

bool CMazanodeBroadcast::Create(const COutPoint& outpoint, const CService& service, const CKey& keyCollateralAddressNew, const CPubKey& pubKeyCollateralAddressNew, const CKey& keyMazanodeNew, const CPubKey& pubKeyMazanodeNew, std::string &strErrorRet, CMazanodeBroadcast &mnbRet)
{
    // wait for reindex and/or import to finish
    if (fImporting || fReindex) return false;

    LogPrint("mazanode", "CMazanodeBroadcast::Create -- pubKeyCollateralAddressNew = %s, pubKeyMazanodeNew.GetID() = %s\n",
             CBitcoinAddress(pubKeyCollateralAddressNew.GetID()).ToString(),
             pubKeyMazanodeNew.GetID().ToString());

    auto Log = [&strErrorRet,&mnbRet](std::string sErr)->bool
    {
        strErrorRet = sErr;
        LogPrintf("CMazanodeBroadcast::Create -- %s\n", strErrorRet);
        mnbRet = CMazanodeBroadcast();
        return false;
    };

    CMazanodePing mnp(outpoint);
    if (!mnp.Sign(keyMazanodeNew, pubKeyMazanodeNew))
        return Log(strprintf("Failed to sign ping, mazanode=%s", outpoint.ToStringShort()));

    mnbRet = CMazanodeBroadcast(service, outpoint, pubKeyCollateralAddressNew, pubKeyMazanodeNew, PROTOCOL_VERSION);

    if (!mnbRet.IsValidNetAddr())
        return Log(strprintf("Invalid IP address, mazanode=%s", outpoint.ToStringShort()));

    mnbRet.lastPing = mnp;
    if (!mnbRet.Sign(keyCollateralAddressNew))
        return Log(strprintf("Failed to sign broadcast, mazanode=%s", outpoint.ToStringShort()));

    return true;
}
#endif // ENABLE_WALLET

bool CMazanodeBroadcast::SimpleCheck(int& nDos)
{
    nDos = 0;

    AssertLockHeld(cs_main);

    // make sure addr is valid
    if(!IsValidNetAddr()) {
        LogPrintf("CMazanodeBroadcast::SimpleCheck -- Invalid addr, rejected: mazanode=%s  addr=%s\n",
                    outpoint.ToStringShort(), addr.ToString());
        return false;
    }

    // make sure signature isn't in the future (past is OK)
    if (sigTime > GetAdjustedTime() + 60 * 60) {
        LogPrintf("CMazanodeBroadcast::SimpleCheck -- Signature rejected, too far into the future: mazanode=%s\n", outpoint.ToStringShort());
        nDos = 1;
        return false;
    }

    // empty ping or incorrect sigTime/unknown blockhash
    if(!lastPing || !lastPing.SimpleCheck(nDos)) {
        // one of us is probably forked or smth, just mark it as expired and check the rest of the rules
        nActiveState = MAZANODE_EXPIRED;
    }

    if(nProtocolVersion < mnpayments.GetMinMazanodePaymentsProto()) {
        LogPrintf("CMazanodeBroadcast::SimpleCheck -- outdated Mazanode: mazanode=%s  nProtocolVersion=%d\n", outpoint.ToStringShort(), nProtocolVersion);
        nActiveState = MAZANODE_UPDATE_REQUIRED;
    }

    CScript pubkeyScript;
    pubkeyScript = GetScriptForDestination(pubKeyCollateralAddress.GetID());

    if(pubkeyScript.size() != 25) {
        LogPrintf("CMazanodeBroadcast::SimpleCheck -- pubKeyCollateralAddress has the wrong size\n");
        nDos = 100;
        return false;
    }

    CScript pubkeyScript2;
    pubkeyScript2 = GetScriptForDestination(pubKeyMazanode.GetID());

    if(pubkeyScript2.size() != 25) {
        LogPrintf("CMazanodeBroadcast::SimpleCheck -- pubKeyMazanode has the wrong size\n");
        nDos = 100;
        return false;
    }

    int mainnetDefaultPort = Params(CBaseChainParams::MAIN).GetDefaultPort();
    if(Params().NetworkIDString() == CBaseChainParams::MAIN) {
        if(addr.GetPort() != mainnetDefaultPort) return false;
    } else if(addr.GetPort() == mainnetDefaultPort) return false;

    return true;
}

bool CMazanodeBroadcast::Update(CMazanode* pmn, int& nDos, CConnman& connman)
{
    nDos = 0;

    AssertLockHeld(cs_main);

    if(pmn->sigTime == sigTime && !fRecovery) {
        // mapSeenMazanodeBroadcast in CMazanodeMan::CheckMnbAndUpdateMazanodeList should filter legit duplicates
        // but this still can happen if we just started, which is ok, just do nothing here.
        return false;
    }

    // this broadcast is older than the one that we already have - it's bad and should never happen
    // unless someone is doing something fishy
    if(pmn->sigTime > sigTime) {
        LogPrintf("CMazanodeBroadcast::Update -- Bad sigTime %d (existing broadcast is at %d) for Mazanode %s %s\n",
                      sigTime, pmn->sigTime, outpoint.ToStringShort(), addr.ToString());
        return false;
    }

    pmn->Check();

    // mazanode is banned by PoSe
    if(pmn->IsPoSeBanned()) {
        LogPrintf("CMazanodeBroadcast::Update -- Banned by PoSe, mazanode=%s\n", outpoint.ToStringShort());
        return false;
    }

    // IsVnAssociatedWithPubkey is validated once in CheckOutpoint, after that they just need to match
    if(pmn->pubKeyCollateralAddress != pubKeyCollateralAddress) {
        LogPrintf("CMazanodeBroadcast::Update -- Got mismatched pubKeyCollateralAddress and outpoint\n");
        nDos = 33;
        return false;
    }

    if (!CheckSignature(nDos)) {
        LogPrintf("CMazanodeBroadcast::Update -- CheckSignature() failed, mazanode=%s\n", outpoint.ToStringShort());
        return false;
    }

    // if ther was no mazanode broadcast recently or if it matches our Mazanode privkey...
    if(!pmn->IsBroadcastedWithin(MAZANODE_MIN_MNB_SECONDS) || (fMazanodeMode && pubKeyMazanode == activeMazanode.pubKeyMazanode)) {
        // take the newest entry
        LogPrintf("CMazanodeBroadcast::Update -- Got UPDATED Mazanode entry: addr=%s\n", addr.ToString());
        if(pmn->UpdateFromNewBroadcast(*this, connman)) {
            pmn->Check();
            Relay(connman);
        }
        mazanodeSync.BumpAssetLastTime("CMazanodeBroadcast::Update");
    }

    return true;
}

bool CMazanodeBroadcast::CheckOutpoint(int& nDos)
{
    // we are a mazanode with the same outpoint (i.e. already activated) and this mnb is ours (matches our Mazanode privkey)
    // so nothing to do here for us
    if(fMazanodeMode && outpoint == activeMazanode.outpoint && pubKeyMazanode == activeMazanode.pubKeyMazanode) {
        return false;
    }

    AssertLockHeld(cs_main);

    int nHeight;
    CollateralStatus err = CheckCollateral(outpoint, pubKeyCollateralAddress, nHeight);
    if (err == COLLATERAL_UTXO_NOT_FOUND) {
        LogPrint("mazanode", "CMazanodeBroadcast::CheckOutpoint -- Failed to find Mazanode UTXO, mazanode=%s\n", outpoint.ToStringShort());
        return false;
    }

    if (err == COLLATERAL_INVALID_AMOUNT) {
        LogPrint("mazanode", "CMazanodeBroadcast::CheckOutpoint -- Mazanode UTXO should have 1000 MAZA, mazanode=%s\n", outpoint.ToStringShort());
        nDos = 33;
        return false;
    }

    if(err == COLLATERAL_INVALID_PUBKEY) {
        LogPrint("mazanode", "CMazanodeBroadcast::CheckOutpoint -- Mazanode UTXO should match pubKeyCollateralAddress, mazanode=%s\n", outpoint.ToStringShort());
        nDos = 33;
        return false;
    }

    if(chainActive.Height() - nHeight + 1 < Params().GetConsensus().nMazanodeMinimumConfirmations) {
        LogPrintf("CMazanodeBroadcast::CheckOutpoint -- Mazanode UTXO must have at least %d confirmations, mazanode=%s\n",
                Params().GetConsensus().nMazanodeMinimumConfirmations, outpoint.ToStringShort());
        // UTXO is legit but has not enough confirmations.
        // Maybe we miss few blocks, let this mnb be checked again later.
        mnodeman.mapSeenMazanodeBroadcast.erase(GetHash());
        return false;
    }

    LogPrint("mazanode", "CMazanodeBroadcast::CheckOutpoint -- Mazanode UTXO verified\n");

    // Verify that sig time is legit, should be at least not earlier than the timestamp of the block
    // at which collateral became nMazanodeMinimumConfirmations blocks deep.
    // NOTE: this is not accurate because block timestamp is NOT guaranteed to be 100% correct one.
    CBlockIndex* pRequiredConfIndex = chainActive[nHeight + Params().GetConsensus().nMazanodeMinimumConfirmations - 1]; // block where tx got nMazanodeMinimumConfirmations
    if(pRequiredConfIndex->GetBlockTime() > sigTime) {
        LogPrintf("CMazanodeBroadcast::CheckOutpoint -- Bad sigTime %d (%d conf block is at %d) for Mazanode %s %s\n",
                  sigTime, Params().GetConsensus().nMazanodeMinimumConfirmations, pRequiredConfIndex->GetBlockTime(), outpoint.ToStringShort(), addr.ToString());
        return false;
    }

    if (!CheckSignature(nDos)) {
        LogPrintf("CMazanodeBroadcast::CheckOutpoint -- CheckSignature() failed, mazanode=%s\n", outpoint.ToStringShort());
        return false;
    }

    // remember the block hash when collateral for this mazanode had minimum required confirmations
    nCollateralMinConfBlockHash = pRequiredConfIndex->GetBlockHash();

    return true;
}

uint256 CMazanodeBroadcast::GetHash() const
{
    // Note: doesn't match serialization

    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << outpoint << uint8_t{} << 0xffffffff; // adding dummy values here to match old hashing format
    ss << pubKeyCollateralAddress;
    ss << sigTime;
    return ss.GetHash();
}

uint256 CMazanodeBroadcast::GetSignatureHash() const
{
    // TODO: replace with "return SerializeHash(*this);" after migration to 70209
    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << outpoint;
    ss << addr;
    ss << pubKeyCollateralAddress;
    ss << pubKeyMazanode;
    ss << sigTime;
    ss << nProtocolVersion;
    return ss.GetHash();
}

bool CMazanodeBroadcast::Sign(const CKey& keyCollateralAddress)
{
    std::string strError;

    sigTime = GetAdjustedTime();

    if (sporkManager.IsSporkActive(SPORK_6_NEW_SIGS)) {
        uint256 hash = GetSignatureHash();

        if (!CHashSigner::SignHash(hash, keyCollateralAddress, vchSig)) {
            LogPrintf("CMazanodeBroadcast::Sign -- SignHash() failed\n");
            return false;
        }

        if (!CHashSigner::VerifyHash(hash, pubKeyCollateralAddress, vchSig, strError)) {
            LogPrintf("CMazanodeBroadcast::Sign -- VerifyMessage() failed, error: %s\n", strError);
            return false;
        }
    } else {
        std::string strMessage = addr.ToString(false) + boost::lexical_cast<std::string>(sigTime) +
                        pubKeyCollateralAddress.GetID().ToString() + pubKeyMazanode.GetID().ToString() +
                        boost::lexical_cast<std::string>(nProtocolVersion);

        if (!CMessageSigner::SignMessage(strMessage, vchSig, keyCollateralAddress)) {
            LogPrintf("CMazanodeBroadcast::Sign -- SignMessage() failed\n");
            return false;
        }

        if (!CMessageSigner::VerifyMessage(pubKeyCollateralAddress, vchSig, strMessage, strError)) {
            LogPrintf("CMazanodeBroadcast::Sign -- VerifyMessage() failed, error: %s\n", strError);
            return false;
        }
    }

    return true;
}

bool CMazanodeBroadcast::CheckSignature(int& nDos) const
{
    std::string strError = "";
    nDos = 0;

    if (sporkManager.IsSporkActive(SPORK_6_NEW_SIGS)) {
        uint256 hash = GetSignatureHash();

        if (!CHashSigner::VerifyHash(hash, pubKeyCollateralAddress, vchSig, strError)) {
            // maybe it's in old format
            std::string strMessage = addr.ToString(false) + boost::lexical_cast<std::string>(sigTime) +
                            pubKeyCollateralAddress.GetID().ToString() + pubKeyMazanode.GetID().ToString() +
                            boost::lexical_cast<std::string>(nProtocolVersion);

            if (!CMessageSigner::VerifyMessage(pubKeyCollateralAddress, vchSig, strMessage, strError)){
                // nope, not in old format either
                LogPrintf("CMazanodeBroadcast::CheckSignature -- Got bad Mazanode announce signature, error: %s\n", strError);
                nDos = 100;
                return false;
            }
        }
    } else {
        std::string strMessage = addr.ToString(false) + boost::lexical_cast<std::string>(sigTime) +
                        pubKeyCollateralAddress.GetID().ToString() + pubKeyMazanode.GetID().ToString() +
                        boost::lexical_cast<std::string>(nProtocolVersion);

        if (!CMessageSigner::VerifyMessage(pubKeyCollateralAddress, vchSig, strMessage, strError)){
            LogPrintf("CMazanodeBroadcast::CheckSignature -- Got bad Mazanode announce signature, error: %s\n", strError);
            nDos = 100;
            return false;
        }
    }

    return true;
}

void CMazanodeBroadcast::Relay(CConnman& connman) const
{
    // Do not relay until fully synced
    if(!mazanodeSync.IsSynced()) {
        LogPrint("mazanode", "CMazanodeBroadcast::Relay -- won't relay until fully synced\n");
        return;
    }

    CInv inv(MSG_MAZANODE_ANNOUNCE, GetHash());
    connman.RelayInv(inv);
}

uint256 CMazanodePing::GetHash() const
{
    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    if (sporkManager.IsSporkActive(SPORK_6_NEW_SIGS)) {
        // TODO: replace with "return SerializeHash(*this);" after migration to 70209
        ss << mazanodeOutpoint;
        ss << blockHash;
        ss << sigTime;
        ss << fSentinelIsCurrent;
        ss << nSentinelVersion;
        ss << nDaemonVersion;
    } else {
        // Note: doesn't match serialization

        ss << mazanodeOutpoint << uint8_t{} << 0xffffffff; // adding dummy values here to match old hashing format
        ss << sigTime;
    }
    return ss.GetHash();
}

uint256 CMazanodePing::GetSignatureHash() const
{
    return GetHash();
}

CMazanodePing::CMazanodePing(const COutPoint& outpoint)
{
    LOCK(cs_main);
    if (!chainActive.Tip() || chainActive.Height() < 12) return;

    mazanodeOutpoint = outpoint;
    blockHash = chainActive[chainActive.Height() - 12]->GetBlockHash();
    sigTime = GetAdjustedTime();
    nDaemonVersion = CLIENT_VERSION;
}

bool CMazanodePing::Sign(const CKey& keyMazanode, const CPubKey& pubKeyMazanode)
{
    std::string strError;

    sigTime = GetAdjustedTime();

    if (sporkManager.IsSporkActive(SPORK_6_NEW_SIGS)) {
        uint256 hash = GetSignatureHash();

        if (!CHashSigner::SignHash(hash, keyMazanode, vchSig)) {
            LogPrintf("CMazanodePing::Sign -- SignHash() failed\n");
            return false;
        }

        if (!CHashSigner::VerifyHash(hash, pubKeyMazanode, vchSig, strError)) {
            LogPrintf("CMazanodePing::Sign -- VerifyHash() failed, error: %s\n", strError);
            return false;
        }
    } else {
        std::string strMessage = CTxIn(mazanodeOutpoint).ToString() + blockHash.ToString() +
                    boost::lexical_cast<std::string>(sigTime);

        if (!CMessageSigner::SignMessage(strMessage, vchSig, keyMazanode)) {
            LogPrintf("CMazanodePing::Sign -- SignMessage() failed\n");
            return false;
        }

        if (!CMessageSigner::VerifyMessage(pubKeyMazanode, vchSig, strMessage, strError)) {
            LogPrintf("CMazanodePing::Sign -- VerifyMessage() failed, error: %s\n", strError);
            return false;
        }
    }

    return true;
}

bool CMazanodePing::CheckSignature(const CPubKey& pubKeyMazanode, int &nDos) const
{
    std::string strError = "";
    nDos = 0;

    if (sporkManager.IsSporkActive(SPORK_6_NEW_SIGS)) {
        uint256 hash = GetSignatureHash();

        if (!CHashSigner::VerifyHash(hash, pubKeyMazanode, vchSig, strError)) {
            std::string strMessage = CTxIn(mazanodeOutpoint).ToString() + blockHash.ToString() +
                        boost::lexical_cast<std::string>(sigTime);

            if (!CMessageSigner::VerifyMessage(pubKeyMazanode, vchSig, strMessage, strError)) {
                LogPrintf("CMazanodePing::CheckSignature -- Got bad Mazanode ping signature, mazanode=%s, error: %s\n", mazanodeOutpoint.ToStringShort(), strError);
                nDos = 33;
                return false;
            }
        }
    } else {
        std::string strMessage = CTxIn(mazanodeOutpoint).ToString() + blockHash.ToString() +
                    boost::lexical_cast<std::string>(sigTime);

        if (!CMessageSigner::VerifyMessage(pubKeyMazanode, vchSig, strMessage, strError)) {
            LogPrintf("CMazanodePing::CheckSignature -- Got bad Mazanode ping signature, mazanode=%s, error: %s\n", mazanodeOutpoint.ToStringShort(), strError);
            nDos = 33;
            return false;
        }
    }

    return true;
}

bool CMazanodePing::SimpleCheck(int& nDos)
{
    // don't ban by default
    nDos = 0;

    if (sigTime > GetAdjustedTime() + 60 * 60) {
        LogPrintf("CMazanodePing::SimpleCheck -- Signature rejected, too far into the future, mazanode=%s\n", mazanodeOutpoint.ToStringShort());
        nDos = 1;
        return false;
    }

    {
        AssertLockHeld(cs_main);
        BlockMap::iterator mi = mapBlockIndex.find(blockHash);
        if (mi == mapBlockIndex.end()) {
            LogPrint("mazanode", "CMazanodePing::SimpleCheck -- Mazanode ping is invalid, unknown block hash: mazanode=%s blockHash=%s\n", mazanodeOutpoint.ToStringShort(), blockHash.ToString());
            // maybe we stuck or forked so we shouldn't ban this node, just fail to accept this ping
            // TODO: or should we also request this block?
            return false;
        }
    }

    LogPrint("mazanode", "CMazanodePing::SimpleCheck -- Mazanode ping verified: mazanode=%s  blockHash=%s  sigTime=%d\n", mazanodeOutpoint.ToStringShort(), blockHash.ToString(), sigTime);
    return true;
}

bool CMazanodePing::CheckAndUpdate(CMazanode* pmn, bool fFromNewBroadcast, int& nDos, CConnman& connman)
{
    AssertLockHeld(cs_main);

    // don't ban by default
    nDos = 0;

    if (!SimpleCheck(nDos)) {
        return false;
    }

    if (pmn == NULL) {
        LogPrint("mazanode", "CMazanodePing::CheckAndUpdate -- Couldn't find Mazanode entry, mazanode=%s\n", mazanodeOutpoint.ToStringShort());
        return false;
    }

    if(!fFromNewBroadcast) {
        if (pmn->IsUpdateRequired()) {
            LogPrint("mazanode", "CMazanodePing::CheckAndUpdate -- mazanode protocol is outdated, mazanode=%s\n", mazanodeOutpoint.ToStringShort());
            return false;
        }

        if (pmn->IsNewStartRequired()) {
            LogPrint("mazanode", "CMazanodePing::CheckAndUpdate -- mazanode is completely expired, new start is required, mazanode=%s\n", mazanodeOutpoint.ToStringShort());
            return false;
        }
    }

    {
        BlockMap::iterator mi = mapBlockIndex.find(blockHash);
        if ((*mi).second && (*mi).second->nHeight < chainActive.Height() - 24) {
            LogPrintf("CMazanodePing::CheckAndUpdate -- Mazanode ping is invalid, block hash is too old: mazanode=%s  blockHash=%s\n", mazanodeOutpoint.ToStringShort(), blockHash.ToString());
            // nDos = 1;
            return false;
        }
    }

    LogPrint("mazanode", "CMazanodePing::CheckAndUpdate -- New ping: mazanode=%s  blockHash=%s  sigTime=%d\n", mazanodeOutpoint.ToStringShort(), blockHash.ToString(), sigTime);

    // LogPrintf("mnping - Found corresponding mn for outpoint: %s\n", mazanodeOutpoint.ToStringShort());
    // update only if there is no known ping for this mazanode or
    // last ping was more then MAZANODE_MIN_MNP_SECONDS-60 ago comparing to this one
    if (pmn->IsPingedWithin(MAZANODE_MIN_MNP_SECONDS - 60, sigTime)) {
        LogPrint("mazanode", "CMazanodePing::CheckAndUpdate -- Mazanode ping arrived too early, mazanode=%s\n", mazanodeOutpoint.ToStringShort());
        //nDos = 1; //disable, this is happening frequently and causing banned peers
        return false;
    }

    if (!CheckSignature(pmn->pubKeyMazanode, nDos)) return false;

    // so, ping seems to be ok

    // if we are still syncing and there was no known ping for this mn for quite a while
    // (NOTE: assuming that MAZANODE_EXPIRATION_SECONDS/2 should be enough to finish mn list sync)
    if(!mazanodeSync.IsMazanodeListSynced() && !pmn->IsPingedWithin(MAZANODE_EXPIRATION_SECONDS/2)) {
        // let's bump sync timeout
        LogPrint("mazanode", "CMazanodePing::CheckAndUpdate -- bumping sync timeout, mazanode=%s\n", mazanodeOutpoint.ToStringShort());
        mazanodeSync.BumpAssetLastTime("CMazanodePing::CheckAndUpdate");
    }

    // let's store this ping as the last one
    LogPrint("mazanode", "CMazanodePing::CheckAndUpdate -- Mazanode ping accepted, mazanode=%s\n", mazanodeOutpoint.ToStringShort());
    pmn->lastPing = *this;

    // and update mnodeman.mapSeenMazanodeBroadcast.lastPing which is probably outdated
    CMazanodeBroadcast mnb(*pmn);
    uint256 hash = mnb.GetHash();
    if (mnodeman.mapSeenMazanodeBroadcast.count(hash)) {
        mnodeman.mapSeenMazanodeBroadcast[hash].second.lastPing = *this;
    }

    // force update, ignoring cache
    pmn->Check(true);
    // relay ping for nodes in ENABLED/EXPIRED/SENTINEL_PING_EXPIRED state only, skip everyone else
    if (!pmn->IsEnabled() && !pmn->IsExpired() && !pmn->IsSentinelPingExpired()) return false;

    LogPrint("mazanode", "CMazanodePing::CheckAndUpdate -- Mazanode ping acceepted and relayed, mazanode=%s\n", mazanodeOutpoint.ToStringShort());
    Relay(connman);

    return true;
}

void CMazanodePing::Relay(CConnman& connman)
{
    // Do not relay until fully synced
    if(!mazanodeSync.IsSynced()) {
        LogPrint("mazanode", "CMazanodePing::Relay -- won't relay until fully synced\n");
        return;
    }

    CInv inv(MSG_MAZANODE_PING, GetHash());
    connman.RelayInv(inv);
}

void CMazanode::AddGovernanceVote(uint256 nGovernanceObjectHash)
{
    if(mapGovernanceObjectsVotedOn.count(nGovernanceObjectHash)) {
        mapGovernanceObjectsVotedOn[nGovernanceObjectHash]++;
    } else {
        mapGovernanceObjectsVotedOn.insert(std::make_pair(nGovernanceObjectHash, 1));
    }
}

void CMazanode::RemoveGovernanceObject(uint256 nGovernanceObjectHash)
{
    std::map<uint256, int>::iterator it = mapGovernanceObjectsVotedOn.find(nGovernanceObjectHash);
    if(it == mapGovernanceObjectsVotedOn.end()) {
        return;
    }
    mapGovernanceObjectsVotedOn.erase(it);
}

/**
*   FLAG GOVERNANCE ITEMS AS DIRTY
*
*   - When mazanode come and go on the network, we must flag the items they voted on to recalc it's cached flags
*
*/
void CMazanode::FlagGovernanceItemsAsDirty()
{
    std::vector<uint256> vecDirty;
    {
        std::map<uint256, int>::iterator it = mapGovernanceObjectsVotedOn.begin();
        while(it != mapGovernanceObjectsVotedOn.end()) {
            vecDirty.push_back(it->first);
            ++it;
        }
    }
    for(size_t i = 0; i < vecDirty.size(); ++i) {
        mnodeman.AddDirtyGovernanceObjectHash(vecDirty[i]);
    }
}
