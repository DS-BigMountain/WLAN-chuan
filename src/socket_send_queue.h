#pragma once
#include <winsock2.h>
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace chuan {
// Each posted buffer remains owned until its overlapped send completes.
// Four MiB per transfer bounds read-ahead while keeping multiple sends in flight.
class SocketSendQueue {
    struct Slot {
        std::vector<char> data;
        WSAOVERLAPPED operation{};
        DWORD length = 0;
        ULONGLONG submitted=0;
        ~Slot() { if (operation.hEvent) WSACloseEvent(operation.hEvent); }
    };
    SOCKET socket_;
    std::array<Slot, 4> slots_;
    size_t depth_, head_ = 0, pending_ = 0;
    std::function<bool()> active_;
    std::function<void(size_t)> completed_;
    int &error_;
    std::function<void(size_t,uint64_t,uint64_t)> observer_;
    void Notify()noexcept{try{if(observer_){uint64_t bytes=0;for(size_t i=0;i<pending_;++i)bytes+=slots_[(head_+i)%depth_].length;observer_(pending_,bytes,pending_?GetTickCount64()-slots_[head_].submitted:0);}}catch(...){}}

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
        head_ = (head_ + 1) % depth_;
        --pending_;
        Notify();
        completed_(sent);
    }
  public:
    void SetObserver(std::function<void(size_t,uint64_t,uint64_t)> observer){observer_=std::move(observer);Notify();}
    static constexpr size_t ChunkBytes = 1024 * 1024;
    SocketSendQueue(SOCKET socket, uint64_t bytes, std::function<bool()> active,
                    std::function<void(size_t)> completed, int &error)
        : socket_(socket), depth_(static_cast<size_t>(std::min<uint64_t>(4, std::max<uint64_t>(1, bytes / ChunkBytes + (bytes % ChunkBytes != 0))))),
          active_(std::move(active)), completed_(std::move(completed)), error_(error) {
        const auto capacity = static_cast<size_t>(std::min<uint64_t>(ChunkBytes, std::max<uint64_t>(1, bytes)));
        for (size_t i = 0; i < depth_; ++i) {
            slots_[i].data.resize(capacity);
            slots_[i].operation.hEvent = WSACreateEvent();
            if (slots_[i].operation.hEvent == WSA_INVALID_EVENT) Fail(WSAGetLastError());
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
    }
    std::vector<char> &Buffer() {
        if (!active_()) throw std::runtime_error("cancel");
        if (pending_ == depth_) CompleteOldest();
        return slots_[(head_ + pending_) % depth_].data;
    }
    void Submit(DWORD bytes) {
        if (!active_()) throw std::runtime_error("cancel");
        auto &slot = slots_[(head_ + pending_) % depth_];
        if (!bytes || bytes > slot.data.size() || pending_ == depth_) throw std::logic_error("send buffer");
        const auto event = slot.operation.hEvent;
        slot.operation = {}; slot.operation.hEvent = event; WSAResetEvent(event);
        slot.length = bytes;
        slot.submitted=GetTickCount64();
        WSABUF buffer{bytes, slot.data.data()};
        DWORD sent = 0;
        const auto status = WSASend(socket_, &buffer, 1, &sent, 0, &slot.operation, nullptr);
        if (status == SOCKET_ERROR) {
            const auto error = WSAGetLastError();
            if (error != WSA_IO_PENDING) Fail(error);
        }
        ++pending_;
        Notify();
    }
    void Finish() { while (pending_) CompleteOldest(); }
};
}
