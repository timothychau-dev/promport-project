#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

// Foreground audio apps share this storage. Release it only after their worker
// has stopped using its stack or buffers.
namespace audio_workspace {
enum class Owner { None, Radio, Xiaozhi };
constexpr std::size_t kRealtimeBytes = 22 * 1024;
// Same exclusive owner as the realtime stack/frame area. Xiaozhi's Opus
// half-duplex encoder/decoder and the Radio compressed FIFO never coexist.
// Codec handoffs require the audio worker to be completely idle.
constexpr std::size_t kAuxBytes = 26 * 1024;
constexpr std::size_t kBytes = kRealtimeBytes + kAuxBytes;
alignas(16) inline std::uint8_t storage[kBytes];
inline std::uint8_t *aux() { return storage + kRealtimeBytes; }
// Explicit app layouts: these slices coexist only within the exclusive owner.
namespace radio {
constexpr std::size_t kCompressedBytes = 16 * 1024;
inline std::uint8_t *stations() { return aux() + kCompressedBytes; }
constexpr std::size_t kStationBytes = kAuxBytes - kCompressedBytes;
}
inline std::atomic<Owner> owner{Owner::None};
inline bool acquire(Owner requested) {
    Owner empty = Owner::None;
    return requested != Owner::None && owner.compare_exchange_strong(empty, requested);
}
inline bool release(Owner requested) {
    return requested != Owner::None && owner.compare_exchange_strong(requested, Owner::None);
}
}
