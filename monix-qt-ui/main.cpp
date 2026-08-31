// monix-qt-ui — Qt 6 Quick/QML frontend host (UI-STACK.md §5).
// Hello-world shell for now: no device access yet. The mixerdevice adapter
// and the DeviceBackend/MockBackend split arrive in phase 1 (DEV-WORKFLOW.md).

#include <QGuiApplication>
#include <QQmlApplicationEngine>

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("monix"));
    app.setApplicationName(QStringLiteral("monix-qt-ui"));

    QQmlApplicationEngine engine;
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed,
        &app, []() { QCoreApplication::exit(-1); }, Qt::QueuedConnection);

    engine.loadFromModule(QStringLiteral("Monix"), QStringLiteral("Main"));

    return app.exec();
}
