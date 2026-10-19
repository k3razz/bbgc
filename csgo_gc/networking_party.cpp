#include "stdafx.h"
#include "networking_party.h"
#include "networking_shared.h"
#include "gc_client.h"
#include "platform.h"
#include "ltp_http.h"

namespace {

// How often we poll the inbox when idle. Sends wake the CV immediately,
// so this only matters for incoming messages.
constexpr auto PollInterval = std::chrono::milliseconds(500);

std::string ToHex(const uint8_t *data, size_t size)
{
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (size_t i = 0; i < size; i++)
    {
        out.push_back(digits[data[i] >> 4]);
        out.push_back(digits[data[i] & 0xF]);
    }
    return out;
}

int HexNibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

std::vector<uint8_t> FromHex(std::string_view hex)
{
    std::vector<uint8_t> out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2)
    {
        out.push_back(static_cast<uint8_t>(
            (HexNibble(hex[i]) << 4) | HexNibble(hex[i + 1])));
    }
    return out;
}

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

struct InboxMessage
{
    uint32_t seq{};
    uint32_t from{};
    std::vector<uint8_t> data;
};

struct InboxPoll
{
    uint32_t latest{};
    std::vector<InboxMessage> messages;
};

InboxPoll ParseInboxPoll(std::string_view json)
{
    InboxPoll out;

    FindJsonUint(json, "latest", out.latest);

    size_t arrStart = json.find("\"messages\"");
    if (arrStart == std::string_view::npos) return out;
    arrStart = json.find('[', arrStart);
    if (arrStart == std::string_view::npos) return out;

    size_t pos = arrStart + 1;
    while (true)
    {
        size_t start = json.find('{', pos);
        if (start == std::string_view::npos) break;
        size_t end = json.find('}', start);
        if (end == std::string_view::npos) break;

        std::string_view obj = json.substr(start, end - start + 1);
        InboxMessage m{};
        FindJsonUint(obj, "seq", m.seq);
        FindJsonUint(obj, "from", m.from);

        size_t dp = obj.find("\"data\":\"");
        if (dp != std::string_view::npos)
        {
            dp += 8;
            size_t de = obj.find('"', dp);
            if (de != std::string_view::npos)
            {
                m.data = FromHex(obj.substr(dp, de - dp));
            }
        }

        if (m.seq && !m.data.empty())
            out.messages.push_back(std::move(m));

        pos = end + 1;
    }

    return out;
}

} // anonymous namespace

NetworkingParty::NetworkingParty(uint32_t accountId, const std::string &serverUrl)
    : m_accountId{ accountId }
    , m_serverUrl{ serverUrl }
{
    if (!m_serverUrl.empty())
        m_pollThread = std::thread{ &NetworkingParty::PollWorker, this };
}

NetworkingParty::~NetworkingParty()
{
    {
        std::lock_guard<std::mutex> lock(m_pollMutex);
        m_pollStopping = true;
        // Whatever is still queued at shutdown isn't going anywhere.
        m_outgoing.clear();
    }
    m_pollCv.notify_all();

    if (m_pollThread.joinable())
        m_pollThread.join();
}

void NetworkingParty::Update(ClientGC *gc)
{
    if (m_serverUrl.empty())
        return;

    {
        std::lock_guard<std::mutex> lock(m_pollMutex);
        m_gcForPoll = gc;
    }
}

void NetworkingParty::PollWorker()
{
    while (true)
    {
        {
            std::unique_lock<std::mutex> lock(m_pollMutex);
            // Wake on: stop, or new outgoing message. The poll interval is a
            // fallback so we still pick up incoming messages when idle.
            m_pollCv.wait_for(lock, PollInterval, [this] {
                return m_pollStopping || !m_outgoing.empty();
            });
            if (m_pollStopping) break;
        }

        // Send anything the game thread queued. This happens here, on our
        // thread, so the game thread never blocks on HTTP.
        DrainOutgoing();

        // Then poll the inbox as before.
        ClientGC *gc = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_pollMutex);
            gc = m_gcForPoll;
        }
        if (!gc) continue;

        std::string url = m_serverUrl + "/inbox/" + std::to_string(m_accountId)
                        + "/poll?since=" + std::to_string(m_lastSeq);

        HttpResult res = HttpGetJson(url);          // safe: worker thread
        if (res.status != 200) continue;

        InboxPoll poll = ParseInboxPoll(res.body);

        bool firstPoll = (m_lastSeq == UINT32_MAX);
        if (!firstPoll)
        {
            for (const InboxMessage &m : poll.messages)
                DispatchToGC(gc, m.from, m.data);
        }

        if (poll.latest > 0)   m_lastSeq = poll.latest;
        else if (firstPoll)    m_lastSeq = 0;
    }
}

void NetworkingParty::DrainOutgoing()
{
    for (;;)
    {
        OutgoingMessage msg;
        {
            std::lock_guard<std::mutex> lock(m_pollMutex);
            if (m_outgoing.empty()) return;
            msg = std::move(m_outgoing.front());
            m_outgoing.pop_front();
        }

        std::string url = m_serverUrl + "/inbox/"
                        + std::to_string(msg.targetAccountId) + "/send";

        std::vector<HttpHeader> headers = {
            { "Content-Type", "application/octet-stream" },
            { "X-From", std::to_string(m_accountId) },
        };

        HttpResult res = HttpPostBinary(url, msg.data.data(), msg.data.size(), headers);
        if (res.status != 200)
        {
            Platform::PrintError("Party send to %u failed (HTTP %d)\n",
                msg.targetAccountId, res.status);
        }
    }
}

