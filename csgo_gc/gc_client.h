#pragma once

#include "config.h"
#include "gc_shared.h"
#include "inventory.h"
#include "keyvalue.h"
#include "networking_shared.h"
#include "networking_party.h"
#include "rate_limit.h"

#include <steam/steam_api_common.h>

struct PartyLobby
{
    uint32_t lobbyId{};
    uint32_t hostAccountId{};
    uint32_t gameType{};
    uint32_t ver{};
    uint32_t launcher{};
    std::vector<uint32_t> memberAccountIds;
    bool active{ false };

    uint32_t leaderAccountId{};
    std::chrono::steady_clock::time_point lastHostHeartbeat{};
    std::chrono::steady_clock::time_point lastMemberPing{};
};

constexpr size_t PartyMaxMembers = 16;

// ВАЖНО: объявлен до первого использования
struct PendingProfileQuery
{
    uint32_t targetAccountId{};
    uint32_t nonce{};
    std::chrono::steady_clock::time_point deadline{};
};

// Parsed view of someone else's profile fetched from the worker.
// This is a *transient* result, never stored: every lookup re-fetches
// from the worker so a stale cached rank / VAC flag can never be shown.
struct RemoteProfile
{
    bool     found = false;
    uint32_t level = 0;
    uint32_t xp = 0;
    uint32_t cmdFriendly = 0;
    uint32_t cmdTeaching = 0;
    uint32_t cmdLeader = 0;
    uint32_t rankId = 0;
    uint32_t rankWins = 0;
    uint32_t rankType = 0;
    bool     vacBanned = false;
    uint32_t penaltySeconds = 0;   // 0 = no active cooldown
    uint32_t penaltyReason  = 0;
};

class ClientGC final : public SharedGC
{
public:
    ClientGC(uint64_t steamId);
    ~ClientGC();
    void CheckFileReloads();

    uint64_t GetSteamId() const { return m_steamId; }
    uint32_t GetAccountId() const { return m_steamId & 0xffffffff; }

    // P2P party entry points (called from NetworkingParty)
    void OnRemotePartyInvite(uint32_t fromAccountId, uint32_t lobbyId, uint32_t gameType);
    void OnRemoteJoinRelay(uint32_t fromAccountId, uint32_t lobbyId);
    void OnRemoteLobbyUpdate(uint32_t fromAccountId, uint32_t lobbyId,
        const std::vector<uint32_t> &members);
    void OnRemoteHeartbeat(uint32_t hostAccountId, uint32_t lobbyId);
    void OnRemotePartyChat(const PartyChatPayload &payload, std::string_view text);
    void OnRemoteProfileQuery(const PartyProfileQueryPayload &q);
    void OnRemoteProfileReply(const PartyProfileReplyPayload &reply);
    void OnRemotePartyKick(const PartyKickPayload &payload);
    void OnRemoteMatchmakingRefused(const PartyMatchmakingRefusedPayload &p);

    const PartyLobby &GetPartyLobby() const { return m_partyLobby; }

    const std::unordered_map<uint32_t, uint32_t> &GetPrivacySettings() const
    { return m_privacySettings; }

    void SetParty(NetworkingParty *party) { m_party = party; }
    NetworkingParty *GetParty() const { return m_party; }
    void Shutdown();
    void RunConsoleCommand(std::string_view command);
    void PostNativeLobbyRoster(const std::vector<uint64_t> &memberSteamIds);
    void ClearNativeLobbyRoster();
private:
    // party
    PartyLobby m_partyLobby;
    std::vector<uint32_t> m_nativeLobbyMembers;
    NetworkingParty *m_party{ nullptr };
    PerPlayerRateLimiter m_chatLimiter{ 3.0f, 5.0f };
    PerPlayerRateLimiter m_profileLimiter{ 0.5f, 2.0f };
    std::chrono::steady_clock::time_point m_nextPartyHeartbeat{};
    std::chrono::steady_clock::time_point m_nextLtpRefresh{};
    std::unordered_map<uint32_t, PendingProfileQuery> m_pendingProfileQueries;

