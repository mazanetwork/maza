// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "pow.h"

#include "arith_uint256.h"
#include "chain.h"
#include "chainparams.h"
#include "primitives/block.h"
#include "uint256.h"

#include <math.h>

static const int64_t nTargetTimespan = 8 * 60; // 8 minutes
static const int64_t nTargetSpacing = 2 * 60; // 2 minutes
static const int64_t nInterval = nTargetTimespan / nTargetSpacing; // 4 blocks
static const int64_t nAveragingInterval = nInterval * 20;
static const int64_t nAveragingTargetTimespan = nAveragingInterval * 120; 
static const int64_t nMaxAdjustDown = 20; // 20% adjustment down
static const int64_t nMaxAdjustUp = 15; // 15% adjustment up
static const int64_t nMinActualTimespan = nAveragingTargetTimespan * (100 - nMaxAdjustUp) / 100;
static const int64_t nMaxActualTimespan = nAveragingTargetTimespan * (100 + nMaxAdjustDown) / 100;
	
unsigned int static DarkGravityWave(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params& params) {
    /* current difficulty formula, maza - DarkGravity v3, written by Evan Duffield - evan@mazacoin.org */
    const arith_uint256 bnPowLimit = UintToArith256(params.powLimit);
    int64_t nPastBlocks = 24;

    // make sure we have at least (nPastBlocks + 1) blocks, otherwise just return powLimit
    if (!pindexLast || pindexLast->nHeight < nPastBlocks) {
        return bnPowLimit.GetCompact();
    }

   /* if (params.fPowAllowMinDifficultyBlocks && (
        // testnet ...
        (params.hashDevnetGenesisBlock.IsNull() && pindexLast->nChainWork >= UintToArith256(uint256S("0x000000000000000000000000000000000000000000000000003e9ccfe0e03e01"))) ||
        // or devnet
        !params.hashDevnetGenesisBlock.IsNull())) {
        // NOTE: 000000000000000000000000000000000000000000000000003e9ccfe0e03e01 is the work of the "wrong" chain,
        // so this rule activates there immediately and new blocks with high diff from that chain are going
        // to be rejected by updated nodes. Note, that old nodes are going to reject blocks from updated nodes
        // after the "right" chain reaches this amount of work too. This is a temporary condition which should
        // be removed when we decide to hard-fork testnet again.
        // TODO: remove "testnet+work OR devnet" part on next testnet hard-fork
        // Special difficulty rule for testnet/devnet:
        // If the new block's timestamp is more than 2* 2.5 minutes
        // then allow mining of a min-difficulty block.

        // start using smoother adjustment on testnet when total work hits
        // 000000000000000000000000000000000000000000000000003ff00000000000
        if (pindexLast->nChainWork >= UintToArith256(uint256S("0x000000000000000000000000000000000000000000000000003ff00000000000"))
            // and immediately on devnet
            || !params.hashDevnetGenesisBlock.IsNull()) {
            // recent block is more than 2 hours old
            if (pblock->GetBlockTime() > pindexLast->GetBlockTime() + 2 * 60 * 60) {
                return bnPowLimit.GetCompact();
            }
            // recent block is more than 10 minutes old
            if (pblock->GetBlockTime() > pindexLast->GetBlockTime() + params.nPowTargetSpacing*4) {
                arith_uint256 bnNew = arith_uint256().SetCompact(pindexLast->nBits) * 10;
                if (bnNew > bnPowLimit) {
                    bnNew = bnPowLimit;
                }
                return bnNew.GetCompact();
            }
        } else {
            // old stuff
            if (pblock->GetBlockTime() > pindexLast->GetBlockTime() + params.nPowTargetSpacing*2) {
                return bnPowLimit.GetCompact();
            }
        }
    }
*/
    const CBlockIndex *pindex = pindexLast;
    arith_uint256 bnPastTargetAvg;

    for (unsigned int nCountBlocks = 1; nCountBlocks <= nPastBlocks; nCountBlocks++) {
        arith_uint256 bnTarget = arith_uint256().SetCompact(pindex->nBits);
        if (nCountBlocks == 1) {
            bnPastTargetAvg = bnTarget;
        } else {
            // NOTE: that's not an average really...
            bnPastTargetAvg = (bnPastTargetAvg * nCountBlocks + bnTarget) / (nCountBlocks + 1);
        }

        if(nCountBlocks != nPastBlocks) {
            assert(pindex->pprev); // should never fail
            pindex = pindex->pprev;
        }
    }

    arith_uint256 bnNew(bnPastTargetAvg);

    int64_t nActualTimespan = pindexLast->GetBlockTime() - pindex->GetBlockTime();
    // NOTE: is this accurate? nActualTimespan counts it for (nPastBlocks - 1) blocks only...
    int64_t nTargetTimespan = nPastBlocks * params.nPowTargetSpacing;

    if (nActualTimespan < nTargetTimespan/3)
        nActualTimespan = nTargetTimespan/3;
    if (nActualTimespan > nTargetTimespan*3)
        nActualTimespan = nTargetTimespan*3;

    // Retarget
    bnNew *= nActualTimespan;
    bnNew /= nTargetTimespan;

    if (bnNew > bnPowLimit) {
        bnNew = bnPowLimit;
    }

    return bnNew.GetCompact();
}

