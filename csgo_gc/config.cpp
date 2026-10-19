#include "stdafx.h"
#include "config.h"
#include "keyvalue.h"
#include "random.h"

constexpr const char *ConfigFilePath = "csgo_gc/config.txt";

void GCConfig::Parse(const KeyValue& config)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    m_appIdOverride = config.GetNumber("appid_override", m_appIdOverride);
    m_showCsgoGCServersOnly = config.GetNumber("show_csgo_gc_servers_only", m_showCsgoGCServersOnly);

    const KeyValue *ranks = config.GetSubkey("ranks");
    if (ranks)
    {
        m_competitiveRank = ranks->GetNumber("competitive_rank", m_competitiveRank);
        m_competitiveWins = ranks->GetNumber("competitive_wins", m_competitiveWins);
        m_wingmanRank = ranks->GetNumber("wingman_rank", m_wingmanRank);
        m_wingmanWins = ranks->GetNumber("wingman_wins", m_wingmanWins);
        m_dangerZoneRank = ranks->GetNumber("dangerzone_rank", m_dangerZoneRank);
        m_dangerZoneWins = ranks->GetNumber("dangerzone_wins", m_dangerZoneWins);
    }

    m_forceMaxRarity = config.GetNumber("force_max_rarity", m_forceMaxRarity);
    m_destroyUsedItems = config.GetNumber("destroy_used_items", m_destroyUsedItems);
    m_randomizeFloat = config.GetNumber("randomize_item_float", m_randomizeFloat);

    m_reportSharing           = config.GetNumber("report_sharing", m_reportSharing);
    m_reportCooldownThreshold = config.GetNumber("report_cooldown_threshold", m_reportCooldownThreshold);
    m_reportCooldownSeconds   = config.GetNumber("report_cooldown_seconds", m_reportCooldownSeconds);

    // LTP worker config
    m_ltpServerUrl = config.GetString("ltp_server_url", m_ltpServerUrl);
    m_useLtpServer = config.GetNumber("use_ltp_server", m_useLtpServer);

    // party backend — always on, the url being empty just disables it
    m_partyServerUrl = config.GetString("party_server_url", m_partyServerUrl);

    const KeyValue *rarityWeights = config.GetSubkey("rarity_weights");
    if (rarityWeights)
    {
        m_rarityWeights.clear();
        m_rarityWeights.reserve(rarityWeights->SubkeyCount());
        for (const KeyValue &subkey : *rarityWeights)
        {
            RarityWeight weight;
            weight.rarity = FromString<uint32_t>(subkey.Name());
            weight.weight = FromString<float>(subkey.String());
            m_rarityWeights.push_back(weight);
        }
    }

    const KeyValue *friends = config.GetSubkey("friends");
    if (friends)
    {
        m_friends.clear();
        m_friends.reserve(friends->SubkeyCount());
        for (const KeyValue &subkey : *friends)
        {
            uint32_t friendId = FromString<uint32_t>(subkey.Name());
            m_friends.push_back(friendId);
        }
    }

    m_vacBanned = config.GetNumber("vac_banned", m_vacBanned);
    m_hasPrime = config.GetNumber("has_prime", 1);
    m_competitiveCooldownSeconds = config.GetNumber("competitive_cooldown_seconds", m_competitiveCooldownSeconds);
    m_cooldownReason = config.GetNumber("competitive_cooldown_reason", m_cooldownReason);
    m_commendedFriendly = config.GetNumber("cmd_friendly", m_commendedFriendly);
    m_commendedTeaching = config.GetNumber("cmd_teaching", m_commendedTeaching);
    m_commendedLeader = config.GetNumber("cmd_leader", m_commendedLeader);
    m_level = config.GetNumber("player_level", m_level);
    m_xp = config.GetNumber("player_cur_xp", m_xp);

    // wrap xp into levels, then wrap the level around itself
    constexpr int MaxPlayerLevel = 40;
    constexpr int XpPerLevel = 5000;

    const int oldLevel = m_level;
    const int oldXp = m_xp;

    while (m_xp >= XpPerLevel)
    {
        m_xp -= XpPerLevel;
        m_level++;
    }

    if (m_level > MaxPlayerLevel)
    {
        m_level = (m_level - 1) % MaxPlayerLevel + 1;
    }

    // save the normalized values back so they don't get re-derived every load
    if (m_level != oldLevel || m_xp != oldXp)
    {
        Save();
    }

    m_country = config.GetString("country", m_country);
    m_currency = config.GetNumber("currency", m_currency);


    // NOTE: we used to zero out level/xp/ranks here when !m_hasPrime, but that
    // ran before the player could buy prime, so flipping the flag later left
    // them with rank 0 forever. the rank gate now lives in BuildMatchmakingHello
    // (rank block only sent if HasPrime), and SendRankUpdate bails on non-prime.
    // that's the correct place to hide ranks, not the parse path.
}