    // helpers
    void SendPartyHeartbeat();
    void CheckPartyHostAlive();
    void QueueLtpRefresh();
    void DoLtpRefresh();
    std::string BuildLtpRegisterBody();

    KeyValue m_priceSheet;
    KeyValue m_passes;
    KeyValue m_unusualLootLists;

    void HandleEvent(GCEvent type, uint64_t id, const std::vector<uint8_t> &buffer) override;
    bool m_isSearching{ false };

    void HandleMessage(uint32_t type, const void *data, uint32_t size);
    void HandleNetMessage(const void *data, uint32_t size);
    void HandleSOCacheRequest();

    void SendMessageToGame(bool sendToGameServer, uint32_t type,
        const google::protobuf::MessageLite &message, uint64_t jobId = JobIdInvalid);

    void OnClientHello(GCMessageRead &messageRead);
    void AdjustItemEquippedState(GCMessageRead &messageRead);
    void AdjustItemEquippedStateMulti(GCMessageRead &messageRead);
    void ClientPlayerDecalSign(GCMessageRead &messageRead);
    void UseItemRequest(GCMessageRead &messageRead);
    void ClientRequestJoinServerData(GCMessageRead &messageRead);
    void SetItemPositions(GCMessageRead &messageRead);
    void IncrementKillCountAttribute(GCMessageRead &messageRead);
    void ApplySticker(GCMessageRead &messageRead);
    void StoreGetUserData(GCMessageRead &messageRead);
    void StorePurchaseInit(GCMessageRead &messageRead);
    void StorePurchaseFinalize(GCMessageRead &messageRead);
    void PartySearch(GCMessageRead &messageRead);
    void RequestCoPlays(GCMessageRead &messageRead);
    void ClientRequestPlayersProfile(GCMessageRead &messageRead);
    void CasketItemLoadContents(GCMessageRead &messageRead);
    void CasketItemAdd(GCMessageRead &messageRead);
    void CasketItemExtract(GCMessageRead &messageRead);
    void StatTrakSwap(GCMessageRead &messageRead);
    void OnClient2GCEconPreviewDataBlockRequest(GCMessageRead &messageRead);
    void OnClientToGCRequestTicket(GCMessageRead &messageRead);
    void OnClientVarValueNotification(GCMessageRead &messageRead);
    void OnReportAbuse(GCMessageRead &messageRead);
    void OnClient2GCTextMsg(GCMessageRead &messageRead);
    void OnWatchInfoUsers(GCMessageRead &messageRead);
    void OnClientRequestWatchInfoFriends2(GCMessageRead &messageRead);
    void OnClientRequestOffers(GCMessageRead &messageRead);
    void OnSetEventFavorite(GCMessageRead &messageRead);
    void OnUpdateItemSchema(GCMessageRead &messageRead);
    void OnSortItems(GCMessageRead &messageRead);
    void OnShowItemsPickedUp(GCMessageRead &messageRead);
    void OnClientConnectionStatus(GCMessageRead &messageRead);
    void OnInviteToParty(GCMessageRead &messageRead);
    void OnPartyInviteResponse(GCMessageRead &messageRead);
    void OnKickFromParty(GCMessageRead &messageRead);
    void OnLeaveParty(GCMessageRead &messageRead);
    void OnReplicateConVars(GCMessageRead &messageRead);
    void OnSystemBroadcast(GCMessageRead &messageRead);
    void OnPrivateQueues(GCMessageRead &messageRead);

    void DeleteItem(GCMessageRead &messageRead);
    void UnlockCrate(GCMessageRead &messageRead);
    void NameItem(GCMessageRead &messageRead);
    void NameBaseItem(GCMessageRead &messageRead);
    void RemoveItemName(GCMessageRead &messageRead);

    void OnClientInitSystemResponse(GCMessageRead &messageRead);
    void SendInitSystem();

