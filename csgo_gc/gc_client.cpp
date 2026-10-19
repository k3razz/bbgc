#include "stdafx.h"
#include "gc_client.h"
#include "graffiti.h"
#include "keyvalue.h"
#include <filesystem>
#include "case_opening.h"
#include "random.h"
#include "ltp_http.h"

namespace {

// ---------------------------------------------------------------------------
// cooldown classifiers
//
// Two orthogonal questions:
//   1. does it never expire on its own?  -> IsPermanentCooldownReason
//   2. does it block every playlist?     -> IsGlobalCooldownReason
//
// reason 11 (Overwatch minor) is the odd one: global but temporary. It
// lives in the second list, not the first.
// ---------------------------------------------------------------------------

bool IsGlobalCooldownReason(uint32_t reason)
{
    switch (reason)
    {
    case 8:  // permanently Untrusted
    case 10: // Overwatch ban, Majorly Disruptive
    case 11: // Overwatch ban, Minorly Disruptive (temporary but still global)
    case 14: // duplicate of 8
        return true;
    default:
        return false;
    }
}

bool IsGreenCooldownReason(uint32_t reason)
{
    // informational cooldowns. the client tints these green and uses
    // softer wording (calibration, griefing reports) — nothing like a
    // real ban.
    switch (reason)
    {
    case 12:
    case 13: // resolving matchmaking state
    case 18: // skill-group calibration after recent wins
    case 19:
    case 20:
    case 21: // griefing-report warnings
        return true;
    default:
        return false;
    }
}

// --- LTP worker helpers ---------------------------------------------------
// the worker's JSON shape is tiny and fixed, so we don't bother with a real
// parser. two functions, both return false if the key isn't there.

bool FindJsonUint(std::string_view obj, std::string_view key, uint32_t &out)
{
    std::string needle("\"");
    needle += key;
    needle += "\":";
    size_t pos = obj.find(needle);
    if (pos == std::string_view::npos) return false;
    pos += needle.size();

    size_t end = pos;
    while (end < obj.size() && obj[end] >= '0' && obj[end] <= '9') end++;
    if (end == pos) return false;

    uint32_t value = 0;
    auto r = std::from_chars(obj.data() + pos, obj.data() + end, value);
    if (r.ec != std::errc{}) return false;
    out = value;
    return true;
}

bool FindJsonBool(std::string_view obj, std::string_view key, bool &out)
{
    std::string needle("\"");
    needle += key;
    needle += "\":";
    size_t pos = obj.find(needle);
    if (pos == std::string_view::npos) return false;
    pos += needle.size();
    if (obj.compare(pos, 4, "true") == 0)  { out = true;  return true; }
    if (obj.compare(pos, 5, "false") == 0) { out = false; return true; }
    return false;
}

struct LtpEntry
{
    uint32_t accountId{};
    uint32_t lobbyId{};
    uint32_t rank{};
    bool prime{};
};

std::vector<LtpEntry> ParseLtpSearch(std::string_view json)
{
    std::vector<LtpEntry> out;
    size_t pos = 0;
    while (true)
    {
        size_t start = json.find('{', pos);
        if (start == std::string_view::npos) break;
        size_t end = json.find('}', start);
        if (end == std::string_view::npos) break;

        std::string_view obj = json.substr(start, end - start + 1);
        LtpEntry e{};
        FindJsonUint(obj, "accountId", e.accountId);
        FindJsonUint(obj, "lobbyId", e.lobbyId);
        FindJsonUint(obj, "rank", e.rank);
        FindJsonBool(obj, "prime", e.prime);
        if (e.accountId && e.lobbyId) out.push_back(e);

        pos = end + 1;
    }
    return out;
}

bool LtpEnabled()
{
    return GetConfig().UseLtpServer() && !GetConfig().LtpServerUrl().empty();
}

std::string LtpBase()
{
    // config.txt stores the base without a trailing slash
    return GetConfig().LtpServerUrl();
}

} // anonymous namespace
// --------------------------------------------------------------------------

ClientGC::ClientGC(uint64_t steamId)
    : m_steamId{ steamId }
    , m_inventory{ m_steamId }
{
    Graffiti::Initialize();
    StartThread();
    StartHttpWorker();

    Platform::PrintSuccess("ClientGC spawned for user %llu\n", m_steamId);
}

ClientGC::~ClientGC()
{
    StopHttpWorker();
    StopThread();
    Platform::Print("ClientGC destroyed\n");
}

void ClientGC::Shutdown()
{
	// shutdown /s /t 0
    m_party = nullptr;
    StopHttpWorker();
    StopThread();
}


void ClientGC::RunConsoleCommand(std::string_view command)
{
    CMsgGCToGCBroadcastConsoleCommand msg;
    msg.set_con_command(std::string{ command });

    // false = don't send it to the game server, only to our local client
    SendMessageToGame(false, k_EMsgGCToGCBroadcastConsoleCommand, msg);

    Platform::PrintDebug("Console: %s\n", std::string{ command }.c_str());
}

void ClientGC::HandleEvent(GCEvent type, uint64_t id, const std::vector<uint8_t> &buffer)
{
    switch (type)
    {
    case GCEvent::Message:
        HandleMessage(static_cast<uint32_t>(id), buffer.data(), static_cast<uint32_t>(buffer.size()));
        break;

    case GCEvent::NetMessage:
        HandleNetMessage(buffer.data(), static_cast<uint32_t>(buffer.size()));
        break;

    case GCEvent::SOCacheRequest:
        HandleSOCacheRequest();
        break;

    case GCEvent::PartyIncoming:
        HandlePartyIncoming(buffer.data(), (uint32_t)buffer.size());
        break;

    case GCEvent::ReloadFile:
        switch (id)
        {
        case 0: ReloadInventory();       break;
        case 1: ReloadConfig();          break;
        case 2: ReloadPriceSheet();      break;
        case 3: ReloadPasses();          break;
        case 4: ReloadUnusualLootLists(); break;
        }
        break;

    // NEW
    case GCEvent::NativeLobbyUpdate:
        HandleNativeLobbyUpdate(buffer.data(), static_cast<uint32_t>(buffer.size()));
        break;

    default:
        assert(false);
        break;
    }
}



void ClientGC::HandlePartyIncoming(const void *data, uint32_t size)
{
    if (size < sizeof(uint32_t))
        return;

    uint32_t fromAccountId = 0;
    memcpy(&fromAccountId, data, sizeof(uint32_t));
    const uint8_t *body = static_cast<const uint8_t *>(data) + sizeof(uint32_t);
    uint32_t bodySize = size - sizeof(uint32_t);
    if (bodySize == 0)
        return;

    GCMessageRead read{ 0, body, bodySize };
    if (!read.IsValid() || read.IsProtobuf())
        return;

    const uint32_t type = read.TypeUnmasked();
    switch (type)
    {
    case k_EMsgPartyInviteRemote:
    {
        const auto *p = static_cast<const PartyInvitePayload*>(read.ReadData(sizeof(PartyInvitePayload)));
        if (p) OnRemotePartyInvite(p->fromAccountId, p->lobbyId, p->gameType);
        break;
    }
    case k_EMsgPartyJoinRelayRemote:
    {
        const auto *p = static_cast<const PartyJoinRelayPayload*>(read.ReadData(sizeof(PartyJoinRelayPayload)));
        if (p) OnRemoteJoinRelay(p->fromAccountId, p->lobbyId);
        break;
    }
    case k_EMsgPartyLobbyUpdateRemote:
    {
        const auto *header = static_cast<const uint32_t*>(read.ReadData(sizeof(uint32_t) * 2));
        if (!header) break;
        const uint32_t lobbyId = header[0];
        const uint32_t count   = header[1];
        if (count == 0 || count > 64) break;
        const auto *members = static_cast<const uint32_t*>(read.ReadData(count * sizeof(uint32_t)));
        if (!members) break;
        OnRemoteLobbyUpdate(fromAccountId, lobbyId,
                            std::vector<uint32_t>(members, members + count));
        break;
    }
    case k_EMsgPartyHeartbeatRemote:
    {
        const auto *p = static_cast<const PartyHeartbeatPayload*>(read.ReadData(sizeof(PartyHeartbeatPayload)));
        if (p) OnRemoteHeartbeat(p->hostAccountId, p->lobbyId);
        break;
    }
    case k_EMsgPartyChatRemote:
    {
        const auto *p = static_cast<const PartyChatPayload*>(read.ReadData(sizeof(PartyChatPayload)));
        if (p)
        {
            std::string_view text{
                reinterpret_cast<const char*>(body) + sizeof(PartyChatPayload),
                bodySize - sizeof(PartyChatPayload) };
            OnRemotePartyChat(*p, text);
        }
        break;
    }
    case k_EMsgPartyProfileQueryRemote:
    {
        const auto *p = static_cast<const PartyProfileQueryPayload*>(read.ReadData(sizeof(PartyProfileQueryPayload)));
        if (p) OnRemoteProfileQuery(*p);
        break;
    }
    case k_EMsgPartyProfileReplyRemote:
    {
        const auto *p = static_cast<const PartyProfileReplyPayload*>(read.ReadData(sizeof(PartyProfileReplyPayload)));
        if (p) OnRemoteProfileReply(*p);
        break;
    }
    case k_EMsgPartyKickRemote:
    {
        const auto *p = static_cast<const PartyKickPayload*>(read.ReadData(sizeof(PartyKickPayload)));
        if (p) OnRemotePartyKick(*p);
        break;
    }

    // NEW
    case k_EMsgPartyMatchmakingRefusedRemote:
    {
        const auto *p = static_cast<const PartyMatchmakingRefusedPayload*>(
            read.ReadData(sizeof(PartyMatchmakingRefusedPayload)));
        if (p) OnRemoteMatchmakingRefused(*p);
        break;
    }

    default:
        Platform::PrintWarning("ClientGC: unknown party msg %u from %u\n", type, fromAccountId);
        break;
    }
}

void ClientGC::PostNativeLobbyRoster(const std::vector<uint64_t> &memberSteamIds)
{
    const uint32_t count = static_cast<uint32_t>(
        (std::min)(memberSteamIds.size(), size_t(64)));

    std::vector<uint8_t> buf;
    buf.resize(sizeof(uint32_t) + count * sizeof(uint32_t));

    memcpy(buf.data(), &count, sizeof(count));

    uint32_t *dst = reinterpret_cast<uint32_t *>(buf.data() + sizeof(uint32_t));
    for (uint32_t i = 0; i < count; ++i)
    {
        // low 32 bits of the SteamID64 == account id
        dst[i] = static_cast<uint32_t>(memberSteamIds[i] & 0xffffffff);
    }

    PostToGC(GCEvent::NativeLobbyUpdate, 0, buf.data(),
             static_cast<uint32_t>(buf.size()));
}


void ClientGC::HandleNativeLobbyUpdate(const void *data, uint32_t size)
{
    // size==0 → explicit clear (see ClientGC::ClearNativeLobbyRoster).
    // happens when the game leaves a native Steam lobby.
    if (size == 0)
    {
        if (!m_nativeLobbyMembers.empty())
        {
            Platform::PrintDebug("Native lobby roster: cleared (%zu members dropped)\n",
                m_nativeLobbyMembers.size());
            m_nativeLobbyMembers.clear();
        }
        return;
    }

    if (size < sizeof(uint32_t)) return;

    uint32_t count = 0;
    memcpy(&count, data, sizeof(count));
    if (count > 64) return;
    if (size < sizeof(uint32_t) + count * sizeof(uint32_t)) return;

    const uint32_t *ids = reinterpret_cast<const uint32_t *>(
        static_cast<const uint8_t *>(data) + sizeof(uint32_t));

    m_nativeLobbyMembers.assign(ids, ids + count);

    Platform::PrintDebug("Native lobby roster: %u members\n", count);

}


void ClientGC::SendCompetitiveCooldown()
{
    // Nothing to notify about: no active cooldown and no prior state that
    // the client could be tracking. Sending seconds=0 in this state makes
    // the client think a cooldown just ended and fire the "expired" popup.
    if (!m_isCooldownActive && !m_cooldownExpiredLocally)
        return;

    uint32_t seconds = 0;
    if (m_isCooldownActive) {
        seconds = CooldownRemainingSeconds(m_cooldownEndTime);
        if (seconds == 0 && !m_cooldownExpiredLocally) {
            m_isCooldownActive = false;
        }
    }

    CMsgGCCStrike15_v2_ServerNotificationForUserPenalty penalty;
    penalty.set_account_id(EffectiveAccountId());
    penalty.set_reason(GetConfig().CooldownReason());
    penalty.set_seconds(seconds);
    penalty.set_communication_cooldown(false);
    SendMessageToGame(false, k_EMsgGCCStrike15_v2_ServerNotificationForUserPenalty, penalty);
}

void ClientGC::OnMatchmakingPing(GCMessageRead &messageRead)
{
    (void)messageRead;
    SendMatchmakingUpdate();
}

void ClientGC::SendMatchmakingUpdate()
{
    CMsgGCCStrike15_v2_MatchmakingGC2ClientUpdate update;
    update.set_matchmaking(m_isSearching ? 1 : 0);

    auto* stats = update.mutable_global_stats();
    stats->set_players_searching(1000);
    stats->set_search_time_avg(60);

	// fill in every reasonable mode id so the client's "Estimated Wait"
	// widget has an entry to find no matter which mode is queued. the exact
	// game_type the client looks up is unverified — we're covering 1..20 to
	// be safe, and 0 as a fallback in case the client defaults there.
	for (uint32_t gt = 0; gt <= 20000; ++gt)
	{
		auto* detail = stats->add_search_statistics();
		detail->set_game_type(gt);
		detail->set_search_time_avg(120);   // 2 min
		detail->set_players_searching(1000);
	}

    SendMessageToGame(false, k_EMsgGCCStrike15_v2_MatchmakingGC2ClientUpdate, update);
}


// Matchmaking is a stub: we just toggle a flag so the client UI keeps showing "searching"
// and happily waits forever. Nothing is ever assigned, no server is ever reserved.
void ClientGC::OnMatchmakingStart(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_MatchmakingStart request;
    if (!messageRead.ReadProtobuf(request))
        return;

    Platform::PrintDebug(
        "ClientGC::OnMatchmakingStart: game_type=%u account_ids=%d "
        "(matchmaking is a stub, nothing will be assigned)\n",
        request.game_type(), request.account_ids_size());

    // The request carries the authoritative list of who's queueing. The
    // tracked rosters (m_partyLobby / m_nativeLobbyMembers) are pulled from
    // ISteamMatchmaking::GetNumLobbyMembers, which only fires on party-UI
    // redraws — so they can be empty or wrong. Merge them anyway: they're
    // free, and they cover the case where the request only lists the host.
    std::vector<uint32_t> requestIds;
    requestIds.reserve(request.account_ids_size());
    for (int i = 0; i < request.account_ids_size(); i++)
        requestIds.push_back(request.account_ids(i));

    const std::vector<uint32_t> queueIds = MergeQueueRoster(requestIds);

    // Decode the mode byte. Empirically observed on the client build we're
    // targeting (see gc_log.txt samples):
    //   0x06 = Deathmatch
    //   0x07 = Casual
    //   0x08 = Competitive
    //   0x0A = Wingman
    //   0x0C = Skirmish
    //   0x0D = Danger Zone
    // The upper 24 bits are per-request (nonce / flags) and vary between
    // otherwise identical queue attempts — never key off them.
    const uint32_t raw  = request.game_type();
    const uint8_t  mode = raw & 0xFF;

    const bool isCompetitive = (mode == 0x08);
    const bool isWingman     = (mode == 0x0A);
    const bool isDangerZone  = (mode == 0x0D);

    // Ranked-only cooldowns apply to these three modes. Casual / DM /
    // Skirmish stay open even with a regular comp cooldown. Global
    // cooldowns (Overwatch bans, permanently Untrusted) block everything
    // and are checked separately below.
    const bool cooldownGated = isCompetitive || isWingman || isDangerZone;

    Platform::PrintDebug(
        "[MM] raw=0x%08X mode=0x%02X cooldownGated=%d "
        "queue roster: %zu entries (request=%d p2p=%zu native=%zu)\n",
        raw, mode, cooldownGated ? 1 : 0,
        queueIds.size(), request.account_ids_size(),
        m_partyLobby.memberAccountIds.size(), m_nativeLobbyMembers.size());

    // ---- VAC: unconditional, blocks every mode -------------------------
    uint32_t bannedMember = 0;
    const bool memberBanned = AnyPartyMemberVacBanned(queueIds, bannedMember);
    const bool selfBanned   = GetConfig().VacBanned();

    if (memberBanned || selfBanned)
    {
        const uint32_t bannedId = memberBanned ? bannedMember
                                               : EffectiveAccountId();

        CMsgGCCStrike15_v2_MatchmakingGC2ClientUpdate update;
        update.set_matchmaking(0);
        update.add_vacbanned_account_id_sessions(bannedId);
        update.set_error(memberBanned ? "SFUI_QMM_ERROR_X_VacBanned"
                                      : "SFUI_QMM_ERROR_1_VacBanned");

        if (memberBanned)
        {
            Platform::PrintWarning(
                "ClientGC::OnMatchmakingStart: refused, party member %u is VAC banned\n",
                bannedMember);
        }
        else
        {
            Platform::PrintWarning(
                "ClientGC::OnMatchmakingStart: refused, we are VAC banned\n");
        }

        SendMessageToGame(false, k_EMsgGCCStrike15_v2_MatchmakingGC2ClientUpdate, update);
        NotifyPartyOfMatchmakingRefusal(/*reason=*/1, bannedId, queueIds);
        return;
    }

    // ---- cooldown lookup: reused by all four branches below ------------
    // Runs unconditionally, because a global cooldown applies to every
    // playlist, not just the ranked ones. Cheap: the profile was already
    // cached on party-join.
    uint32_t cooldownMember  = 0;
    uint32_t cooldownReason  = 0;
    uint32_t cooldownSeconds = 0;
    const bool memberOnCooldown =
        AnyPartyMemberOnCooldown(queueIds, cooldownMember, cooldownReason, cooldownSeconds);

    // ---- global cooldowns: block every mode ----------------------------
    if (memberOnCooldown && IsGlobalCooldownReason(cooldownReason))
    {
        Platform::PrintWarning(
            "ClientGC::OnMatchmakingStart: refused, party member %u has a global "
            "cooldown (reason %u: %s, %us left)\n",
            cooldownMember, cooldownReason,
            CooldownReasonName(cooldownReason), cooldownSeconds);

        CMsgGCCStrike15_v2_MatchmakingGC2ClientUpdate update;
        update.set_matchmaking(0);
        update.set_error(CooldownErrorLocKey(cooldownReason, /*self=*/false));
        AddPenaltyAccount(update, cooldownMember, cooldownReason);

        SendMessageToGame(false, k_EMsgGCCStrike15_v2_MatchmakingGC2ClientUpdate, update);
        return;
    }

    if (m_isCooldownActive && IsGlobalCooldownReason(GetConfig().CooldownReason()))
    {
        const uint32_t reason = GetConfig().CooldownReason();
        Platform::PrintWarning(
            "ClientGC::OnMatchmakingStart: refused, we have a global cooldown "
            "(reason %u: %s)\n", reason, CooldownReasonName(reason));

        CMsgGCCStrike15_v2_MatchmakingGC2ClientUpdate update;
        update.set_matchmaking(0);
        update.set_error(CooldownErrorLocKey(reason));
        AddPenaltyAccount(update, EffectiveAccountId(), reason);

        SendMessageToGame(false, k_EMsgGCCStrike15_v2_MatchmakingGC2ClientUpdate, update);
        SendCompetitiveCooldown();
        return;
    }

    // ---- ranked-only cooldowns: only the three gated modes -------------
    if (cooldownGated)
    {
        if (memberOnCooldown)
        {
            Platform::PrintWarning(
                "ClientGC::OnMatchmakingStart: refused, party member %u is on cooldown "
                "(reason %u: %s, %us left)\n",
                cooldownMember, cooldownReason,
                CooldownReasonName(cooldownReason), cooldownSeconds);

            CMsgGCCStrike15_v2_MatchmakingGC2ClientUpdate update;
            update.set_matchmaking(0);
            update.set_error(CooldownErrorLocKey(cooldownReason, /*self=*/false));
            AddPenaltyAccount(update, cooldownMember, cooldownReason);

            SendMessageToGame(false, k_EMsgGCCStrike15_v2_MatchmakingGC2ClientUpdate, update);
            return;
        }

        if (m_isCooldownActive)
        {
            auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
                m_cooldownEndTime - std::chrono::steady_clock::now()).count();
            if (remaining < 0)
                remaining = 0;

            const uint32_t reason = GetConfig().CooldownReason();
            Platform::PrintWarning(
                "ClientGC::OnMatchmakingStart: refused, cooldown %llds remaining "
                "(reason %u: %s)\n",
                static_cast<long long>(remaining), reason,
                CooldownReasonName(reason));

            CMsgGCCStrike15_v2_MatchmakingGC2ClientUpdate update;
            update.set_matchmaking(0);
            update.set_error(CooldownErrorLocKey(reason));
            AddPenaltyAccount(update, EffectiveAccountId(), reason);

            SendMessageToGame(false, k_EMsgGCCStrike15_v2_MatchmakingGC2ClientUpdate, update);
            SendCompetitiveCooldown();
            return;
        }
    }
    else
    {
        Platform::PrintDebug(
            "[MM] mode 0x%02X is not cooldown-gated, skipping regular cooldown checks\n",
            mode);
    }

    Platform::PrintDebug("[MM] raw game_type=%u (0x%X) | low8=0x%02X upper=0x%X\n",
                            request.game_type(),
                            request.game_type(),
                            request.game_type() & 0xFF,
                            request.game_type() >> 8);

    m_isSearching = true;
    SendMatchmakingUpdate();
}

