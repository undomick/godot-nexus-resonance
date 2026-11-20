#ifndef RESONANCE_OWNER_LINK_H
#define RESONANCE_OWNER_LINK_H

#include <atomic>
#include <thread>

namespace resonance {

/// Back pointer from an object another thread uses to an owner that may be destroyed first.
/// The user thread reads the owner only through a Lease; orphan() blocks until open leases end,
/// so the owner stays alive while a lease that saw it is open.
template <typename T>
class OwnerLink {
  public:
    class Lease {
      public:
        explicit Lease(OwnerLink& link) : link_(link) {
            link_.leases_.fetch_add(1, std::memory_order_seq_cst);
            owner_ = link_.owner_.load(std::memory_order_seq_cst);
        }
        ~Lease() { link_.leases_.fetch_sub(1, std::memory_order_release); }
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&&) = delete;
        Lease& operator=(Lease&&) = delete;

        T* get() const { return owner_; }

      private:
        OwnerLink& link_;
        T* owner_ = nullptr;
    };

    void set(T* owner) { owner_.store(owner, std::memory_order_seq_cst); }

    /// Safe while holding a lease, or on the thread that calls orphan().
    T* get() const { return owner_.load(std::memory_order_acquire); }

    Lease lease() { return Lease(*this); }

    /// Must not be called while the calling thread holds a lease on this link.
    void orphan() {
        owner_.store(nullptr, std::memory_order_seq_cst);
        while (leases_.load(std::memory_order_seq_cst) > 0)
            std::this_thread::yield();
    }

  private:
    std::atomic<T*> owner_{nullptr};
    std::atomic<int> leases_{0};
};

} // namespace resonance

#endif // RESONANCE_OWNER_LINK_H
