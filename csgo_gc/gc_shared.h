// gc_shared.h
#pragma once

#include "gc_message.h"

enum class HostEvent
{
    Message,
    NetMessage,
    MicroTransactionResponse,
    PartyInvite,
    PartyJoinRelay,
    PartyLobbyUpdate,
};

enum class GCEvent
{
    Message,
    NetMessage,
    SOCacheRequest,
    ClientSOCacheUnsubscribe,
    PartyIncoming,
    ReloadFile,
    NativeLobbyUpdate,      // NEW: roster mirrored from SteamMatchmakingProxy
};

// shared logic between client and server gcs
class SharedGC
{
public:
    void PostToGC(GCEvent type, uint64_t id, const void *data, uint32_t dataSize);

    bool PollFromGC(HostEvent &type, uint64_t &id, std::vector<uint8_t> &buffer);
    void StartThread();
	
    void StopThread();
protected:

    void PostToHost(HostEvent type, uint64_t id, const void *data, uint32_t dataSize);

private:
    virtual void HandleEvent(GCEvent type, uint64_t id, const std::vector<uint8_t> &buffer) = 0;

    void WorkerThread();

    struct Event
    {
        int type;
        uint64_t id;
        std::vector<uint8_t> buffer;
    };

    std::mutex m_gcEventMutex;
    std::condition_variable m_cv;
    std::queue<Event> m_gcEvents;
    std::thread m_thread;
    bool m_stopping{ false };

    std::mutex m_hostEventMutex;
    std::queue<Event> m_hostEvents;
};

const char *MessageName(uint32_t type);