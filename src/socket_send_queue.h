#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mstcpip.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace chuan {
// Each posted buffer remains owned until its overlapped send completes.
// Four MiB per transfer bounds read-ahead while adaptive BDP/RTT pacing prevents bufferbloat.
class SocketSendQueue {
  public:
    struct TcpSample {
        uint32_t rttUs = 0;
        uint32_t minRttUs = 0;
        uint32_t bytesInFlight = 0;
        uint32_t cwndBytes = 0;
        uint32_t idealBacklogBytes = 0;
        uint32_t retransmittedBytes = 0;
        bool valid = false;
    };
    static constexpr size_t ChunkBytes = 1024 * 1024;
    static constexpr size_t MinChunkBytes = 64 * 1024;
    static constexpr size_t MinBacklogBytes = 128 * 1024;
    static constexpr size_t MaxTotalBacklogBytes = 4 * ChunkBytes;

    static bool IsLoopbackSocket(SOCKET socket) noexcept {
        if (socket == INVALID_SOCKET) return true;
        sockaddr_in peer{};
        int length = sizeof(peer);
        if (getpeername(socket, reinterpret_cast<sockaddr *>(&peer), &length) != 0) return true;
        if (peer.sin_family != AF_INET) return true;
        return (ntohl(peer.sin_addr.s_addr) >> 24) == 127;
    }
    static TcpSample SampleSocket(SOCKET socket) noexcept {
        TcpSample sample{};
        if (socket == INVALID_SOCKET) return sample;
        TCP_INFO_v0 tcp{};
        ULONG version = 0;
        DWORD returned = 0;
        if (WSAIoctl(socket, SIO_TCP_INFO, &version, sizeof(version), &tcp, sizeof(tcp), &returned, nullptr, nullptr) == 0) {
            sample.rttUs = tcp.RttUs;
            sample.minRttUs = tcp.MinRttUs;
            sample.bytesInFlight = tcp.BytesInFlight;
            sample.cwndBytes = tcp.Cwnd;
            sample.retransmittedBytes = tcp.BytesRetrans;
            sample.valid = true;
        }
        ULONG ideal = 0;
        if (WSAIoctl(socket, SIO_IDEAL_SEND_BACKLOG_QUERY, nullptr, 0, &ideal, sizeof(ideal), &returned, nullptr, nullptr) == 0) {
            sample.idealBacklogBytes = ideal;
        }
        return sample;
    }
    static void PaceBlockingSocket(SOCKET socket, const std::function<bool()> &active) {
        if (IsLoopbackSocket(socket)) return;
        const auto sample = SampleSocket(socket);
        if (!sample.valid || sample.minRttUs == 0 || sample.rttUs <= sample.minRttUs + 18000) return;
        const uint32_t targetRttUs = sample.minRttUs + 10000;
        const uint32_t window = std::max(sample.idealBacklogBytes, sample.cwndBytes);
        const size_t activeQueues = std::max<size_t>(1, activeNetworkQueues_.load(std::memory_order_relaxed));
        const size_t maxBudget = std::max<size_t>(MinBacklogBytes, MaxTotalBacklogBytes / activeQueues);
        const size_t budget = window > 0
            ? std::clamp<size_t>(static_cast<size_t>(static_cast<uint64_t>(window) * targetRttUs / std::max(sample.rttUs, targetRttUs)), MinBacklogBytes, maxBudget)
            : MinBacklogBytes;
        if (sample.bytesInFlight < budget) return;
        const auto deadline = GetTickCount64() + 12;
        while (GetTickCount64() < deadline) {
            if (!active()) throw std::runtime_error("cancel");
            Sleep(1);
            const auto next = SampleSocket(socket);
            if (!next.valid || next.bytesInFlight < budget || next.rttUs <= next.minRttUs + 18000) break;
        }
    }

  private:
    struct Slot {
        std::vector<char> data;
        WSAOVERLAPPED operation{};
        DWORD length = 0;
        ULONGLONG submitted = 0;
        ~Slot() { if (operation.hEvent) WSACloseEvent(operation.hEvent); }
    };
    static inline std::atomic<size_t> activeNetworkQueues_{0};
    SOCKET socket_;
    std::array<Slot, 4> slots_;
    size_t depth_, capacity_, head_ = 0, pending_ = 0, pendingBytes_ = 0;
    bool adaptive_ = false, countedQueue_ = false, hasSampled_ = false;
    size_t targetDepth_ = 4;
    size_t targetBacklogBytes_ = MaxTotalBacklogBytes;
    size_t chunkSize_ = ChunkBytes;
    uint32_t baseRttUs_ = UINT32_MAX;
    uint32_t queueDelayUs_ = 0;
    uint32_t lastRetransBytes_ = 0;
    double deliveryRateBytesPerMs_ = 0.0;
    ULONGLONG lastCompletionTick_ = 0;
    ULONGLONG lastSampleTick_ = 0;
    TcpSample lastSample_{};
    std::function<TcpSample()> sampler_;
    std::function<bool()> active_;
    std::function<void(size_t)> completed_;
    int &error_;
    std::function<void(size_t, uint64_t, uint64_t)> observer_;