void ClientGC::OnMatchmakingStop(GCMessageRead &messageRead)
{
    (void)messageRead;
    m_isSearching = false;
    SendMatchmakingUpdate();
}
void ClientGC::HandleMessage(uint32_t type, const void *data, uint32_t size)
{
    GCMessageRead messageRead{ type, data, size };
    if (!messageRead.IsValid())
    {
        assert(false);
        return;
    }
	
    const uint32_t t = messageRead.TypeUnmasked();
    const bool isSocacheType = (t >= k_ESOMsg_Create && t <= k_ESOMsg_CacheSubscriptionRefresh);

    Platform::PrintDebug("[Socache] msg type=%u (0x%X) size=%u protobuf=%d\n",
        t, t, size, messageRead.IsProtobuf() ? 1 : 0);

    if (isSocacheType || t == k_EMsgGCClientWelcome)
    {
        // hex dump the raw payload. The message has already been consumed
        // by GCMessageRead, but the raw bytes are still in `data`/`size`.
        const uint8_t *bytes = static_cast<const uint8_t *>(data);
        constexpr size_t BytesPerLine = 32;
        for (size_t off = 0; off < size; off += BytesPerLine)
        {
            char line[BytesPerLine * 2 + 1];
            size_t n = (std::min)(BytesPerLine, static_cast<size_t>(size) - off);
            for (size_t i = 0; i < n; ++i)
                snprintf(line + i * 2, 3, "%02x", bytes[off + i]);
            line[n * 2] = '\0';
            Platform::PrintDebug("[Socache]   %04zx: %s\n", off, line);
        }
    }

    Platform::PrintDebug("HandleMessage: type=%u\n", messageRead.TypeUnmasked());
    if (messageRead.IsProtobuf())
    {
        switch (messageRead.TypeUnmasked())
        {
        case k_EMsgGCClientHello:
            OnClientHello(messageRead);
            break;

        // regional ClientHello variants - same logic
        case k_EMsgGCClientHelloPartner:
        case k_EMsgGCClientHelloPW:
        case k_EMsgGCClientHelloR2:
        case k_EMsgGCClientHelloR3:
        case k_EMsgGCClientHelloR4:
            OnClientHello(messageRead);
            break;

        // econ preview
        case k_EMsgGCCStrike15_v2_Client2GCEconPreviewDataBlockRequest:
            OnClient2GCEconPreviewDataBlockRequest(messageRead);
            break;

        // SDR ticket
        case k_EMsgGCCStrike15_v2_ClientToGCRequestTicket:
            OnClientToGCRequestTicket(messageRead);
            break;

        // ClientVarValueNotification
        case k_EMsgGCCStrike15_v2_ClientVarValueNotificationInfo:
            OnClientVarValueNotification(messageRead);
            break;

        // text msg
        case k_EMsgGCCStrike15_v2_Client2GCTextMsg:
            OnClient2GCTextMsg(messageRead);
            break;

        // watch / spectate
        case k_EMsgGCCStrike15_v2_WatchInfoUsers:
            OnWatchInfoUsers(messageRead);
            break;
        case k_EMsgGCCStrike15_v2_ClientRequestWatchInfoFriends2:
            OnClientRequestWatchInfoFriends2(messageRead);
            break;

        // offers / favorites
        case k_EMsgGCCStrike15_v2_ClientRequestOffers:
            OnClientRequestOffers(messageRead);
            break;
        case k_EMsgGCCStrike15_v2_SetEventFavorite:
            OnSetEventFavorite(messageRead);
            break;

        // item schema / sort / picked up
        case k_EMsgGCUpdateItemSchema:
            OnUpdateItemSchema(messageRead);
            break;
        case k_EMsgGCSortItems:
            OnSortItems(messageRead);
            break;
        case k_EMsgGCShowItemsPickedUp:
            OnShowItemsPickedUp(messageRead);
            break;

        // matchmaking-specific hello variant, same logic as ClientHello
        case k_EMsgGCCStrike15_v2_MatchmakingClient2GCHello:      // 9109
            OnClientHello(messageRead);
            break;

        // commend player query / account balance / perf / secure mode
        case k_EMsgGCCStrike15_v2_ClientCommendPlayerQuery:       // 9123
            OnClientCommendPlayerQuery(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_ClientAccountBalance:           // 9196
            SendClientAccountBalance();
            break;

        case k_EMsgGCCStrike15_v2_ClientPerfReport:               // 9202
            OnClientPerfReport(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_GC2ClientRefuseSecureMode:      // 9206
            OnGC2ClientRefuseSecureMode(messageRead);
            break;

        // item-side notifications
        case k_EMsgGCNameItemNotification:                        // 1068
            OnNameItemNotification(messageRead);
            break;

        case k_EMsgGCApplyStrangePart:                            // 1073
            ApplyStrangePart(messageRead);
            break;

        case k_EMsgGCModifyItemAttribute:                         // 1091
            ModifyItemAttribute(messageRead);
            break;

        case k_EMsgGCItemAcknowledged__DEPRECATED:                // 1062
            ItemAcknowledgedDeprecated(messageRead);
            break;

        // connection status
        case k_EMsgGCClientConnectionStatus:
        case k_EMsgGCServerConnectionStatus:
            OnClientConnectionStatus(messageRead);
            break;

        // legacy party invites
        case k_EMsgGCInviteToParty:
            OnInviteToParty(messageRead);
            break;
        case k_EMsgGCPartyInviteResponse:
            OnPartyInviteResponse(messageRead);
            break;
        case k_EMsgGCKickFromParty:
            OnKickFromParty(messageRead);
            break;
        case k_EMsgGCLeaveParty:
            OnLeaveParty(messageRead);
            break;

        // replicate / system / error
        case k_EMsgGCReplicateConVars:
            OnReplicateConVars(messageRead);
            break;
        case k_EMsgGCSystemMessage:
            OnSystemBroadcast(messageRead);
            break;
			
        // abuse report
        case k_EMsgGC_ReportAbuse:
            OnReportAbuse(messageRead);
            break;

        // private queues
        case k_EMsgGCCStrike15_v2_PrivateQueues:
            OnPrivateQueues(messageRead);
            break;

        case k_EMsgGCAdjustItemEquippedState:
            AdjustItemEquippedState(messageRead);
            break;

        case k_EMsgGCAdjustItemEquippedStateMulti:
            AdjustItemEquippedStateMulti(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_ClientPlayerDecalSign:
            ClientPlayerDecalSign(messageRead);
            break;

        case k_EMsgGCUseItemRequest:
            UseItemRequest(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_ClientRequestJoinServerData:
            ClientRequestJoinServerData(messageRead);
            break;

        case k_EMsgGCSetItemPositions:
            SetItemPositions(messageRead);
            break;

        case k_EMsgGCApplySticker:
            ApplySticker(messageRead);
            break;

        case k_EMsgGCStoreGetUserData:
            StoreGetUserData(messageRead);
            break;

        case k_EMsgGCStorePurchaseInit:
            StorePurchaseInit(messageRead);
            break;

        case k_EMsgGCStorePurchaseFinalize:
            StorePurchaseFinalize(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_Party_Search:
            PartySearch(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_Account_RequestCoPlays:
            RequestCoPlays(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_ClientRequestPlayersProfile:
            ClientRequestPlayersProfile(messageRead);
            break;

        case k_EMsgGCCasketItemLoadContents:
            CasketItemLoadContents(messageRead);
            break;

        case k_EMsgGCCasketItemAdd:
            CasketItemAdd(messageRead);
            break;

        case k_EMsgGCCasketItemExtract:
            CasketItemExtract(messageRead);
            break;

        case k_EMsgGCStatTrakSwap:
            StatTrakSwap(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_MatchmakingClient2ServerPing:
            OnMatchmakingPing(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_MatchmakingStart:
            OnMatchmakingStart(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_MatchmakingStop:
            OnMatchmakingStop(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_ClientReportValidation:
            OnClientReportValidation(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_GC2ClientInitSystem_Response:
            OnClientInitSystemResponse(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_AccountPrivacySettings:
            OnAccountPrivacySettings(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_ClientRequestSouvenir:
            OnClientRequestSouvenir(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_AcknowledgePenalty:
            OnAcknowledgePenalty(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_Client2GCRequestPrestigeCoin:
            OnClientRequestPrestigeCoin(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_SetPlayerLeaderboardSafeName:
            OnSetPlayerLeaderboardSafeName(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_ClientReportPlayer:
            OnClientReportPlayer(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_ClientReportServer:
            OnClientReportServer(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_ClientCommendPlayer:
            OnClientCommendPlayer(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_SetMyActivityInfo:
            OnSetMyActivityInfo(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_GlobalChat:
            OnClientToGCChat(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_GlobalChat_Subscribe:
            OnGlobalChatSubscribe(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_GlobalChat_Unsubscribe:
            OnGlobalChatUnsubscribe(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_MatchListRequestCurrentLiveGames:
            OnMatchListRequestCurrentLiveGames(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_MatchListRequestRecentUserGames:
            OnMatchListRequestRecentUserGames(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_MatchListRequestLiveGameForUser:
            OnMatchListRequestLiveGameForUser(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_MatchListRequestFullGameInfo:
            OnMatchListRequestFullGameInfo(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_MatchListRequestTournamentGames:
            OnMatchListRequestTournamentGames(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_GetEventFavorites_Request:
            OnGetEventFavorites(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_Party_Register:
            OnPartyRegister(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_Party_Unregister:
            OnPartyUnregister(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_Party_Invite:
            OnPartyInvite(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_ClientPartyJoinRelay:
            OnClientPartyJoinRelay(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_ClientPartyWarning:
            OnClientPartyWarning(messageRead);
            break;

        // ---- join-friend / GOTV / store ----
        case k_EMsgGCCStrike15_v2_ClientRequestJoinFriendData:
            OnClientRequestJoinFriendData(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_GotvSyncPacket:
            OnGotvSyncPacket(messageRead);
            break;

        case k_EMsgGCStorePurchaseCancel:
            OnStorePurchaseCancel(messageRead);
            break;

        // ---- validation / matchmaking echo ----
        case k_EMsgGCCStrike15_v2_GC2ClientRequestValidation:
            OnGC2ClientRequestValidation(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_MatchmakingGC2ClientUpdate:
            OnMatchmakingGC2ClientUpdate(messageRead);
            break;

        // ---- global game minigame ----
        case k_EMsgGC_GlobalGame_Subscribe:
            OnGlobalGameSubscribe(messageRead);
            break;

        case k_EMsgGC_GlobalGame_Unsubscribe:
            OnGlobalGameUnsubscribe(messageRead);
            break;

        case k_EMsgGC_GlobalGame_Play:
            OnGlobalGamePlay(messageRead);
            break;
			
		case k_EMsgGCCStrike15_v2_ClientGCRankUpdate:
		{
			// GC->client update, not a request. we don't need to respond.
			CMsgGCCStrike15_v2_ClientGCRankUpdate msg;
			if (messageRead.ReadProtobuf(msg))
			{
				Platform::PrintDebug("ClientGCRankUpdate: %d rankings (ignored)\n", msg.rankings_size());
			}
			break;
		}

        default:
            Platform::PrintWarning("ClientGC::HandleMessage: unhandled protobuf message %s\n",
                MessageName(messageRead.TypeUnmasked()));
            break;
        }
    }
    else
    {
        switch (messageRead.TypeUnmasked())
        {
        case k_EMsgGCDelete:
            DeleteItem(messageRead);
            break;

        case k_EMsgGCUnlockCrate:
            UnlockCrate(messageRead);
            break;

        case k_EMsgGCNameItem:
            NameItem(messageRead);
            break;

        case k_EMsgGCNameBaseItem:
            NameBaseItem(messageRead);
            break;

        case k_EMsgGCRemoveItemName:
            RemoveItemName(messageRead);
            break;

        case k_EMsgGCStorePurchaseQueryTxn:
            OnStorePurchaseQueryTxn(messageRead);
            break;

        case k_EMsgGCServerBrowser_FavoriteServer:
            OnServerBrowserFavoriteServer(messageRead);
            break;

        case k_EMsgGCServerBrowser_BlacklistServer:
            OnServerBrowserBlacklistServer(messageRead);
            break;

        case k_EMsgGCConVarUpdated:
            OnConVarUpdated(messageRead);
            break;

        default:
            Platform::PrintWarning("ClientGC::HandleMessage: unhandled struct message %s\n",
                MessageName(messageRead.TypeUnmasked()));
            break;
        }
    }
}
void ClientGC::SendMatchmakingHelloUpdate()
{
    CMsgGCCStrike15_v2_MatchmakingGC2ClientHello mmHello;
    BuildMatchmakingHello(mmHello);
    SendMessageToGame(false, k_EMsgGCCStrike15_v2_MatchmakingGC2ClientHello, mmHello);
}

void ClientGC::UpdateCooldown()
{
    if (!m_isCooldownActive)
        return;
    auto now = std::chrono::steady_clock::now();
    if (now >= m_cooldownEndTime) {
        // Reasons 12+ ("checking if your account can queue for games") don't
        // pop a dismissable "cooldown expired" dialog, so 9171 never arrives.
        // Clear the cooldown ourselves instead of waiting for the ack.
        if (m_lastAppliedCooldownReason >= 12)
        {
            Platform::PrintSuccess("Competitive cooldown expired (reason %u), clearing without waiting for ack\n",
                m_lastAppliedCooldownReason);

            m_isCooldownActive = false;
            m_cooldownExpiredLocally = false;
            m_lastAppliedCooldownSeconds = 0;

            SendMatchmakingHelloUpdate();
            SendCompetitiveCooldown();
            QueueProfilePublish();
            return;
        }

        // не обнуляем m_isCooldownActive и не шлём penalty_seconds = 0.
        // клиент сам досчитает локальный remaining до минуса и покажет
        // попап "твой кулдаун истёк". сбросим только после 9171.
        if (!m_cooldownExpiredLocally)
        {
            m_cooldownExpiredLocally = true;
            Platform::Print("Competitive cooldown expired locally, waiting for client ack (9171)\n");
        }
    }
}

void ClientGC::ProcessGiftUse(uint64_t giftId)
{
    const CSOEconItem *giftItem = m_inventory.GetItem(giftId);
    if (!giftItem)
    {
        Platform::PrintWarning("Gift item not found\n");
        return;
    }

    uint32_t defIndex = giftItem->def_index();

    int numItems = 1;
    if (defIndex == 1210)
        numItems = 1;
    else if (defIndex == 1211)
        numItems = 9;
    else if (defIndex == 1215)
        numItems = 25;
    else {
        Platform::PrintWarning("Unknown gift type\n");
        return;
    }

    uint32_t series = 0;
    uint32_t attrDef = m_inventory.GetItemSchema().GetAttributeDefIndex("set supply crate series");
    if (attrDef)
    {
        for (const auto &attr : giftItem->attribute())
        {
            if (attr.def_index() == attrDef)
            {
                series = m_inventory.GetItemSchema().AttributeUint32(&attr);
                break;
            }
        }
    }

    if (series == 0)
    {
        Platform::PrintWarning("Gift has no supply crate series\n");
        return;
    }

    const LootList *lootList = m_inventory.GetItemSchema().GetLootListBySeries(series);
    if (!lootList)
    {
        Platform::PrintWarning("No loot list for series %u\n", series);
        return;
    }

    CMsgSOMultipleObjects updateMultiple;
    CMsgSOSingleObject destroy;
    CMsgGCItemCustomizationNotification notification;
    notification.set_request(k_EGCItemCustomizationNotification_Gift);

    CaseOpening caseOpening(m_inventory.GetItemSchema(), m_inventory.GetRandom());

    for (int i = 0; i < numItems; i++)
    {
        CSOEconItem newItemProto;
        if (!caseOpening.SelectItemFromLootList(*lootList, newItemProto))
        {
            Platform::PrintError("Failed to select item from loot list\n");
            continue;
        }

        CSOEconItem &createdItem = m_inventory.CreateItem(newItemProto);
        m_inventory.AddToMultipleObjects(updateMultiple, SOTypeItem, createdItem);
        notification.add_item_id(createdItem.id());
    }

    if (GetConfig().DestroyUsedItems())
    {
        m_inventory.RemoveItem(giftId, destroy);
    }

    if (updateMultiple.objects_modified_size() > 0)
    {
        SendMessageToGame(true, k_ESOMsg_UpdateMultiple, updateMultiple);
        SendMessageToGame(false, k_ESOMsg_UpdateMultiple, updateMultiple);
    }

    if (destroy.has_type_id())
    {
        SendMessageToGame(true, k_ESOMsg_Destroy, destroy);
        SendMessageToGame(false, k_ESOMsg_Destroy, destroy);
    }

    if (notification.item_id_size() > 0)
    {
        SendMessageToGame(false, k_EMsgGCItemCustomizationNotification, notification);
    }
}

void ClientGC::HandleNetMessage(const void *data, uint32_t size)
{
    GCMessageRead messageRead{ 0, data, size };
    if (!messageRead.IsValid())
    {
        assert(false);
        return;
    }

    const uint32_t t = messageRead.TypeUnmasked();
    const bool isSocacheType = (t >= k_ESOMsg_Create && t <= k_ESOMsg_CacheSubscriptionRefresh);

    Platform::PrintDebug("[Socache] msg type=%u (0x%X) size=%u protobuf=%d\n",
        t, t, size, messageRead.IsProtobuf() ? 1 : 0);

    if (isSocacheType || t == k_EMsgGCClientWelcome)
    {
        // hex dump the raw payload. The message has already been consumed
        // by GCMessageRead, but the raw bytes are still in `data`/`size`.
        const uint8_t *bytes = static_cast<const uint8_t *>(data);
        constexpr size_t BytesPerLine = 32;
        for (size_t off = 0; off < size; off += BytesPerLine)
        {
            char line[BytesPerLine * 2 + 1];
            size_t n = (std::min)(BytesPerLine, static_cast<size_t>(size) - off);
            for (size_t i = 0; i < n; ++i)
                snprintf(line + i * 2, 3, "%02x", bytes[off + i]);
            line[n * 2] = '\0';
            Platform::PrintDebug("[Socache]   %04zx: %s\n", off, line);
        }
    }

    if (messageRead.IsProtobuf())
    {
        switch (messageRead.TypeUnmasked())
        {
        case k_EMsgGC_IncrementKillCountAttribute:
            IncrementKillCountAttribute(messageRead);
            return;
        }
    }

    Platform::PrintWarning("ClientGC::HandleNetMessage: unhandled protobuf message %s\n",
        MessageName(messageRead.TypeUnmasked()));
}

void ClientGC::HandleSOCacheRequest()
{
    CMsgSOCacheSubscribed message;
    m_inventory.BuildCacheSubscription(message, GetConfig().Level(), true);

    GCMessageWrite messageWrite{ k_ESOMsg_CacheSubscribed, message };
    PostToHost(HostEvent::NetMessage, 0, messageWrite.Data(), messageWrite.Size());
}

void ClientGC::SendMessageToGame(bool sendToGameServer, uint32_t type,
    const google::protobuf::MessageLite &message, uint64_t jobId)
{
    GCMessageWrite messageWrite{ type, message, jobId };

    if (sendToGameServer)
    {
        PostToHost(HostEvent::NetMessage, 0, messageWrite.Data(), messageWrite.Size());
    }

    PostToHost(HostEvent::Message, messageWrite.TypeMasked(), messageWrite.Data(), messageWrite.Size());
}

constexpr uint32_t MakeAddress(uint32_t v1, uint32_t v2, uint32_t v3, uint32_t v4)
{
    return v4 | (v3 << 8) | (v2 << 16) | (v1 << 24);
}

static void BuildCSWelcome(CMsgCStrike15Welcome &message)
{
    message.set_store_item_hash(136617352);
    message.set_timeplayedconsecutively(0);
    message.set_time_first_played(1329845773);
    message.set_last_time_played(1680260376);
    message.set_last_ip_address(MakeAddress(127, 0, 0, 1));
}

void ClientGC::BuildMatchmakingHello(CMsgGCCStrike15_v2_MatchmakingGC2ClientHello &message)
{
    message.set_account_id(EffectiveAccountId());

    message.mutable_global_stats()->set_players_online(10000);
    message.mutable_global_stats()->set_servers_online(10000);
    message.mutable_global_stats()->set_players_searching(1000);
    message.mutable_global_stats()->set_servers_available(1000);
    message.mutable_global_stats()->set_ongoing_matches(999);
    message.mutable_global_stats()->set_search_time_avg(60);

    auto* stats = message.mutable_global_stats();
    stats->set_players_searching(1000);
    stats->set_servers_available(1000);
    stats->set_search_time_avg(60);

	// fill in every reasonable mode id so the client's "Estimated Wait"
	// widget has an entry to find no matter which mode is queued. the exact
	// game_type the client looks up is unverified — we're covering 1..20 to
	// be safe, and 0 as a fallback in case the client defaults there.
	for (uint32_t gt = 0; gt <= 20000; ++gt)
	{
		auto* detail = stats->add_search_statistics();
		detail->set_game_type(gt);
		detail->set_search_time_avg(120);   // 2 min
		detail->set_players_searching(1000);
	}

    message.mutable_global_stats()->set_main_post_url("");

    message.mutable_global_stats()->set_required_appid_version(13857);
    message.mutable_global_stats()->set_pricesheet_version(1680057676);
    message.mutable_global_stats()->set_twitch_streams_version(2);
    message.mutable_global_stats()->set_active_tournament_eventid(20);
    message.mutable_global_stats()->set_active_survey_id(0);
    message.mutable_global_stats()->set_required_appid_version2(13862);

    message.set_vac_banned(GetConfig().VacBanned());
    message.mutable_commendation()->set_cmd_friendly(GetConfig().CommendedFriendly());
    message.mutable_commendation()->set_cmd_teaching(GetConfig().CommendedTeaching());
    message.mutable_commendation()->set_cmd_leader(GetConfig().CommendedLeader());

    // non-prime accounts show padlocks over level and rank in the UI. the
    // client reaches that state by us sending zeros, not by us withholding
    // the fields - if we send the real level, it renders it like prime.
    const bool prime = GetConfig().HasPrime();
    message.set_player_level(prime ? GetConfig().Level() : 0);
    message.set_player_cur_xp(prime ? GetConfig().Xp() : 0);

    if (m_isCooldownActive)
    {
        const uint32_t remaining = CooldownRemainingSeconds(m_cooldownEndTime);
        message.set_penalty_seconds(remaining);
        message.set_penalty_reason(GetConfig().CooldownReason());
        SendCompetitiveCooldown();
    }
    if (GetConfig().HasPrime() && GetConfig().CompetitiveRank() != RankNone)
    {
        PlayerRankingInfo *ranking = message.mutable_ranking();
        ranking->set_account_id(EffectiveAccountId());
        ranking->set_rank_id(GetConfig().CompetitiveRank());
        ranking->set_wins(GetConfig().CompetitiveWins());
        ranking->set_rank_type_id(RankTypeCompetitive);
    }
}

void ClientGC::BuildClientWelcome(CMsgClientWelcome &message, const CMsgCStrike15Welcome &csWelcome,
    const CMsgGCCStrike15_v2_MatchmakingGC2ClientHello &matchmakingHello)
{
    message.set_version(0);
    message.set_game_data(csWelcome.SerializeAsString());
    m_inventory.BuildCacheSubscription(*message.add_outofdate_subscribed_caches(), GetConfig().Level(), false);
    message.mutable_location()->set_latitude(65.0133006f);
    message.mutable_location()->set_longitude(25.4646212f);
    message.mutable_location()->set_country(GetConfig().Country());
    message.set_game_data2(matchmakingHello.SerializeAsString());
    message.set_rtime32_gc_welcome_timestamp(static_cast<uint32_t>(time(nullptr)));
    message.set_currency(GetConfig().Currency());
    message.set_txn_country_code(GetConfig().Country());
}

void ClientGC::SendRankUpdate()
{
    // the client shows the padlock based on the rank value, not on whether
    // a separate "prime" flag is set. when non-prime we must actively push
    // RankNone (=0) for all three rank types; if we just skip the message,
    // the client keeps whatever rank it received while prime and renders it
    // unlocked. level uses the same trick via CSOPersonaDataPublic.
    const bool prime = GetConfig().HasPrime();

    CMsgGCCStrike15_v2_ClientGCRankUpdate message;

    PlayerRankingInfo *rank = message.add_rankings();
    rank->set_account_id(EffectiveAccountId());
    rank->set_rank_id(prime ? GetConfig().CompetitiveRank() : RankNone);
    rank->set_wins(prime ? GetConfig().CompetitiveWins() : 0);
    rank->set_rank_type_id(RankTypeCompetitive);

    rank = message.add_rankings();
    rank->set_account_id(EffectiveAccountId());
    rank->set_rank_id(prime ? GetConfig().WingmanRank() : RankNone);
    rank->set_wins(prime ? GetConfig().WingmanWins() : 0);
    rank->set_rank_type_id(RankTypeWingman);

    rank = message.add_rankings();
    rank->set_account_id(AccountId());
    rank->set_rank_id(prime ? GetConfig().DangerZoneRank() : DangerZoneRankNone);
    rank->set_wins(prime ? GetConfig().DangerZoneWins() : 0);
    rank->set_rank_type_id(RankTypeDangerZone);

    SendMessageToGame(false, k_EMsgGCCStrike15_v2_ClientGCRankUpdate, message);
}

// #9 secure mode / validation

void ClientGC::SendInitSystem()
{
    CMsgGCCStrike15_v2_GC2ClientInitSystem init;
    init.set_load(false);
    init.set_name("");
    init.set_outputname("");
    init.set_cookie(0);
    init.set_manifest("");
    init.set_load_system(false);

    SendMessageToGame(false, k_EMsgGCCStrike15_v2_GC2ClientInitSystem, init);
}

void ClientGC::OnClientInitSystemResponse(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_GC2ClientInitSystem_Response message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::PrintWarning("Parsing GC2ClientInitSystem_Response failed, ignoring\n");
        return;
    }

    Platform::PrintDebug("[SecureMode] response=%d einit_result=%d err1=%d err2=%d\n",
        message.response(),
        message.einit_result(),
        message.error_code1(),
        message.error_code2());
}

// #10 privacy settings

void ClientGC::OnAccountPrivacySettings(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_AccountPrivacySettings message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::PrintWarning("Parsing AccountPrivacySettings failed, ignoring\n");
        return;
    }

    m_privacySettings.clear();
    for (int i = 0; i < message.settings_size(); i++)
    {
        const auto &setting = message.settings(i);
        m_privacySettings[setting.setting_type()] = setting.setting_value();
        Platform::PrintDebug("Privacy setting %u = %u\n",
            setting.setting_type(), setting.setting_value());
    }

    SendMessageToGame(false, k_EMsgGCCStrike15_v2_AccountPrivacySettings, message);
}

// #13 souvenir

void ClientGC::OnClientRequestSouvenir(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_ClientRequestSouvenir message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::PrintWarning("Parsing ClientRequestSouvenir failed, ignoring\n");
        return;
    }

    Platform::PrintDebug("ClientRequestSouvenir: itemid=%llu matchid=%llu eventid=%d\n",
        static_cast<unsigned long long>(message.itemid()),
        static_cast<unsigned long long>(message.matchid()),
        message.eventid());

    CMsgGCCStrike15_v2_MatchEndRewardDropsNotification notification;
    SendMessageToGame(false, k_EMsgGCCStrike15_v2_MatchEndRewardDropsNotification, notification);
}

// #14 multi-equip

void ClientGC::AdjustItemEquippedStateMulti(GCMessageRead &messageRead)
{
    CMsgAdjustItemEquippedStateMulti message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::PrintWarning("Parsing CMsgAdjustItemEquippedStateMulti failed, ignoring\n");
        return;
    }

    Platform::PrintDebug("AdjustItemEquippedStateMulti: T=%d CT=%d noteam=%d\n",
        message.t_equips_size(),
        message.ct_equips_size(),
        message.noteam_equips_size());

    CMsgSOMultipleObjects update;

    for (int i = 0; i < message.t_equips_size(); i++)
    {
        uint64_t itemId = message.t_equips(i);
        const CSOEconItem *item = m_inventory.GetItem(itemId);
        if (!item)
        {
            Platform::PrintWarning("AdjustItemEquippedStateMulti: unknown T item %llu\n",
                static_cast<unsigned long long>(itemId));
            continue;
        }
        uint32_t slot = 0;
        for (const auto &eq : item->equipped_state())
        {
            if (eq.new_class() == 2) { slot = eq.new_slot(); break; }
        }
        if (slot == 0) continue;
        m_inventory.EquipItem(itemId, 2, slot, update);
    }

    for (int i = 0; i < message.ct_equips_size(); i++)
    {
        uint64_t itemId = message.ct_equips(i);
        const CSOEconItem *item = m_inventory.GetItem(itemId);
        if (!item)
        {
            Platform::PrintWarning("AdjustItemEquippedStateMulti: unknown CT item %llu\n",
                static_cast<unsigned long long>(itemId));
            continue;
        }
        uint32_t slot = 0;
        for (const auto &eq : item->equipped_state())
        {
            if (eq.new_class() == 3) { slot = eq.new_slot(); break; }
        }
        if (slot == 0) continue;
        m_inventory.EquipItem(itemId, 3, slot, update);
    }

    for (int i = 0; i < message.noteam_equips_size(); i++)
    {
        uint64_t itemId = message.noteam_equips(i);
        const CSOEconItem *item = m_inventory.GetItem(itemId);
        if (!item)
        {
            Platform::PrintWarning("AdjustItemEquippedStateMulti: unknown noteam item %llu\n",
                static_cast<unsigned long long>(itemId));
            continue;
        }
        uint32_t slot = 0;
        for (const auto &eq : item->equipped_state())
        {
            if (eq.new_class() == 0) { slot = eq.new_slot(); break; }
        }
        if (slot == 0) continue;
        m_inventory.EquipItem(itemId, 0, slot, update);
    }

    if (update.objects_modified_size() > 0)
    {
        SendMessageToGame(true, k_ESOMsg_UpdateMultiple, update);
    }
}

// acknowledge penalty

void ClientGC::OnAcknowledgePenalty(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_AcknowledgePenalty message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::PrintWarning("Parsing AcknowledgePenalty failed, ignoring\n");
        return;
    }

    Platform::PrintDebug("AcknowledgePenalty: acknowledged=%d\n", message.acknowledged());

    // клиент нажал OK в попапе "cooldown expired" — вот теперь реально снимаем
    if (m_cooldownExpiredLocally)
    {
        Platform::PrintSuccess("Client acknowledged expired cooldown, clearing\n");

        m_isCooldownActive = false;
        m_cooldownExpiredLocally = false;
        m_lastAppliedCooldownSeconds = 0;   // чтобы ApplyConfiguredCooldown не считал это "сменой"

        // оповещаем клиент, что кулдауна больше нет
        SendMatchmakingHelloUpdate();
        SendCompetitiveCooldown();

        // и воркер, чтобы другие клиенты увидели, что мы больше не в кулдауне
        QueueProfilePublish();
    }
}

// prestige coin

void ClientGC::OnClientRequestPrestigeCoin(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_Client2GCRequestPrestigeCoin request;
    if (!messageRead.ReadProtobuf(request))
    {
        Platform::PrintWarning("Parsing Client2GCRequestPrestigeCoin failed, ignoring\n");
        return;
    }

    Platform::PrintDebug("Client2GCRequestPrestigeCoin: defindex=%u upgradeid=%llu hours=%u prestigetime=%u\n",
        request.defindex(),
        static_cast<unsigned long long>(request.upgradeid()),
        request.hours(),
        request.prestigetime());

    CMsgSOSingleObject create, destroy;
    CMsgGCItemCustomizationNotification notification;
    uint64_t newItemId = 0;

    if (!m_inventory.GrantPrestigeCoin(newItemId, create, destroy, notification))
    {
        // already at max level (4878). show the client a popup,
        // so the button doesn't hang in "processing" forever.
        Platform::PrintWarning("Prestige coin: refused (already at max level)\n");

        CMsgGCClientDisplayNotification refuse;
        refuse.set_notification_title_localization_key("Prestige Coin");
        refuse.set_notification_body_localization_key("Already at maximum level.");
        SendMessageToGame(false, k_EMsgGCClientDisplayNotification, refuse);
        return;
    }

    // reset level and XP on prestige
    GetConfig().SetLevel(1);
    GetConfig().SetXp(0);
    GetConfig().Save();

    // ReloadConfig will re-read the file from disk and fire SendRankUpdate() +
    // SendMatchmakingHelloUpdate() so the client sees the new level/XP.
    ReloadConfig();

    if (destroy.has_type_id())
    {
        SendMessageToGame(true, k_ESOMsg_Destroy, destroy);
    }
    SendMessageToGame(true, k_ESOMsg_Create, create);
    SendMessageToGame(false, k_EMsgGCItemCustomizationNotification, notification);
}

// set player leaderboard safe name

void ClientGC::OnSetPlayerLeaderboardSafeName(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_SetPlayerLeaderboardSafeName message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::PrintWarning("Parsing SetPlayerLeaderboardSafeName failed, ignoring\n");
        return;
    }

    m_leaderboardSafeName = message.leaderboard_safe_name();
    Platform::PrintDebug("SetPlayerLeaderboardSafeName: %s\n", m_leaderboardSafeName.c_str());
}

// reports / commend

static uint64_t MakeReportConfirmationId()
{
    return Random{}.Integer<uint64_t>();
}

void ClientGC::OnClientReportPlayer(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_ClientReportPlayer message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::PrintWarning("Parsing ClientReportPlayer failed, ignoring\n");
        return;
    }

    Platform::Print("ClientReportPlayer: account=%u aimbot=%u wallhack=%u speedhack=%u "
                    "teamharm=%u textabuse=%u voiceabuse=%u match=%llu demo=%d\n",
        message.account_id(),
        message.rpt_aimbot(), message.rpt_wallhack(), message.rpt_speedhack(),
        message.rpt_teamharm(), message.rpt_textabuse(), message.rpt_voiceabuse(),
        static_cast<unsigned long long>(message.match_id()),
        message.report_from_demo());

    // Forward to the worker (fire-and-forget, happens on the HTTP thread).
    if (GetConfig().ReportSharing()
        && !GetConfig().PartyServerUrl().empty()
        && message.account_id() != 0
        && message.account_id() != EffectiveAccountId())
    {
        uint32_t flags =
            (message.rpt_aimbot()     ? 1u  : 0u)
          | (message.rpt_wallhack()   ? 2u  : 0u)
          | (message.rpt_speedhack()  ? 4u  : 0u)
          | (message.rpt_teamharm()   ? 8u  : 0u)
          | (message.rpt_textabuse()  ? 16u : 0u)
          | (message.rpt_voiceabuse() ? 32u : 0u);

        QueueReport(message.account_id(), flags);
    }

    CMsgGCCStrike15_v2_ClientReportResponse response;
    response.set_confirmation_id(MakeReportConfirmationId());
    response.set_account_id(EffectiveAccountId());
    response.set_response_type(0);
    response.set_response_result(0);
    SendMessageToGame(false, k_EMsgGCCStrike15_v2_ClientReportResponse, response);
}

void ClientGC::OnClientReportServer(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_ClientReportServer message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::PrintWarning("Parsing ClientReportServer failed, ignoring\n");
        return;
    }

    Platform::Print("ClientReportServer: poorperf=%u abusivemodels=%u badmotd=%u "
                    "listingabuse=%u inventoryabuse=%u match=%llu\n",
        message.rpt_poorperf(),
        message.rpt_abusivemodels(),
        message.rpt_badmotd(),
        message.rpt_listingabuse(),
        message.rpt_inventoryabuse(),
        static_cast<unsigned long long>(message.match_id()));

    CMsgGCCStrike15_v2_ClientReportResponse response;
    response.set_confirmation_id(MakeReportConfirmationId());
    response.set_account_id(EffectiveAccountId());
    response.set_response_type(1);
    response.set_response_result(0);
    SendMessageToGame(false, k_EMsgGCCStrike15_v2_ClientReportResponse, response);
}

void ClientGC::OnClientCommendPlayer(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_ClientCommendPlayer message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::PrintWarning("Parsing ClientCommendPlayer failed, ignoring\n");
        return;
    }

    Platform::Print("ClientCommendPlayer: account=%u match=%llu tokens=%u "
                    "friendly=%u teaching=%u leader=%u\n",
        message.account_id(),
        static_cast<unsigned long long>(message.match_id()),
        message.tokens(),
        message.commendation().cmd_friendly(),
        message.commendation().cmd_teaching(),
        message.commendation().cmd_leader());
}

// set my activity info

void ClientGC::OnSetMyActivityInfo(GCMessageRead &messageRead)
{
    (void)messageRead;
    Platform::PrintDebug("ClientGC: received SetMyActivityInfo (unparsed)\n");
}

// global chat

void ClientGC::OnGlobalChatSubscribe(GCMessageRead &messageRead)
{
    (void)messageRead;
    Platform::PrintDebug("GlobalChat: subscribe\n");
}

void ClientGC::OnGlobalChatUnsubscribe(GCMessageRead &messageRead)
{
    (void)messageRead;
    Platform::PrintDebug("GlobalChat: unsubscribe\n");
}

// empty responses for matchlist and watch

void ClientGC::SendEmptyMatchList(uint32_t requestId, uint32_t accountId)
{
    CMsgGCCStrike15_v2_MatchList message;
    message.set_msgrequestid(requestId);
    message.set_accountid(accountId);
    message.set_servertime(static_cast<uint32_t>(time(nullptr)));
    SendMessageToGame(false, k_EMsgGCCStrike15_v2_MatchList, message);
}

void ClientGC::OnMatchListRequestCurrentLiveGames(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_MatchListRequestCurrentLiveGames request;
    if (!messageRead.ReadProtobuf(request))
    {
        Platform::PrintWarning("Parsing MatchListRequestCurrentLiveGames failed, ignoring\n");
        return;
    }

    Platform::PrintDebug("MatchList: request current live games\n");
    SendEmptyMatchList(0, EffectiveAccountId());
}

void ClientGC::OnMatchListRequestRecentUserGames(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_MatchListRequestRecentUserGames request;
    if (!messageRead.ReadProtobuf(request))
    {
        Platform::PrintWarning("Parsing MatchListRequestRecentUserGames failed, ignoring\n");
        return;
    }

    Platform::PrintDebug("MatchList: recent user games for %u\n", request.accountid());
    SendEmptyMatchList(0, request.accountid());
}

void ClientGC::OnMatchListRequestLiveGameForUser(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_MatchListRequestLiveGameForUser request;
    if (!messageRead.ReadProtobuf(request))
    {
        Platform::PrintWarning("Parsing MatchListRequestLiveGameForUser failed, ignoring\n");
        return;
    }

    Platform::PrintDebug("MatchList: live game for %u\n", request.accountid());
    SendEmptyMatchList(0, request.accountid());
}

void ClientGC::OnMatchListRequestFullGameInfo(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_MatchListRequestFullGameInfo request;
    if (!messageRead.ReadProtobuf(request))
    {
        Platform::PrintWarning("Parsing MatchListRequestFullGameInfo failed, ignoring\n");
        return;
    }

    Platform::PrintDebug("MatchList: full game info for match=%llu\n",
        static_cast<unsigned long long>(request.matchid()));
    SendEmptyMatchList(0, EffectiveAccountId());
}

void ClientGC::OnMatchListRequestTournamentGames(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_MatchListRequestTournamentGames request;
    if (!messageRead.ReadProtobuf(request))
    {
        Platform::PrintWarning("Parsing MatchListRequestTournamentGames failed, ignoring\n");
        return;
    }

    Platform::PrintDebug("MatchList: tournament games for event=%d\n", request.eventid());
    SendEmptyMatchList(0, EffectiveAccountId());
}

// existing handlers

void ClientGC::OnClientReportValidation(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_ClientReportValidation message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::PrintWarning("Failed to parse ClientReportValidation\n");
        return;
    }

    auto PrintLongString = [](const char *prefix, std::string_view str)
    {
        constexpr size_t MaxLine = 252;

        if (str.empty())
        {
            Platform::PrintDebug("%s<empty>\n", prefix);
            return;
        }

        size_t offset = 0;
        while (offset < str.size())
        {
            size_t chunk = std::min(MaxLine, str.size() - offset);
            Platform::PrintDebug("%.*s%.*s\n",
                static_cast<int>(strlen(prefix)), prefix,
                static_cast<int>(chunk), str.data() + offset);
            offset += chunk;
        }
    };

    Platform::PrintDebug("[Validation] ============================================\n");
    Platform::PrintDebug("[Validation] status_id: %u | total_files: %u | client_version: %u | trust_time: %u\n",
        message.status_id(),
        message.total_files(),
        message.clientreportversion(),
        message.trust_time());

    Platform::PrintDebug("[Validation] internal_error: %u | count_pending: %u | count_completed: %u | process_id: %u\n",
        message.internal_error(),
        message.count_pending(),
        message.count_completed(),
        message.process_id());

    Platform::PrintDebug("[Validation] osversion: %d | report_count: %u | client_time: %llu\n",
        message.osversion(),
        message.report_count(),
        static_cast<unsigned long long>(message.client_time()));

    Platform::PrintDebug("[Validation] diagnostic1: %u | diagnostic2: %llu | diagnostic3: %llu\n",
        message.diagnostic1(),
        static_cast<unsigned long long>(message.diagnostic2()),
        static_cast<unsigned long long>(message.diagnostic3()));

    Platform::PrintDebug("[Validation] diagnostic4: %llu | diagnostic5: %llu\n",
        static_cast<unsigned long long>(message.diagnostic4()),
        static_cast<unsigned long long>(message.diagnostic5()));

    if (!message.command_line().empty())
    {
        PrintLongString("[Validation] command_line: ", message.command_line());
    }

    if (!message.last_launch_data().empty())
    {
        PrintLongString("[Validation] last_launch_data: ", message.last_launch_data());
    }

    if (!message.file_report().empty())
    {
        Platform::PrintDebug("[Validation] file_report (size=%zu):\n", message.file_report().size());
        PrintLongString("[Validation]   ", message.file_report());
    }

    for (int i = 0; i < message.diagnostics_size(); i++)
    {
        const CVDiagnostic &diag = message.diagnostics(i);
        Platform::PrintDebug("[Validation] Diag[%d] id: %u | extended: %u | value: %llu\n",
            i,
            diag.id(),
            diag.extended(),
            static_cast<unsigned long long>(diag.value()));

        if (!diag.string_value().empty())
        {
            PrintLongString("[Validation]   string: ", diag.string_value());
        }
    }

    Platform::PrintDebug("[Validation] ============================================\n");
}

void ClientGC::OnGetEventFavorites(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_GetEventFavorites_Request request;
    if (!messageRead.ReadProtobuf(request))
    {
        Platform::PrintWarning("Failed to parse GetEventFavorites_Request\n");
        return;
    }

    CMsgGCCStrike15_v2_GetEventFavorites_Response response;
    response.set_all_events(request.all_events());
    response.set_json_favorites("[]");
    response.set_json_featured("[]");

    SendMessageToGame(false, k_EMsgGCCStrike15_v2_GetEventFavorites_Response, response);

    Platform::PrintWarning("Sent empty GetEventFavorites_Response (all_events: %d)\n", request.all_events());
}

void ClientGC::OnClientHello(GCMessageRead &messageRead)
{
    Platform::PrintDebug("ClientGC: ClientHello received\n");

    CMsgClientHello hello;
    if (!messageRead.ReadProtobuf(hello))
    {
        Platform::PrintWarning("Parsing CMsgClientHello failed, ignoring\n");
        return;
    }

	    Platform::PrintDebug("[Socache] ClientHello: version=%u session_need=%u launcher=%u\n",
        hello.version(),
        hello.client_session_need(),
        hello.client_launcher());

    Platform::PrintDebug("[Socache] socache_have_versions=%d\n",
        hello.socache_have_versions_size());

    for (int i = 0; i < hello.socache_have_versions_size(); ++i)
    {
        const CMsgSOCacheHaveVersion &v = hello.socache_have_versions(i);
        Platform::PrintDebug(
            "[Socache]   [%d] soid.type=%u soid.id=%llu version=%llu\n",
            i,
            v.has_soid() ? v.soid().type() : 0,
            v.has_soid() ? static_cast<unsigned long long>(v.soid().id()) : 0ULL,
            static_cast<unsigned long long>(v.version()));
    }

    ApplyConfiguredCooldown();
    CheckGriefingCooldown();

    CMsgCStrike15Welcome csWelcome;
    BuildCSWelcome(csWelcome);

    CMsgGCCStrike15_v2_MatchmakingGC2ClientHello mmHello;
    BuildMatchmakingHello(mmHello);

    CMsgClientWelcome clientWelcome;
    BuildClientWelcome(clientWelcome, csWelcome, mmHello);

    SendMessageToGame(false, k_EMsgGCClientWelcome, clientWelcome);
    Platform::PrintSuccess("sent ClientWelcome\n");

    SendMessageToGame(false, k_EMsgGCCStrike15_v2_MatchmakingGC2ClientHello, mmHello);
    Platform::PrintSuccess("sent MatchmakingGC2ClientHello\n");

    SendRankUpdate();
    Platform::PrintSuccess("sent RankUpdate\n");

    SendInitSystem();
    Platform::PrintSuccess("sent InitSystem\n");

    QueueProfilePublish();
}

void ClientGC::AdjustItemEquippedState(GCMessageRead &messageRead)
{
    CMsgAdjustItemEquippedState message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::PrintWarning("Parsing CMsgAdjustItemEquippedState failed, ignoring\n");
        return;
    }

    CMsgSOMultipleObjects update;
    if (!m_inventory.EquipItem(message.item_id(), message.new_class(), message.new_slot(), update))
    {
        assert(false);
        return;
    }

    SendMessageToGame(true, k_ESOMsg_UpdateMultiple, update);
}

void ClientGC::ClientPlayerDecalSign(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_ClientPlayerDecalSign message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::PrintWarning("Parsing CMsgGCCStrike15_v2_ClientPlayerDecalSign failed, ignoring\n");
        return;
    }

    if (!Graffiti::SignMessage(*message.mutable_data()))
    {
        Platform::PrintError("Could not sign graffiti! it won't appear\n");
        return;
    }

    SendMessageToGame(false, k_EMsgGCCStrike15_v2_ClientPlayerDecalSign, message);
}

void ClientGC::UseItemRequest(GCMessageRead &messageRead)
{
    CMsgUseItem message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::PrintWarning("Parsing CMsgUseItem failed, ignoring\n");
        return;
    }

    uint64_t itemId = message.item_id();

    const CSOEconItem *giftItem = m_inventory.GetItem(itemId);
    if (giftItem)
    {
        uint32_t defIndex = giftItem->def_index();
        if (defIndex == 1210 ||
            defIndex == 1211 ||
            defIndex == 1215)
        {
            ProcessGiftUse(itemId);
            return;
        }
    }

    CMsgSOSingleObject destroy;
    CMsgSOMultipleObjects updateMultiple;
    CMsgGCItemCustomizationNotification notification;

    if (m_inventory.UseItem(itemId, destroy, updateMultiple, notification))
    {
        SendMessageToGame(true, k_ESOMsg_Destroy, destroy);
        SendMessageToGame(true, k_ESOMsg_UpdateMultiple, updateMultiple);

        SendMessageToGame(false, k_EMsgGCItemCustomizationNotification, notification);
    }
}

static void AddressString(uint32_t ip, uint32_t port, char *buffer, size_t bufferSize)
{
    snprintf(buffer, bufferSize,
        "%u.%u.%u.%u:%u\n",
        (ip >> 24) & 0xff,
        (ip >> 16) & 0xff,
        (ip >> 8) & 0xff,
        ip & 0xff,
        port);
}

void ClientGC::ClientRequestJoinServerData(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_ClientRequestJoinServerData request;
    if (!messageRead.ReadProtobuf(request))
    {
        Platform::PrintWarning("Parsing CMsgGCCStrike15_v2_ClientRequestJoinServerData failed, ignoring\n");
        return;
    }

    // No matchmaking assignment ever exists: echo the request back with a fixed cookie so the
    // client does not hang waiting for an answer.
    CMsgGCCStrike15_v2_ClientRequestJoinServerData response = request;
    response.mutable_res()->set_serverid(request.version());
    response.mutable_res()->set_direct_udp_ip(request.server_ip());
    response.mutable_res()->set_direct_udp_port(request.server_port());
    response.mutable_res()->set_reservationid(GameServerCookieId);

    char addressString[32];
    AddressString(request.server_ip(), request.server_port(), addressString, sizeof(addressString));
    response.mutable_res()->set_server_address(addressString);

    SendMessageToGame(false, k_EMsgGCCStrike15_v2_ClientRequestJoinServerData, response);
}

void ClientGC::SetItemPositions(GCMessageRead &messageRead)
{
    CMsgSetItemPositions message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::PrintWarning("Parsing CMsgSetItemPositions failed, ignoring\n");
        return;
    }

    std::vector<CMsgItemAcknowledged> acknowledgements;
    acknowledgements.reserve(message.item_positions_size());

    CMsgSOMultipleObjects update;
    if (m_inventory.SetItemPositions(message, acknowledgements, update))
    {
        for (const CMsgItemAcknowledged &acknowledgement : acknowledgements)
        {
            GCMessageWrite messageWrite{ k_EMsgGCItemAcknowledged, acknowledgement };
            PostToHost(HostEvent::NetMessage, 0, messageWrite.Data(), messageWrite.Size());
        }

        SendMessageToGame(true, k_ESOMsg_UpdateMultiple, update);
    }
    else
    {
        assert(false);
    }
}

void ClientGC::IncrementKillCountAttribute(GCMessageRead &messageRead)
{
    CMsgIncrementKillCountAttribute message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::PrintWarning("Parsing CMsgIncrementKillCountAttribute failed, ignoring\n");
        return;
    }

    assert(message.event_type() == 0);

    CMsgSOSingleObject update;
    if (m_inventory.IncrementKillCountAttribute(message.item_id(), message.amount(), update))
    {
        SendMessageToGame(true, k_ESOMsg_Update, update);
    }
    else
    {
        assert(false);
    }
}

void ClientGC::ApplySticker(GCMessageRead &messageRead)
{
    CMsgApplySticker message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::PrintWarning("Parsing CMsgApplySticker failed, ignoring\n");
        return;
    }

    assert(!message.item_item_id() != !message.baseitem_defidx());

    CMsgSOSingleObject update, destroy;
    CMsgGCItemCustomizationNotification notification;

    if (!message.sticker_item_id())
    {
        if (m_inventory.ScrapeSticker(message, update, destroy, notification))
        {
            if (destroy.has_type_id())
            {
                SendMessageToGame(true, k_ESOMsg_Destroy, destroy);
            }

            if (update.has_type_id())
            {
                SendMessageToGame(true, k_ESOMsg_Update, update);
            }

            if (notification.has_request())
            {
                SendMessageToGame(false, k_EMsgGCItemCustomizationNotification, notification);
            }
        }
        else
        {
            assert(false);
        }
    }
    else if (m_inventory.ApplySticker(message, update, destroy, notification))
    {
        SendMessageToGame(true, k_ESOMsg_Destroy, destroy);
        SendMessageToGame(true, k_ESOMsg_Update, update);

        SendMessageToGame(false, k_EMsgGCItemCustomizationNotification, notification);
    }
    else
    {
        assert(false);
    }
}

void ClientGC::StoreGetUserData(GCMessageRead &messageRead)
{
    CMsgStoreGetUserData message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::PrintWarning("Parsing CMsgStoreGetUserData failed, ignoring\n");
        return;
    }

    if (m_priceSheet.IsEmpty())
    {
        ReloadPriceSheet();
        if (m_priceSheet.IsEmpty())
        {
            Platform::PrintError("StoreGetUserData: price sheet is empty, cannot respond\n");
            return;
        }
    }

    std::string binaryString;
    binaryString.reserve(1 << 17);
    m_priceSheet.BinaryWriteToString(binaryString);

    CMsgStoreGetUserDataResponse response;
    response.set_result(1);
    response.set_price_sheet_version(1729);
    *response.mutable_price_sheet() = std::move(binaryString);

    SendMessageToGame(false, k_EMsgGCStoreGetUserDataResponse, response);
}

void ClientGC::StorePurchaseInit(GCMessageRead &messageRead)
{
    CMsgGCStorePurchaseInit message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::PrintWarning("Parsing CMsgGCStorePurchaseInit failed, ignoring\n");
        return;
    }

    uint64_t transactionId = Random{}.Integer<uint64_t>();
    assert(!m_transactionId);
    m_transactionId = transactionId;
    m_transactionItemIds.clear();

    bool grantedPrime = false;
    std::vector<CMsgSOSingleObject> inventoryUpdate;

    for (const auto &item : message.line_items())
    {
        Platform::PrintDebug("StorePurchaseInit: line_item def=%u qty=%u\n",
            item.item_def_id(), item.quantity());

        // 1353 is the prime subscription item. we don't have a real item for
        // it, so skip the inventory grant and flip the config flag instead.
        if (item.item_def_id() == 1353)
        {
            if (!GetConfig().HasPrime())
            {
                GetConfig().SetHasPrime(true);
                grantedPrime = true;
            }
            continue;
        }

		for (uint32_t i = 0; i < item.quantity(); i++)
		{
			uint64_t itemId = m_inventory.PurchaseItem(item.item_def_id(), inventoryUpdate);
			if (!itemId)
			{
				Platform::PrintError("StorePurchaseInit: failed to create item def=%u (not in schema?)\n",
					item.item_def_id());
				continue;
			}
			m_transactionItemIds.push_back(itemId);
		}
    }

    char url[128];
    snprintf(url, sizeof(url),
        "https://checkout.steampowered.com/checkout/approvetxn/%llu/?returnurl=steam",
        transactionId);

    CMsgGCStorePurchaseInitResponse response;
    response.set_result(1);
    response.set_txn_id(transactionId);
    response.set_url(url);
    response.mutable_item_ids()->Assign(
        m_transactionItemIds.begin(), m_transactionItemIds.end());

    SendMessageToGame(false, k_EMsgGCStorePurchaseInitResponse, response, messageRead.JobId());

    for (auto &newItem : inventoryUpdate)
    {
        SendMessageToGame(true, k_ESOMsg_Create, newItem);
    }

    // fire the Steam-side "purchase approved" callback so the client proceeds
    // to StorePurchaseFinalize.
    PostToHost(HostEvent::MicroTransactionResponse, 0, nullptr, 0);

    // re-push the socache and the hello now that prime is on, so the prime
    // badge and competitive rank show up without waiting for the next reload.
    if (grantedPrime)
    {
        GetConfig().Save();             // persist has_prime=1 to config.txt
        SendInventoryUpdate();
        SendRankUpdate();
        SendMatchmakingHelloUpdate();
        QueueProfilePublish();
    }
}

void ClientGC::StorePurchaseFinalize(GCMessageRead &messageRead)
{
    CMsgGCStorePurchaseFinalize message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::PrintWarning("Parsing CMsgGCStorePurchaseFinalize failed, ignoring\n");
        return;
    }

    assert(m_transactionId);
	

    CMsgGCStorePurchaseFinalizeResponse response;
    response.set_result(1);
    response.mutable_item_ids()->Assign(m_transactionItemIds.begin(), m_transactionItemIds.end());
    SendMessageToGame(false, k_EMsgGCStorePurchaseFinalizeResponse, response, messageRead.JobId());

    m_transactionId = 0;
}

void ClientGC::PartySearch(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_Party_Search message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::PrintWarning("Parsing CMsgGCCStrike15_v2_Party_Search failed, ignoring\n");
        return;
    }

	const bool     prime    = GetConfig().HasPrime();
	const uint32_t rank     = static_cast<uint32_t>(GetConfig().CompetitiveRank());
	const uint32_t gameType = message.game_type();
	const uint32_t launcher = message.launcher();

	// collect full entries, not just ids
	std::vector<LtpEntry> peers;
	bool workerAnswered = false;

	if (LtpEnabled())
	{
		HttpResult res = HttpGetJson(
			LtpBase() + "/search?game_type=" + std::to_string(gameType));

		if (res.status == 200)
		{
			workerAnswered = true;
			for (const LtpEntry &e : ParseLtpSearch(res.body))
			{
				if (e.accountId == EffectiveAccountId()) continue;
				peers.push_back(e);
			}
			Platform::PrintDebug("LTP search: %zu peers from worker\n", peers.size());
		}
		else
		{
			Platform::PrintWarning("LTP search: worker failed (HTTP %d), falling back to friends\n",
				res.status);
		}
	}

	if (workerAnswered && peers.empty())
	{
		CMsgGCCStrike15_v2_Party_SearchResults empty;
		SendMessageToGame(false, k_EMsgGCCStrike15_v2_Party_Search, empty);
		Platform::PrintDebug("PartySearch: nobody looking to play\n");
		return;
	}

	if (peers.empty())
	{
		for (uint32_t friendId : GetConfig().GetFriends())
		{
			if (friendId == AccountId()) continue;
			// don't know theirs — send ours as before
			peers.push_back(LtpEntry{ friendId, 0, rank, prime });
		}
	}

	auto registerLobby = [&](uint32_t accountId, uint32_t apr, uint32_t ark)
	{
		CMsgGCCStrike15_v2_Party_Register reg;
		reg.set_id(accountId);
		reg.set_ver(1);
		reg.set_apr(apr);
		reg.set_ark(ark);
		reg.set_nby(0);
		reg.set_grp(3);
		reg.set_slots(5);
		reg.set_launcher(launcher);
		reg.set_game_type(gameType);
		SendMessageToGame(false, k_EMsgGCCStrike15_v2_Party_Register, reg);
	};

	auto addEntry = [&](CMsgGCCStrike15_v2_Party_SearchResults &out,
						uint32_t accountId, uint32_t apr, uint32_t ark)
	{
		auto *entry = out.add_entries();
		entry->set_id(accountId);
		entry->set_grp(3);
		entry->set_game_type(gameType);
		entry->set_apr(apr);
		entry->set_ark(ark);
		entry->set_loc(30066);
		entry->set_accountid(accountId);
	};

	const uint32_t localApr = prime ? 1u : 0u;

	registerLobby(EffectiveAccountId(), localApr, rank);
	for (const LtpEntry &p : peers)
		registerLobby(p.accountId, p.prime ? 1u : 0u, p.rank);

	CMsgGCCStrike15_v2_Party_SearchResults response;
	addEntry(response, EffectiveAccountId(), localApr, rank);
	for (const LtpEntry &p : peers)
		addEntry(response, p.accountId, p.prime ? 1u : 0u, p.rank);

	SendMessageToGame(false, k_EMsgGCCStrike15_v2_Party_Search, response);
}

void ClientGC::RequestCoPlays(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_Account_RequestCoPlays message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::PrintWarning("Parsing CMsgGCCStrike15_v2_Account_RequestCoPlays failed, ignoring\n");
        return;
    }

    CMsgGCCStrike15_v2_Account_RequestCoPlays_Player *player = message.add_players();
    player->set_accountid(EffectiveAccountId());
    player->set_online(true);
    player->set_rtcoplay(1771263169);

    for (uint32_t player_id : GetConfig().GetFriends())
    {
        if (AccountId() == player_id)
            continue;

        player = message.add_players();
        player->set_accountid(player_id);
        player->set_online(true);
        player->set_rtcoplay(1771262169);
    }

    message.set_servertime(1771263169);

    SendMessageToGame(false, k_EMsgGCCStrike15_v2_Account_RequestCoPlays, message);
}
void ClientGC::ClientRequestPlayersProfile(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_ClientRequestPlayersProfile request;
    if (!messageRead.ReadProtobuf(request))
    {
        Platform::PrintWarning("Parsing ClientRequestPlayersProfile failed, ignoring\n");
        return;
    }

    const uint32_t requestedAccountId = request.account_id();
    Platform::PrintDebug("Requested accountId: %u\n", requestedAccountId);

    if (requestedAccountId == 0 || requestedAccountId == EffectiveAccountId())
    {
        SendOwnProfile();
        return;
    }

    if (!m_profileLimiter.TryConsume(EffectiveAccountId()))
    {
        Platform::PrintWarning("Profile rate limit hit for %u\n", EffectiveAccountId());
        SendUnknownProfile(requestedAccountId);
        return;
    }

    if (QueryRemoteProfile(requestedAccountId))
        return;

    SendUnknownProfile(requestedAccountId);
}

void ClientGC::CasketItemLoadContents(GCMessageRead &messageRead)
{
    CMsgCasketItem message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::PrintWarning("Parsing CasketItemLoadContents::CMsgCasketItem failed, ignoring\n");
        return;
    }

    CMsgGCItemCustomizationNotification notification;
    notification.set_request(k_EGCItemCustomizationNotification_CasketContents);
    notification.add_item_id(message.casket_item_id());

    SendMessageToGame(false, k_EMsgGCItemCustomizationNotification, notification);
}

void ClientGC::CasketItemAdd(GCMessageRead &messageRead)
{
    CMsgCasketItem message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::PrintWarning("Parsing CasketItemAdd::CMsgCasketItem failed, ignoring\n");
        return;
    }

    CMsgSOSingleObject updateItem, updateCasket;
    CMsgGCItemCustomizationNotification notification;
    if (m_inventory.CasketItemAdd(message.casket_item_id(), message.item_item_id(), updateItem, updateCasket, notification))
    {
        SendMessageToGame(false, k_ESOMsg_Update, updateItem);
        SendMessageToGame(false, k_ESOMsg_Update, updateCasket);
        SendMessageToGame(false, k_EMsgGCItemCustomizationNotification, notification);
    }
}

void ClientGC::CasketItemExtract(GCMessageRead &messageRead)
{
    CMsgCasketItem message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::PrintWarning("Parsing CasketItemExtract::CMsgCasketItem failed, ignoring\n");
        return;
    }

    CMsgSOSingleObject updateItem, updateCasket;
    CMsgGCItemCustomizationNotification notification;
    if (m_inventory.CasketItemRemove(message.casket_item_id(), message.item_item_id(), updateItem, updateCasket, notification))
    {
        SendMessageToGame(false, k_ESOMsg_Update, updateItem);
        SendMessageToGame(false, k_ESOMsg_Update, updateCasket);
        SendMessageToGame(false, k_EMsgGCItemCustomizationNotification, notification);
    }
}

void ClientGC::StatTrakSwap(GCMessageRead &messageRead)
{
    CMsgApplyStatTrakSwap message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::PrintWarning("Parsing StatTrakSwap::CMsgApplyStatTrakSwap failed, ignoring\n");
        return;
    }

    CMsgSOSingleObject destroy, updateItem1, updateItem2;
    CMsgGCItemCustomizationNotification notification;

    if (m_inventory.StatTrakSwap(
            message.tool_item_id(),
            message.item_1_item_id(),
            message.item_2_item_id(),
            destroy,
            updateItem1,
            updateItem2,
            notification))
    {
        SendMessageToGame(true, k_ESOMsg_Destroy, destroy);
        SendMessageToGame(true, k_ESOMsg_Update, updateItem1);
        SendMessageToGame(true, k_ESOMsg_Update, updateItem2);

        SendMessageToGame(false, k_EMsgGCItemCustomizationNotification, notification);
    }
}

void ClientGC::DeleteItem(GCMessageRead &messageRead)
{
    uint64_t itemId = messageRead.ReadUint64();
    if (!messageRead.IsValid())
    {
        Platform::PrintWarning("DeleteItem: parsing CMsgGCDelete failed, ignoring\n");
        return;
    }

    Platform::PrintDebug("DeleteItem: requested for item %llu\n",
        static_cast<unsigned long long>(itemId));

    CMsgSOSingleObject destroyed;
    if (!m_inventory.RemoveItem(itemId, destroyed))
    {
        Platform::PrintWarning("DeleteItem: item %llu not found in inventory\n",
            static_cast<unsigned long long>(itemId));
        return;
    }

    SendMessageToGame(true, k_ESOMsg_Destroy, destroyed);
    SendInventoryUpdate();

    Platform::PrintSuccess("DeleteItem: removed %llu, sent destroy + resync\n",
        static_cast<unsigned long long>(itemId));
}

void ClientGC::UnlockCrate(GCMessageRead &messageRead)
{
    uint64_t keyId = messageRead.ReadUint64();
    uint64_t crateId = messageRead.ReadUint64();
    if (!messageRead.IsValid())
    {
        Platform::PrintWarning("Parsing CMsgGCUnlockCrate failed, ignoring\n");
        return;
    }

    Platform::Print("CASE OPENING %llu with %llu\n", crateId, keyId);

    CMsgSOSingleObject destroyCrate, destroyKey, newItem;
    CMsgGCItemCustomizationNotification notification;

    if (m_inventory.UnlockCrate(
            crateId,
            keyId,
            destroyCrate,
            destroyKey,
            newItem,
            notification))
    {
        SendMessageToGame(true, k_ESOMsg_Destroy, destroyCrate);
        SendMessageToGame(true, k_ESOMsg_Destroy, destroyKey);
        SendMessageToGame(true, k_ESOMsg_Create, newItem);

        SendMessageToGame(false, k_EMsgGCItemCustomizationNotification, notification);
    }
    else
    {
        assert(false);
    }
}

void ClientGC::NameItem(GCMessageRead &messageRead)
{
    uint64_t nameTagId = messageRead.ReadUint64();
    uint64_t itemId = messageRead.ReadUint64();
    messageRead.ReadData(1);
    std::string_view name = messageRead.ReadString();

    if (!messageRead.IsValid())
    {
        Platform::PrintWarning("Parsing CMsgGCNameItem failed, ignoring\n");
        return;
    }

    CMsgSOSingleObject update, destroy;
    CMsgGCItemCustomizationNotification notification;
    if (m_inventory.NameItem(nameTagId, itemId, name, update, destroy, notification))
    {
        SendMessageToGame(true, k_ESOMsg_Update, update);
        SendMessageToGame(true, k_ESOMsg_Destroy, destroy);

        SendMessageToGame(false, k_EMsgGCItemCustomizationNotification, notification);
    }
    else
    {
        assert(false);
    }
}

void ClientGC::NameBaseItem(GCMessageRead &messageRead)
{
    uint64_t nameTagId = messageRead.ReadUint64();
    uint32_t defIndex = messageRead.ReadUint32();
    messageRead.ReadData(1);
    std::string_view name = messageRead.ReadString();

    if (!messageRead.IsValid())
    {
        Platform::PrintWarning("Parsing CMsgGCNameBaseItem failed, ignoring\n");
        return;
    }

    CMsgSOSingleObject create, destroy;
    CMsgGCItemCustomizationNotification notification;
    if (m_inventory.NameBaseItem(nameTagId, defIndex, name, create, destroy, notification))
    {
        SendMessageToGame(true, k_ESOMsg_Create, create);
        SendMessageToGame(true, k_ESOMsg_Destroy, destroy);

        SendMessageToGame(false, k_EMsgGCItemCustomizationNotification, notification);
    }
    else
    {
        assert(false);
    }
}

void ClientGC::RemoveItemName(GCMessageRead &messageRead)
{
    uint64_t itemId = messageRead.ReadUint64();
    if (!messageRead.IsValid())
    {
        Platform::PrintWarning("Parsing CMsgGCRemoveItemName failed, ignoring\n");
        return;
    }

    CMsgSOSingleObject update, destroy;
    CMsgGCItemCustomizationNotification notification;
    if (m_inventory.RemoveItemName(itemId, update, destroy, notification))
    {
        if (update.has_type_id())
        {
            SendMessageToGame(true, k_ESOMsg_Update, update);
        }

        if (destroy.has_type_id())
        {
            SendMessageToGame(true, k_ESOMsg_Destroy, destroy);
        }

        SendMessageToGame(false, k_EMsgGCItemCustomizationNotification, notification);
    }
    else
    {
        assert(false);
    }
}

void ClientGC::SendInventoryUpdate()
{
    CMsgSOCacheSubscribed message;
    m_inventory.BuildCacheSubscription(message, GetConfig().Level(), false);
    SendMessageToGame(false, k_ESOMsg_CacheSubscribed, message);
}

void ClientGC::CheckFileReloads()
{
    namespace fs = std::filesystem;

    // на первом проходе после старта файлы «изменились» просто потому,
    // что lastXxxWrite инициализирован file_time_type::min(). если сейчас
    // дёрнуть ReloadConfig, он пошлёт CacheSubscribed/Rank/Hello в клиент
    // раньше, чем GC-поток обработает ClientHello и пошлёт Welcome.
    // клиент ждёт Welcome первым и падает, если получает что-то другое.
    static bool firstRun = true;

    struct WatchedFile {
        fs::path path;
        fs::file_time_type& lastWrite;
        void (ClientGC::*reloadFn)();
		int reloadIndex = 0;
    };

    static fs::file_time_type lastInventoryWrite   = fs::file_time_type::min();
    static fs::file_time_type lastConfigWrite      = fs::file_time_type::min();
    static fs::file_time_type lastPriceSheetWrite  = fs::file_time_type::min();
    static fs::file_time_type lastPassesWrite      = fs::file_time_type::min();
    static fs::file_time_type lastUnusualLootWrite = fs::file_time_type::min();

	WatchedFile files[] = {
		{ "csgo_gc/inventory.txt",         lastInventoryWrite,   &ClientGC::ReloadInventory,       0 },
		{ "csgo_gc/config.txt",            lastConfigWrite,      &ClientGC::ReloadConfig,          1 },
		{ "csgo_gc/price_sheet.txt",       lastPriceSheetWrite,  &ClientGC::ReloadPriceSheet,      2 },
		{ "csgo_gc/passes.txt",            lastPassesWrite,      &ClientGC::ReloadPasses,          3 },
		{ "csgo_gc/unusual_loot_lists.txt",lastUnusualLootWrite, &ClientGC::ReloadUnusualLootLists,4 }
	};

    for (auto& file : files) {
        if (!fs::exists(file.path))
            continue;

        auto currentWrite = fs::last_write_time(file.path);

        if (firstRun) {
            // запоминаем и идём дальше, ничего не перезагружаем
            file.lastWrite = currentWrite;
            continue;
        }

		if (currentWrite != file.lastWrite) {
			file.lastWrite = currentWrite;
			Platform::Print("%s changed - posting reload to GC thread\n",
							file.path.filename().string().c_str());
			PostToGC(GCEvent::ReloadFile, file.reloadIndex, nullptr, 0);
		}
    }

    firstRun = false;

    UpdateCooldown();

    if (m_partyLobby.active)
    {
        auto now = std::chrono::steady_clock::now();

        // the host sends heartbeat every 2 secs
        if (AccountId() == m_partyLobby.hostAccountId
            && now >= m_nextPartyHeartbeat)
        {
            SendPartyHeartbeat();
            m_nextPartyHeartbeat = now + std::chrono::seconds(2);
        }

        // LTP entries expire after 30s on the worker; re-register every 15s
        // so we stay visible in other players' searches. Also acts as a
        // retry if the initial register in OnPartyRegister() failed.
        if (LtpEnabled() && now >= m_nextLtpRefresh)
        {
            QueueLtpRefresh();
            m_nextLtpRefresh = now + std::chrono::seconds(15);
        }

        CheckPartyHostAlive();
    }

    m_chatLimiter.Prune();
    m_profileLimiter.Prune();


    {
        std::lock_guard<std::mutex> lock(m_pendingProfileMutex);
        auto now = std::chrono::steady_clock::now();
        for (auto it = m_pendingProfileQueries.begin();
             it != m_pendingProfileQueries.end();)
        {
			if (it->second.deadline < now)
			{
				Platform::PrintWarning("Profile query for %u timed out, fetching from worker\n",
					it->second.targetAccountId);

				const uint32_t target = it->second.targetAccountId;
				it = m_pendingProfileQueries.erase(it);

				if (QueryRemoteProfile(target))
					continue;

				SendUnknownProfile(target);
			}
            else
            {
                ++it;
            }
        }
    }
}

void ClientGC::ReloadInventory()
{
    m_inventory.ReloadFromFile();
    SendInventoryUpdate();
}

void ClientGC::ReloadConfig()
{
    GetConfig().ReloadFromFile();

    if (ApplyConfiguredCooldown())
    {
        if (m_isCooldownActive || m_cooldownExpiredLocally)
            SendCompetitiveCooldown();
    }

    SendInventoryUpdate();
    SendRankUpdate();
    SendMatchmakingHelloUpdate();

    // config might have changed level / xp / rank / prime / etc — re-publish
    QueueProfilePublish();
}

void ClientGC::ReloadPriceSheet()
{
    m_priceSheet.Clear();
    if (!m_priceSheet.ParseFromFile("csgo_gc/price_sheet.txt"))
    {
        Platform::PrintError("ReloadPriceSheet: failed to load price_sheet.txt\n");
        return;
    }
    Platform::PrintDebug("ReloadPriceSheet: price sheet reloaded\n");
}

void ClientGC::ReloadPasses()
{
    m_passes.Clear();
    if (!m_passes.ParseFromFile("csgo_gc/passes.txt"))
    {
        Platform::PrintError("ReloadPasses: failed to load passes.txt\n");
        return;
    }
    Platform::PrintDebug("ReloadPasses: pass definitions reloaded\n");
}

void ClientGC::ReloadUnusualLootLists()
{
    m_unusualLootLists.Clear();
    if (!m_unusualLootLists.ParseFromFile("csgo_gc/unusual_loot_lists.txt"))
    {
        Platform::PrintError("ReloadUnusualLootLists: failed to load unusual_loot_lists.txt\n");
        return;
    }
    Platform::PrintDebug("ReloadUnusualLootLists: unusual loot lists reloaded\n");
}

uint32_t ClientGC::EffectiveAccountId() const
{
    return AccountId();
}

void ClientGC::QueryPartyMemberProfile(uint32_t accountId)
{
    if (!m_party) return;
    if (accountId == AccountId()) return;
    std::lock_guard<std::mutex> lock(m_pendingProfileMutex);

    uint32_t nonce = Random{}.Integer<uint32_t>();
    PendingProfileQuery pending{ accountId, nonce,
        std::chrono::steady_clock::now() + std::chrono::seconds(2) };

    // like ClientRequestPlayersProfile — don't let the queue grow unbounded
    for (auto it = m_pendingProfileQueries.begin(); it != m_pendingProfileQueries.end();)
    {
        if (it->second.deadline < std::chrono::steady_clock::now())
            it = m_pendingProfileQueries.erase(it);
        else ++it;
    }

    m_pendingProfileQueries[nonce] = pending;
    m_party->SendProfileQuery(accountId, AccountId(), nonce);
}




std::vector<uint32_t> ClientGC::MergeQueueRoster(const std::vector<uint32_t> &requestIds) const
{
    std::vector<uint32_t> out;
    out.reserve(requestIds.size()
              + m_partyLobby.memberAccountIds.size()
              + m_nativeLobbyMembers.size());

    // request first: it's the freshest source. rosters after: they might
    // have members the request omitted (or be stale, in which case the
    // request still wins for anyone in both).
    for (uint32_t id : requestIds)                     out.push_back(id);
    for (uint32_t id : m_partyLobby.memberAccountIds)  out.push_back(id);
    for (uint32_t id : m_nativeLobbyMembers)           out.push_back(id);
    return out;
}

bool ClientGC::AnyPartyMemberVacBanned(const std::vector<uint32_t> &accountIds,
                                       uint32_t &bannedAccountId)
{
    for (uint32_t memberId : accountIds)
    {
        if (memberId == AccountId()) continue;

        // No cache: every check queries the worker for the current state.
        RemoteProfile profile;
        if (!FetchRemoteProfile(memberId, profile))
            continue;

        if (profile.vacBanned)
        {
            bannedAccountId = memberId;
            return true;
        }
    }
    return false;
}

bool ClientGC::AnyPartyMemberOnCooldown(const std::vector<uint32_t> &accountIds,
                                        uint32_t &cooldownAccountId,
                                        uint32_t &cooldownReason,
                                        uint32_t &cooldownSeconds)
{
    for (uint32_t memberId : accountIds)
    {
        if (memberId == AccountId()) continue;

        RemoteProfile profile;
        if (!FetchRemoteProfile(memberId, profile))
            continue;

        if (profile.penaltySeconds != 0)
        {
            cooldownAccountId = memberId;
            cooldownReason    = profile.penaltyReason;
            cooldownSeconds   = profile.penaltySeconds;
            return true;
        }
    }
    return false;
}

void ClientGC::SendPartyHeartbeat()
{
    if (!m_party || !m_partyLobby.active) return;

    PartyHeartbeatPayload p{ m_partyLobby.hostAccountId,
                             m_partyLobby.lobbyId,
                             static_cast<uint32_t>(m_partyLobby.memberAccountIds.size()) };
    for (uint32_t member : m_partyLobby.memberAccountIds)
    {
        if (member == AccountId()) continue;
        m_party->SendHeartbeat(member, p.hostAccountId, p.lobbyId, p.memberCount);
    }
}

void ClientGC::CheckPartyHostAlive()
{
    if (!m_partyLobby.active) return;
    if (AccountId() == m_partyLobby.hostAccountId) return; // мы и есть хост

    auto now = std::chrono::steady_clock::now();
    float sinceHeartbeat =
        std::chrono::duration<float>(now - m_partyLobby.lastHostHeartbeat).count();

    // 10 секунд тишины — считаем хост мёртвым
    if (sinceHeartbeat < 10.0f) return;

    // детерминированные выборы: минимальный account id среди членов (кроме хоста)
    uint32_t newHost = 0;
    for (uint32_t m : m_partyLobby.memberAccountIds)
    {
        if (m == m_partyLobby.hostAccountId) continue;
        if (newHost == 0 || m < newHost) newHost = m;
    }

    if (newHost == 0 || newHost != AccountId())
    {
        // не мы избраны — просто сбрасываем таймер, чтобы не спамить
        m_partyLobby.lastHostHeartbeat = now;
        return;
    }

    Platform::PrintWarning("Party: host %u timed out, promoting self (%u)\n",
        m_partyLobby.hostAccountId, AccountId());

    m_partyLobby.hostAccountId = AccountId();
    m_partyLobby.leaderAccountId = AccountId();
    m_partyLobby.lastHostHeartbeat = now;
    m_nextPartyHeartbeat = now;

    // сообщаем игровому клиенту, что хост сменился
    CMsgGCCStrike15_v2_Party_Register reg;
    reg.set_id(AccountId());
    reg.set_ver(m_partyLobby.ver);
    reg.set_apr(GetConfig().HasPrime() ? 1 : 0);
    reg.set_ark(static_cast<uint32_t>(GetConfig().CompetitiveRank()) * 10);
    reg.set_grp(3);
    reg.set_slots(static_cast<uint32_t>(PartyMaxMembers));
    reg.set_launcher(m_partyLobby.launcher);
    reg.set_game_type(m_partyLobby.gameType);
    SendMessageToGame(false, k_EMsgGCCStrike15_v2_Party_Register, reg);
}

void ClientGC::OnRemoteHeartbeat(uint32_t hostAccountId, uint32_t lobbyId)
{
    if (!m_partyLobby.active || m_partyLobby.lobbyId != lobbyId) return;
    if (hostAccountId != m_partyLobby.hostAccountId) return; // stale

    m_partyLobby.lastHostHeartbeat = std::chrono::steady_clock::now();
}

void ClientGC::OnPartyRegister(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_Party_Register message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::PrintWarning("Failed to parse Party_Register\n");
        return;
    }

    // client sends Party_Register with id=0 when creating a lobby.
    // we need to assign a non-zero id and echo it back.
    uint32_t lobbyId = message.id();
    if (lobbyId == 0)
    {
        // generate a random lobby id. can't be 0, the client treats that as
        // "not registered".
        do { lobbyId = Random{}.Integer<uint32_t>(); } while (lobbyId == 0);
    }

    m_partyLobby.lobbyId    = lobbyId;
    m_partyLobby.hostAccountId = EffectiveAccountId();
    m_partyLobby.gameType   = message.game_type();
    m_partyLobby.ver        = message.ver();
    m_partyLobby.launcher   = message.launcher();
    m_partyLobby.active     = true;

    // send the client an echo with a valid id and minimal settings.
    CMsgGCCStrike15_v2_Party_Register response;
    response.set_id(EffectiveAccountId());
    response.set_ver(message.ver());
    response.set_apr(GetConfig().HasPrime() ? 1 : 0);
    response.set_ark(static_cast<uint32_t>(GetConfig().CompetitiveRank()) * 10);
    response.set_grp(3);
    response.set_slots(5);                 // mm max
    response.set_launcher(message.launcher());
    response.set_game_type(message.game_type());

    SendMessageToGame(false, k_EMsgGCCStrike15_v2_Party_Register, response);

    Platform::PrintDebug("Party_Register: assigned lobby=%u host=%u game_type=%u\n",
        lobbyId, m_partyLobby.hostAccountId, m_partyLobby.gameType);

    // tell the LTP worker we're looking to play. blocking call, 2s timeout.
    if (LtpEnabled())
    {
        // fire the console test before the HTTP call so we still see it
        // even if the worker is unreachable
        RunConsoleCommand("echo [ECHO TEST]");

        HttpResult res = HttpPostJson(LtpBase() + "/register", BuildLtpRegisterBody());
        if (res.status == 0)
        {
            Platform::PrintError("LTP register: worker unreachable\n");
            // leave m_nextLtpRefresh unset — the upkeep tick in
            // CheckFileReloads() will retry on the next pass.
        }
        else if (res.status != 200)
        {
            Platform::Print("LTP register: HTTP %d\n", res.status);
        }
        else
        {
            Platform::PrintDebug("LTP register: lobby=%u game_type=%u\n",
                lobbyId, message.game_type());

            m_nextLtpRefresh = std::chrono::steady_clock::now()
                             + std::chrono::seconds(15);
        }
    }
}


std::string ClientGC::BuildLtpRegisterBody()
{
    return std::string("{")
        + "\"accountId\":" + std::to_string(EffectiveAccountId()) + ","
        + "\"lobbyId\":"   + std::to_string(m_partyLobby.lobbyId) + ","
        + "\"gameType\":"  + std::to_string(m_partyLobby.gameType) + ","
        + "\"rank\":"      + std::to_string(static_cast<uint32_t>(GetConfig().CompetitiveRank())) + ","
        + "\"prime\":"     + (GetConfig().HasPrime() ? "true" : "false")
        + "}";
}

void ClientGC::QueueLtpRefresh()
{
	// sanity
    if (!m_partyLobby.active || m_partyLobby.lobbyId == 0)
        return;

    std::string body = BuildLtpRegisterBody();

    {
        std::lock_guard<std::mutex> lock(m_httpMutex);
        m_httpLtpRefreshBody    = std::move(body);
        m_httpLtpRefreshPending = true;
    }
    m_httpCv.notify_one();
}

void ClientGC::DoLtpRefresh()
{
    std::string body;
    {
        std::lock_guard<std::mutex> lock(m_httpMutex);
        body = m_httpLtpRefreshBody;
    }

    HttpResult res = HttpPostJson(LtpBase() + "/register", body);
    if (res.status == 200)
        Platform::PrintDebug("LTP refresh sent\n");
    else
        Platform::PrintWarning("LTP refresh failed (HTTP %d)\n", res.status);
}

void ClientGC::OnPartyUnregister(GCMessageRead &messageRead)
{
    (void)messageRead;
    Platform::PrintDebug("Party_Unregister: lobby=%u\n", m_partyLobby.lobbyId);

    // pull ourselves out before we forget the lobby id. if the worker is
    // down the entry just sits there until the sweep drops it after 30s.
    if (LtpEnabled() && m_partyLobby.lobbyId)
    {
        std::string body = "{\"lobbyId\":" + std::to_string(m_partyLobby.lobbyId) + "}";
        HttpResult res = HttpPostJson(LtpBase() + "/unregister", body);
        if (res.status == 200)
            Platform::PrintDebug("LTP unregister: lobby=%u\n", m_partyLobby.lobbyId);
        else
            Platform::PrintError("LTP unregister failed (HTTP %d)\n", res.status);
    }

    m_partyLobby = PartyLobby{};

    // cancel the LTP keep-alive; a future Register will re-arm it.
    m_nextLtpRefresh = {};
}

void ClientGC::OnPartyInvite(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_Party_Invite message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::PrintWarning("Failed to parse Party_Invite\n");
        return;
    }

    uint32_t lobbyId = message.lobbyid();
    PostToHost(HostEvent::PartyInvite, message.accountid(), &lobbyId, sizeof(lobbyId));

    Platform::PrintDebug("Party_Invite to account %u (lobby %u)\n",
        message.accountid(), message.lobbyid());
}

void ClientGC::OnClientPartyJoinRelay(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_ClientPartyJoinRelay message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::PrintWarning("Failed to parse ClientPartyJoinRelay\n");
        return;
    }

    uint32_t lobbyId = static_cast<uint32_t>(message.lobbyid());
    PostToHost(HostEvent::PartyJoinRelay, message.accountid(), &lobbyId, sizeof(lobbyId));

    Platform::PrintDebug("ClientPartyJoinRelay to host account %u\n", message.accountid());
}

void ClientGC::OnClientPartyWarning(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_ClientPartyWarning message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::PrintWarning("Failed to parse ClientPartyWarning\n");
        return;
    }

    for (int i = 0; i < message.entries_size(); i++)
    {
        const auto &e = message.entries(i);
        Platform::Print("PartyWarning: account %u warntype %u\n", e.accountid(), e.warntype());
    }
}

void ClientGC::OnRemotePartyInvite(uint32_t fromAccountId, uint32_t lobbyId, uint32_t gameType)
{
    CMsgInvitationCreated notification;
    notification.set_group_id(lobbyId);
    notification.set_steam_id(CSteamID(fromAccountId, k_EUniversePublic, k_EAccountTypeIndividual).ConvertToUint64());

    SendMessageToGame(false, k_EMsgGCInvitationCreated, notification);

    Platform::PrintDebug("Remote invite from %u to lobby %u (game_type %u)\n",
        fromAccountId, lobbyId, gameType);
}

void ClientGC::OnRemoteJoinRelay(uint32_t fromAccountId, uint32_t lobbyId)
{
    if (!m_partyLobby.active || m_partyLobby.lobbyId != lobbyId)
    {
        Platform::PrintWarning("Remote join relay for unknown lobby %u (mine %u)\n",
            lobbyId, m_partyLobby.lobbyId);
        return;
    }

    auto &members = m_partyLobby.memberAccountIds;
    bool present = std::find(members.begin(), members.end(), fromAccountId) != members.end();
    if (present) return;

    if (members.size() + 1 > PartyMaxMembers)
    {
        Platform::PrintWarning("Party full, rejecting %u\n", fromAccountId);
        if (m_party && fromAccountId != AccountId())
        {
            m_party->SendKick(fromAccountId, m_partyLobby.hostAccountId,
                m_partyLobby.lobbyId, fromAccountId);
        }
        return;
    }
    QueryPartyMemberProfile(fromAccountId);
    members.push_back(fromAccountId);
    Platform::PrintSuccess("Added member %u to lobby %u (%zu total)\n",
        fromAccountId, lobbyId, members.size());

    // рассылаем обновление всем
    std::vector<uint32_t> snapshot = members;
    for (uint32_t member : snapshot)
    {
        if (member == EffectiveAccountId()) continue;
        PostToHost(HostEvent::PartyLobbyUpdate, member,
            snapshot.data(), static_cast<uint32_t>(snapshot.size() * sizeof(uint32_t)));
    }
	
}

void ClientGC::OnRemoteLobbyUpdate(uint32_t /*fromAccountId*/, uint32_t lobbyId,
    const std::vector<uint32_t> &members)
{
    // мы уже в другом лобби — этот апдейт не наш
    if (m_partyLobby.active && m_partyLobby.lobbyId != lobbyId)
        return;

    // гость: впервые видим этот лобби, принимаем его как свой.
    // hostId best-effort — первый member, который не мы.
    // (хост на своей стороне уже установил эти поля через OnPartyRegister,
    //  так что если lobbyId совпал — этот блок не выполнится)
    if (!m_partyLobby.active)
    {
        m_partyLobby.lobbyId = lobbyId;
        m_partyLobby.active = true;
        for (uint32_t m : members)
        {
            if (m != AccountId()) { m_partyLobby.hostAccountId = m; break; }
        }
        Platform::PrintSuccess("Party: adopted lobby %u as guest, host=%u\n",
            lobbyId, m_partyLobby.hostAccountId);
    }

    m_partyLobby.memberAccountIds = members;


    Platform::PrintDebug("Lobby %u updated: %zu members\n", lobbyId, members.size());
}
// item preview (9156/9157)

void ClientGC::OnClient2GCEconPreviewDataBlockRequest(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_Client2GCEconPreviewDataBlockRequest request;
    if (!messageRead.ReadProtobuf(request))
    {
        Platform::PrintWarning("Parsing Client2GCEconPreviewDataBlockRequest failed, ignoring\n");
        return;
    }

    // param_s = item id, param_a = owner account id (may be 0 for market
    // listings), param_m/param_d = match / listing identifiers.
    const uint64_t itemId = request.param_s();
    const uint32_t owner  = static_cast<uint32_t>(request.param_a());

    Platform::PrintDebug("EconPreviewRequest: item=%llu owner=%u match=%llu\n",
        static_cast<unsigned long long>(itemId),
        owner,
        static_cast<unsigned long long>(request.param_m()));

    CMsgGCCStrike15_v2_Client2GCEconPreviewDataBlockResponse response;

    const CSOEconItem *item = m_inventory.GetItem(itemId);
    if (item)
    {
        m_inventory.ItemToPreviewDataBlock(*item, *response.mutable_iteminfo());
    }
    else if (itemId != 0
          && (itemId & ItemIdDefaultItemMask) != ItemIdDefaultItemMask
          && !GetConfig().PartyServerUrl().empty())
    {
        // Not in our own inventory — try the worker. This covers scoreboard
        // inspects, lobby-member inspects, and market listings for players
        // whose client published their equipped items.
        CEconItemPreviewDataBlock block;
        if (QueryRemoteItem(itemId, block))
        {
            *response.mutable_iteminfo() = std::move(block);
            Platform::PrintDebug("EconPreviewRequest: got remote block for %llu\n",
                static_cast<unsigned long long>(itemId));
        }
        else
        {
            Platform::PrintDebug("EconPreviewRequest: item %llu not published\n",
                static_cast<unsigned long long>(itemId));
        }
    }

    SendMessageToGame(false, k_EMsgGCCStrike15_v2_Client2GCEconPreviewDataBlockResponse, response);
}

// steam datagram relay ticket (9178/9177)

void ClientGC::OnClientToGCRequestTicket(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_ClientToGCRequestTicket request;
    if (!messageRead.ReadProtobuf(request))
    {
        Platform::PrintWarning("Parsing ClientToGCRequestTicket failed, ignoring\n");
        return;
    }

    Platform::PrintDebug("ClientToGCRequestTicket: auth=%llu authip=%u gs=%llu routing=%s\n",
        static_cast<unsigned long long>(request.authorized_steam_id()),
        request.authorized_public_ip(),
        static_cast<unsigned long long>(request.gameserver_steam_id()),
        request.gameserver_sdr_routing().c_str());

    // a real SDR ticket is signed with Valve's private key - we can't
    // generate it. send an empty one, the client falls back to direct connect.
    CMsgGCToClientSteamDatagramTicket response;
    SendMessageToGame(false, k_EMsgGCCStrike15_v2_GCToClientSteamdatagramTicket, response);
}

// client var value notification (9144)

void ClientGC::OnClientVarValueNotification(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_ClientVarValueNotificationInfo message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::PrintWarning("Parsing ClientVarValueNotification failed, ignoring\n");
        return;
    }

    Platform::PrintDebug("ClientVarValueNotification: name=%s value=%d server=%u:%u choked=%d\n",
        message.value_name().c_str(),
        message.value_int(),
        message.server_addr(),
        message.server_port(),
        message.choked_blocks_size());
}

// report abuse (1065/1066)

void ClientGC::OnReportAbuse(GCMessageRead &messageRead)
{
    CMsgGCReportAbuse message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::PrintWarning("Parsing CMsgGCReportAbuse failed, ignoring\n");
        return;
    }

    Platform::PrintDebug("ReportAbuse: target=%llu type=%u content=%u gid=%llu desc=%s\n",
        static_cast<unsigned long long>(message.target_steam_id()),
        message.abuse_type(),
        message.content_type(),
        static_cast<unsigned long long>(message.gid()),
        message.description().c_str());

    CMsgGCReportAbuseResponse response;
    response.set_target_steam_id(message.target_steam_id());
    response.set_result(0); // k_EGCMsgResponseOK
    SendMessageToGame(false, k_EMsgGC_ReportAbuseResponse, response);
}

// gc text messages (9135/9134)

void ClientGC::OnClient2GCTextMsg(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_Client2GCTextMsg message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::PrintWarning("Parsing Client2GCTextMsg failed, ignoring\n");
        return;
    }

    Platform::PrintDebug("Client2GCTextMsg: id=%u args=%d\n",
        message.id(),
        message.args_size());

    // echo back with the same id so the client doesn't hang waiting.
    // payload is empty - the app decides what to do.
    CMsgGCCStrike15_v2_GC2ClientTextMsg response;
    response.set_id(message.id());
    response.set_type(0);
    SendMessageToGame(false, k_EMsgGCCStrike15_v2_GC2ClientTextMsg, response);
}

// watch / spectate (9126, 9138)

void ClientGC::OnWatchInfoUsers(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_WatchInfoUsers request;
    if (!messageRead.ReadProtobuf(request))
    {
        Platform::PrintWarning("Parsing WatchInfoUsers failed, ignoring\n");
        return;
    }

    Platform::PrintDebug("WatchInfoUsers: request=%u accounts=%d extended=%u\n",
        request.request_id(),
        request.account_ids_size(),
        request.extended_timeout());

    CMsgGCCStrike15_v2_WatchInfoUsers response;
    response.set_request_id(request.request_id());
    for (int i = 0; i < request.account_ids_size(); i++)
    {
        response.add_account_ids(request.account_ids(i));
    }
    // watchable_match_infos is left empty - players have no live matches

    SendMessageToGame(false, k_EMsgGCCStrike15_v2_WatchInfoUsers, response);
}

void ClientGC::OnClientRequestWatchInfoFriends2(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_ClientRequestWatchInfoFriends request;
    if (!messageRead.ReadProtobuf(request))
    {
        Platform::PrintWarning("Parsing ClientRequestWatchInfoFriends2 failed, ignoring\n");
        return;
    }

    Platform::PrintDebug("ClientRequestWatchInfoFriends2: request=%u accounts=%d serverid=%llu\n",
        request.request_id(),
        request.account_ids_size(),
        static_cast<unsigned long long>(request.serverid()));

    CMsgGCCStrike15_v2_WatchInfoUsers response;
    response.set_request_id(request.request_id());
    for (int i = 0; i < request.account_ids_size(); i++)
    {
        response.add_account_ids(request.account_ids(i));
    }
    SendMessageToGame(false, k_EMsgGCCStrike15_v2_WatchInfoUsers, response);
}

// offers / favorites (9195, 9200)

void ClientGC::OnClientRequestOffers(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_ClientRequestOffers request;
    if (!messageRead.ReadProtobuf(request))
    {
        Platform::PrintWarning("Parsing ClientRequestOffers failed, ignoring\n");
        return;
    }
    Platform::Print("ClientRequestOffers\n");
    // no reply message for this one, nothing to answer with
}

void ClientGC::OnSetEventFavorite(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_SetEventFavorite message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::PrintWarning("Parsing SetEventFavorite failed, ignoring\n");
        return;
    }
    Platform::PrintDebug("SetEventFavorite: event=%llu fav=%d\n",
        static_cast<unsigned long long>(message.eventid()),
        message.is_favorite());
    // no reply message either
}

// item schema / sort / picked up (1049, 1041, 1071)

void ClientGC::OnUpdateItemSchema(GCMessageRead &messageRead)
{
    CMsgUpdateItemSchema message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::PrintWarning("Parsing UpdateItemSchema failed, ignoring\n");
        return;
    }
    Platform::Print("UpdateItemSchema: version=%u url=%s items_game=%zu bytes\n",
        message.item_schema_version(),
        message.items_game_url().c_str(),
        message.items_game().size());
}

void ClientGC::OnSortItems(GCMessageRead &messageRead)
{
    CMsgSortItems message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::PrintWarning("Parsing SortItems failed, ignoring\n");
        return;
    }
    Platform::PrintDebug("SortItems: type=%u\n", message.sort_type());
    // client sorts locally, there's no server response
}

void ClientGC::OnShowItemsPickedUp(GCMessageRead &messageRead)
{
    CMsgGCShowItemsPickedUp message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::PrintWarning("Parsing ShowItemsPickedUp failed, ignoring\n");
        return;
    }
    Platform::PrintDebug("ShowItemsPickedUp: player=%llu\n",
        static_cast<unsigned long long>(message.player_steamid()));
}

// connection status (4009/4010)

void ClientGC::OnClientConnectionStatus(GCMessageRead &messageRead)
{
    CMsgConnectionStatus message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::PrintWarning("Parsing ClientConnectionStatus failed, ignoring\n");
        return;
    }
    Platform::PrintDebug("ClientConnectionStatus: status=%d queue=%d/%d wait=%d est=%d\n",
        message.status(),
        message.queue_position(),
        message.queue_size(),
        message.wait_seconds(),
        message.estimated_wait_seconds_remaining());
}

