/************************************************************
 *
 *                 OPEN TRANSACTIONS
 *
 *       Financial Cryptography and Digital Cash
 *       Library, Protocol, API, Server, CLI, GUI
 *
 *       -- Anonymous Numbered Accounts.
 *       -- Untraceable Digital Cash.
 *       -- Triple-Signed Receipts.
 *       -- Cheques, Vouchers, Transfers, Inboxes.
 *       -- Basket Currencies, Markets, Payment Plans.
 *       -- Signed, XML, Ricardian-style Contracts.
 *       -- Scripted smart contracts.
 *
 *  EMAIL:
 *  fellowtraveler@opentransactions.org
 *
 *  WEBSITE:
 *  http://www.opentransactions.org/
 *
 *  -----------------------------------------------------
 *
 *   LICENSE:
 *   This Source Code Form is subject to the terms of the
 *   Mozilla Public License, v. 2.0. If a copy of the MPL
 *   was not distributed with this file, You can obtain one
 *   at http://mozilla.org/MPL/2.0/.
 *
 *   DISCLAIMER:
 *   This program is distributed in the hope that it will
 *   be useful, but WITHOUT ANY WARRANTY; without even the
 *   implied warranty of MERCHANTABILITY or FITNESS FOR A
 *   PARTICULAR PURPOSE.  See the Mozilla Public License
 *   for more details.
 *
 ************************************************************/

#include "opentxs/core/app/Wallet.hpp"

#include "opentxs/core/app/App.hpp"
#include "opentxs/core/app/Dht.hpp"
#include "opentxs/core/contract/peer/PeerObject.hpp"
#include "opentxs/core/Identifier.hpp"
#include "opentxs/core/Log.hpp"
#include "opentxs/core/Message.hpp"
#include "opentxs/core/Nym.hpp"
#include "opentxs/core/Proto.hpp"
#include "opentxs/core/String.hpp"
#include "opentxs/storage/Storage.hpp"

#include <stdint.h>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>

