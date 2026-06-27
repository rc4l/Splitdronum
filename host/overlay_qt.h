// overlay_qt -- the Qt Quick (cyberpunk) overlay for the host compositor. The implementation owns the Qt
// machinery (QGuiApplication + QQuickRenderControl + qml/Overlay.qml) and renders it into a D3D11 texture
// on the HOST's device each frame; the compositor alpha-blends the returned SRV over the seats.
//
// C ABI on purpose: host.cpp includes this WITHOUT any Qt headers. All calls happen on the RENDER thread
// (which owns the D3D device/context), matching the de-risked single-thread Qt model. State is pushed via
// QML root properties -- no QObject, so no moc in the host build.
#pragma once

namespace ovl {

// Create the Qt overlay on the calling (render) thread, using the host's D3D11 device + context. qmlFile
// is the absolute path to qml/Overlay.qml. Returns false if Qt/QML init fails (host falls back to GDI).
bool Init(void* d3dDevice, void* d3dContext, const wchar_t* qmlFile);

// Push state (render thread, between frames). screen = backbuffer size so pane px map correctly.
void SetScreen(int w, int h);
void SetSeat0Gone(int gone);
// active=0 clears the join card; else the create-wizard state for the controller pane.
void SetJoin(int active, int controller, int step, const char* word1, const char* word2,
             int crosshair, int motion, int taken, int paneX, int paneY, int paneW, int paneH);

// Render the overlay into a w x h texture and return its ID3D11ShaderResourceView* (as void*), or null.
// Recreates the target texture when the size changes. Call once per composited frame.
void* Render(int w, int h);

void Shutdown();

}