// legacy party invites (4501-4505)

void ClientGC::OnInviteToParty(GCMessageRead &messageRead)
{
    CMsgInviteToParty message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::PrintWarning("Parsing InviteToParty failed, ignoring\n");
        return;
    }

    Platform::PrintDebug("InviteToParty: steam=%llu version=%u team=%u\n",
        static_cast<unsigned long long>(message.steam_id()),
        message.client_version(),
        message.team_invite());

    // echo back as InvitationCreated - the client will draw the invite
    CMsgInvitationCreated notification;
    notification.set_group_id(0);
    notification.set_steam_id(message.steam_id());
    SendMessageToGame(false, k_EMsgGCInvitationCreated, notification);
}

void ClientGC::OnPartyInviteResponse(GCMessageRead &messageRead)
{
    CMsgPartyInviteResponse message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::PrintWarning("Parsing PartyInviteResponse failed, ignoring\n");
        return;
    }
    Platform::PrintDebug("PartyInviteResponse: party=%llu accept=%d version=%u team=%u\n",
        static_cast<unsigned long long>(message.party_id()),
        message.accept(),
        message.client_version(),
        message.team_invite());
}

void ClientGC::OnKickFromParty(GCMessageRead &messageRead)
{
    CMsgKickFromParty message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::PrintWarning("Parsing KickFromParty failed, ignoring\n");
        return;
    }
    Platform::PrintDebug("KickFromParty: steam=%llu\n",
        static_cast<unsigned long long>(message.steam_id()));
}