GCConfig::GCConfig()
{
    KeyValue config{ "config" };
    if (!config.ParseFromFile(ConfigFilePath))
        return;
    Parse(config);
}

void GCConfig::ReloadFromFile()
{
    KeyValue config{ "config" };
    if (!config.ParseFromFile(ConfigFilePath))
        return;
    Parse(config);
}

// helper: find `"key"` in a line, then the next quoted string after it,
// and replace its contents in-place. keeps indentation, tabs, trailing
// comments, etc.
static bool ReplaceQuotedValueInLine(std::string &line, std::string_view key, std::string_view newValue)
{
    const std::string needle = "\"" + std::string(key) + "\"";

    size_t keyPos = line.find(needle);
    if (keyPos == std::string::npos)
        return false;

    size_t valStart = line.find('"', keyPos + needle.size());
    if (valStart == std::string::npos)
        return false;

    size_t valEnd = line.find('"', valStart + 1);
    if (valEnd == std::string::npos)
        return false;

    line.replace(valStart + 1, valEnd - valStart - 1, newValue);
    return true;
}

void GCConfig::Save() const
{
    // load the file as plain text so we keep comments, tabs, blank lines,
    // and key ordering intact. we only rewrite three values in-place.
    std::string data = LoadFile(ConfigFilePath);

    bool foundLevel = false;
    bool foundXp = false;
    bool foundPrime = false;

    std::string out;
    out.reserve(data.size() + 64);

    size_t pos = 0;
    while (pos < data.size())
    {
        size_t eol = data.find('\n', pos);
        bool hasNewline = (eol != std::string::npos);
        if (!hasNewline)
            eol = data.size();

        std::string line = data.substr(pos, eol - pos);

        if (!foundLevel && ReplaceQuotedValueInLine(line, "player_level", std::to_string(m_level)))
        {
            foundLevel = true;
        }
        else if (!foundXp && ReplaceQuotedValueInLine(line, "player_cur_xp", std::to_string(m_xp)))
        {
            foundXp = true;
        }
        else if (!foundPrime && ReplaceQuotedValueInLine(line, "has_prime", m_hasPrime ? "1" : "0"))
        {
            foundPrime = true;
        }

        out += line;
        if (hasNewline)
            out += '\n';

        pos = eol + (hasNewline ? 1 : 0);
    }

    // if any key was missing from the file, insert it just before the
    // last closing brace so it still lives inside the "config" block.
    if (!foundLevel || !foundXp || !foundPrime)
    {
        std::string insert;
        if (!foundLevel)
            insert += "\t\"player_level\"\t\t\"" + std::to_string(m_level) + "\"\n";
        if (!foundXp)
            insert += "\t\"player_cur_xp\"\t\t\"" + std::to_string(m_xp) + "\"\n";
        if (!foundPrime)
            insert += "\t\"has_prime\"\t\t\"" + std::string(m_hasPrime ? "1" : "0") + "\"\n";

        size_t closePos = out.rfind('}');
        if (closePos != std::string::npos)
            out.insert(closePos, insert);
        else
            out += insert; // no closing brace? just append
    }

    FILE *f = fopen(ConfigFilePath, "wb");
    if (!f)
    {
        Platform::PrintError("GCConfig::Save: failed to open %s for writing\n", ConfigFilePath);
        return;
    }

    fwrite(out.data(), 1, out.size(), f);
    fclose(f);

    Platform::PrintSuccess("config.txt updated: player_level=%d player_cur_xp=%d has_prime=%d\n",
        m_level, m_xp, m_hasPrime ? 1 : 0);
}

float GCConfig::GetRarityWeight(uint32_t rarity) const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    for (const RarityWeight &weight : m_rarityWeights)
    {
        if (weight.rarity == rarity)
        {
            return weight.weight;
        }
    }
    return 0;
}

GCConfig &GetConfig()
{
    static GCConfig config;
    return config;
}