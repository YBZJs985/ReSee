#include "app/PlaybackController.h"
#include "core/Logger.h"
#include "render/VideoRenderItem.h"

#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QImage>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTimer>

#ifdef SYNC_PLATFORM_WINDOWS
#include <windows.h>
#endif

static void qtMessageHandler(QtMsgType type, const QMessageLogContext&, const QString& msg) {
    if (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg) {
        mcs::Logger::instance().log(
            type == QtWarningMsg ? mcs::LogLevel::Warn : mcs::LogLevel::Error,
            QStringLiteral("QT"), msg);
    }
}

int main(int argc, char* argv[]) {
#ifdef SYNC_PLATFORM_WINDOWS
    SetConsoleOutputCP(CP_UTF8);
#endif

    qInstallMessageHandler(&qtMessageHandler);

    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    QGuiApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("ReSee"));
    app.setApplicationName(QStringLiteral("ReSee"));
    app.setWindowIcon(QIcon(QStringLiteral(":/icons/resources/app_icon.png")));

    int rc = 0;
    {
        mcs::PlaybackController controller;
        controller.initialize();

        {
            QQmlApplicationEngine engine;
            engine.rootContext()->setContextProperty(QStringLiteral("player"), &controller);

            QObject::connect(
                &engine, &QQmlApplicationEngine::objectCreationFailed,
                &app, []() { QCoreApplication::exit(-1); },
                Qt::QueuedConnection);

            engine.loadFromModule(QStringLiteral("ReSee"), QStringLiteral("Main"));

#ifdef SYNC_PLATFORM_WINDOWS
            if (!engine.rootObjects().isEmpty()) {
                if (auto* win = qobject_cast<QQuickWindow*>(engine.rootObjects().first())) {
                    const HWND hwnd = reinterpret_cast<HWND>(win->winId());
                    if (hwnd) {
                        if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll")) {
                            using DwmSetAttrFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
                            auto setAttr = reinterpret_cast<DwmSetAttrFn>(GetProcAddress(dwm, "DwmSetWindowAttribute"));
                            if (setAttr) {
                                BOOL dark = TRUE;
                                setAttr(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
                                COLORREF captionColor = RGB(0x0E, 0x0E, 0x0E);
                                setAttr(hwnd, 35 /* DWMWA_CAPTION_COLOR */, &captionColor, sizeof(captionColor));
                                COLORREF textColor = RGB(0xE2, 0xE8, 0xF0);
                                setAttr(hwnd, 36 /* DWMWA_TEXT_COLOR */, &textColor, sizeof(textColor));
                            }
                            FreeLibrary(dwm);
                        }
                    }
                }
            }
#endif

            int grabMs = 0;
            QString grabOut;
            bool quitAfterGrab = false;

            const QStringList args = app.arguments();
            for (int i = 1; i < args.size(); ++i) {
                if (args[i] == QLatin1String("--seek") && i + 1 < args.size()) {
                    controller.jumpToDateTimeString(args[++i]);
                } else if (args[i] == QLatin1String("--play")) {
                    controller.play();
                } else if (args[i] == QLatin1String("--rate") && i + 1 < args.size()) {
                    controller.setPlaybackRate(args[++i].toDouble());
                } else if (args[i] == QLatin1String("--layout") && i + 1 < args.size()) {
                    controller.setLayoutMode(args[++i]);
                } else if (args[i] == QLatin1String("--focus") && i + 1 < args.size()) {
                    controller.setFocusedStream(args[++i].toInt());
                } else if (args[i] == QLatin1String("--diag")) {
                    controller.setDiagnosticsVisible(true);
                } else if (args[i] == QLatin1String("--grab-ms") && i + 1 < args.size()) {
                    grabMs = args[++i].toInt();
                } else if (args[i] == QLatin1String("--grab-out") && i + 1 < args.size()) {
                    grabOut = args[++i];
                } else if (args[i] == QLatin1String("--quit-after-grab")) {
                    quitAfterGrab = true;
                }
            }

            if (grabMs > 0 && !grabOut.isEmpty() && !engine.rootObjects().isEmpty()) {
                auto* win = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
                if (win) {
                    QTimer::singleShot(grabMs, &app, [win, grabOut, quitAfterGrab, &app]() {
                        QDir().mkpath(QFileInfo(grabOut).absolutePath());
                        const QImage img = win->grabWindow();
                        img.save(grabOut, "PNG");
                        if (quitAfterGrab) {
                            app.quit();
                        }
                    });
                }
            }

            rc = app.exec();
        }
    }

    qInstallMessageHandler(nullptr);
    return rc;
}