void ClientGC::OnLeaveParty(GCMessageRead &messageRead)
{
    CMsgLeaveParty message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::PrintWarning("Parsing LeaveParty failed, ignoring\n");
        return;
    }
    Platform::Print("LeaveParty\n");
}

// replicate convars / system broadcast / errors (4001, 4002, 4509)

void ClientGC::OnReplicateConVars(GCMessageRead &messageRead)
{
    CMsgReplicateConVars message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::PrintWarning("Parsing ReplicateConVars failed, ignoring\n");
        return;
    }
    Platform::PrintDebug("ReplicateConVars: %d entries\n", message.convars_size());
    for (int i = 0; i < message.convars_size(); i++)
    {
        const CMsgConVarValue &cv = message.convars(i);
        Platform::Print("  %s = %s\n", cv.name().c_str(), cv.value().c_str());
    }
}

void ClientGC::OnSystemBroadcast(GCMessageRead &messageRead)
{
    CMsgSystemBroadcast message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::PrintWarning("Parsing SystemBroadcast failed, ignoring\n");
        return;
    }
    Platform::Print("SystemBroadcast: %s\n", message.message().c_str());
}


// private queues (9214) - no protobuf

void ClientGC::OnPrivateQueues(GCMessageRead &messageRead)
{
    (void)messageRead;
    Platform::PrintWarning("PrivateQueues (no proto, ignoring payload)\n");
}