unsigned int GetNextWorkRequiredBTC(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params& params)
{


    unsigned int nProofOfWorkLimit = UintToArith256(params.powLimit).GetCompact();
	unsigned int nStartingDifficulty = UintToArith256(params.startingDifficulty).GetCompact();
    // Genesis block
    if (pindexLast == NULL)
        return nProofOfWorkLimit;
	if (pindexLast->nHeight+1 < nAveragingInterval)
        return nStartingDifficulty;
    // Only change once per interval
    if ((pindexLast->nHeight+1) % nInterval != 0)
    {
        if (params.fPowAllowMinDifficultyBlocks)
        {
            // Special difficulty rule for testnet:
            // If the new block's timestamp is more than 2* 2 minutes
            // then allow mining of a min-difficulty block.
            if (pblock->GetBlockTime() > pindexLast->GetBlockTime() + params.nPowTargetSpacing*2)
                return nProofOfWorkLimit;
            else
            {
                // Return the last non-special-min-difficulty-rules-block
                const CBlockIndex* pindex = pindexLast;
                while (pindex->pprev && pindex->nHeight % nInterval != 0 && pindex->nBits == nProofOfWorkLimit)
                    pindex = pindex->pprev;
                return pindex->nBits;
            }
        }
        return pindexLast->nBits;
    }

    // Go back by what we want to be nAveragingInterval blocks
    const CBlockIndex* pindexFirst = pindexLast;
    for (int i = 0; pindexFirst && i < nAveragingInterval-1; i++)
        pindexFirst = pindexFirst->pprev;
    assert(pindexFirst);

   return CalculateNextWorkRequired(pindexLast, pindexFirst->GetBlockTime(), params);
}

unsigned int GetNextWorkRequired(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params& params)
{
    // Most recent algo first
    if (pindexLast->nHeight + 1 >= params.nPowDGWHeight) {
        return DarkGravityWave(pindexLast, pblock, params);
    }

    else {
        return GetNextWorkRequiredBTC(pindexLast, pblock, params);
    }
}


// for DIFF_BTC only!
unsigned int CalculateNextWorkRequired(const CBlockIndex* pindexLast, int64_t nFirstBlockTime, const Consensus::Params& params)
{
	
    if (params.fPowNoRetargeting)
        return pindexLast->nBits;

    // Limit adjustment step
    int64_t nActualTimespan = pindexLast->GetBlockTime() - nFirstBlockTime;
    if (nActualTimespan < nMinActualTimespan)
        nActualTimespan = nMinActualTimespan;
    if (nActualTimespan > nMaxActualTimespan)
        nActualTimespan = nMaxActualTimespan;

    // Retarget
    const arith_uint256 bnPowLimit = UintToArith256(params.powLimit);
    arith_uint256 bnNew;
    bnNew.SetCompact(pindexLast->nBits);
    bnNew *= nActualTimespan;
    bnNew /= nAveragingTargetTimespan;

    if (bnNew > bnPowLimit)
        bnNew = bnPowLimit;

    return bnNew.GetCompact();
}

bool CheckProofOfWork(uint256 hash, unsigned int nBits, const Consensus::Params& params)
{
    bool fNegative;
    bool fOverflow;
    arith_uint256 bnTarget;

    bnTarget.SetCompact(nBits, &fNegative, &fOverflow);

    // Check range
    if (fNegative || bnTarget == 0 || fOverflow || bnTarget > UintToArith256(params.powLimit))
        return false;

    // Check proof of work matches claimed amount
    if (UintToArith256(hash) > bnTarget)
        return false;

    return true;
}
