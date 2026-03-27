#pragma once

#include <cstdint>

inline constexpr int64_t PRICE_SCALE = 1'000'000;

inline double to_display(int64_t fp) { return static_cast<double>(fp) / PRICE_SCALE; }

enum class EventType : uint8_t {
    ADD_LIMIT     = 0,
    ADD_MARKET    = 1,
    CANCEL        = 2,
    EXECUTE       = 3,
    TRADE         = 4,
    ADD_LIMIT_MKT = 5,   // marketable limit: executes immediately, residual rests passively
    RESET         = 255,
};

enum class Side : uint8_t { BID = 0, ASK = 1 };

inline const char* event_type_str(EventType t) {
    switch (t) {
        case EventType::ADD_LIMIT:  return "ADD_L";
        case EventType::ADD_MARKET: return "ADD_M";
        case EventType::CANCEL:     return "CANCL";
        case EventType::EXECUTE:    return "EXEC ";
        case EventType::TRADE:        return "TRADE";
        case EventType::ADD_LIMIT_MKT: return "ADD_LM";
        case EventType::RESET:        return "RESET";
    }
    return "?????";
}