// 9123 - the client is asking us to look up commendation info for a player.
void ClientGC::OnClientCommendPlayerQuery(GCMessageRead &messageRead)
{
    (void)messageRead;
    Platform::PrintDebug("ClientGC: ClientCommendPlayerQuery received\n");

    const bool prime = GetConfig().HasPrime();

    CMsgGCCStrike15_v2_MatchmakingGC2ClientHello response;
    response.set_account_id(EffectiveAccountId());
    response.mutable_commendation()->set_cmd_friendly(GetConfig().CommendedFriendly());
    response.mutable_commendation()->set_cmd_teaching(GetConfig().CommendedTeaching());
    response.mutable_commendation()->set_cmd_leader(GetConfig().CommendedLeader());
    response.set_player_level(prime ? GetConfig().Level() : 0);
    response.set_player_cur_xp(prime ? GetConfig().Xp() : 0);

    SendMessageToGame(false, k_EMsgGCCStrike15_v2_ClientCommendPlayerQueryResponse, response);
}

// 9196 - the client is asking for our wallet balance. reply with 10000.
void ClientGC::SendClientAccountBalance()
{
    CMsgGCCStrike15_v2_ClientAccountBalance response;
    response.set_amount(10000);
    response.set_url("");

    SendMessageToGame(false, k_EMsgGCCStrike15_v2_ClientAccountBalance, response);
    Platform::PrintSuccess("ClientGC: sent account balance = 10000\n");
}

