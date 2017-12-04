// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2016 The Bitcoin Core developers
// Copyright (c) 2017-2017 The Merit Foundation developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "refmempool.h"

#include "consensus/validation.h"
#include "policy/fees.h"
#include "policy/policy.h"
#include "reverse_iterator.h"
#include "streams.h"
#include "validation.h"

#include <algorithm>
#include <numeric>

namespace referral
{
RefMemPoolEntry::RefMemPoolEntry(const Referral& _entry, int64_t _nTime, unsigned int _entryHeight) : MemPoolEntry(_entry, _nTime, _entryHeight)
{
    nWeight = GetReferralWeight(_entry);
    nUsageSize = sizeof(RefMemPoolEntry);
}

size_t RefMemPoolEntry::GetSize() const
{
    return GetVirtualReferralSize(nWeight);
}

bool ReferralTxMemPool::AddUnchecked(const uint256& hash, const RefMemPoolEntry& entry)
{
    NotifyEntryAdded(entry.GetSharedEntryValue());

    LOCK(cs);

    refiter newit = mapRTx.insert(entry).first;
    mapLinks.insert(std::make_pair(newit, RefLinks()));

    // check mempool referrals for a parent
    auto parentit =
        std::find_if(mapRTx.begin(), mapRTx.end(),
            [entry](const referral::RefMemPoolEntry& parent) {
                return parent.GetSharedEntryValue()->address == entry.GetEntryValue().parentAddress;
            });

    if (parentit != mapRTx.end()) {
        mapLinks[parentit].children.insert(newit);
    }

    return true;
}

void ReferralTxMemPool::CalculateDescendants(refiter entryit, setEntries& setDescendants) const
{
    setEntries stage;
    if (setDescendants.count(entryit) == 0) {
        stage.insert(entryit);
    }

    // Traverse down the children of entry, only adding children that are not
    // accounted for in setDescendants already (because those children have either
    // already been walked, or will be walked in this iteration).
    while (!stage.empty()) {
        refiter it = *stage.begin();
        setDescendants.insert(it);
        stage.erase(it);

        const setEntries& setChildren = GetMemPoolChildren(it);
        for (const refiter& childit : setChildren) {
            if (!setDescendants.count(childit)) {
                stage.insert(childit);
            }
        }
    }
}

const ReferralTxMemPool::setEntries& ReferralTxMemPool::GetMemPoolChildren(refiter entryit) const
{
    assert(entryit != mapRTx.end());
    reflinksMap::const_iterator it = mapLinks.find(entryit);
    assert(it != mapLinks.end());
    return it->second.children;
}

void ReferralTxMemPool::RemoveRecursive(const Referral& origRef, MemPoolRemovalReason reason)
{
    // Remove referrals from memory pool
    LOCK(cs);

    refiter origit = mapRTx.find(origRef.GetHash());

    if (origit != mapRTx.end()) {
        setEntries toRemove;

        CalculateDescendants(origit, toRemove);
        RemoveStaged(toRemove, reason);
    }
}

void ReferralTxMemPool::RemoveForBlock(const std::vector<ReferralRef>& vRefs)
{
    LOCK(cs);

    for (const auto& ref : vRefs) {
        auto it = mapRTx.find(ref->GetHash());

        if (it != mapRTx.end()) {
            RemoveUnchecked(it, MemPoolRemovalReason::BLOCK);
        }
    }
}

void ReferralTxMemPool::RemoveUnchecked(refiter it, MemPoolRemovalReason reason)
{
    NotifyEntryRemoved(it->GetSharedEntryValue(), reason);

    mapRTx.erase(it);
    mapLinks.erase(it);
}

void ReferralTxMemPool::RemoveStaged(setEntries& stage, MemPoolRemovalReason reason)
{
    AssertLockHeld(cs);
    // UpdateForRemoveFromMempool(stage, updateDescendants);
    for (const refiter& it : stage) {
        RemoveUnchecked(it, reason);
    }
}

int ReferralTxMemPool::Expire(int64_t time) {
    LOCK(cs);

    indexed_referrals_set::index<entry_time>::type::iterator it = mapRTx.get<entry_time>().begin();
    setEntries toRemove;
    while (it != mapRTx.get<entry_time>().end() && it->GetTime() < time) {
        toRemove.insert(mapRTx.project<0>(it));
        it++;
    }
    setEntries stage;
    for (refiter removeit : toRemove) {
        CalculateDescendants(removeit, stage);
    }
    RemoveStaged(stage, MemPoolRemovalReason::EXPIRY);

    return stage.size();
}

ReferralRef ReferralTxMemPool::get(const uint256& hash) const
{
    LOCK(cs);
    auto it = mapRTx.find(hash);
    return it != mapRTx.end() ? it->GetSharedEntryValue() : nullptr;
}

ReferralRef ReferralTxMemPool::GetWithAddress(const Address& address) const
{
    LOCK(cs);
    for (const auto& it : mapRTx) {
        const auto ref = it.GetSharedEntryValue();
        if (ref->address == address) {
            return ref;
        }
    }

    return nullptr;
}

bool ReferralTxMemPool::ExistsWithAddress(const Address& address) const
{
    return GetWithAddress(address) != nullptr;
}

void ReferralTxMemPool::GetReferralsForTransaction(const CTransactionRef& tx, referral::ReferralTxMemPool::setEntries& txReferrals)
{
    // check addresses used for vouts are beaconed
    for (const auto& txout : tx->vout) {
        CTxDestination dest;
        uint160 addr;
        // CNoDestination script
        if (!ExtractDestination(txout.scriptPubKey, dest)) {
            continue;
        }

        assert(GetUint160(dest, addr));

        bool addressBeaconed = prefviewcache->WalletIdExists(addr);

        // check cache for beaconed address
        if (addressBeaconed) {
            continue;
        }

        // check mempoolReferral for beaconed address
        const auto it = std::find_if(
            mapRTx.begin(), mapRTx.end(),
            [&addr](const referral::RefMemPoolEntry& entry) {
                return entry.GetEntryValue().address == addr;
            });

        if (it != mapRTx.end()) {
            txReferrals.insert(it);
        }
    }
}

std::vector<ReferralRef> ReferralTxMemPool::GetReferrals() const
{
    LOCK(cs);

    std::vector<ReferralRef> refs(mapRTx.size());

    std::transform(mapRTx.begin(), mapRTx.end(), refs.begin(),
            [](const referral::RefMemPoolEntry& entry) {
                return entry.GetSharedEntryValue();
            });

    return refs;
}

size_t ReferralTxMemPool::DynamicMemoryUsage() const
{
    LOCK(cs);
    return memusage::MallocUsage(sizeof(RefMemPoolEntry) + 15 * sizeof(void*)) * mapRTx.size() + memusage::DynamicUsage(mapLinks);
}


void ReferralTxMemPool::Clear()
{
    LOCK(cs);
    mapLinks.clear();
    mapRTx.clear();
}
}