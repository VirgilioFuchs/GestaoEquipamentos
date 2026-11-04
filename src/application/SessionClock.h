#pragma once
#include <QDateTime>
#include <QElapsedTimer>

class SessionClock {
public:
    virtual ~SessionClock() = default;
    virtual QDateTime nowUtc() const { return QDateTime::currentDateTimeUtc(); }
    virtual qint64 monotonicMs() const {
        static const QElapsedTimer timer = [] { QElapsedTimer t; t.start(); return t; }();
        return timer.elapsed();
    }
    static const SessionClock &system() { static const SessionClock clock; return clock; }
};
