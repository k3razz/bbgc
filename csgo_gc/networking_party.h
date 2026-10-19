#pragma once

#include <cstdint>
#include <chrono>
#include <deque>
#include <string>
#include "networking_shared.h"

enum EPartyInternalMsg : uint32_t
{
    k_EMsgPartyInviteRemote              = 0x60000001,
    k_EMsgPartyJoinRelayRemote           = 0x60000002,
    k_EMsgPartyLobbyUpdateRemote         = 0x60000003,
    k_EMsgPartyHeartbeatRemote           = 0x60000004,
    k_EMsgPartyChatRemote                = 0x60000005,
    k_EMsgPartyProfileQueryRemote        = 0x60000006,
    k_EMsgPartyProfileReplyRemote        = 0x60000007,
    k_EMsgPartyKickRemote                = 0x60000008,
    k_EMsgPartyMatchmakingRefusedRemote  = 0x60000009,   // NEW
};


struct PartyMatchmakingRefusedPayload
{
    uint32_t reason;
    uint32_t bannedAccountId;   // account id of the VAC'd member
};

struct PartyInvitePayload
{
    uint32_t lobbyId;
    uint32_t gameType;
    uint32_t fromAccountId;
};

struct PartyJoinRelayPayload
{
    uint32_t lobbyId;
    uint32_t fromAccountId;
};

struct PartyHeartbeatPayload
{
    uint32_t hostAccountId;
    uint32_t lobbyId;
    uint32_t memberCount;
};

struct PartyChatPayload
{
    uint32_t fromAccountId;
    uint32_t lobbyId;
};

struct PartyProfileQueryPayload
{
    uint32_t fromAccountId;
    uint32_t targetAccountId;
    uint32_t nonce;
};

struct PartyProfileReplyPayload
{
    uint32_t fromAccountId;
    uint32_t targetAccountId;
    uint32_t nonce;

    uint32_t playerLevel;
    uint32_t playerCurXp;
    uint32_t cmdFriendly;
    uint32_t cmdTeaching;
    uint32_t cmdLeader;
    uint32_t vacBanned;
    uint32_t rankId;
    uint32_t rankWins;
    uint32_t rankType;
    uint32_t penaltySeconds;
    uint32_t penaltyReason;
};

struct PartyKickPayload
{
    uint32_t hostAccountId;
    uint32_t lobbyId;
    uint32_t targetAccountId;
};

class ClientGC;
class GCMessageWrite;

// Party transport.
//
// All Send* methods enqueue a message and return immediately. The actual
// HTTP POST happens on the poll thread. This keeps the game thread (which
// calls into us via Hk_SteamAPI_RunCallbacks) from ever blocking on the
// network, which is what caused visible stutter when the worker was slow.
class NetworkingParty
{
public:
    NetworkingParty(uint32_t accountId, const std::string &serverUrl);
    ~NetworkingParty();

    void Update(ClientGC *gc);

    void SendInvite(uint32_t targetAccountId, uint32_t fromAccountId,
                    uint32_t lobbyId, uint32_t gameType);
    void SendJoinRelay(uint32_t hostAccountId, uint32_t fromAccountId,
                       uint32_t lobbyId);
    void SendLobbyUpdate(uint32_t targetAccountId, uint32_t lobbyId,
                         const std::vector<uint32_t> &members);
    void SendHeartbeat(uint32_t targetAccountId, uint32_t hostAccountId,
                       uint32_t lobbyId, uint32_t memberCount);
    void SendChat(uint32_t targetAccountId, uint32_t fromAccountId,
                  uint32_t lobbyId, std::string_view text);
    void SendProfileQuery(uint32_t targetAccountId, uint32_t fromAccountId,
                          uint32_t nonce);
    void SendProfileReply(uint32_t targetAccountId,
                          const PartyProfileReplyPayload &p);
    void SendKick(uint32_t targetAccountId, uint32_t hostAccountId,
                  uint32_t lobbyId, uint32_t kickedAccountId);
    void SendMatchmakingRefused(uint32_t targetAccountId,
                                uint32_t reason,
                                uint32_t bannedAccountId);

private:
    void SendRaw(uint32_t targetAccountId, const GCMessageWrite &write);
    void PollWorker();
    void DrainOutgoing();
    void DispatchToGC(ClientGC *gc, uint32_t fromAccountId,
                      const std::vector<uint8_t> &buffer);

    struct OutgoingMessage
    {
        uint32_t targetAccountId = 0;
        std::vector<uint8_t> data;
    };

    const uint32_t m_accountId;
    const std::string m_serverUrl;

    std::thread m_pollThread;
    std::mutex m_pollMutex;
    std::condition_variable m_pollCv;
    bool m_pollStopping{ false };
    ClientGC *m_gcForPoll{ nullptr };

    std::deque<OutgoingMessage> m_outgoing;

    uint32_t m_lastSeq{ UINT32_MAX };
};