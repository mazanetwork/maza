// Copyright (c) 2014-2017 The Maza Network developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef ACTIVEMAZANODE_H
#define ACTIVEMAZANODE_H

#include "chainparams.h"
#include "key.h"
#include "net.h"
#include "primitives/transaction.h"

class CActiveMazanode;

static const int ACTIVE_MAZANODE_INITIAL          = 0; // initial state
static const int ACTIVE_MAZANODE_SYNC_IN_PROCESS  = 1;
static const int ACTIVE_MAZANODE_INPUT_TOO_NEW    = 2;
static const int ACTIVE_MAZANODE_NOT_CAPABLE      = 3;
static const int ACTIVE_MAZANODE_STARTED          = 4;

extern CActiveMazanode activeMazanode;

// Responsible for activating the Mazanode and pinging the network
class CActiveMazanode
{
public:
    enum mazanode_type_enum_t {
        MAZANODE_UNKNOWN = 0,
        MAZANODE_REMOTE  = 1
    };

private:
    // critical section to protect the inner data structures
    mutable CCriticalSection cs;

    mazanode_type_enum_t eType;

    bool fPingerEnabled;

    /// Ping Mazanode
    bool SendMazanodePing(CConnman& connman);

    //  sentinel ping data
    int64_t nSentinelPingTime;
    uint32_t nSentinelVersion;

public:
    // Keys for the active Mazanode
    CPubKey pubKeyMazanode;
    CKey keyMazanode;

    // Initialized while registering Mazanode
    COutPoint outpoint;
    CService service;

    int nState; // should be one of ACTIVE_MAZANODE_XXXX
    std::string strNotCapableReason;


    CActiveMazanode()
        : eType(MAZANODE_UNKNOWN),
          fPingerEnabled(false),
          pubKeyMazanode(),
          keyMazanode(),
          outpoint(),
          service(),
          nState(ACTIVE_MAZANODE_INITIAL)
    {}

    /// Manage state of active Mazanode
    void ManageState(CConnman& connman);

    std::string GetStateString() const;
    std::string GetStatus() const;
    std::string GetTypeString() const;

    bool UpdateSentinelPing(int version);

private:
    void ManageStateInitial(CConnman& connman);
    void ManageStateRemote();
};

#endif
