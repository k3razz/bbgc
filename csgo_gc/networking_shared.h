#pragma once

#include <steam/isteamnetworkingmessages.h>

constexpr int NetMessageSendFlags = k_nSteamNetworkingSend_Reliable;

// Каналы SteamNetworkingMessages. Должны быть разными, чтобы серверный
// и party-трафик не пересекались.
constexpr int NetMessageChannel = 7;   // клиент <-> свой gameserver
constexpr int PartyChannel      = 8;   // P2P между игроками (party)

// NOTE: these are used as gc message types!
// if they overlap with the game's gc messages, we're doomed
enum ENetworkMsg : uint32_t
{
    // sent by the server to client when they connect, data is the auth ticket
    k_EMsgNetworkConnect = (1u << 31) - 1,
};