    void OnAccountPrivacySettings(GCMessageRead &messageRead);
    void OnClientRequestSouvenir(GCMessageRead &messageRead);

    void OnAcknowledgePenalty(GCMessageRead &messageRead);
    void OnSetPlayerLeaderboardSafeName(GCMessageRead &messageRead);
    void OnClientReportPlayer(GCMessageRead &messageRead);
    void OnClientReportServer(GCMessageRead &messageRead);
    void OnClientCommendPlayer(GCMessageRead &messageRead);
    void OnSetMyActivityInfo(GCMessageRead &messageRead);
    void OnGlobalChatSubscribe(GCMessageRead &messageRead);
    void OnGlobalChatUnsubscribe(GCMessageRead &messageRead);
    void OnClientToGCChat(GCMessageRead &messageRead);

    void OnMatchListRequestCurrentLiveGames(GCMessageRead &messageRead);
    void OnMatchListRequestRecentUserGames(GCMessageRead &messageRead);
    void OnMatchListRequestLiveGameForUser(GCMessageRead &messageRead);
    void OnMatchListRequestFullGameInfo(GCMessageRead &messageRead);
    void OnMatchListRequestTournamentGames(GCMessageRead &messageRead);

    void SendEmptyMatchList(uint32_t requestId, uint32_t accountId);

    void BuildMatchmakingHello(CMsgGCCStrike15_v2_MatchmakingGC2ClientHello &message);
    void BuildClientWelcome(CMsgClientWelcome &message, const CMsgCStrike15Welcome &csWelcome,
        const CMsgGCCStrike15_v2_MatchmakingGC2ClientHello &matchmakingHello);
    void SendRankUpdate();
    void OnMatchmakingPing(GCMessageRead &messageRead);
    void OnMatchmakingStart(GCMessageRead &messageRead);
    void OnMatchmakingStop(GCMessageRead &messageRead);
    void SendMatchmakingUpdate();

    const uint64_t m_steamId;
    void ProcessGiftUse(uint64_t giftId);
    Inventory m_inventory;

    uint64_t m_transactionId{};
    std::vector<uint64_t> m_transactionItemIds;

    void SendInventoryUpdate();
    void ReloadInventory();
    void ReloadConfig();
    void ReloadPriceSheet();
    void ReloadPasses();
    void ReloadUnusualLootLists();

    std::unordered_map<uint32_t, uint32_t> m_privacySettings;

    std::string m_leaderboardSafeName;

    void SendMatchmakingHelloUpdate();
    uint32_t AccountId() const { return m_steamId & 0xffffffff; }
    uint32_t EffectiveAccountId() const;

    bool m_isCooldownActive{ false };
    bool m_cooldownExpiredLocally{ false };
    uint32_t m_lastAppliedCooldownSeconds{ 0 };
    std::chrono::steady_clock::time_point m_cooldownEndTime;
    void SendCompetitiveCooldown();
    void UpdateCooldown();
    bool ApplyConfiguredCooldown();
    uint32_t m_lastAppliedCooldownReason{ 0 };
    static uint32_t CooldownRemainingSeconds(std::chrono::steady_clock::time_point endTime);
    static const char *CooldownReasonName(uint32_t reason);
    static bool IsPermanentCooldownReason(uint32_t reason);
    static const char *CooldownErrorLocKey(uint32_t reason, bool self = true);

    static void AddPenaltyAccount(CMsgGCCStrike15_v2_MatchmakingGC2ClientUpdate &update,
                                  uint32_t accountId, uint32_t reason);

    // helpers
    void QueryPartyMemberProfile(uint32_t accountId);

    std::vector<uint32_t> MergeQueueRoster(const std::vector<uint32_t> &requestIds) const;

    bool AnyPartyMemberVacBanned(const std::vector<uint32_t> &accountIds,
                                 uint32_t &bannedAccountId);
    bool AnyPartyMemberOnCooldown(const std::vector<uint32_t> &accountIds,
                                  uint32_t &cooldownAccountId,
                                  uint32_t &cooldownReason,
                                  uint32_t &cooldownSeconds);

