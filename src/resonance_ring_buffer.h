#ifndef RESONANCE_RING_BUFFER_H
#define RESONANCE_RING_BUFFER_H

#include <algorithm>
#include <cstring>
#include <vector>

namespace godot {

/// SPSC FIFO for bridging Godot `mix_audio()` (variable/partial frame counts) to fixed IPL `frameSize` in ResonancePlayer / AmbisonicPlayer.
/// Single producer, single consumer. When full, write() stores only what fits and drops the rest; compare the
/// return value to n or poll get_overflow_drop_count() to detect loss. resize(0) makes write/read no-ops.
/// `T` must be trivially copyable (`memcpy`).
template <typename T>
class RingBuffer {
  private:
    std::vector<T> buffer;
    size_t read_pos = 0;
    size_t write_pos = 0;
    size_t capacity = 0;
    size_t count = 0;
    size_t overflow_drop_count = 0;

  public:
    /// Resize buffer; resets read/write positions. Call only before first use or when no read/write is in progress.
    void resize(size_t p_capacity) {
        buffer.resize(p_capacity, 0);
        capacity = p_capacity;
        read_pos = 0;
        write_pos = 0;
        count = 0;
        overflow_drop_count = 0;
    }

    size_t get_available_read() const { return count; }
    size_t get_available_write() const { return capacity - count; }
    size_t get_overflow_drop_count() const { return overflow_drop_count; }

    /// Returns the number of elements written (may be less than n when the buffer is full).
    size_t write(const T* data, size_t n) {
        if (capacity == 0 || n == 0) {
            return 0;
        }
        const size_t requested = n;
        if (n > get_available_write())
            n = get_available_write();
        if (requested > n)
            overflow_drop_count += requested - n;

        size_t first_chunk = std::min(n, capacity - write_pos);
        memcpy(buffer.data() + write_pos, data, first_chunk * sizeof(T));

        size_t second_chunk = n - first_chunk;
        if (second_chunk > 0) {
            memcpy(buffer.data(), data + first_chunk, second_chunk * sizeof(T));
        }

        write_pos = (write_pos + n) % capacity;
        count += n;
        return n;
    }

    void read(T* out_data, size_t n) {
        if (capacity == 0 || n == 0) {
            return;
        }
        if (n > count)
            n = count;

        size_t first_chunk = std::min(n, capacity - read_pos);
        memcpy(out_data, buffer.data() + read_pos, first_chunk * sizeof(T));

        size_t second_chunk = n - first_chunk;
        if (second_chunk > 0) {
            memcpy(out_data + first_chunk, buffer.data(), second_chunk * sizeof(T));
        }

        read_pos = (read_pos + n) % capacity;
        count -= n;
    }

    void clear() {
        read_pos = 0;
        write_pos = 0;
        count = 0;
        overflow_drop_count = 0;
    }
};

} // namespace godot

#endif // RESONANCE_RING_BUFFER_H
