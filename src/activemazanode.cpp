// Copyright (c) 2014-2017 The Maza Network developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "activemazanode.h"
#include "mazanode.h"
#include "mazanode-sync.h"
#include "mazanodeman.h"
#include "netbase.h"
#include "protocol.h"

// Keep track of the active Mazanode
CActiveMazanode activeMazanode;

void CActiveMazanode::ManageState(CConnman& connman)
{
    LogPrint("mazanode", "CActiveMazanode::ManageState -- Start\n");
    if(!fMazanodeMode) {
        LogPrint("mazanode", "CActiveMazanode::ManageState -- Not a mazanode, returning\n");
        return;
    }

    if(Params().NetworkIDString() != CBaseChainParams::REGTEST && !mazanodeSync.IsBlockchainSynced()) {
        nState = ACTIVE_MAZANODE_SYNC_IN_PROCESS;
        LogPrintf("CActiveMazanode::ManageState -- %s: %s\n", GetStateString(), GetStatus());
        return;
    }

    if(nState == ACTIVE_MAZANODE_SYNC_IN_PROCESS) {
        nState = ACTIVE_MAZANODE_INITIAL;
    }

    LogPrint("mazanode", "CActiveMazanode::ManageState -- status = %s, type = %s, pinger enabled = %d\n", GetStatus(), GetTypeString(), fPingerEnabled);

    if(eType == MAZANODE_UNKNOWN) {
        ManageStateInitial(connman);
    }

    if(eType == MAZANODE_REMOTE) {
        ManageStateRemote();
    }

    SendMazanodePing(connman);
}

std::string CActiveMazanode::GetStateString() const
{
    switch (nState) {
        case ACTIVE_MAZANODE_INITIAL:         return "INITIAL";
        case ACTIVE_MAZANODE_SYNC_IN_PROCESS: return "SYNC_IN_PROCESS";
        case ACTIVE_MAZANODE_INPUT_TOO_NEW:   return "INPUT_TOO_NEW";
        case ACTIVE_MAZANODE_NOT_CAPABLE:     return "NOT_CAPABLE";
        case ACTIVE_MAZANODE_STARTED:         return "STARTED";
        default:                                return "UNKNOWN";
    }
}

std::string CActiveMazanode::GetStatus() const
{
    switch (nState) {
        case ACTIVE_MAZANODE_INITIAL:         return "Node just started, not yet activated";
        case ACTIVE_MAZANODE_SYNC_IN_PROCESS: return "Sync in progress. Must wait until sync is complete to start Mazanode";
        case ACTIVE_MAZANODE_INPUT_TOO_NEW:   return strprintf("Mazanode input must have at least %d confirmations", Params().GetConsensus().nMazanodeMinimumConfirmations);
        case ACTIVE_MAZANODE_NOT_CAPABLE:     return "Not capable mazanode: " + strNotCapableReason;
        case ACTIVE_MAZANODE_STARTED:         return "Mazanode successfully started";
        default:                                return "Unknown";
    }
}

std::string CActiveMazanode::GetTypeString() const
{
    std::string strType;
    switch(eType) {
    case MAZANODE_REMOTE:
        strType = "REMOTE";
        break;
    default:
        strType = "UNKNOWN";
        break;
    }
    return strType;
}

bool CActiveMazanode::SendMazanodePing(CConnman& connman)
{
    if(!fPingerEnabled) {
        LogPrint("mazanode", "CActiveMazanode::SendMazanodePing -- %s: mazanode ping service is disabled, skipping...\n", GetStateString());
        return false;
    }

    if(!mnodeman.Has(outpoint)) {
        strNotCapableReason = "Mazanode not in mazanode list";
        nState = ACTIVE_MAZANODE_NOT_CAPABLE;
        LogPrintf("CActiveMazanode::SendMazanodePing -- %s: %s\n", GetStateString(), strNotCapableReason);
        return false;
    }

    CMazanodePing mnp(outpoint);
    mnp.nSentinelVersion = nSentinelVersion;
    mnp.fSentinelIsCurrent =
            (abs(GetAdjustedTime() - nSentinelPingTime) < MAZANODE_SENTINEL_PING_MAX_SECONDS);
    if(!mnp.Sign(keyMazanode, pubKeyMazanode)) {
        LogPrintf("CActiveMazanode::SendMazanodePing -- ERROR: Couldn't sign Mazanode Ping\n");
        return false;
    }

    // Update lastPing for our mazanode in Mazanode list
    if(mnodeman.IsMazanodePingedWithin(outpoint, MAZANODE_MIN_MNP_SECONDS, mnp.sigTime)) {
        LogPrintf("CActiveMazanode::SendMazanodePing -- Too early to send Mazanode Ping\n");
        return false;
    }

    mnodeman.SetMazanodeLastPing(outpoint, mnp);

    LogPrintf("CActiveMazanode::SendMazanodePing -- Relaying ping, collateral=%s\n", outpoint.ToStringShort());
    mnp.Relay(connman);

    return true;
}

bool CActiveMazanode::UpdateSentinelPing(int version)
{
    nSentinelVersion = version;
    nSentinelPingTime = GetAdjustedTime();

    return true;
}

