#include "workbench_random_color.h"

#include <QByteArray>

#include <cstring>

namespace classiCAD {
namespace {

quint32 blenderStringHash(const QByteArray &string)
{
    constexpr quint32 multiplier = 0x5bd1e995U;
    // BLI_ghashutil_strhash_p_murmur hashes strlen(string) + 1 bytes, so the
    // terminating NUL is part of Blender's Murmur2 input.
    QByteArray bytes = string;
    bytes.append('\0');
    const uchar *data = reinterpret_cast<const uchar *>(bytes.constData());
    int remaining = bytes.size();
    quint32 hash = static_cast<quint32>(remaining);
    while (remaining >= 4) {
        quint32 chunk = 0;
        std::memcpy(&chunk, data, sizeof(chunk));
        chunk *= multiplier;
        chunk ^= chunk >> 24;
        chunk *= multiplier;
        hash *= multiplier;
        hash ^= chunk;
        data += 4;
        remaining -= 4;
    }
    if (remaining == 3) hash ^= static_cast<quint32>(data[2]) << 16;
    if (remaining >= 2) hash ^= static_cast<quint32>(data[1]) << 8;
    if (remaining >= 1) {
        hash ^= data[0];
        hash *= multiplier;
    }
    hash ^= hash >> 13;
    hash *= multiplier;
    hash ^= hash >> 15;
    return hash;
}

quint32 blenderHashInt(quint32 value)
{
    // BLI_hash_int(k) delegates to BLI_hash_int_2d(k, 0).
    quint32 a = 0xdeadbeefU + (2U << 2U) + 13U;
    quint32 b = a;
    quint32 c = a;
    a += value;
    const auto rotate = [](quint32 input, unsigned int count) {
        return (input << count) | (input >> (32U - count));
    };
    c ^= b;
    c -= rotate(b, 14U);
    a ^= c;
    a -= rotate(c, 11U);
    b ^= a;
    b -= rotate(a, 25U);
    c ^= b;
    c -= rotate(b, 16U);
    a ^= c;
    a -= rotate(c, 4U);
    b ^= a;
    b -= rotate(a, 14U);
    c ^= b;
    c -= rotate(b, 24U);
    return c;
}

qreal blenderRandomObjectHue(std::uint64_t objectId,
                             const QString &displayName,
                             const QString &libraryPath)
{
    quint32 hash = 0;
    if (!displayName.isEmpty()) {
        hash = blenderStringHash(QByteArrayLiteral("OB") +
                                 displayName.toUtf8());
        if (!libraryPath.isEmpty()) {
            hash = (hash * 13U) ^ blenderStringHash(libraryPath.toUtf8());
        }
        return static_cast<float>(blenderHashInt(hash)) /
               static_cast<float>(0xffffffffU);
    }

    hash = static_cast<quint32>(objectId) ^
           static_cast<quint32>(objectId >> 32);
    hash ^= hash >> 16;
    hash *= 0x7feb352dU;
    hash ^= hash >> 15;
    hash *= 0x846ca68bU;
    hash ^= hash >> 16;
    // Retain classiCAD's existing identity fallback for older/native-only
    // objects that do not have a Blender object name.
    return static_cast<qreal>(hash) / 4294967296.0;
}

} // namespace

QColor workbenchRandomObjectColor(std::uint64_t objectId,
                                  const QString &displayName,
                                  const QString &libraryPath)
{
    return QColor::fromHsvF(
        blenderRandomObjectHue(objectId, displayName, libraryPath), 0.5, 0.8);
}

QColor workbenchRandomObjectWireColor(std::uint64_t objectId,
                                      const QString &displayName,
                                      const QString &libraryPath)
{
    return QColor::fromHsvF(
        blenderRandomObjectHue(objectId, displayName, libraryPath), 0.75, 0.8);
}

} // namespace classiCAD