    void RegisterAdaptiveQueue() noexcept {
        if (!countedQueue_) {
            countedQueue_ = true;
            activeNetworkQueues_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void UnregisterAdaptiveQueue() noexcept {
        if (countedQueue_) {
            countedQueue_ = false;
            activeNetworkQueues_.fetch_sub(1, std::memory_order_relaxed);
        }
    }
    void InitializeAdaptiveDefaults() noexcept {
        const size_t activeQueues = std::max<size_t>(1, activeNetworkQueues_.load(std::memory_order_relaxed));
        const size_t maxBudget = std::max<size_t>(MinBacklogBytes, MaxTotalBacklogBytes / activeQueues);
        targetBacklogBytes_ = std::clamp<size_t>(ChunkBytes / activeQueues, MinBacklogBytes, maxBudget);
        targetDepth_ = std::min<size_t>(depth_, 2);
        const size_t rawChunk = targetBacklogBytes_ / std::max<size_t>(2, targetDepth_);
        const size_t alignedChunk = (rawChunk / MinChunkBytes) * MinChunkBytes;
        chunkSize_ = std::clamp<size_t>(alignedChunk ? alignedChunk : MinChunkBytes, std::min(MinChunkBytes, capacity_), capacity_);
    }
    void SampleAndAdapt() noexcept {
        if (!adaptive_) return;
        lastSampleTick_ = GetTickCount64();
        const TcpSample sample = sampler_ ? sampler_() : SampleSocket(socket_);
        lastSample_ = sample;
        const size_t activeQueues = std::max<size_t>(1, activeNetworkQueues_.load(std::memory_order_relaxed));
        const size_t maxBudget = std::max<size_t>(MinBacklogBytes, MaxTotalBacklogBytes / activeQueues);
        if (!sample.valid) {
            if (deliveryRateBytesPerMs_ > 0.0) {
                const size_t appBdp = static_cast<size_t>(deliveryRateBytesPerMs_ * 14.0);
                targetBacklogBytes_ = std::clamp<size_t>((targetBacklogBytes_ * 2 + appBdp) / 3, MinBacklogBytes, maxBudget);
                const size_t rawChunk = targetBacklogBytes_ / std::max<size_t>(2, targetDepth_);
                const size_t alignedChunk = (rawChunk / MinChunkBytes) * MinChunkBytes;
                chunkSize_ = std::clamp<size_t>(alignedChunk ? alignedChunk : MinChunkBytes, std::min(MinChunkBytes, capacity_), capacity_);
            }
            return;
        }
        if (sample.minRttUs > 0) baseRttUs_ = std::min(baseRttUs_, sample.minRttUs);
        if (sample.rttUs > 0) baseRttUs_ = std::min(baseRttUs_, sample.rttUs);
        const uint32_t baseRtt = (baseRttUs_ == UINT32_MAX || baseRttUs_ == 0) ? 2000u : baseRttUs_;
        const uint32_t rtt = sample.rttUs > 0 ? sample.rttUs : baseRtt;
        queueDelayUs_ = rtt > baseRtt ? (rtt - baseRtt) : 0;
        const bool retransmitting = sample.retransmittedBytes > lastRetransBytes_;
        lastRetransBytes_ = sample.retransmittedBytes;

        uint32_t targetQueueUs = 10000;
        double gain = 1.0;
        size_t desiredDepth = std::min<size_t>(depth_, activeQueues <= 2 ? 3 : 2);
        if (queueDelayUs_ <= 8000 && !retransmitting) {
            targetQueueUs = 15000;
            gain = 1.35;
            desiredDepth = std::min<size_t>(depth_, activeQueues == 1 ? 4 : (activeQueues == 2 ? 3 : 2));
        } else if (queueDelayUs_ > 18000 || retransmitting) {
            targetQueueUs = 6000;
            gain = 0.85;
            desiredDepth = std::min<size_t>(depth_, queueDelayUs_ > 35000 ? 1 : 2);
        }

        const uint32_t targetRttUs = baseRtt + targetQueueUs;
        double estimatedBdp = 0.0;
        const uint32_t kernelWindow = std::max(sample.idealBacklogBytes, sample.cwndBytes);
        if (kernelWindow > 0) {
            const double debloatRatio = static_cast<double>(targetRttUs) / static_cast<double>(std::max(rtt, targetRttUs));
            estimatedBdp = static_cast<double>(kernelWindow) * debloatRatio;
        }
        if (deliveryRateBytesPerMs_ > 0.0) {
            const double appBdp = deliveryRateBytesPerMs_ * (static_cast<double>(targetRttUs) / 1000.0);
            if (estimatedBdp <= 0.0) {
                estimatedBdp = appBdp;
            } else if (queueDelayUs_ > 18000 || retransmitting) {
                estimatedBdp = std::min(estimatedBdp, appBdp * 1.25);
            } else {
                estimatedBdp = std::max(estimatedBdp, appBdp);
            }
        }
        if (estimatedBdp <= 0.0) {
            estimatedBdp = static_cast<double>(targetBacklogBytes_);
        }

        const size_t desiredBudget = std::clamp<size_t>(static_cast<size_t>(estimatedBdp * gain), MinBacklogBytes, maxBudget);
        if (!hasSampled_) {
            targetBacklogBytes_ = desiredBudget;
            hasSampled_ = true;
        } else if (queueDelayUs_ > 18000 || retransmitting) {
            targetBacklogBytes_ = std::min(desiredBudget, (targetBacklogBytes_ + desiredBudget) / 2);
        } else {
            targetBacklogBytes_ = (targetBacklogBytes_ * 2 + desiredBudget) / 3;
        }
        targetBacklogBytes_ = std::clamp<size_t>(targetBacklogBytes_, MinBacklogBytes, maxBudget);
        targetDepth_ = std::max<size_t>(1, desiredDepth);

        const size_t slotsForChunk = std::max<size_t>(2, targetDepth_);
        const size_t rawChunk = targetBacklogBytes_ / slotsForChunk;
        const size_t alignedChunk = (rawChunk / MinChunkBytes) * MinChunkBytes;
        chunkSize_ = std::clamp<size_t>(alignedChunk ? alignedChunk : MinChunkBytes, std::min(MinChunkBytes, capacity_), capacity_);
    }
    void PaceKernelInFlight() {
        if (!adaptive_ || !lastSample_.valid || queueDelayUs_ <= 18000) return;
        while (pending_ > 0 && pendingBytes_ + lastSample_.bytesInFlight >= targetBacklogBytes_ && queueDelayUs_ > 18000) {
            CompleteOldest();
        }
        if (pending_ == 0 && lastSample_.bytesInFlight >= targetBacklogBytes_ && queueDelayUs_ > 18000) {
            const auto deadline = GetTickCount64() + 12;
            while (GetTickCount64() < deadline) {
                if (!active_()) throw std::runtime_error("cancel");
                Sleep(1);
                SampleAndAdapt();
                if (!lastSample_.valid || lastSample_.bytesInFlight < targetBacklogBytes_ || queueDelayUs_ <= 18000)
                    break;
            }
        }
    }
    void Notify() noexcept {
        try {
            if (observer_) {
                observer_(pending_, pendingBytes_, pending_ ? GetTickCount64() - slots_[head_].submitted : 0);
            }
        } catch (...) {}
    }

    [[noreturn]] void Fail(int error) {
        error_ = error;
        throw std::runtime_error("send");
    }
    void CompleteOldest() {
        auto &slot = slots_[head_];
        const auto deadline = GetTickCount64() + 30000;
        for (;;) {
            if (!active_()) throw std::runtime_error("cancel");
            const auto status = WSAWaitForMultipleEvents(1, &slot.operation.hEvent, TRUE, 100, FALSE);
            if (status == WSA_WAIT_EVENT_0) break;
            if (status == WSA_WAIT_FAILED) Fail(WSAGetLastError());
            if (GetTickCount64() >= deadline) Fail(WSAETIMEDOUT);
        }
        DWORD sent = 0, flags = 0;
        if (!WSAGetOverlappedResult(socket_, &slot.operation, &sent, FALSE, &flags)) Fail(WSAGetLastError());
        // An incomplete posted send cannot be requeued behind later data.
        if (sent != slot.length) Fail(WSAECONNABORTED);
        const auto now = GetTickCount64();
        const auto baseTick = lastCompletionTick_ > slot.submitted ? lastCompletionTick_ : slot.submitted;
        const auto elapsedMs = std::max<ULONGLONG>(1, now >= baseTick ? now - baseTick : 1);
        lastCompletionTick_ = now;
        const double instantRate = static_cast<double>(sent) / static_cast<double>(elapsedMs);
        deliveryRateBytesPerMs_ = deliveryRateBytesPerMs_ <= 0.0
            ? instantRate
            : (deliveryRateBytesPerMs_ * 0.7 + instantRate * 0.3);
        head_ = (head_ + 1) % depth_;
        --pending_;
        pendingBytes_ = pendingBytes_ >= sent ? pendingBytes_ - sent : 0;
        SampleAndAdapt();
        Notify();
        completed_(sent);
    }

  public:
    void SetObserver(std::function<void(size_t, uint64_t, uint64_t)> observer) {
        observer_ = std::move(observer);
        Notify();
    }
    void EnableAdaptive(std::function<TcpSample()> sampler = {}) {
        sampler_ = std::move(sampler);
        adaptive_ = true;
        hasSampled_ = false;
        RegisterAdaptiveQueue();
        InitializeAdaptiveDefaults();
        SampleAndAdapt();
    }
    size_t ChunkSize() const noexcept { return adaptive_ ? chunkSize_ : capacity_; }
    size_t TargetDepth() const noexcept { return adaptive_ ? targetDepth_ : depth_; }
    size_t TargetBacklogBytes() const noexcept { return adaptive_ ? targetBacklogBytes_ : depth_ * capacity_; }
    uint32_t QueueDelayUs() const noexcept { return queueDelayUs_; }

    SocketSendQueue(SOCKET socket, uint64_t bytes, std::function<bool()> active,
                    std::function<void(size_t)> completed, int &error)
        : socket_(socket),
          depth_(static_cast<size_t>(std::min<uint64_t>(4, std::max<uint64_t>(1, (bytes + MinChunkBytes - 1) / MinChunkBytes)))),
          capacity_(static_cast<size_t>(std::min<uint64_t>(ChunkBytes, std::max<uint64_t>(1, bytes)))),
          targetDepth_(depth_),
          targetBacklogBytes_(depth_ * capacity_),
          chunkSize_(capacity_),
          active_(std::move(active)),
          completed_(std::move(completed)),
          error_(error) {
        for (size_t i = 0; i < depth_; ++i) {
            slots_[i].data.resize(capacity_);
            slots_[i].operation.hEvent = WSACreateEvent();
            if (slots_[i].operation.hEvent == WSA_INVALID_EVENT) Fail(WSAGetLastError());
        }
        if (!IsLoopbackSocket(socket_)) {
            EnableAdaptive();
        }
    }
    SocketSendQueue(const SocketSendQueue &) = delete;
    SocketSendQueue &operator=(const SocketSendQueue &) = delete;
    ~SocketSendQueue() {
        // Cancellation must finish before the OS can release or reuse our buffers.
        for (size_t i = 0; i < pending_; ++i)
            CancelIoEx(reinterpret_cast<HANDLE>(socket_), &slots_[(head_ + i) % depth_].operation);
        for (size_t i = 0; i < pending_; ++i) {
            DWORD sent = 0, flags = 0;
            WSAGetOverlappedResult(socket_, &slots_[(head_ + i) % depth_].operation, &sent, TRUE, &flags);
        }
        UnregisterAdaptiveQueue();
    }
    std::vector<char> &Buffer() {
        if (!active_()) throw std::runtime_error("cancel");
        if (!adaptive_) {
            if (pending_ == depth_) CompleteOldest();
            return slots_[(head_ + pending_) % depth_].data;
        }
        if (GetTickCount64() - lastSampleTick_ >= 4) SampleAndAdapt();
        while (pending_ >= std::min(depth_, targetDepth_) ||
               (pending_ > 0 && pendingBytes_ >= targetBacklogBytes_)) {
            CompleteOldest();
        }
        PaceKernelInFlight();
        return slots_[(head_ + pending_) % depth_].data;
    }
    void Submit(DWORD bytes) {
        if (!active_()) throw std::runtime_error("cancel");
        auto &slot = slots_[(head_ + pending_) % depth_];
        if (!bytes || bytes > slot.data.size() || pending_ == depth_) throw std::logic_error("send buffer");
        const auto event = slot.operation.hEvent;
        slot.operation = {};
        slot.operation.hEvent = event;
        WSAResetEvent(event);
        slot.length = bytes;
        slot.submitted = GetTickCount64();
        WSABUF buffer{bytes, slot.data.data()};
        DWORD sent = 0;
        const auto status = WSASend(socket_, &buffer, 1, &sent, 0, &slot.operation, nullptr);
        if (status == SOCKET_ERROR) {
            const auto error = WSAGetLastError();
            if (error != WSA_IO_PENDING) Fail(error);
        }
        ++pending_;
        pendingBytes_ += bytes;
        Notify();
    }
    void Finish() { while (pending_) CompleteOldest(); }
};
}
