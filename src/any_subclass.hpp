#pragma once

// Inline storage for a polymorphic object whose concrete subclass is
// chosen at runtime (see Skia's SkAnySubclass). Holds exactly one live
// instance at a time. Base must have a virtual destructor.
//
//     AnySubclass<WindowSession, 128> session;
//     session.emplace<XcbWindowSession>();
//     session->poll_events(...);
//
// emplace<T>() static_asserts that T fits in Size and respects Align, so
// a subclass outgrowing the buffer is a build error, not a corruption.

#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

template <typename Base, std::size_t Size, std::size_t Align = alignof(void*)>
class AnySubclass {
public:
    AnySubclass() = default;
    AnySubclass(const AnySubclass&) = delete;
    AnySubclass& operator=(const AnySubclass&) = delete;
    ~AnySubclass() { reset(); }

    template <typename T, typename... Args>
    T& emplace(Args&&... args)
    {
        static_assert(std::is_base_of_v<Base, T>,
                      "AnySubclass: T must derive from Base");
        static_assert(sizeof(T) <= Size,
                      "AnySubclass: Size too small for T — grow the buffer");
        static_assert(alignof(T) <= Align,
                      "AnySubclass: Align too small for T — grow the alignment");
        reset();
        T* p = ::new (static_cast<void*>(buf_)) T(std::forward<Args>(args)...);
        ptr_ = p;
        return *p;
    }

    void reset()
    {
        if (ptr_) {
            ptr_->~Base();
            ptr_ = nullptr;
        }
    }

    explicit operator bool() const { return ptr_ != nullptr; }

    Base*       get()        { return ptr_; }
    const Base* get() const  { return ptr_; }
    Base*       operator->() { return ptr_; }
    const Base* operator->() const { return ptr_; }
    Base&       operator*()        { return *ptr_; }
    const Base& operator*() const  { return *ptr_; }

private:
    alignas(Align) unsigned char buf_[Size]{};
    Base* ptr_ = nullptr;
};
