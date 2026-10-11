#pragma once

#include <QtGlobal>

namespace classiCAD {
class CollectionId final {
public:
    constexpr CollectionId() = default;
    static constexpr CollectionId invalid() { return CollectionId(); }
    static constexpr CollectionId fromValue(quint64 value) { return CollectionId(value); }
    constexpr quint64 value() const { return value_; }
    constexpr bool isValid() const { return value_ != 0; }
    friend constexpr bool operator==(CollectionId a, CollectionId b) { return a.value_ == b.value_; }
    friend constexpr bool operator!=(CollectionId a, CollectionId b) { return !(a == b); }
private:
    explicit constexpr CollectionId(quint64 value) : value_(value) {}
    quint64 value_ = 0;
};
} // namespace classiCAD
