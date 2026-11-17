#pragma once

#include <cstdint>
#include <string>

namespace voltstore {

// Unix time in milliseconds; Entry uses zero for no expiration.
using ClockMs = std::int64_t;

// Current wall-clock time in Unix milliseconds for persisted TTL deadlines.
ClockMs now_ms();

// Entry owns its value and stores an optional absolute expiry time.
struct Entry {
    std::string value;
    ClockMs expires_at_ms{0};
};

// An entry expires at or after its absolute deadline. Caller supplies the
// timestamp so one operation can make a consistent expiry decision.
inline bool is_expired(const Entry& e, ClockMs now) {
    return e.expires_at_ms != 0 && e.expires_at_ms <= now;
}
}  // namespace voltstore
