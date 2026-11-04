#pragma once
#include <QByteArray>

namespace crypto {
void initialize();
QByteArray randomBytes(int count);
bool constantTimeEquals(const QByteArray &a, const QByteArray &b);
void wipe(QByteArray &bytes);
} // namespace crypto