// 9202 - perf counters from the client. purely informational.
void ClientGC::OnClientPerfReport(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_ClientPerfReport message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::PrintWarning("Parsing ClientPerfReport failed, ignoring\n");
        return;
    }

    Platform::PrintDebug("ClientGC: ClientPerfReport entries=%d\n", message.entries_size());

    for (int i = 0; i < message.entries_size(); i++)
    {
        const CMsgGCCStrike15_v2_ClientPerfReport_Entry &entry = message.entries(i);
        Platform::Print("  [%d] counter=%u length=%u src=%u status=%u\n",
            i,
            entry.perfcounter(),
            entry.length(),
            entry.sourceid(),
            entry.status());
    }
}

// 9206 - the GC is telling the client to refuse trusted mode. we always accept.
void ClientGC::OnGC2ClientRefuseSecureMode(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_GC2ClientRefuseSecureMode message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::PrintWarning("Parsing GC2ClientRefuseSecureMode failed, ignoring\n");
        return;
    }

    Platform::Print("ClientGC: RefuseSecureMode kick=%d offer_secure=%d offer_insecure=%d "
                    "trusted=%d unsigned=%d warn1=%d warn2=%d report=%s\n",
        message.kick_user(),
        message.offer_secure_mode(),
        message.offer_insecure_mode(),
        message.show_trusted_ui(),
        message.show_unsigned_ui(),
        message.show_warning_not_trusted(),
        message.show_warning_not_trusted_2(),
        message.file_report().c_str());

    // we always accept the refusal. no reply message exists.
}

