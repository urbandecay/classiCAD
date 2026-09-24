#pragma once

#include <QtGlobal>

namespace classiCAD {

// LayerId is the stable identity of a document layer, not its display order.
class LayerId final {
public:
    constexpr LayerId() = default;

    static constexpr LayerId invalid()
    {
        return LayerId();
    }

    static constexpr LayerId fromValue(quint64 value)
    {
        return LayerId(value);
    }

    constexpr quint64 value() const
    {
        return value_;
    }

    constexpr bool isValid() const
    {
        return value_ != 0;
    }

    friend constexpr bool operator==(LayerId first, LayerId second)
    {
        return first.value_ == second.value_;
    }

    friend constexpr bool operator!=(LayerId first, LayerId second)
    {
        return !(first == second);
    }

private:
    explicit constexpr LayerId(quint64 value)
        : value_(value)
    {
    }

    quint64 value_ = 0;
};

} // namespace classiCAD
