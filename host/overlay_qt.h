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
// Wait-screen start (auto-start off, nothing spawned yet): show the "hold Enter / press Start" prompt.
void SetWaitStart(int waiting);
// Keyboard hold-to-join ring progress (0..1000); drives a centered ring on the wait-screen and on rejoin.
void SetKbHold(int permille);
// Saved-config browser list (active=0 clears). names[i]/taken[i] for i<count; index = highlighted entry.
void SetBrowse(int active, int count, const char* const* names, const int* taken, int index);
// Controller-unplugged seats this frame: for each, the seat index (color) + pane rect (backbuffer px).
// count <= 4; seats[i] = seat index; paneXYWH[i*4 .. i*4+3] = x,y,w,h. count 0 clears them.
void SetDisconnects(int count, const int* seats, const int* paneXYWH);
// active=0 clears the panel; else the player-setup state (field = focused field) for the controller pane.
// seat = the pane index (0-3) the player will occupy -- drives its per-seat accent color. known = 1 when the
// composed name has a saved profile that just loaded.
void SetJoin(int active, int controller, int seat, int field, const char* word1, const char* word2,
             int crosshair, int motion, int taken, int known, int holdPermille,
             int mode, const char* variant,
             int paneX, int paneY, int paneW, int paneH);

// Render the overlay into a w x h texture and return its ID3D11ShaderResourceView* (as void*), or null.
// Recreates the target texture when the size changes. Call once per composited frame.
void* Render(int w, int h);

void Shutdown();

}