    // Synchronously fetch a profile from the worker's store.
    // The result is a one-shot value returned to the caller — it is NEVER
    // stored anywhere, so every call hits the worker for fresh data.
    // Returns false if the worker is unreachable or doesn't know the account.
    bool FetchRemoteProfile(uint32_t accountId, RemoteProfile &out);

    void NotifyPartyOfMatchmakingRefusal(uint32_t reason, uint32_t bannedAccountId,
                                         const std::vector<uint32_t> &recipients);

    void QueueReport(uint32_t targetAccountId, uint32_t reasonFlags);
    void DoReportPublish(uint32_t targetAccountId, uint32_t reasonFlags);

    bool QueryRemoteItem(uint64_t itemId, CEconItemPreviewDataBlock &block);

    void CheckGriefingCooldown();

    void SendOwnProfile();
    void SendUnknownProfile(uint32_t accountId);

    // Fetch from worker, then send the response to the game.
    // No caching: returns true if a profile was published and the reply
    // was sent to the client.
    bool QueryRemoteProfile(uint32_t accountId);

    void OnPartyRegister(GCMessageRead &messageRead);
    void OnPartyUnregister(GCMessageRead &messageRead);
    void OnPartyInvite(GCMessageRead &messageRead);
    void OnClientPartyJoinRelay(GCMessageRead &messageRead);
    void OnClientPartyWarning(GCMessageRead &messageRead);

    void OnClientReportValidation(GCMessageRead &messageRead);
    void OnGetEventFavorites(GCMessageRead &messageRead);
    void OnClientRequestPrestigeCoin(GCMessageRead &messageRead);

    void OnClientCommendPlayerQuery(GCMessageRead &messageRead);
    void SendClientAccountBalance();
    void OnClientPerfReport(GCMessageRead &messageRead);
    void OnGC2ClientRefuseSecureMode(GCMessageRead &messageRead);
    void OnNameItemNotification(GCMessageRead &messageRead);
    void ApplyStrangePart(GCMessageRead &messageRead);
    void ModifyItemAttribute(GCMessageRead &messageRead);
    void ItemAcknowledgedDeprecated(GCMessageRead &messageRead);

    void OnClientRequestJoinFriendData(GCMessageRead &messageRead);
    void OnGotvSyncPacket(GCMessageRead &messageRead);
    void OnStorePurchaseCancel(GCMessageRead &messageRead);
    void OnStorePurchaseQueryTxn(GCMessageRead &messageRead);
    void OnServerBrowserFavoriteServer(GCMessageRead &messageRead);
    void OnServerBrowserBlacklistServer(GCMessageRead &messageRead);
    void OnConVarUpdated(GCMessageRead &messageRead);
    void OnGC2ClientRequestValidation(GCMessageRead &messageRead);
    void OnMatchmakingGC2ClientUpdate(GCMessageRead &messageRead);
    void OnGlobalGameSubscribe(GCMessageRead &messageRead);
    void OnGlobalGameUnsubscribe(GCMessageRead &messageRead);
    void OnGlobalGamePlay(GCMessageRead &messageRead);

    void StartHttpWorker();
    void StopHttpWorker();
    void HttpWorkerThread();
    void QueueProfilePublish();
    void DoProfilePublish();
    void HandlePartyIncoming(const void *data, uint32_t size);
    void HandleNativeLobbyUpdate(const void *data, uint32_t size);

    mutable std::mutex m_pendingProfileMutex;
    std::thread m_httpThread;
    std::mutex m_httpMutex;
    std::condition_variable m_httpCv;
    bool m_httpStopping{ false };
    bool m_httpPublishPending{ false };
    bool m_httpLtpRefreshPending{ false };
    std::string m_httpLtpRefreshBody;

    struct PendingReport
    {
        uint32_t targetAccountId;
        uint32_t reasonFlags;
    };
    std::queue<PendingReport> m_httpReports;
};