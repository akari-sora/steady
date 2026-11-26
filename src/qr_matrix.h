#pragma once

#include <QByteArray>
#include <QString>

struct QrMatrix {
    static constexpr int quietZone = 4;
    int size = 0;
    QByteArray modules;

    bool isDark(int x, int y) const
    {
        return x >= 0 && y >= 0 && x < size && y < size && modules[y * size + x] != 0;
    }
};

QrMatrix encodeQr(const QString &text);
