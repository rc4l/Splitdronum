// splitdronum launcher -- a thin GUI over play.ps1's parameters, built on Qt Quick (matching the
// overlay, which is also Qt). The UI lives in qml/Launcher.qml; this file is the entry point plus a
// small Backend object the QML calls to launch the game.
//
// The launcher UI is portable Qt; the *launch action* shells out to play.ps1 and is Windows-only (so
// is the splitdronum runtime: DLL injection + WGL + the D3D11 compositor). It's guarded behind _WIN32
// -- on other OSes the UI runs but Play reports the runtime isn't ported there yet.
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QIcon>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QCoreApplication>
#include <QTimer>
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>

// Is a process with this exact image path currently running? Used to track the launched host.exe so the
// launcher can hide itself while the game runs and reappear when it closes. Full-path match (not just the
// "host.exe" name) so an unrelated host.exe elsewhere can't fool it.
static bool ProcessImageRunning(const std::wstring& fullPath) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe = { sizeof(pe) };
    bool found = false;
    if (Process32FirstW(snap, &pe)) do {
        if (_wcsicmp(pe.szExeFile, L"host.exe") != 0) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!h) continue;
        wchar_t img[MAX_PATH]; DWORD sz = MAX_PATH;
        if (QueryFullProcessImageNameW(h, 0, img, &sz) && _wcsicmp(img, fullPath.c_str()) == 0) found = true;
        CloseHandle(h);
    } while (!found && Process32NextW(snap, &pe));
    CloseHandle(snap);
    return found;
}
#endif

// Backend -- exposed to QML as `backend`. Holds the status line and runs play.ps1.
class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)   // true while the game is running -> QML hides the window
public:
    Backend() {
        m_timer.setInterval(750);                        // poll for host.exe appearing/closing
        QObject::connect(&m_timer, &QTimer::timeout, this, &Backend::poll);
    }
    QString status() const { return m_status; }
    bool busy() const { return m_busy; }

    // repo root = where play.ps1 lives, found relative to this exe (built into <repo>/build).
    Q_INVOKABLE QString repoRoot() const {
        QDir d(QCoreApplication::applicationDirPath());   // ...\build
        d.cdUp();                                         // repo root
        return d.absolutePath();
    }

    // The directory that holds the seats' profiles/*.cfg: the chosen game's folder, else the bundled engine.
    QString profilesDir(const QString& gamePath) const {
        QString base = gamePath.isEmpty() ? (repoRoot() + "/engine") : QFileInfo(gamePath).absolutePath();
        return base + "/profiles";
    }

    // Saved profile names (profiles/*.cfg minus the extension). The KB+M dropdown lists these. The per-seat
    // ".seatN.last" recall files end in .last, so the *.cfg filter skips them automatically.
    Q_INVOKABLE QStringList kbmProfiles(const QString& gamePath) const {
        QStringList out;
        QDir pd(profilesDir(gamePath));
        const auto files = pd.entryInfoList(QStringList() << "*.cfg", QDir::Files, QDir::Name);
        for (const QFileInfo& fi : files) {
            QString base = fi.completeBaseName();
            if (!base.isEmpty() && !base.startsWith('.')) out << base;
        }
        return out;
    }

    // Remember / recall which profile the KB+M player last chose (only meaningful when several exist).
    Q_INVOKABLE QString lastKbmProfile() const {
        QSettings s(repoRoot() + "/launcher.ini", QSettings::IniFormat);
        return s.value("kbmProfile").toString();
    }

    // Build the play.ps1 invocation from the form state and run it detached. Render scale is left to
    // play.ps1's default (1.0) + its SmartScale, which auto-lowers each seat's res by live player count.
    Q_INVOKABLE void launch(const QString& iwad, const QString& gamePath,
                            int fpsMode, int fpsCustom, bool autoStartKbm, const QString& kbmProfile) {
#ifdef _WIN32
        const QString root = repoRoot();
        const int fps = (fpsMode == 0) ? -1 : (fpsMode == 1) ? 0 : fpsCustom;   // refresh / uncapped / custom
        QString a = "powershell.exe -NoProfile -ExecutionPolicy Bypass -File \""
                    + QDir::toNativeSeparators(root + "/play.ps1") + "\"";
        a += " -Players 1";   // solo start; controllers join live, so the launcher never sets a seat count
        a += " -Iwad \"" + iwad + "\"";
        a += QString(" -Fps %1").arg(fps);
        if (!autoStartKbm) a += " -NoAutoStartKBM";   // a switch -- survives `powershell -File` (a [bool]:$false does not)
        if (autoStartKbm && !kbmProfile.isEmpty()) {  // KB+M loads the chosen saved profile; remember it for next time
            a += " -KbmProfile \"" + kbmProfile + "\"";
            QSettings s(root + "/launcher.ini", QSettings::IniFormat); s.setValue("kbmProfile", kbmProfile);
        }
        if (!gamePath.isEmpty()) a += " -GamePath \"" + QDir::toNativeSeparators(gamePath) + "\"";

        std::wstring cmd = a.toStdWString();
        std::wstring cwd = QDir::toNativeSeparators(root).toStdWString();
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi = { 0 };
        std::vector<wchar_t> buf(cmd.begin(), cmd.end()); buf.push_back(L'\0');
        if (CreateProcessW(NULL, buf.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL,
                           cwd.c_str(), &si, &pi)) {
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
            setStatus("Launched. Press Start on a controller in-game to add a player.");
            // host.exe is launched (detached) by play.ps1, so we can't wait on the powershell we spawned.
            // Instead watch for OUR host.exe by full path: hide the launcher once it's up, reappear when it exits.
            m_hostPath = QDir::toNativeSeparators(QCoreApplication::applicationDirPath() + "/host.exe").toStdWString();
            m_sawHost = false; m_ticks = 0;
            setBusy(true);
            m_timer.start();
        } else {
            setStatus(QString("Could not start play.ps1 (error %1). Is play.ps1 in the repo root?")
                      .arg(GetLastError()));
        }
#else
        Q_UNUSED(iwad); Q_UNUSED(gamePath); Q_UNUSED(fpsMode); Q_UNUSED(fpsCustom); Q_UNUSED(autoStartKbm); Q_UNUSED(kbmProfile);
        setStatus("The splitdronum runtime is Windows-only for now -- launching isn't available on this OS yet.");
#endif
    }