void CActiveMazanode::ManageStateInitial(CConnman& connman)
{
    LogPrint("mazanode", "CActiveMazanode::ManageStateInitial -- status = %s, type = %s, pinger enabled = %d\n", GetStatus(), GetTypeString(), fPingerEnabled);

    // Check that our local network configuration is correct
    if (!fListen) {
        // listen option is probably overwritten by smth else, no good
        nState = ACTIVE_MAZANODE_NOT_CAPABLE;
        strNotCapableReason = "Mazanode must accept connections from outside. Make sure listen configuration option is not overwritten by some another parameter.";
        LogPrintf("CActiveMazanode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
        return;
    }

    // First try to find whatever local address is specified by externalip option
    bool fFoundLocal = GetLocal(service) && CMazanode::IsValidNetAddr(service);
    if(!fFoundLocal) {
        bool empty = true;
        // If we have some peers, let's try to find our local address from one of them
        connman.ForEachNodeContinueIf(CConnman::AllNodes, [&fFoundLocal, &empty, this](CNode* pnode) {
            empty = false;
            if (pnode->addr.IsIPv4())
                fFoundLocal = GetLocal(service, &pnode->addr) && CMazanode::IsValidNetAddr(service);
            return !fFoundLocal;
        });
        // nothing and no live connections, can't do anything for now
        if (empty) {
            nState = ACTIVE_MAZANODE_NOT_CAPABLE;
            strNotCapableReason = "Can't detect valid external address. Will retry when there are some connections available.";
            LogPrintf("CActiveMazanode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }
    }

    if(!fFoundLocal) {
        nState = ACTIVE_MAZANODE_NOT_CAPABLE;
        strNotCapableReason = "Can't detect valid external address. Please consider using the externalip configuration option if problem persists. Make sure to use IPv4 address only.";
        LogPrintf("CActiveMazanode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
        return;
    }

    int mainnetDefaultPort = Params(CBaseChainParams::MAIN).GetDefaultPort();
    if(Params().NetworkIDString() == CBaseChainParams::MAIN) {
        if(service.GetPort() != mainnetDefaultPort) {
            nState = ACTIVE_MAZANODE_NOT_CAPABLE;
            strNotCapableReason = strprintf("Invalid port: %u - only %d is supported on mainnet.", service.GetPort(), mainnetDefaultPort);
            LogPrintf("CActiveMazanode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }
    } else if(service.GetPort() == mainnetDefaultPort) {
        nState = ACTIVE_MAZANODE_NOT_CAPABLE;
        strNotCapableReason = strprintf("Invalid port: %u - %d is only supported on mainnet.", service.GetPort(), mainnetDefaultPort);
        LogPrintf("CActiveMazanode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
        return;
    }

    // Check socket connectivity
    LogPrintf("CActiveMazanode::ManageStateInitial -- Checking inbound connection to '%s'\n", service.ToString());
    SOCKET hSocket;
    bool fConnected = ConnectSocket(service, hSocket, nConnectTimeout) && IsSelectableSocket(hSocket);
    CloseSocket(hSocket);

    if (!fConnected) {
        nState = ACTIVE_MAZANODE_NOT_CAPABLE;
        strNotCapableReason = "Could not connect to " + service.ToString();
        LogPrintf("CActiveMazanode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
        return;
    }

    // Default to REMOTE
    eType = MAZANODE_REMOTE;

    LogPrint("mazanode", "CActiveMazanode::ManageStateInitial -- End status = %s, type = %s, pinger enabled = %d\n", GetStatus(), GetTypeString(), fPingerEnabled);
}

void CActiveMazanode::ManageStateRemote()
{
    LogPrint("mazanode", "CActiveMazanode::ManageStateRemote -- Start status = %s, type = %s, pinger enabled = %d, pubKeyMazanode.GetID() = %s\n", 
             GetStatus(), GetTypeString(), fPingerEnabled, pubKeyMazanode.GetID().ToString());

    mnodeman.CheckMazanode(pubKeyMazanode, true);
    mazanode_info_t infoMn;
    if(mnodeman.GetMazanodeInfo(pubKeyMazanode, infoMn)) {
        if(infoMn.nProtocolVersion != PROTOCOL_VERSION) {
            nState = ACTIVE_MAZANODE_NOT_CAPABLE;
            strNotCapableReason = "Invalid protocol version";
            LogPrintf("CActiveMazanode::ManageStateRemote -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }
        if(service != infoMn.addr) {
            nState = ACTIVE_MAZANODE_NOT_CAPABLE;
            strNotCapableReason = "Broadcasted IP doesn't match our external address. Make sure you issued a new broadcast if IP of this mazanode changed recently.";
            LogPrintf("CActiveMazanode::ManageStateRemote -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }
        if(!CMazanode::IsValidStateForAutoStart(infoMn.nActiveState)) {
            nState = ACTIVE_MAZANODE_NOT_CAPABLE;
            strNotCapableReason = strprintf("Mazanode in %s state", CMazanode::StateToString(infoMn.nActiveState));
            LogPrintf("CActiveMazanode::ManageStateRemote -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }
        if(nState != ACTIVE_MAZANODE_STARTED) {
            LogPrintf("CActiveMazanode::ManageStateRemote -- STARTED!\n");
            outpoint = infoMn.outpoint;
            service = infoMn.addr;
            fPingerEnabled = true;
            nState = ACTIVE_MAZANODE_STARTED;
        }
    }
    else {
        nState = ACTIVE_MAZANODE_NOT_CAPABLE;
        strNotCapableReason = "Mazanode not in mazanode list";
        LogPrintf("CActiveMazanode::ManageStateRemote -- %s: %s\n", GetStateString(), strNotCapableReason);
    }
}