namespace opentxs
{

std::unique_ptr<Message> Wallet::Mail(
    const Identifier& nym,
    const Identifier& id,
    const StorageBox& box) const
{
    std::string raw, alias;
    const bool loaded = App::Me().DB().Load(
        String(nym).Get(),
        String(id).Get(),
        box,
        raw,
        alias,
        true);

    std::unique_ptr<Message> output;

    if (!loaded) { return output; }
    if (raw.empty()) { return output; }

    output.reset(new Message);

    OT_ASSERT(output);

    if (!output->LoadContractFromString(String(raw.c_str()))) {
        output.reset();
    }

    return output;
}

std::string Wallet::Mail(
    const Identifier& nym,
    const Message& mail,
    const StorageBox box) const
{
    Identifier id;
    mail.CalculateContractID(id);
    std::string output = String(id).Get();

    const String data(mail);
    std::string alias;

    const bool saved = App::Me().DB().Store(
        String(nym).Get(),
        mail.m_strNymID2.Get(),
        output,
        mail.m_lTime,
        alias,
        data.Get(),
        box);

    if (saved) {

        return output;
    }

    return "";
}

ObjectList Wallet::Mail(const Identifier& nym, const StorageBox box) const
{
    return App::Me().DB().NymBoxList(String(nym).Get(), box);
}

bool Wallet::MailRemove(
    const Identifier& nym,
    const Identifier& id,
    const StorageBox box) const
{
    const std::string nymid = String(nym).Get();
    const std::string mail = String(id).Get();

    return App::Me().DB().RemoveNymBoxItem(nymid, box, mail);
}

ConstNym Wallet::Nym(
    const Identifier& id,
    const std::chrono::milliseconds& timeout)
{
    const std::string nym = String(id).Get();
    std::unique_lock<std::mutex> mapLock(nym_map_lock_);
    bool inMap = (nym_map_.find(nym) != nym_map_.end());
    bool valid = false;

    if (!inMap) {
        std::shared_ptr<proto::CredentialIndex> serialized;

        std::string alias;
        bool loaded = App::Me().DB().Load(nym, serialized, alias, true);

        if (loaded) {
            auto& pNym = nym_map_[nym].second;
            pNym.reset(new class Nym(id));

            if (pNym) {
                if (pNym->LoadCredentialIndex(*serialized)) {
                    valid = pNym->VerifyPseudonym();
                    pNym->alias_ = alias;
                }
            }
        } else {
            App::Me().DHT().GetPublicNym(nym);

            if (timeout > std::chrono::milliseconds(0)) {
                mapLock.unlock();
                auto start = std::chrono::high_resolution_clock::now();
                auto end = start + timeout;
                const auto interval = std::chrono::milliseconds(100);

                while (std::chrono::high_resolution_clock::now() < end) {
                    std::this_thread::sleep_for(interval);
                    mapLock.lock();
                    bool found = (nym_map_.find(nym) != nym_map_.end());
                    mapLock.unlock();

                    if (found) {
                        break;
                    }
                }

                return Nym(id);  // timeout of zero prevents infinite recursion
            }
        }
    } else {
        auto& pNym = nym_map_[nym].second;
        if (pNym) {
            valid = pNym->VerifyPseudonym();
        }
    }

    if (valid) {
        return nym_map_[nym].second;
    }

    return nullptr;
}

ConstNym Wallet::Nym(const proto::CredentialIndex& publicNym)
{
    const auto& id = publicNym.nymid();
    Identifier nym(id);

    auto existing = Nym(Identifier(nym));

    if (existing) {
        if (existing->Revision() >= publicNym.revision()) {

            return existing;
        }
    }
    existing.reset();

    std::unique_ptr<class Nym> candidate(new class Nym(nym));

    if (candidate) {
        candidate->LoadCredentialIndex(publicNym);

        if (candidate->VerifyPseudonym()) {
            candidate->WriteCredentials();
            std::unique_lock<std::mutex> mapLock(nym_map_lock_);
            nym_map_.erase(id);
            mapLock.unlock();
        }
    }

    return Nym(nym);
}

std::mutex& Wallet::peer_lock(const std::string& nymID) const
{
    std::unique_lock<std::mutex> map_lock(peer_map_lock_);
    auto& output = peer_lock_[nymID];
    map_lock.unlock();

    return output;
}

std::shared_ptr<proto::PeerReply> Wallet::PeerReply(
    const Identifier& nym,
    const Identifier& reply,
    const StorageBox& box) const
{
    const std::string nymID = String(nym).Get();
    std::lock_guard<std::mutex> lock(peer_lock(nymID));
    std::shared_ptr<proto::PeerReply> output;

    App::Me().DB().Load(
        nymID,
        String(reply).Get(),
        box,
        output);

    return output;
}

bool Wallet::PeerReplyComplete(
    const Identifier& nym,
    const Identifier& replyID) const
{
    const std::string nymID = String(nym).Get();
    std::lock_guard<std::mutex> lock(peer_lock(nymID));
    std::shared_ptr<proto::PeerReply> reply;
    const bool haveReply =
        App::Me().DB().Load(
            nymID,
            String(replyID).Get(),
            StorageBox::SENTPEERREPLY,
            reply,
            false);

    if (!haveReply) {
        otErr << __FUNCTION__ << ": sent reply not found."
              << std::endl;

        return false;
    }

    // This reply may have been loaded by request id.
    const auto& realReplyID = reply->id();

    const bool savedReply =
        App::Me().DB().Store(*reply, nymID, StorageBox::FINISHEDPEERREPLY);

    if (!savedReply) {
        otErr << __FUNCTION__ << ": failed to save finished reply."
              << std::endl;

        return false;
    }

    const bool removedReply = App::Me().DB().RemoveNymBoxItem(
        nymID,
        StorageBox::SENTPEERREPLY,
        realReplyID);

    if (!removedReply) {
        otErr << __FUNCTION__ << ": failed to delete finished reply from sent "
              << "box." << std::endl;
    }

    return removedReply;
}

bool Wallet::PeerReplyCreate(
    const Identifier& nym,
    const proto::PeerRequest& request,
    const proto::PeerReply& reply) const
{
    const std::string nymID = String(nym).Get();
    std::lock_guard<std::mutex> lock(peer_lock(nymID));

    if (reply.cookie() != request.id()) {
        otErr << __FUNCTION__ << ": reply cookie does not match request id."
              << std::endl;

        return false;
    }

    if (reply.type() != request.type()) {
        otErr << __FUNCTION__ << ": reply type does not match request type."
              << std::endl;

        return false;
    }

    const bool createdReply = App::Me().DB().Store(
        reply, nymID, StorageBox::SENTPEERREPLY);

    if (!createdReply) {
        otErr << __FUNCTION__ << ": failed to save sent reply."
              << std::endl;

        return false;
    }

    const bool processedRequest = App::Me().DB().Store(
        request, nymID, StorageBox::PROCESSEDPEERREQUEST);

    if (!processedRequest) {
        otErr << __FUNCTION__ << ": failed to save processed request."
              << std::endl;

        return false;
    }

    const bool movedRequest = App::Me().DB().RemoveNymBoxItem(
        nymID, StorageBox::INCOMINGPEERREQUEST, request.id());

    if (!processedRequest) {
        otErr << __FUNCTION__ << ": failed to delete processed request from "
              << "incoming box." << std::endl;
    }

    return movedRequest;
}

bool Wallet::PeerReplyCreateRollback(
    const Identifier& nym,
    const Identifier& request,
    const Identifier& reply) const
{
    const std::string nymID = String(nym).Get();
    std::lock_guard<std::mutex> lock(peer_lock(nymID));
    const std::string requestID = String(request).Get();
    const std::string replyID = String(reply).Get();
    std::shared_ptr<proto::PeerRequest> requestItem;
    bool output = true;
    const bool loadedRequest = App::Me().DB().Load(
        nymID, requestID, StorageBox::PROCESSEDPEERREQUEST, requestItem);

    if (loadedRequest) {
        const bool requestRolledBack = App::Me().DB().Store(
            *requestItem, nymID, StorageBox::INCOMINGPEERREQUEST);

        if (requestRolledBack) {
            const bool purgedRequest = App::Me().DB().RemoveNymBoxItem(
                nymID, StorageBox::PROCESSEDPEERREQUEST, requestID);
            if (!purgedRequest) {
                otErr << __FUNCTION__ << ": Failed to delete request from"
                      << "processed box." << std::endl;
                output = false;
            }
        } else {
            otErr << __FUNCTION__ << ": Failed to save request to"
                  << "incoming box." << std::endl;
            output = false;
        }
    } else {
        otErr << __FUNCTION__ << ": Did not find the request in the "
              << "processed box." << std::endl;
        output = false;
    }

    const bool removedReply = App::Me().DB().RemoveNymBoxItem(
        nymID, StorageBox::SENTPEERREPLY, replyID);

    if (!removedReply) {
        otErr << __FUNCTION__ << ": Failed to delete reply from"
              << "send box." << std::endl;
        output = false;
    }

    return output;
}

ObjectList Wallet::PeerReplySent(const Identifier& nym) const
{
    const std::string nymID = String(nym).Get();
    std::lock_guard<std::mutex> lock(peer_lock(nymID));

    return App::Me().DB().NymBoxList(nymID, StorageBox::SENTPEERREPLY);
}

ObjectList Wallet::PeerReplyIncoming(const Identifier& nym) const
{
    const std::string nymID = String(nym).Get();
    std::lock_guard<std::mutex> lock(peer_lock(nymID));

    return App::Me().DB().NymBoxList(nymID, StorageBox::INCOMINGPEERREPLY);
}

ObjectList Wallet::PeerReplyFinished(const Identifier& nym) const
{
    const std::string nymID = String(nym).Get();
    std::lock_guard<std::mutex> lock(peer_lock(nymID));

    return App::Me().DB().NymBoxList(nymID, StorageBox::FINISHEDPEERREPLY);
}

ObjectList Wallet::PeerReplyProcessed(const Identifier& nym) const
{
    const std::string nymID = String(nym).Get();
    std::lock_guard<std::mutex> lock(peer_lock(nymID));

    return App::Me().DB().NymBoxList(nymID, StorageBox::PROCESSEDPEERREPLY);
}

bool Wallet::PeerReplyReceive(
    const Identifier& nym,
    const PeerObject& reply) const
{
    if (proto::PEEROBJECT_RESPONSE != reply.Type()) {
        otErr << __FUNCTION__ << ": this is not a peer reply." << std::endl;

        return false;
    }

    if (!reply.Request()) {
        otErr << __FUNCTION__ << ": Null request." << std::endl;

        return false;
    }

    if (!reply.Reply()) {
        otErr << __FUNCTION__ << ": Null reply." << std::endl;

        return false;
    }

    const std::string nymID = String(nym).Get();
    std::lock_guard<std::mutex> lock(peer_lock(nymID));
    auto requestID = reply.Request()->ID();

    std::shared_ptr<proto::PeerRequest> request;
    const bool haveRequest =
        App::Me().DB().Load(
            nymID,
            String(requestID).Get(),
            StorageBox::SENTPEERREQUEST,
            request,
            false);

    if (!haveRequest) {
        otErr << __FUNCTION__ << ": the request for this reply does not exist "
              << "in the sent box." << std::endl;

        return false;
    }

    const bool receivedReply = App::Me().DB().Store(
        reply.Reply()->Contract(), nymID, StorageBox::INCOMINGPEERREPLY);

    if (!receivedReply) {
        otErr << __FUNCTION__ << ": failed to save incoming reply."
              << std::endl;

        return false;
    }

    const bool finishedRequest = App::Me().DB().Store(
        *request, nymID, StorageBox::FINISHEDPEERREQUEST);

    if (!finishedRequest) {
        otErr << __FUNCTION__ << ": failed to save request to finished box."
              << std::endl;

        return false;
    }

    const bool removedRequest = App::Me().DB().RemoveNymBoxItem(
        nymID, StorageBox::SENTPEERREQUEST, String(requestID).Get());

    if (!finishedRequest) {
        otErr << __FUNCTION__ << ": failed to delete finished request from "
              << "sent box." << std::endl;
    }

    return removedRequest;
}

std::shared_ptr<proto::PeerRequest> Wallet::PeerRequest(
    const Identifier& nym,
    const Identifier& request,
    const StorageBox& box) const
{
    const std::string nymID = String(nym).Get();
    std::lock_guard<std::mutex> lock(peer_lock(nymID));
    std::shared_ptr<proto::PeerRequest> output;

    App::Me().DB().Load(
        nymID,
        String(request).Get(),
        box,
        output);

    return output;
}

bool Wallet::PeerRequestComplete(
    const Identifier& nym,
    const Identifier& replyID) const
{
    const std::string nymID = String(nym).Get();
    std::lock_guard<std::mutex> lock(peer_lock(nymID));
    std::shared_ptr<proto::PeerReply> reply;
    const bool haveReply =
        App::Me().DB().Load(
            nymID,
            String(replyID).Get(),
            StorageBox::INCOMINGPEERREPLY,
            reply,
            false);

    if (!haveReply) {
        otErr << __FUNCTION__ << ": the request does not exist in the incoming "
              << "box." << std::endl;

        return false;
    }

    // This reply may have been loaded by request id.
    const auto& realReplyID = reply->id();

    const bool storedReply =
        App::Me().DB().Store(*reply, nymID, StorageBox::PROCESSEDPEERREPLY);

    if (!storedReply) {
        otErr << __FUNCTION__ << ": failed to save reply to processed box."
              << std::endl;

        return false;
    }

    const bool removedReply = App::Me().DB().RemoveNymBoxItem(
        nymID, StorageBox::INCOMINGPEERREPLY, realReplyID);

    if (!removedReply) {
        otErr << __FUNCTION__ << ": failed to delete completed reply from "
              << "incoming box." << std::endl;
    }

    return removedReply;
}

bool Wallet::PeerRequestCreate(
    const Identifier& nym,
    const proto::PeerRequest& request) const
{
    const std::string nymID = String(nym).Get();
    std::lock_guard<std::mutex> lock(peer_lock(nymID));

    return App::Me().DB().Store(
        request, String(nym).Get(), StorageBox::SENTPEERREQUEST);
}

bool Wallet::PeerRequestCreateRollback(
    const Identifier& nym,
    const Identifier& request) const
{
    const std::string nymID = String(nym).Get();
    std::lock_guard<std::mutex> lock(peer_lock(nymID));

    return App::Me().DB().RemoveNymBoxItem(
        String(nym).Get(), StorageBox::SENTPEERREQUEST, String(request).Get());
}

ObjectList Wallet::PeerRequestSent(const Identifier& nym) const
{
    const std::string nymID = String(nym).Get();
    std::lock_guard<std::mutex> lock(peer_lock(nymID));

    return App::Me().DB().NymBoxList(
        String(nym).Get(), StorageBox::SENTPEERREQUEST);
}

ObjectList Wallet::PeerRequestIncoming(const Identifier& nym) const
{
    const std::string nymID = String(nym).Get();
    std::lock_guard<std::mutex> lock(peer_lock(nymID));

    return App::Me().DB().NymBoxList(
        String(nym).Get(), StorageBox::INCOMINGPEERREQUEST);
}

ObjectList Wallet::PeerRequestFinished(const Identifier& nym) const
{
    const std::string nymID = String(nym).Get();
    std::lock_guard<std::mutex> lock(peer_lock(nymID));

    return App::Me().DB().NymBoxList(
        String(nym).Get(), StorageBox::FINISHEDPEERREQUEST);
}

ObjectList Wallet::PeerRequestProcessed(const Identifier& nym) const
{
    const std::string nymID = String(nym).Get();
    std::lock_guard<std::mutex> lock(peer_lock(nymID));

    return App::Me().DB().NymBoxList(
        String(nym).Get(), StorageBox::PROCESSEDPEERREQUEST);
}

bool Wallet::PeerRequestReceive(
    const Identifier& nym,
    const PeerObject& request) const
{
    if (proto::PEEROBJECT_REQUEST != request.Type()) {
        otErr << __FUNCTION__ << ": this is not a peer request." << std::endl;

        return false;
    }

    if (!request.Request()) {
        otErr << __FUNCTION__ << ": Null request." << std::endl;

        return false;
    }

    const std::string nymID = String(nym).Get();
    std::lock_guard<std::mutex> lock(peer_lock(nymID));

    return App::Me().DB().Store(
        request.Request()->Contract(),
        nymID,
        StorageBox::INCOMINGPEERREQUEST);
}

bool Wallet::RemoveServer(const Identifier& id)
{
    std::string server(String(id).Get());
    std::unique_lock<std::mutex> mapLock(server_map_lock_);
    auto deleted = server_map_.erase(server);

    if (0 != deleted) {
        return App::Me().DB().RemoveServer(server);
    }

    return false;
}

bool Wallet::RemoveUnitDefinition(const Identifier& id)
{
    std::string unit(String(id).Get());
    std::unique_lock<std::mutex> mapLock(unit_map_lock_);
    auto deleted = unit_map_.erase(unit);

    if (0 != deleted) {
        return App::Me().DB().RemoveUnitDefinition(unit);
    }

    return false;
}

ConstServerContract Wallet::Server(
    const Identifier& id,
    const std::chrono::milliseconds& timeout)
{
    const String strID(id);
    const std::string server = strID.Get();
    std::unique_lock<std::mutex> mapLock(server_map_lock_);
    bool inMap = (server_map_.find(server) != server_map_.end());
    bool valid = false;

    if (!inMap) {
        std::shared_ptr<proto::ServerContract> serialized;

        std::string alias;
        bool loaded = App::Me().DB().Load(server, serialized, alias, true);

        if (loaded) {
            auto nym = Nym(Identifier(serialized->nymid()));

            if (!nym && serialized->has_publicnym()) {
                nym = Nym(serialized->publicnym());
            }

            if (nym) {
                auto& pServer = server_map_[server];
                pServer.reset(ServerContract::Factory(nym, *serialized));

                if (pServer) {
                    valid = true;  // Factory() performs validation
                    pServer->Signable::SetAlias(alias);
                }
            }
        } else {
            App::Me().DHT().GetServerContract(server);

            if (timeout > std::chrono::milliseconds(0)) {
                mapLock.unlock();
                auto start = std::chrono::high_resolution_clock::now();
                auto end = start + timeout;
                const auto interval = std::chrono::milliseconds(100);

                while (std::chrono::high_resolution_clock::now() < end) {
                    std::this_thread::sleep_for(interval);
                    mapLock.lock();
                    bool found =
                        (server_map_.find(server) != server_map_.end());
                    mapLock.unlock();

                    if (found) {
                        break;
                    }
                }

                return Server(
                    id);  // timeout of zero prevents infinite recursion
            }
        }
    } else {
        auto& pServer = server_map_[server];
        if (pServer) {
            valid = pServer->Validate();
        }
    }

    if (valid) {
        return server_map_[server];
    }

    return nullptr;
}

ConstServerContract Wallet::Server(
    std::unique_ptr<class ServerContract>& contract)
{
    std::string server = String(contract->ID()).Get();

    if (contract) {
        if (contract->Validate()) {
            if (App::Me().DB().Store(contract->Contract(), contract->Alias())) {
                std::unique_lock<std::mutex> mapLock(server_map_lock_);
                server_map_[server].reset(contract.release());
                mapLock.unlock();
            }
        }
    }

    return Server(Identifier(server));
}

ConstServerContract Wallet::Server(const proto::ServerContract& contract)
{
    std::string server = contract.id();
    auto nym = Nym(Identifier(contract.nymid()));

    if (!nym && contract.has_publicnym()) {
        nym = Nym(contract.publicnym());
    }

    if (nym) {
        std::unique_ptr<ServerContract> candidate(
            ServerContract::Factory(nym, contract));

        if (candidate) {
            if (candidate->Validate()) {
                if (App::Me().DB().Store(
                        candidate->Contract(), candidate->Alias())) {
                    std::unique_lock<std::mutex> mapLock(server_map_lock_);
                    server_map_[server].reset(candidate.release());
                    mapLock.unlock();
                }
            }
        }
    }

    return Server(Identifier(server));
}

ConstServerContract Wallet::Server(
    const std::string& nymid,
    const std::string& name,
    const std::string& terms,
    const std::list<ServerContract::Endpoint>& endpoints)
{
    std::string server;

    auto nym = Nym(Identifier(nymid));

    if (nym) {
        std::unique_ptr<ServerContract> contract;
        contract.reset(ServerContract::Create(nym, endpoints, terms, name));

        if (contract) {

            return (Server(contract));
        } else {
            otErr << __FUNCTION__ << ": Error: failed to create contract."
                  << std::endl;
        }
    } else {
        otErr << __FUNCTION__ << ": Error: nym does not exist." << std::endl;
    }

    return Server(Identifier(server));
}

ObjectList Wallet::ServerList() { return App::Me().DB().ServerList(); }

bool Wallet::SetNymAlias(const Identifier& id, const std::string& alias)
{
    return App::Me().DB().SetNymAlias(String(id).Get(), alias);
}

bool Wallet::SetServerAlias(const Identifier& id, const std::string& alias)
{
    const std::string server = String(id).Get();
    const bool saved = App::Me().DB().SetServerAlias(server, alias);

    if (saved) {
        std::lock_guard<std::mutex> mapLock(server_map_lock_);
        server_map_.erase(server);

        return true;
    }

    return false;
}

bool Wallet::SetUnitDefinitionAlias(
    const Identifier& id,
    const std::string& alias)
{
    const std::string unit = String(id).Get();
    const bool saved = App::Me().DB().SetUnitDefinitionAlias(unit, alias);

    if (saved) {
        std::lock_guard<std::mutex> mapLock(unit_map_lock_);
        unit_map_.erase(unit);

        return true;
    }

    return false;
}

ObjectList Wallet::Threads(const Identifier& nym) const
{
    return App::Me().DB().ThreadList(String(nym).Get());
}

ObjectList Wallet::UnitDefinitionList()
{
    return App::Me().DB().UnitDefinitionList();
}

ConstUnitDefinition Wallet::UnitDefinition(
    const Identifier& id,
    const std::chrono::milliseconds& timeout)
{
    const String strID(id);
    const std::string unit = strID.Get();
    std::unique_lock<std::mutex> mapLock(unit_map_lock_);
    bool inMap = (unit_map_.find(unit) != unit_map_.end());
    bool valid = false;

    if (!inMap) {
        std::shared_ptr<proto::UnitDefinition> serialized;

        std::string alias;
        bool loaded = App::Me().DB().Load(unit, serialized, alias, true);

        if (loaded) {
            auto nym = Nym(Identifier(serialized->nymid()));

            if (!nym && serialized->has_publicnym()) {
                nym = Nym(serialized->publicnym());
            }

            if (nym) {
                auto& pUnit = unit_map_[unit];
                pUnit.reset(UnitDefinition::Factory(nym, *serialized));

                if (pUnit) {
                    valid = true;  // Factory() performs validation
                    pUnit->Signable::SetAlias(alias);
                }
            }
        } else {
            App::Me().DHT().GetUnitDefinition(unit);

            if (timeout > std::chrono::milliseconds(0)) {
                mapLock.unlock();
                auto start = std::chrono::high_resolution_clock::now();
                auto end = start + timeout;
                const auto interval = std::chrono::milliseconds(100);

                while (std::chrono::high_resolution_clock::now() < end) {
                    std::this_thread::sleep_for(interval);
                    mapLock.lock();
                    bool found = (unit_map_.find(unit) != unit_map_.end());
                    mapLock.unlock();

                    if (found) {
                        break;
                    }
                }

                return UnitDefinition(id);  // timeout of zero prevents
                                            // infinite recursion
            }
        }
    } else {
        auto& pUnit = unit_map_[unit];
        if (pUnit) {
            valid = pUnit->Validate();
        }
    }

    if (valid) {
        return unit_map_[unit];
    }

    return nullptr;
}

ConstUnitDefinition Wallet::UnitDefinition(
    std::unique_ptr<class UnitDefinition>& contract)
{
    std::string unit = String(contract->ID()).Get();

    if (contract) {
        if (contract->Validate()) {
            if (App::Me().DB().Store(contract->Contract(), contract->Alias())) {
                std::unique_lock<std::mutex> mapLock(unit_map_lock_);
                unit_map_[unit].reset(contract.release());
                mapLock.unlock();
            }
        }
    }

    return UnitDefinition(Identifier(unit));
}

ConstUnitDefinition Wallet::UnitDefinition(
    const proto::UnitDefinition& contract)
{
    std::string unit = contract.id();
    auto nym = Nym(Identifier(contract.nymid()));

    if (!nym && contract.has_publicnym()) {
        nym = Nym(contract.publicnym());
    }

    if (nym) {
        std::unique_ptr<class UnitDefinition> candidate(
            UnitDefinition::Factory(nym, contract));

        if (candidate) {
            if (candidate->Validate()) {
                if (App::Me().DB().Store(
                        candidate->Contract(), candidate->Alias())) {
                    std::unique_lock<std::mutex> mapLock(unit_map_lock_);
                    unit_map_[unit].reset(candidate.release());
                    mapLock.unlock();
                }
            }
        }
    }

    return UnitDefinition(Identifier(unit));
}

ConstUnitDefinition Wallet::UnitDefinition(
    const std::string& nymid,
    const std::string& shortname,
    const std::string& name,
    const std::string& symbol,
    const std::string& terms,
    const std::string& tla,
    const uint32_t& power,
    const std::string& fraction)
{
    std::string unit;

    auto nym = Nym(Identifier(nymid));

    if (nym) {
        std::unique_ptr<class UnitDefinition> contract;
        contract.reset(
            UnitDefinition::Create(
                nym, shortname, name, symbol, terms, tla, power, fraction));
        if (contract) {

            return (UnitDefinition(contract));
        } else {
            otErr << __FUNCTION__ << ": Error: failed to create contract."
                  << std::endl;
        }
    } else {
        otErr << __FUNCTION__ << ": Error: nym does not exist." << std::endl;
    }

    return UnitDefinition(Identifier(unit));
}

ConstUnitDefinition Wallet::UnitDefinition(
    const std::string& nymid,
    const std::string& shortname,
    const std::string& name,
    const std::string& symbol,
    const std::string& terms)
{
    std::string unit;

    auto nym = Nym(Identifier(nymid));

    if (nym) {
        std::unique_ptr<class UnitDefinition> contract;
        contract.reset(
            UnitDefinition::Create(nym, shortname, name, symbol, terms));
        if (contract) {

            return (UnitDefinition(contract));
        } else {
            otErr << __FUNCTION__ << ": Error: failed to create contract."
                  << std::endl;
        }
    } else {
        otErr << __FUNCTION__ << ": Error: nym does not exist." << std::endl;
    }

    return UnitDefinition(Identifier(unit));
}

}  // namespace opentxs
