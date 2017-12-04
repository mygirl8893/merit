// Copyright (c) 2017 The Merit Foundation developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "refdb.h"

#include "base58.h"

namespace referral
{
namespace
{
const char DB_CHILDREN = 'c';
const char DB_REFERRALS = 'r';
const char DB_REFERRALS_BY_KEY_ID = 'k';
const char DB_PARENT_KEY = 'p';
const char DB_ANV = 'a';
const size_t MAX_LEVELS = std::numeric_limits<size_t>::max();
}

using ANVTuple = std::tuple<char, Address, CAmount>;

ReferralsViewDB::ReferralsViewDB(size_t nCacheSize, bool fMemory, bool fWipe, const std::string& db_name) :
    m_db(GetDataDir() / db_name, nCacheSize, fMemory, fWipe, true) {}

MaybeReferral ReferralsViewDB::GetReferral(const Address& address) const {
     MutableReferral referral;
     return m_db.Read(std::make_pair(DB_REFERRALS, address), referral) ?
         MaybeReferral{referral} : MaybeReferral{};
}

MaybeAddress ReferralsViewDB::GetReferrer(const Address& address) const
{
    Address parent;
    return m_db.Read(std::make_pair(DB_PARENT_KEY, address), parent) ?
        MaybeAddress{parent} : MaybeAddress{};
}

ChildAddresses ReferralsViewDB::GetChildren(const Address& address) const
{
    ChildAddresses children;
    m_db.Read(std::make_pair(DB_CHILDREN, address), children);
    return children;
}

bool ReferralsViewDB::InsertReferral(const Referral& referral, bool allow_no_parent) {

    debug("Inserting referral %s code %s parent %s",
            CMeritAddress(referral.addressType, referral.pubKeyId).ToString(),
            referral.codeHash.GetHex(),
            referral.previousReferral.GetHex());

    //write referral by code hash
    if(!m_db.Write(std::make_pair(DB_REFERRALS, referral.address), referral)) {
        return false;
    }

    ANVTuple anv{referral.addressType, referral.pubKeyId, CAmount{0}};
    if(!m_db.Write(std::make_pair(DB_ANV, referral.pubKeyId), anv)) {
        return false;
    }

    // Typically because the referral should be written in order we should
    // be able to find the parent referral. We can then write the child->parent
    // mapping of public addresses
    Address parent_address;
    if(auto parent_referral = GetReferral(referral.parentAddress)) {
        debug("\tInserting parent reference %s parent %s paddress %s",
                CMeritAddress(referral.addressType, referral.address).ToString(),
                parent_referral->address.GetHex(),
                CMeritAddress(parent_referral->addressType, parent_referral->address).ToString()
                );

        parent_address = parent_referral->address;
        if(!m_db.Write(std::make_pair(DB_PARENT_KEY, referral.address), parent_address))
            return false;

        // Now we update the children of the parent address by inserting into the
        // child address array for the parent.
        ChildAddresses children;
        m_db.Read(std::make_pair(DB_CHILDREN, parent_address), children);

        children.push_back(referral.address);

        if(!m_db.Write(std::make_pair(DB_CHILDREN, parent_address), children))
            return false;

    } else if(!allow_no_parent) {
        assert(false && "parent referral missing");
        return false;
    } else {
        debug("\tWarning Parent missing for code %s", referral.previousReferral.GetHex());
    }

    return true;
}

bool ReferralsViewDB::RemoveReferral(const Referral& referral) {
    debug("Removing Referral %d", CMeritAddress{referral.addressType, referral.address}.ToString());

    if(!m_db.Erase(std::make_pair(DB_REFERRALS, referral.address)))
        return false;

    Address parent_address;
    if(auto parent_referral = GetReferral(referral.parentAddress))
        parent_address = parent_referral->address;

    if(!m_db.Erase(std::make_pair(DB_PARENT_KEY, referral.address)))
        return false;

    ChildAddresses children;
    m_db.Read(std::make_pair(DB_CHILDREN, parent_address), children);

    children.erase(std::remove(std::begin(children), std::end(children), referral.address), std::end(children));
    if(!m_db.Write(std::make_pair(DB_CHILDREN, parent_address), children))
        return false;

    return true;
}

bool ReferralsViewDB::ReferralCodeExists(const uint256& code_hash) const {
    return m_db.Exists(std::make_pair(DB_REFERRALS, code_hash));
}

bool ReferralsViewDB::ReferralAddressExists(const referral::Address& address) const {
    return m_db.Exists(std::make_pair(DB_REFERRALS, address));
}

bool ReferralsViewDB::WalletIdExists(const Address& address) const
{
    return m_db.Exists(std::make_pair(DB_PARENT_KEY, address));
}

/**
 * Updates ANV for the address and all parents. Note change can be negative if
 * there was a debit.
 */

bool ReferralsViewDB::UpdateANV(char addressType, const Address& start_address, CAmount change)
{
    debug("\tUpdateANV: %s + %d", CMeritAddress(addressType, start_address).ToString(), change);

    MaybeAddress address = start_address;
    size_t level = 0;

    //MAX_LEVELS guards against cycles in DB
    while(address && level < MAX_LEVELS)
    {
        //it's possible address didn't exist yet so an ANV of 0 is assumed.
        ANVTuple anv;
        if(!m_db.Read(std::make_pair(DB_ANV, *address), anv)) {
            debug("\tFailed to read ANV for %s", address->GetHex());
            return false;
        }

        assert(std::get<0>(anv) != 0);
        assert(!std::get<1>(anv).IsNull());

        debug(
                "\t\t %d %s %d + %d",
                level,
                CMeritAddress(std::get<0>(anv),std::get<1>(anv)).ToString(),
                std::get<2>(anv),
                change);

        std::get<2>(anv) += change;

        assert(std::get<2>(anv) >= 0);

        if(!m_db.Write(std::make_pair(DB_ANV, *address), anv)) {
            //TODO: Do we rollback anv computation for already processed address?
            // likely if we can't write then rollback will fail too.
            // figure out how to mark database as corrupt.
            return false;
        }

        address = GetReferrer(*address);
        level++;
    }

    // We should never have cycles in the DB.
    // Hacked? Bug?
    assert(level < MAX_LEVELS && "reached max levels. Referral DB cycle detected");
    return true;
}

MaybeAddressANV ReferralsViewDB::GetANV(const Address& address) const
{
    ANVTuple anv;
    return m_db.Read(std::make_pair(DB_ANV, address), anv) ?
        MaybeAddressANV{{ std::get<0>(anv), std::get<1>(anv), std::get<2>(anv) }} :
        MaybeAddressANV{};
}

AddressANVs ReferralsViewDB::GetAllANVs() const
{
    std::unique_ptr<CDBIterator> iter{m_db.NewIterator()};
    iter->SeekToFirst();

    AddressANVs anvs;
    auto address = std::make_pair(DB_ANV, Address{});
    while(iter->Valid())
    {
        //filter non ANV addresss
        if(!iter->GetKey(address)) {
            iter->Next();
            continue;
        }

        if(address.first != DB_ANV) {
            iter->Next();
            continue;
        }

        ANVTuple anv;
        if(!iter->GetValue(anv)) {
            iter->Next();
            continue;
        }

        anvs.push_back({
                std::get<0>(anv),
                std::get<1>(anv),
                std::get<2>(anv)
                });

        iter->Next();
    }
    return anvs;
}

AddressANVs ReferralsViewDB::GetAllRewardableANVs() const
{
    std::unique_ptr<CDBIterator> iter{m_db.NewIterator()};
    iter->SeekToFirst();

    AddressANVs anvs;
    auto address = std::make_pair(DB_ANV, Address{});
    while(iter->Valid())
    {
        //filter non ANV addresss
        if(!iter->GetKey(address)) {
            iter->Next();
            continue;
        }

        if(address.first != DB_ANV) {
            iter->Next();
            continue;
        }

        ANVTuple anv;
        if(!iter->GetValue(anv)) {
            iter->Next();
            continue;
        }

        const auto addressType = std::get<0>(anv);
        if(addressType != 1 && addressType != 2) {
            iter->Next();
            continue;
        }

        anvs.push_back({
                addressType,
                std::get<1>(anv),
                std::get<2>(anv)
                });

        iter->Next();
    }
    return anvs;
}

/*
 * Orders referrals by constructing a dependency graph and doing a breath
 * first walk through the forrest.
 */
bool ReferralsViewDB::OrderReferrals(referral::ReferralRefs& refs)
{
    if(refs.empty()) {
        return true;
    }

    auto end_roots =
        std::partition(refs.begin(), refs.end(),
            [this](const referral::ReferralRef& ref) -> bool {
            return static_cast<bool>(GetReferral(ref->previousReferral));
    });

    //If we don't have any roots, we have an invalid block.
    if(end_roots == refs.begin()) {
        return false;
    }

    std::map<uint256, referral::ReferralRefs> graph;

    //insert roots of trees into graph
    std::for_each(
            refs.begin(), end_roots,
            [&graph](const referral::ReferralRef& ref) {
                graph[ref->codeHash] =  referral::ReferralRefs{};
            });

    //Insert disconnected referrals
    std::for_each(end_roots, refs.end(),
            [&graph](const referral::ReferralRef& ref) {
                graph[ref->previousReferral].push_back(ref);
            });

    //copy roots to work queue
    std::deque<referral::ReferralRef> to_process(std::distance(refs.begin(), end_roots));
    std::copy(refs.begin(), end_roots, to_process.begin());

    //do breath first walk through the trees to create correct referral
    //ordering
    auto replace = refs.begin();
    while(!to_process.empty() && replace != refs.end()) {
        const auto& ref = to_process.front();
        *replace = ref;
        to_process.pop_front();
        replace++;

        const auto& children = graph[ref->codeHash];
        to_process.insert(to_process.end(), children.begin(), children.end());
    }

    //If any of these conditions are not met, it means we have an invalid block
    if(replace != refs.end() || !to_process.empty()) {
        return false;
    }

    return true;
}

}
