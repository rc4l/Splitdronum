// Standalone preview harness: shows qml/Preview.qml in a window so the cyberpunk overlay can be designed
// and screenshotted without the host. (The host integration uses QQuickRenderControl to render the same
// QML into a D3D11 texture instead -- this just proves the look.)
#include <QGuiApplication>
#include <QQuickView>
#include <QUrl>

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    QQuickView view;
    view.setTitle("overlay_preview");
    view.setResizeMode(QQuickView::SizeViewToRootObject);
    view.setColor(Qt::black);
    const QString qml = (argc > 1) ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("qml/Preview.qml");
    view.setSource(QUrl::fromLocalFile(qml));
    if (view.status() == QQuickView::Error)
        return 2;
    view.show();
    return app.exec();
}
