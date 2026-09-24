#pragma once

#include <QtGlobal>

namespace classiCAD {

// ObjectId is assigned once to a persistent scene object. It is deliberately
// independent of the object's position in a layer or document container.
class ObjectId final {
public:
    constexpr ObjectId() = default;

    static constexpr ObjectId invalid()
    {
        return ObjectId();
    }

    static constexpr ObjectId fromValue(quint64 value)
    {
        return ObjectId(value);
    }

    constexpr quint64 value() const
    {
        return value_;
    }

    constexpr bool isValid() const
    {
        return value_ != 0;
    }

    friend constexpr bool operator==(ObjectId first, ObjectId second)
    {
        return first.value_ == second.value_;
    }

    friend constexpr bool operator!=(ObjectId first, ObjectId second)
    {
        return !(first == second);
    }

private:
    explicit constexpr ObjectId(quint64 value)
        : value_(value)
    {
    }

    quint64 value_ = 0;
};

} // namespace classiCAD