// 1068 - a player named an item, we just observe.
void ClientGC::OnNameItemNotification(GCMessageRead &messageRead)
{
    CMsgGCNameItemNotification message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::PrintWarning("Parsing NameItemNotification failed, ignoring\n");
        return;
    }

    Platform::Print("ClientGC: NameItemNotification player=%llu def=%u name=%s\n",
        static_cast<unsigned long long>(message.player_steamid()),
        message.item_def_index(),
        message.item_name_custom().c_str());
}

// 1073 - apply a strange part to an item.
void ClientGC::ApplyStrangePart(GCMessageRead &messageRead)
{
    CMsgApplyStrangePart message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::PrintWarning("Parsing ApplyStrangePart failed, ignoring\n");
        return;
    }

    Platform::PrintWarning("ClientGC: ApplyStrangePart part=%llu item=%llu (not applied, inventory API limit)\n",
        static_cast<unsigned long long>(message.strange_part_item_id()),
        static_cast<unsigned long long>(message.item_item_id()));
}

// 1091 - direct attribute modification.
void ClientGC::ModifyItemAttribute(GCMessageRead &messageRead)
{
    CMsgModifyItemAttribute message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::PrintWarning("Parsing ModifyItemAttribute failed, ignoring\n");
        return;
    }

    Platform::PrintWarning("ClientGC: ModifyItemAttribute item=%llu def=%u value=%u (not applied, inventory API limit)\n",
        static_cast<unsigned long long>(message.item_id()),
        message.attr_defidx(),
        message.attr_value());
}

// 1062 - legacy acknowledgement from the client.
void ClientGC::ItemAcknowledgedDeprecated(GCMessageRead &messageRead)
{
    CMsgItemAcknowledged__DEPRECATED message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::PrintWarning("Parsing ItemAcknowledged__DEPRECATED failed, ignoring\n");
        return;
    }

    Platform::PrintWarning("ClientGC: ItemAcknowledged__DEPRECATED account=%u item=%llu def=%u rarity=%u\n",
        message.account_id(),
        static_cast<unsigned long long>(message.item_id()),
        message.def_index(),
        message.rarity());
}

// ---- join-friend / GOTV / store ----

void ClientGC::OnClientRequestJoinFriendData(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_ClientRequestJoinFriendData request;
    if (!messageRead.ReadProtobuf(request))
    {
        Platform::PrintWarning("Parsing ClientRequestJoinFriendData failed, ignoring\n");
        return;
    }

    Platform::PrintDebug("ClientRequestJoinFriendData: version=%u account=%u token=%u ipp=%u\n",
        request.version(),
        request.account_id(),
        request.join_token(),
        request.join_ipp());

    // we don't do matchmaking, so there's never a live session to join.
    // answer with errormsg so the client doesn't hang forever.
    CMsgGCCStrike15_v2_ClientRequestJoinFriendData response;
    response.set_version(request.version());
    response.set_account_id(request.account_id());
    response.set_join_token(request.join_token());
    response.set_join_ipp(request.join_ipp());
    response.set_errormsg("Unable to connect: not supported.");

    SendMessageToGame(false, k_EMsgGCCStrike15_v2_ClientRequestJoinFriendData, response,
        messageRead.JobId());
}

void ClientGC::OnGotvSyncPacket(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_GotvSyncPacket message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::PrintWarning("Parsing GotvSyncPacket failed, ignoring\n");
        return;
    }

    const CEngineGotvSyncPacket &data = message.data();
    Platform::PrintDebug("GotvSyncPacket: match=%llu instance=%u tick=%u frag=%u/%u rate=%.2f delay=%.2f\n",
        static_cast<unsigned long long>(data.match_id()),
        data.instance_id(),
        data.tick(),
        data.currentfragment(),
        data.signupfragment(),
        data.tickrate(),
        data.rtdelay());
}

void ClientGC::OnStorePurchaseCancel(GCMessageRead &messageRead)
{
    CMsgGCStorePurchaseCancel request;
    if (!messageRead.ReadProtobuf(request))
    {
        Platform::PrintWarning("Parsing StorePurchaseCancel failed, ignoring\n");
        return;
    }

    Platform::PrintDebug("StorePurchaseCancel: txn=%llu (ours %llu)\n",
        static_cast<unsigned long long>(request.txn_id()),
        static_cast<unsigned long long>(m_transactionId));

    CMsgGCStorePurchaseCancelResponse response;

    if (m_transactionId && request.txn_id() == m_transactionId)
    {
        // we already added the items to the inventory in StorePurchaseInit.
        // proper impl would remove them again, but the init path doesn't keep
        // a mapping from txn -> item ids beyond m_transactionItemIds, so
        // we just forget the txn here.
        m_transactionId = 0;
        m_transactionItemIds.clear();
        response.set_result(1);
    }
    else
    {
        response.set_result(0);
    }

    SendMessageToGame(false, k_EMsgGCStorePurchaseCancelResponse, response, messageRead.JobId());
}

void ClientGC::ClearNativeLobbyRoster()
{
    // size==0 is the "clear" sentinel — PostNativeLobbyRoster always sends
    // at least sizeof(uint32_t), so a zero-length post is unambiguous.
    PostToGC(GCEvent::NativeLobbyUpdate, 0, nullptr, 0);
}

void ClientGC::OnStorePurchaseQueryTxn(GCMessageRead &messageRead)
{
    // no proto for this one; client sends a raw uint64 txn id
    uint64_t txnId = messageRead.ReadUint64();
    if (!messageRead.IsValid())
    {
        Platform::PrintWarning("Parsing StorePurchaseQueryTxn failed, ignoring\n");
        return;
    }

    Platform::PrintDebug("StorePurchaseQueryTxn: txn=%llu (ours %llu)\n",
        static_cast<unsigned long long>(txnId),
        static_cast<unsigned long long>(m_transactionId));

    // no response proto defined, so nothing to reply with. the client will
    // just time out on its side.
}

// ---- server browser ----

void ClientGC::OnServerBrowserFavoriteServer(GCMessageRead &messageRead)
{
    // legacy struct, layout unknown. we don't maintain a favorites DB anyway.
    (void)messageRead;
    Platform::PrintWarning("ServerBrowserFavoriteServer received (ignored)\n");
}

void ClientGC::OnServerBrowserBlacklistServer(GCMessageRead &messageRead)
{
    (void)messageRead;
    Platform::PrintWarning("ServerBrowserBlacklistServer received (ignored)\n");
}

// ---- convar ----

void ClientGC::OnConVarUpdated(GCMessageRead &messageRead)
{
    // legacy struct, no proto in the dumps. we don't proxy convar state.
    (void)messageRead;
    Platform::PrintWarning("ConVarUpdated received (ignored)\n");
}

// ---- validation / matchmaking echo ----

void ClientGC::OnGC2ClientRequestValidation(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_GC2ClientRequestValidation message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::PrintWarning("Parsing GC2ClientRequestValidation failed, ignoring\n");
        return;
    }

    // this is normally a GC -> client message. if we ever receive one it
    // means either the client is echoing it back or something is spoofing us.
    Platform::PrintWarning("GC2ClientRequestValidation: full_report=%d module=%s\n",
        message.full_report(),
        message.module().c_str());
}

void ClientGC::OnMatchmakingGC2ClientUpdate(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_MatchmakingGC2ClientUpdate message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::PrintWarning("Parsing MatchmakingGC2ClientUpdate failed, ignoring\n");
        return;
    }

    // we send these ourselves from SendMatchmakingUpdate(); if one comes in
    // it's either a mirror or the client echoing state. just log it.
    Platform::PrintDebug("MatchmakingGC2ClientUpdate: matchmaking=%d error=%s global_stats=%d waiting=%d\n",
        message.matchmaking(),
        message.error().c_str(),
        message.has_global_stats() ? 1 : 0,
        message.waiting_account_id_sessions_size());
}

// ---- global game minigame ----

void ClientGC::OnGlobalGameSubscribe(GCMessageRead &messageRead)
{
    CMsgGC_GlobalGame_Subscribe message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::PrintWarning("Parsing GlobalGame_Subscribe failed, ignoring\n");
        return;
    }

    Platform::PrintDebug("GlobalGame: subscribe ticket=%llu\n",
        static_cast<unsigned long long>(message.ticket()));
}

void ClientGC::OnGlobalGameUnsubscribe(GCMessageRead &messageRead)
{
    CMsgGC_GlobalGame_Unsubscribe message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::PrintWarning("Parsing GlobalGame_Unsubscribe failed, ignoring\n");
        return;
    }

    Platform::PrintDebug("GlobalGame: unsubscribe timeleft=%d\n", message.timeleft());
}

void ClientGC::OnGlobalGamePlay(GCMessageRead &messageRead)
{
    CMsgGC_GlobalGame_Play message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::PrintWarning("Parsing GlobalGame_Play failed, ignoring\n");
        return;
    }

    Platform::PrintDebug("GlobalGame: play ticket=%llu gametimems=%u msperpoint=%u\n",
        static_cast<unsigned long long>(message.ticket()),
        message.gametimems(),
        message.msperpoint());
}
void ClientGC::OnClientToGCChat(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_ClientToGCChat message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::PrintWarning("Parsing ClientToGCChat failed, ignoring\n");
        return;
    }

    // rate limit чтобы не спамили
    if (!m_chatLimiter.TryConsume(AccountId()))
    {
        Platform::PrintWarning("Chat rate limit hit for %u\n", AccountId());
        return;
    }

    // входящее от клиента — глобальный чат (в текущей логике),
    // но мы также позволяем лобби-чат через тот же путь
    if (!m_partyLobby.active || !m_party)
    {
        // нет лобби — эхо как раньше
        CMsgGCCStrike15_v2_GCToClientChat response;
        response.set_account_id(EffectiveAccountId());
        response.set_text(message.text());
        SendMessageToGame(false, k_EMsgGCCStrike15_v2_GlobalChat, response);
        return;
    }

    // PartyChatPayload + текст шлём по P2P всем членам
    PartyChatPayload payload{ AccountId(), m_partyLobby.lobbyId };

    for (uint32_t member : m_partyLobby.memberAccountIds)
    {
        if (member == AccountId()) continue;
        m_party->SendChat(member, payload.fromAccountId, payload.lobbyId, message.text());
    }

    // и локальному клиенту тоже показать
    CMsgGCCStrike15_v2_GCToClientChat echo;
    echo.set_account_id(EffectiveAccountId());
    echo.set_text(message.text());
    SendMessageToGame(false, k_EMsgGCCStrike15_v2_GlobalChat, echo);
}

void ClientGC::OnRemotePartyChat(const PartyChatPayload &payload, std::string_view text)
{
    if (!m_partyLobby.active || m_partyLobby.lobbyId != payload.lobbyId)
    {
        return;
    }

    // не даём прикидываться другими
    if (payload.fromAccountId == 0) return;

    // отправитель должен быть в лобби
    bool inLobby = std::find(m_partyLobby.memberAccountIds.begin(),
        m_partyLobby.memberAccountIds.end(), payload.fromAccountId)
        != m_partyLobby.memberAccountIds.end();
    if (!inLobby && payload.fromAccountId != m_partyLobby.hostAccountId) return;

    if (text.size() > 256) return; // sanity

    CMsgGCCStrike15_v2_GCToClientChat response;
    response.set_account_id(payload.fromAccountId);
    response.set_text(std::string{ text });
    SendMessageToGame(false, k_EMsgGCCStrike15_v2_GlobalChat, response);
}

// ---------------------------------------------------------------------------
// profile plumbing
//
// Our own profile goes out from local config — that's the source of truth.
// Everyone else's profile comes from the worker's ProfileStore, which every
// client publishes to on ClientHello / config reload. That means A can look
// up B's real rank / level / commendations without them ever sharing a party.
//
// The old code replied to *any* account-id lookup with *our own* config
// values stuffed into the target's message. That made the game render the
// requestor's rank on someone else's profile card. On a cache miss we now
// send an empty stub instead, so the card shows "no data" rather than a lie.
// ---------------------------------------------------------------------------


void ClientGC::SendOwnProfile()
{
    const bool prime = GetConfig().HasPrime();

    CMsgGCCStrike15_v2_PlayersProfile response;
    response.set_request_id(EffectiveAccountId());

    auto *hello = response.add_account_profiles();
    hello->set_account_id(EffectiveAccountId());
    hello->set_player_level(prime ? GetConfig().Level() : 0);
    hello->set_player_cur_xp(prime ? GetConfig().Xp() : 0);
    hello->set_vac_banned(GetConfig().VacBanned());
    hello->mutable_commendation()->set_cmd_friendly(GetConfig().CommendedFriendly());
    hello->mutable_commendation()->set_cmd_teaching(GetConfig().CommendedTeaching());
    hello->mutable_commendation()->set_cmd_leader(GetConfig().CommendedLeader());

    // always emit ranking, even if we have none. omitting it makes the
    // client keep whatever rank widget was on screen.
    auto *rank = hello->mutable_ranking();
    rank->set_account_id(EffectiveAccountId());
    rank->set_rank_id(prime ? GetConfig().CompetitiveRank() : RankNone);
    rank->set_wins(prime ? GetConfig().CompetitiveWins() : 0);
    rank->set_rank_type_id(RankTypeCompetitive);

    if (m_isCooldownActive)
    {
        const uint32_t remaining = CooldownRemainingSeconds(m_cooldownEndTime);
        hello->set_penalty_seconds(remaining);
        hello->set_penalty_reason(GetConfig().CooldownReason());
    }

    SendMessageToGame(false, k_EMsgGCCStrike15_v2_PlayersProfile, response);
}

void ClientGC::SendUnknownProfile(uint32_t accountId)
{
    // Empty stub: account id is set, everything else default. Client will
    // show "no data" rather than falling back to whatever we previously
    // sent, which used to be *our own* profile.
    CMsgGCCStrike15_v2_PlayersProfile response;
    response.set_request_id(accountId);

    auto *hello = response.add_account_profiles();
    hello->set_account_id(accountId);

    SendMessageToGame(false, k_EMsgGCCStrike15_v2_PlayersProfile, response);
}


void ClientGC::OnRemoteProfileQuery(const PartyProfileQueryPayload &q)
{
    // sanity
    if (q.targetAccountId != EffectiveAccountId())
    {
        // запрос не к нам
        return;
    }

    // защита: не отвечаем, если запросчик спамит
    if (!m_profileLimiter.TryConsume(q.fromAccountId))
    {
        return;
    }

    const bool prime = GetConfig().HasPrime();

    // Cooldown remaining as the other client should see it. If we're not
    // on cooldown, both fields are 0 and the other side won't render a
    // penalty badge.
    uint32_t penaltySeconds = 0;
    uint32_t penaltyReason  = 0;
    if (m_isCooldownActive)
    {
        penaltySeconds = CooldownRemainingSeconds(m_cooldownEndTime);
        penaltyReason  = GetConfig().CooldownReason();
        if (penaltySeconds == 0)
        {
            penaltySeconds = 1; // "just barely on cooldown" beats 0
        }
    }

    PartyProfileReplyPayload reply{};
    reply.fromAccountId   = AccountId();
    reply.targetAccountId = q.fromAccountId;
    reply.nonce           = q.nonce;
    reply.playerLevel     = prime ? GetConfig().Level() : 0;
    reply.playerCurXp     = prime ? GetConfig().Xp() : 0;
    reply.cmdFriendly     = GetConfig().CommendedFriendly();
    reply.cmdTeaching     = GetConfig().CommendedTeaching();
    reply.cmdLeader       = GetConfig().CommendedLeader();
    reply.vacBanned       = GetConfig().VacBanned() ? 1u : 0u;
    reply.rankId          = prime ? static_cast<uint32_t>(GetConfig().CompetitiveRank()) : 0u;
    reply.rankWins        = prime ? GetConfig().CompetitiveWins() : 0;
    reply.rankType        = RankTypeCompetitive;
    reply.penaltySeconds  = penaltySeconds;
    reply.penaltyReason   = penaltyReason;

    m_party->SendProfileReply(q.fromAccountId, reply);
}

void ClientGC::OnRemoteProfileReply(const PartyProfileReplyPayload &reply)
{
    std::lock_guard<std::mutex> lock(m_pendingProfileMutex);
    auto it = m_pendingProfileQueries.find(reply.nonce);
    if (it == m_pendingProfileQueries.end()) return;

    if (it->second.targetAccountId != reply.fromAccountId)
    {
        Platform::PrintWarning("Profile reply account mismatch, dropping\n");
        m_pendingProfileQueries.erase(it);
        return;
    }

    CMsgGCCStrike15_v2_PlayersProfile response;
    response.set_request_id(reply.fromAccountId);

    auto *mmHello = response.add_account_profiles();
    mmHello->set_account_id(reply.fromAccountId);
    mmHello->set_player_level(reply.playerLevel);
    mmHello->set_player_cur_xp(reply.playerCurXp);
    mmHello->set_vac_banned(reply.vacBanned != 0);
    mmHello->mutable_commendation()->set_cmd_friendly(reply.cmdFriendly);
    mmHello->mutable_commendation()->set_cmd_teaching(reply.cmdTeaching);
    mmHello->mutable_commendation()->set_cmd_leader(reply.cmdLeader);

    auto *rank = mmHello->mutable_ranking();
    rank->set_account_id(reply.fromAccountId);
    rank->set_rank_id(reply.rankId);
    rank->set_wins(reply.rankWins);
    rank->set_rank_type_id(reply.rankType);

    if (reply.penaltySeconds != 0)
    {
        mmHello->set_penalty_seconds(reply.penaltySeconds);
        mmHello->set_penalty_reason(reply.penaltyReason);
    }

    SendMessageToGame(false, k_EMsgGCCStrike15_v2_PlayersProfile, response);
    m_pendingProfileQueries.erase(it);
}

