#pragma once

#include <chrono>
#include <algorithm>
#include <memory>
#include <unordered_map>

// простой token bucket. не потокобезопасен сам по себе —
// вызывающая сторона должна держать свой mutex
class TokenBucket
{
public:
    TokenBucket(float ratePerSecond, float burst)
        : m_rate{ ratePerSecond }
        , m_burst{ burst }
        , m_tokens{ burst }
        , m_last{ std::chrono::steady_clock::now() }
    {
    }

    bool TryConsume(float amount = 1.0f)
    {
        Refill();
        if (m_tokens >= amount) { m_tokens -= amount; return true; }
        return false;
    }

    float SecondsUntil(float amount = 1.0f) const
    {
        if (m_tokens >= amount) return 0.0f;
        return (amount - m_tokens) / m_rate;
    }

private:
    void Refill()
    {
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - m_last).count();
        m_last = now;
        m_tokens = (std::min)(m_burst, m_tokens + dt * m_rate);
    }

    const float m_rate;
    const float m_burst;
    float m_tokens;
    std::chrono::steady_clock::time_point m_last;
};

// per-player лимитер. чистит старые бакеты, чтобы не течь по памяти
class PerPlayerRateLimiter
{
public:
    PerPlayerRateLimiter(float rate, float burst, float pruneAfterSeconds = 300.0f)
        : m_rate{ rate }
        , m_burst{ burst }
        , m_pruneAfter{ pruneAfterSeconds }
    {
    }

    bool TryConsume(uint32_t accountId)
    {
        auto it = m_buckets.find(accountId);
        if (it == m_buckets.end())
        {
            it = m_buckets.emplace(accountId,
                std::make_unique<BucketEntry>()).first;
            it->second->bucket = std::make_unique<TokenBucket>(m_rate, m_burst);
        }
        it->second->lastSeen = std::chrono::steady_clock::now();
        return it->second->bucket->TryConsume();
    }

    // вызывать раз в N секунд из main loop
    void Prune()
    {
        auto now = std::chrono::steady_clock::now();
        for (auto it = m_buckets.begin(); it != m_buckets.end();)
        {
            float idle = std::chrono::duration<float>(now - it->second->lastSeen).count();
            if (idle > m_pruneAfter) { it = m_buckets.erase(it); }
            else { ++it; }
        }
    }

private:
    struct BucketEntry
    {
        std::unique_ptr<TokenBucket> bucket;
        std::chrono::steady_clock::time_point lastSeen{
            std::chrono::steady_clock::now() };
    };

    const float m_rate;
    const float m_burst;
    const float m_pruneAfter;
    std::unordered_map<uint32_t, std::unique_ptr<BucketEntry>> m_buckets;
};