void NetworkingParty::DispatchToGC(ClientGC *gc, uint32_t fromAccountId,
                                   const std::vector<uint8_t> &buffer)
{
    std::vector<uint8_t> payload;
    payload.reserve(sizeof(uint32_t) + buffer.size());
    payload.resize(sizeof(uint32_t));
    memcpy(payload.data(), &fromAccountId, sizeof(uint32_t));
    payload.insert(payload.end(), buffer.begin(), buffer.end());

    gc->PostToGC(GCEvent::PartyIncoming, 0, payload.data(),
                 static_cast<uint32_t>(payload.size()));
}

void NetworkingParty::SendRaw(uint32_t targetAccountId, const GCMessageWrite &write)
{
    if (m_serverUrl.empty())
        return;

    OutgoingMessage msg;
    msg.targetAccountId = targetAccountId;
    const uint8_t *bytes = static_cast<const uint8_t *>(write.Data());
    msg.data.assign(bytes, bytes + write.Size());

    {
        std::lock_guard<std::mutex> lock(m_pollMutex);
        if (m_pollStopping) return;   // don't queue work during shutdown
        m_outgoing.push_back(std::move(msg));
    }
    m_pollCv.notify_one();
}

// --- senders (unchanged except they now go through the new SendRaw) -----

void NetworkingParty::SendInvite(uint32_t targetAccountId, uint32_t fromAccountId,
                                 uint32_t lobbyId, uint32_t gameType)
{
    PartyInvitePayload payload{ lobbyId, gameType, fromAccountId };

    GCMessageWrite write{ k_EMsgPartyInviteRemote };
    write.WriteData(&payload, sizeof(payload));
    SendRaw(targetAccountId, write);
}

void NetworkingParty::SendJoinRelay(uint32_t hostAccountId, uint32_t fromAccountId,
                                    uint32_t lobbyId)
{
    PartyJoinRelayPayload payload{ lobbyId, fromAccountId };

    GCMessageWrite write{ k_EMsgPartyJoinRelayRemote };
    write.WriteData(&payload, sizeof(payload));
    SendRaw(hostAccountId, write);
}

void NetworkingParty::SendLobbyUpdate(uint32_t targetAccountId, uint32_t lobbyId,
                                      const std::vector<uint32_t> &members)
{
    GCMessageWrite write{ k_EMsgPartyLobbyUpdateRemote };
    write.WriteUint32(lobbyId);
    write.WriteUint32(static_cast<uint32_t>(members.size()));
    if (!members.empty())
    {
        write.WriteData(members.data(),
            static_cast<uint32_t>(members.size() * sizeof(uint32_t)));
    }
    SendRaw(targetAccountId, write);
}

void NetworkingParty::SendHeartbeat(uint32_t targetAccountId, uint32_t hostAccountId,
                                    uint32_t lobbyId, uint32_t memberCount)
{
    PartyHeartbeatPayload p{ hostAccountId, lobbyId, memberCount };
    GCMessageWrite write{ k_EMsgPartyHeartbeatRemote };
    write.WriteData(&p, sizeof(p));
    SendRaw(targetAccountId, write);
}

void NetworkingParty::SendChat(uint32_t targetAccountId, uint32_t fromAccountId,
                               uint32_t lobbyId, std::string_view text)
{
    PartyChatPayload p{ fromAccountId, lobbyId };
    GCMessageWrite write{ k_EMsgPartyChatRemote };
    write.WriteData(&p, sizeof(p));
    write.WriteData(text.data(), static_cast<uint32_t>(text.size()));
    SendRaw(targetAccountId, write);
}

void NetworkingParty::SendProfileQuery(uint32_t targetAccountId, uint32_t fromAccountId,
                                       uint32_t nonce)
{
    PartyProfileQueryPayload p{ fromAccountId, targetAccountId, nonce };
    GCMessageWrite write{ k_EMsgPartyProfileQueryRemote };
    write.WriteData(&p, sizeof(p));
    SendRaw(targetAccountId, write);
}

void NetworkingParty::SendProfileReply(uint32_t targetAccountId, const PartyProfileReplyPayload &p)
{
    GCMessageWrite write{ k_EMsgPartyProfileReplyRemote };
    write.WriteData(&p, sizeof(p));
    SendRaw(targetAccountId, write);
}

void NetworkingParty::SendKick(uint32_t targetAccountId, uint32_t hostAccountId,
                               uint32_t lobbyId, uint32_t kickedAccountId)
{
    PartyKickPayload p{ hostAccountId, lobbyId, kickedAccountId };
    GCMessageWrite write{ k_EMsgPartyKickRemote };
    write.WriteData(&p, sizeof(p));
    SendRaw(targetAccountId, write);
}
void NetworkingParty::SendMatchmakingRefused(uint32_t targetAccountId,
                                             uint32_t reason,
                                             uint32_t bannedAccountId)
{
    PartyMatchmakingRefusedPayload p{ reason, bannedAccountId };
    GCMessageWrite write{ k_EMsgPartyMatchmakingRefusedRemote };
    write.WriteData(&p, sizeof(p));
    SendRaw(targetAccountId, write);
}