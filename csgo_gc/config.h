#pragma once

#include "gc_const_csgo.h"
#include "item_schema.h" // rarity constants

struct RarityWeight
{
    uint32_t rarity;
    float weight;
};

class GCConfig
{
public:
    GCConfig();
    void ReloadFromFile();
    void Save() const;

    uint32_t AppIdOverride() const { return m_appIdOverride; }
    bool ShowCsgoGCServersOnly() const { return m_showCsgoGCServersOnly; }

    RankId CompetitiveRank() const { return m_competitiveRank; }
    int CompetitiveWins() const { return m_competitiveWins; }
    RankId WingmanRank() const { return m_wingmanRank; }
    int WingmanWins() const { return m_wingmanWins; }
    DangerZoneRankId DangerZoneRank() const { return m_dangerZoneRank; }
    int DangerZoneWins() const { return m_dangerZoneWins; }

    bool ForceMaxRarity() const { return m_forceMaxRarity; }
    bool DestroyUsedItems() const { return m_destroyUsedItems; }
    bool RandomizeFloat() const { return m_randomizeFloat; }

    bool     ReportSharing()           const { return m_reportSharing; }
    uint32_t ReportCooldownThreshold() const { return m_reportCooldownThreshold; }
    uint32_t ReportCooldownSeconds()   const { return m_reportCooldownSeconds; }

    bool VacBanned() const { return m_vacBanned; }
    bool HasPrime() const { return m_hasPrime; }
    uint32_t CompetitiveCooldownSeconds() const { return m_competitiveCooldownSeconds; }
    // 6 is competitive cooldown, the rest of the values are not documented
    // SFUI_MainMenu_Global_Ban_Title is somewhere in there try a few
    uint32_t CooldownReason() const { return m_cooldownReason; }

    int CommendedFriendly() const { return m_commendedFriendly; }
    int CommendedTeaching() const { return m_commendedTeaching; }
    int CommendedLeader() const { return m_commendedLeader; }
    int Level() const { return m_level; }
    int Xp() const { return m_xp; }

    void SetLevel(int level) { m_level = level; }
    void SetXp(int xp) { m_xp = xp; }

    // these two return std::string - they absolutely need the lock,
    // otherwise use-after-free is possible during ReloadFromFile
    std::string Country() const
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return m_country;
    }
    int Currency() const { return m_currency; }

    float GetRarityWeight(uint32_t rarity) const;

    std::vector<int> GetFriends() const
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return m_friends;
    }
	bool TestDiag() const { return false; }
	void SetHasPrime(bool prime) { m_hasPrime = prime; }

    // --- LTP (looking to play) worker ---
    // returns the full base URL including scheme, no trailing slash
    // (e.g. "https://csgo-gc.example.workers.dev")
    std::string LtpServerUrl() const
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return m_ltpServerUrl;
    }
    bool UseLtpServer() const { return m_useLtpServer; }

    // --- party backend ---
    // empty = party traffic is disabled (messages get dropped silently)
    std::string PartyServerUrl() const
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return m_partyServerUrl;
    }

private:
    void Parse(const KeyValue& config);
    mutable std::recursive_mutex m_mutex;
    // actually default to 4465480 instead of 730
    uint32_t m_appIdOverride{ 4465480 };
    bool m_showCsgoGCServersOnly{ true };

    RankId m_competitiveRank{ RankNone };
    int m_competitiveWins{ 0 };
    RankId m_wingmanRank{ RankNone };
    int m_wingmanWins{ 0 };
    DangerZoneRankId m_dangerZoneRank{ DangerZoneRankNone };
    int m_dangerZoneWins{ 0 };

    bool m_forceMaxRarity{ false };
    bool m_destroyUsedItems{ true };
    bool m_randomizeFloat{ true };

    bool     m_reportSharing{ true };
    uint32_t m_reportCooldownThreshold{ 0 };
    uint32_t m_reportCooldownSeconds{ 0 };

    bool m_vacBanned{ false };
    bool m_hasPrime{ true };
    uint32_t m_competitiveCooldownSeconds{ 0 };
    uint32_t m_cooldownReason{ 6 };
    int m_commendedFriendly{ 0 };
    int m_commendedTeaching{ 0 };
    int m_commendedLeader{ 0 };
    int m_level{ 0 };
    int m_xp{ 0 };

    std::string m_country{ "RU" };
    int m_currency{ 3 };

    // --- LTP worker ---
    // empty by default so the old friends-list fallback still works if
    // the operator forgets to set it
    std::string m_ltpServerUrl;
    bool m_useLtpServer{ false };

    // --- party backend ---
    std::string m_partyServerUrl;

    std::vector<RarityWeight> m_rarityWeights{
        { ItemSchema::RarityCommon, 10000000 },
        { ItemSchema::RarityUncommon, 2000000 },
        { ItemSchema::RarityRare, 400000 },
        { ItemSchema::RarityMythical, 80000 },
        { ItemSchema::RarityLegendary, 16000 },
        { ItemSchema::RarityAncient, 3200 },
        { ItemSchema::RarityUnusual, 1280 },
    };

    std::vector<int> m_friends{ 1140104601 };
};

GCConfig &GetConfig();