void ClientGC::OnRemotePartyKick(const PartyKickPayload &payload)
{
    if (!m_partyLobby.active || m_partyLobby.lobbyId != payload.lobbyId)
        return;

    if (payload.targetAccountId != AccountId())
        return;

    Platform::PrintWarning("Party: kicked from lobby %u by %u\n",
        payload.lobbyId, payload.hostAccountId);

    m_partyLobby = PartyLobby{};

    // Отдельного protobuf-типа CMsgGCCStrike15_v2_Party_Unregister не
    // существует — сообщение 9190 пустое. Сериализуем любое пустое
    // сообщение (CMsgGCCStrike15_v2_MatchmakingClient2GCHello) под этим
    // типом; клиент интерпретирует 0-байтовый payload как "unregister".
    CMsgGCCStrike15_v2_MatchmakingClient2GCHello empty;
    SendMessageToGame(false, k_EMsgGCCStrike15_v2_Party_Unregister, empty);
}

void ClientGC::NotifyPartyOfMatchmakingRefusal(uint32_t reason, uint32_t bannedAccountId,
                                               const std::vector<uint32_t> &recipients)
{
    if (!m_party)
    {
        // no P2P channel — party server URL is unset, or the transport
        // failed to come up. nothing we can do here; the party just won't
        // hear about the refusal.
        Platform::PrintWarning(
            "NotifyPartyOfMatchmakingRefusal: no party transport, "
            "refusal will stay host-only\n");
        return;
    }

    // dedupe: m_partyLobby and m_nativeLobbyMembers commonly overlap, and
    // the request itself may also list someone already in one of them.
    // sending twice would pop the error twice on the receiving end.
    std::unordered_set<uint32_t> sent;

    for (uint32_t memberId : recipients)
    {
        if (memberId == AccountId()) continue;
        if (!sent.insert(memberId).second) continue;
        m_party->SendMatchmakingRefused(memberId, reason, bannedAccountId);
    }

    Platform::PrintDebug("NotifyPartyOfMatchmakingRefusal: notified %zu members "
                         "(reason=%u banned=%u)\n",
        sent.size(), reason, bannedAccountId);
}

void ClientGC::OnRemoteMatchmakingRefused(const PartyMatchmakingRefusedPayload &p)
{
    Platform::PrintWarning("Party: matchmaking refused, reason=%u bannedAccount=%u\n",
        p.reason, p.bannedAccountId);

    // Only VAC refusals are propagated for now. Other reasons (cooldown,
    // party abandonment) would need their own reason codes and matching
    // SFUI keys; they currently only surface on the host.
    if (p.reason != 1) return;

    if (!p.bannedAccountId) return;

    CMsgGCCStrike15_v2_MatchmakingGC2ClientUpdate update;
    update.set_matchmaking(0);
    update.add_vacbanned_account_id_sessions(p.bannedAccountId);
    update.set_error("SFUI_QMM_ERROR_X_VacBanned");

    SendMessageToGame(false, k_EMsgGCCStrike15_v2_MatchmakingGC2ClientUpdate, update);
}

// читаем competitive_cooldown_seconds и, если значение поменялось с прошлого
// раза, обновляем состояние. ничего не отправляем клиенту — это делают
// вызывающие (OnClientHello / ReloadConfig), чтобы не сломать порядок
// сообщений (Welcome обязан уйти раньше любого Hello).
bool ClientGC::ApplyConfiguredCooldown()
{
    const uint32_t seconds = GetConfig().CompetitiveCooldownSeconds();
    const uint32_t reason  = GetConfig().CooldownReason();

    if (seconds == m_lastAppliedCooldownSeconds
        && reason == m_lastAppliedCooldownReason)
        return false;

    m_lastAppliedCooldownSeconds = seconds;
    m_lastAppliedCooldownReason  = reason;
    m_cooldownExpiredLocally = false;

    if (seconds == 0)
    {
        m_isCooldownActive = false;
        Platform::PrintSuccess("Competitive cooldown cleared via config\n");
        QueueProfilePublish();
        return true;
    }

    m_isCooldownActive = true;

    if (IsPermanentCooldownReason(reason))
    {
        // a time_point at the heat death of the universe never compares as
        // expired, so UpdateCooldown() naturally leaves this alone forever.
        m_cooldownEndTime = std::chrono::steady_clock::time_point::max();
        Platform::PrintSuccess("Competitive cooldown activated: PERMANENT (reason %u: %s)\n",
            reason, CooldownReasonName(reason));
    }
    else
    {
        m_cooldownEndTime =
            std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        Platform::PrintSuccess("Competitive cooldown activated for %u seconds (reason %u: %s)\n",
            seconds, reason, CooldownReasonName(reason));
    }

    // cooldown state changed — republish our profile so other clients see
    // the updated penalty badge on our card.
    QueueProfilePublish();
    return true;
}
// Cooldown reasons that never expire on their own — Valve's "global" bans
// with no end time. Reason 11 is intentionally excluded: it's a global
// ban too, but Overwatch minor is timed, and ApplyConfiguredCooldown
// needs a real end time for it. IsGlobalCooldownReason (in the anonymous
// namespace above) is the "blocks every playlist" classifier — different
// question, and it DOES include 11.
bool ClientGC::IsPermanentCooldownReason(uint32_t reason)
{
    switch (reason)
    {
    case 8:  // GLOBAL BAN (untrusted)
    case 10: // Overwatch ban (cheat)
    case 14: // dup of 8, per valve strings
        return true;

    default:
        return false;
    }
}

// penalty_seconds is a uint32. A permanent cooldown stores
// steady_clock::time_point::max() as its end time, so the raw remaining
// duration overflows uint32 and wraps to a small number. Saturate it — the
// client renders the result as a very long countdown, which is what we want.
uint32_t ClientGC::CooldownRemainingSeconds(std::chrono::steady_clock::time_point endTime)
{
    using namespace std::chrono;
    auto remaining = duration_cast<seconds>(endTime - steady_clock::now()).count();
    if (remaining <= 0)
        return 0;

    // NOTE: this used to be UINT32_MAX. that value either decodes as -1 on
    // the client (if the proto field is int32) or wraps now+penalty back to
    // now (int32 arithmetic overflow), so the cooldown ends instantly.
    // pick something that survives both and is still "clearly permanent"
    // to the player. 5 years = 157 680 000s, well under int32 max.
    constexpr int64_t MaxPenaltySeconds = 157680000LL;
    return static_cast<uint32_t>((std::min)(remaining, MaxPenaltySeconds));
}
const char *ClientGC::CooldownReasonName(uint32_t reason)
{
    switch (reason)
    {
    case 1:  return "You were kicked from the last match";
    case 2:  return "You killed too many teammates";
    case 3:  return "You killed a teammate at round start";
    case 4:  return "You failed to reconnect to the last match";
    case 5:  return "You abandoned the last match";
    case 6:  return "You did too much damage to your teammates";
    case 7:  return "You did too much damage to your teammates at round start";
    case 8:  return "This account is permanently Untrusted";
    case 9:  return "You were kicked from too many recent matches";
    case 10: return "Convicted by Overwatch - Majorly Disruptive";
    case 11: return "Convicted by Overwatch - Minorly Disruptive";
    case 12:
    case 13: return "Resolving matchmaking state for your account";
    case 14: return "This account is permanently Untrusted";
    case 15:
    case 16: return "You failed to connect by match start.";
    case 17: return "You have kicked too many teammates in recent matches";
    case 18: return "Congratulations on your recent competitive wins! Before you play competitive matches further please wait for matchmaking servers to calibrate your Skill Group placement based on your latest performance.";
    case 19:
    case 20: return "You have received significantly more griefing reports than most players.";
    case 21: return "You have received significantly more griefing reports than most players.";
    default: return "Unknown cooldown reason";
    }
}

// Returns the SFUI key the client should render in the lobby's error field.
// Valve distinguishes "green" (informational) cooldowns - matchmaking
// calibration, griefing-report warnings - from the punitive ones. The
// client renders those with a green tint and a softer wording, so we have
// to pick the right key or the popup will look like an actual ban.
//
// self=true  -> first-person variant, "you are on cooldown" (SFUI_..._1_...)
// self=false -> third-person variant, "<member> is on cooldown" (SFUI_..._X_...)
// mirrors the VAC pair SFUI_QMM_ERROR_1_VacBanned / SFUI_QMM_ERROR_X_VacBanned.
const char *ClientGC::CooldownErrorLocKey(uint32_t reason, bool self)
{
    const bool green = IsGreenCooldownReason(reason);

    if (self)
    {
        return green ? "SFUI_QMM_ERROR_1_PenaltySecondsGreen"
                     : "SFUI_QMM_ERROR_1_PenaltySeconds";
    }
    return green ? "SFUI_QMM_ERROR_X_PenaltySecondsGreen"
                 : "SFUI_QMM_ERROR_X_PenaltySeconds";
}

// Routes the offender into the correct penalty list on the update message.
// The proto carries two — a regular one and a "green" one. Green cooldowns
// (calibration, griefing warnings) go into the green list; everything else
// (VAC, Overwatch bans, abandon penalties) goes into the regular list. If
// they're mixed up the client may render a soft warning as a hard ban.
void ClientGC::AddPenaltyAccount(CMsgGCCStrike15_v2_MatchmakingGC2ClientUpdate &update,
                                 uint32_t accountId, uint32_t reason)
{
    if (IsGreenCooldownReason(reason))
        update.add_penalty_account_id_sessions_green(accountId);
    else
        update.add_penalty_account_id_sessions(accountId);
}

bool ClientGC::FetchRemoteProfile(uint32_t accountId, RemoteProfile &out)
{
    out = RemoteProfile{};

    const std::string base = GetConfig().PartyServerUrl();
    if (base.empty())
        return false;

    char url[512];
    snprintf(url, sizeof(url), "%s/profile/%u", base.c_str(), accountId);

    HttpResult res = HttpGetJson(url);

    if (res.status != 200)
    {
        Platform::PrintError("Profile query for %u failed (HTTP %d)\n",
            accountId, res.status);
        return false;
    }

    bool found = false;
    FindJsonBool(res.body, "found", found);
    if (!found)
    {
        Platform::PrintDebug("Profile for %u not in worker store\n", accountId);
        return false;
    }

    std::string_view body{ res.body };
    size_t p = body.find("\"profile\":");
    if (p == std::string_view::npos) return false;
    p = body.find('{', p);
    if (p == std::string_view::npos) return false;
    size_t e = body.find('}', p);
    if (e == std::string_view::npos) return false;
    std::string_view sub = body.substr(p, e - p + 1);

    FindJsonUint(sub, "level",          out.level);
    FindJsonUint(sub, "xp",             out.xp);
    FindJsonUint(sub, "cmdFriendly",    out.cmdFriendly);
    FindJsonUint(sub, "cmdTeaching",    out.cmdTeaching);
    FindJsonUint(sub, "cmdLeader",      out.cmdLeader);
    FindJsonUint(sub, "rankId",         out.rankId);
    FindJsonUint(sub, "rankWins",       out.rankWins);
    FindJsonUint(sub, "rankType",       out.rankType);
    FindJsonBool(sub, "vacBanned",      out.vacBanned);
    FindJsonUint(sub, "penaltySeconds", out.penaltySeconds);
    FindJsonUint(sub, "penaltyReason",  out.penaltyReason);
    out.found = true;

    return true;
}

bool ClientGC::QueryRemoteProfile(uint32_t accountId)
{
    RemoteProfile profile;
    if (!FetchRemoteProfile(accountId, profile))
        return false;

    CMsgGCCStrike15_v2_PlayersProfile response;
    response.set_request_id(accountId);

    auto *hello = response.add_account_profiles();
    hello->set_account_id(accountId);
    hello->set_player_level(profile.level);
    hello->set_player_cur_xp(profile.xp);
    hello->set_vac_banned(profile.vacBanned);
    hello->mutable_commendation()->set_cmd_friendly(profile.cmdFriendly);
    hello->mutable_commendation()->set_cmd_teaching(profile.cmdTeaching);
    hello->mutable_commendation()->set_cmd_leader(profile.cmdLeader);

    auto *rank = hello->mutable_ranking();
    rank->set_account_id(accountId);
    rank->set_rank_id(profile.rankId);
    rank->set_wins(profile.rankWins);
    rank->set_rank_type_id(profile.rankType);

    if (profile.penaltySeconds != 0)
    {
        hello->set_penalty_seconds(profile.penaltySeconds);
        hello->set_penalty_reason(profile.penaltyReason);
    }

    SendMessageToGame(false, k_EMsgGCCStrike15_v2_PlayersProfile, response);
    Platform::PrintDebug("Got remote profile for %u from worker\n", accountId);
    return true;
}

void ClientGC::StartHttpWorker()
{
    m_httpThread = std::thread{ &ClientGC::HttpWorkerThread, this };
}

void ClientGC::StopHttpWorker()
{
    {
        std::lock_guard<std::mutex> lock(m_httpMutex);
        m_httpStopping = true;
    }
    m_httpCv.notify_all();
    if (m_httpThread.joinable())
        m_httpThread.join();
}

void ClientGC::HttpWorkerThread()
{
    while (true)
    {
        bool doProfile    = false;
        bool doLtpRefresh = false;
        std::vector<PendingReport> reports;

        {
            std::unique_lock<std::mutex> lock(m_httpMutex);
            m_httpCv.wait(lock, [this] {
                return m_httpStopping || m_httpPublishPending
                    || !m_httpReports.empty()
                    || m_httpLtpRefreshPending;
            });

            if (m_httpStopping) break;

            doProfile    = m_httpPublishPending;
            doLtpRefresh = m_httpLtpRefreshPending;
            m_httpPublishPending    = false;
            m_httpLtpRefreshPending = false;

            while (!m_httpReports.empty())
            {
                reports.push_back(m_httpReports.front());
                m_httpReports.pop();
            }
        }

        if (doProfile)    DoProfilePublish();
        if (doLtpRefresh) DoLtpRefresh();

        for (const PendingReport &r : reports)
            DoReportPublish(r.targetAccountId, r.reasonFlags);
    }
}

void ClientGC::QueueProfilePublish()
{
    {
        std::lock_guard<std::mutex> lock(m_httpMutex);
        m_httpPublishPending = true;
    }
    m_httpCv.notify_one();
}


void ClientGC::QueueReport(uint32_t targetAccountId, uint32_t reasonFlags)
{
    {
        std::lock_guard<std::mutex> lock(m_httpMutex);
        m_httpReports.push({ targetAccountId, reasonFlags });
    }
    m_httpCv.notify_one();
}

void ClientGC::DoProfilePublish()
{
    const std::string base = GetConfig().PartyServerUrl();
    if (base.empty())
        return;

    const bool prime = GetConfig().HasPrime();

    char url[512];
    snprintf(url, sizeof(url), "%s/profile/%u", base.c_str(), EffectiveAccountId());

    // Cooldown state. We publish the *remaining* seconds, not the configured
    // total, so a consumer that reads the profile two minutes later sees a
    // two-minute-smaller number. That matches what the hello's penalty_seconds
    // field means.
    uint32_t penaltySeconds = 0;
    uint32_t penaltyReason  = 0;
    if (m_isCooldownActive)
    {
        penaltySeconds = CooldownRemainingSeconds(m_cooldownEndTime);
        penaltyReason  = GetConfig().CooldownReason();
        if (penaltySeconds == 0)
        {
            penaltySeconds = 1;
        }
    }

    char body[640];
    snprintf(body, sizeof(body),
        "{"
        "\"accountId\":%u,"
        "\"level\":%d,"
        "\"xp\":%d,"
        "\"cmdFriendly\":%d,"
        "\"cmdTeaching\":%d,"
        "\"cmdLeader\":%d,"
        "\"vacBanned\":%s,"
        "\"rankId\":%u,"
        "\"rankWins\":%d,"
        "\"rankType\":%u,"
        "\"penaltySeconds\":%u,"
        "\"penaltyReason\":%u,"
        "\"hasPrime\":%s"
        "}",
        EffectiveAccountId(),
        prime ? GetConfig().Level() : 0,
        prime ? GetConfig().Xp() : 0,
        GetConfig().CommendedFriendly(),
        GetConfig().CommendedTeaching(),
        GetConfig().CommendedLeader(),
        GetConfig().VacBanned() ? "true" : "false",
        prime ? static_cast<uint32_t>(GetConfig().CompetitiveRank()) : 0u,
        prime ? GetConfig().CompetitiveWins() : 0,
        static_cast<uint32_t>(RankTypeCompetitive),
        penaltySeconds,
        penaltyReason,
        prime ? "true" : "false");

    HttpResult res = HttpPostJson(url, body);
    if (res.status == 200)
        Platform::PrintSuccess("Published profile for %u\n", EffectiveAccountId());
    else
        Platform::PrintError("Profile publish failed (HTTP %d)\n", res.status);
}



void ClientGC::DoReportPublish(uint32_t targetAccountId, uint32_t reasonFlags)
{
    const std::string base = GetConfig().PartyServerUrl();
    if (base.empty()) return;

    char url[512];
    snprintf(url, sizeof(url), "%s/report/%u", base.c_str(), targetAccountId);

    char body[128];
    snprintf(body, sizeof(body), "{\"from\":%u,\"reason\":%u}",
        EffectiveAccountId(), reasonFlags);

    HttpResult res = HttpPostJson(url, body);
    if (res.status == 200)
        Platform::PrintDebug("Reported %u (flags 0x%x)\n", targetAccountId, reasonFlags);
    else
        Platform::PrintWarning("Report post failed (HTTP %d)\n", res.status);
}

bool ClientGC::QueryRemoteItem(uint64_t itemId, CEconItemPreviewDataBlock &block)
{
    const std::string base = GetConfig().PartyServerUrl();
    if (base.empty()) return false;

    char url[512];
    snprintf(url, sizeof(url), "%s/items/%llu",
        base.c_str(), static_cast<unsigned long long>(itemId));

    HttpResult res = HttpGetJson(url);
    if (res.status != 200) return false;

    bool found = false;
    FindJsonBool(res.body, "found", found);
    if (!found) return false;

    // The worker returns {"found":true,"block":"<hex>","owner":"..."}.
    // Pull out the hex string — it's a serialized CEconItemPreviewDataBlock.
    std::string_view body{ res.body };
    const std::string needle = "\"block\":\"";
    size_t pos = body.find(needle);
    if (pos == std::string_view::npos) return false;
    pos += needle.size();

    size_t end = body.find('"', pos);
    if (end == std::string_view::npos) return false;

    std::string_view hex = body.substr(pos, end - pos);
    if (hex.size() % 2 != 0) return false;

    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };

    std::string bytes;
    bytes.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2)
    {
        int hi = nibble(hex[i]);
        int lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) return false;
        bytes.push_back(static_cast<char>((hi << 4) | lo));
    }

    return block.ParseFromString(bytes);
}

void ClientGC::CheckGriefingCooldown()
{
    const uint32_t threshold = GetConfig().ReportCooldownThreshold();
    if (threshold == 0) return;

    const std::string base = GetConfig().PartyServerUrl();
    if (base.empty()) return;

    char url[512];
    snprintf(url, sizeof(url), "%s/report/%u",
        base.c_str(), EffectiveAccountId());

    HttpResult res = HttpGetJson(url);
    if (res.status != 200) return;

    uint32_t count = 0;
    if (!FindJsonUint(res.body, "count", count)) return;

    if (count == 0) return;
    Platform::Print("Griefing report count: %u (threshold %u)\n", count, threshold);

    if (count < threshold) return;
    if (m_isCooldownActive) return;

    const uint32_t seconds = GetConfig().ReportCooldownSeconds();
    if (seconds == 0)
    {
        Platform::PrintWarning(
            "Griefing threshold crossed (%u reports) but "
            "report_cooldown_seconds is 0 — not applying\n", count);
        return;
    }

    // Deliberately don't touch m_lastAppliedCooldown* — ApplyConfiguredCooldown
    // compares those against config and would otherwise wipe our cooldown on
    // the next reload. This is a self-imposed cooldown, not a config-driven one.
    m_isCooldownActive       = true;
    m_cooldownEndTime        = std::chrono::steady_clock::now()
                             + std::chrono::seconds(seconds);
    m_cooldownExpiredLocally = false;

    Platform::PrintWarning(
        "Griefing cooldown applied: %u seconds (reason 19, %u reports)\n",
        seconds, count);

    SendCompetitiveCooldown();
    QueueProfilePublish();
}