signals:
    void statusChanged();
    void busyChanged();

private:
    void setStatus(const QString& s) { m_status = s; emit statusChanged(); }
    void setBusy(bool b) { if (m_busy != b) { m_busy = b; emit busyChanged(); } }

    // Poll the launched game: hide the launcher until host.exe exits, then reappear. A grace window covers
    // host.exe's startup (play.ps1 builds + spawns it); if it never shows (failed launch) we un-hide so the
    // launcher isn't stranded off-screen.
    void poll() {
#ifdef _WIN32
        bool running = ProcessImageRunning(m_hostPath);
        ++m_ticks;
        if (running) m_sawHost = true;
        if (m_sawHost && !running) {                       // game closed -> show the launcher again
            m_timer.stop(); setStatus(""); setBusy(false);
        } else if (!m_sawHost && m_ticks > 40) {           // ~30s and host never came up -> recover
            m_timer.stop(); setStatus("The game didn't start. Check the IWAD / game path and try again.");
            setBusy(false);
        }
#endif
    }

    QString m_status;
    bool    m_busy = false;
    QTimer  m_timer;
    bool    m_sawHost = false;
    int     m_ticks = 0;
    std::wstring m_hostPath;
};

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle("Fusion");   // honors our dark palette (the native style ignores it)
    app.setWindowIcon(QIcon(":/splitdronum.ico"));   // runtime window / taskbar icon

    Backend backend;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.load(QUrl(QStringLiteral("qrc:/qml/Launcher.qml")));
    if (engine.rootObjects().isEmpty()) return 2;
    return app.exec();
}

#include "launcher.moc"
