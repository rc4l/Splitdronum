// Qt Quick overlay implementation -- see overlay_qt.h. Mirrors the verified rcthread harness: Qt owns the
// render thread, renders qml/Overlay.qml into a host-device D3D11 texture, driven by host-set QML props.
#include "overlay_qt.h"

#include <QGuiApplication>
#include <QQmlEngine>
#include <QQmlComponent>
#include <QQuickWindow>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickGraphicsDevice>
#include <QQuickItem>
#include <QSGRendererInterface>
#include <QVariantMap>
#include <QUrl>
#include <d3d11.h>

namespace {
QGuiApplication*          g_app  = nullptr;
QQuickRenderControl*      g_rc   = nullptr;
QQuickWindow*            g_win  = nullptr;
QQmlEngine*              g_engine = nullptr;
QObject*                 g_root = nullptr;
ID3D11Device*            g_dev  = nullptr;
ID3D11Texture2D*         g_tex  = nullptr;
ID3D11ShaderResourceView* g_srv = nullptr;
int  g_w = 0, g_h = 0;
bool g_ok = false;
}

bool ovl::Init(void* dev, void* ctx, const wchar_t* qmlFile)
{
    static int argc = 1;
    static char  arg0[] = "splitdronum";
    static char* argv[] = { arg0, nullptr };
    g_app = new QGuiApplication(argc, argv);
    g_dev = static_cast<ID3D11Device*>(dev);

    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
    g_rc  = new QQuickRenderControl();
    g_win = new QQuickWindow(g_rc);
    g_win->setColor(Qt::transparent);
    g_win->setGraphicsDevice(QQuickGraphicsDevice::fromDeviceAndContext(dev, ctx));   // host's D3D device

    g_engine = new QQmlEngine();
    QQmlComponent comp(g_engine, QUrl::fromLocalFile(QString::fromWCharArray(qmlFile)));
    g_root = comp.create();
    if (!g_root) { qWarning("overlay QML error: %s", comp.errorString().toLocal8Bit().constData()); return false; }
    QQuickItem* item = qobject_cast<QQuickItem*>(g_root);
    item->setParentItem(g_win->contentItem());

    if (!g_rc->initialize()) { qWarning("overlay rc.initialize failed"); return false; }
    g_ok = true;
    return true;
}

void ovl::SetScreen(int w, int h)
{
    if (!g_root) return;
    g_root->setProperty("screenW", w);
    g_root->setProperty("screenH", h);
}

void ovl::SetSeat0Gone(int gone)
{
    if (g_root) g_root->setProperty("seat0Gone", gone != 0);
}

void ovl::SetJoin(int active, int controller, int step, const char* word1, const char* word2,
                  int crosshair, int motion, int taken, int px, int py, int pw, int ph)
{
    if (!g_root) return;
    if (!active) { g_root->setProperty("join", QVariant()); return; }
    QVariantMap pane{ {"x",px}, {"y",py}, {"w",pw}, {"h",ph} };
    g_root->setProperty("join", QVariantMap{
        {"controller", controller}, {"step", step},
        {"word1", QString::fromUtf8(word1)}, {"word2", QString::fromUtf8(word2)},
        {"crosshair", crosshair}, {"motion", motion != 0}, {"taken", taken != 0}, {"pane", pane} });
}

void* ovl::Render(int w, int h)
{
    if (!g_ok || w <= 0 || h <= 0) return nullptr;
    if (w != g_w || h != g_h || !g_tex) {                  // (re)create the target texture at this size
        if (g_srv) { g_srv->Release(); g_srv = nullptr; }
        if (g_tex) { g_tex->Release(); g_tex = nullptr; }
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &g_tex))) return nullptr;
        g_dev->CreateShaderResourceView(g_tex, nullptr, &g_srv);
        g_win->setRenderTarget(QQuickRenderTarget::fromD3D11Texture(g_tex, (uint)DXGI_FORMAT_B8G8R8A8_UNORM, QSize(w, h)));
        g_win->setWidth(w); g_win->setHeight(h);
        qobject_cast<QQuickItem*>(g_root)->setSize(QSizeF(w, h));
        g_w = w; g_h = h;
    }
    QCoreApplication::processEvents();                     // apply any pushed state / animations
    g_rc->polishItems();
    g_rc->beginFrame();
    g_rc->sync();
    g_rc->render();
    g_rc->endFrame();
    return g_srv;
}

void ovl::Shutdown()
{
    if (g_srv) { g_srv->Release(); g_srv = nullptr; }
    if (g_tex) { g_tex->Release(); g_tex = nullptr; }
    g_ok = false;
}
