#pragma once

#include <QString>

namespace mcs {

class PlatformNotifier {
public:
    static void showSystemNotification(const QString& title, const QString& message, bool isError = true);
};

} // namespace mcs
