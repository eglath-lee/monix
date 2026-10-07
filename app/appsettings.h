#pragma once
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>

struct AppSettings {
    bool trayEnabled = true;
    bool closeToTray = true;
    // Startup profile: the device firmware powers up with optical-in = ADAT and
    // clock = Internal and does not remember these, so Monix re-applies them on
    // every connect. opticalInSpdif: optical input format (true = S/PDIF).
    // autoClock: slave the clock to optical when a valid optical clock is present;
    // when false, clockSourceManual (0=Internal, 1=Optical) is used instead.
    bool opticalInSpdif = true;
    bool autoClock = true;
    int  clockSourceManual = 0;
};

inline std::string monixConfigDir() {
    const char* home = getenv("HOME");
    std::string base = home ? home : ".";
    mkdir((base + "/.config").c_str(), 0755);
    std::string dir = base + "/.config/monix";
    mkdir(dir.c_str(), 0755);
    return dir;
}

inline std::string appSettingsPath() {
    return monixConfigDir() + "/app.conf";
}

inline bool loadAppSettings(AppSettings& s) {
    std::ifstream f(appSettingsPath());
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string k;
        int v = 0;
        if (ss >> k >> v) {
            if (k == "tray") s.trayEnabled = v != 0;
            else if (k == "close_to_tray") s.closeToTray = v != 0;
            else if (k == "optical_in_spdif") s.opticalInSpdif = v != 0;
            else if (k == "auto_clock") s.autoClock = v != 0;
            else if (k == "clock_source") s.clockSourceManual = v;
        }
    }
    return true;
}

inline void saveAppSettings(const AppSettings& s) {
    std::ofstream f(appSettingsPath());
    if (!f) return;
    f << "tray " << (s.trayEnabled ? 1 : 0) << '\n';
    f << "close_to_tray " << (s.closeToTray ? 1 : 0) << '\n';
    f << "optical_in_spdif " << (s.opticalInSpdif ? 1 : 0) << '\n';
    f << "auto_clock " << (s.autoClock ? 1 : 0) << '\n';
    f << "clock_source " << s.clockSourceManual << '\n';
}

inline std::string currentExecutablePath() {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return "monix-gui";
    buf[n] = '\0';
    return buf;
}

inline std::string desktopQuote(const std::string& s) {
    bool needsQuote = s.find_first_of(" \t\n\"'\\") != std::string::npos;
    if (!needsQuote) return s;
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\' || c == '$' || c == '`') {
            out += '\\';
            out += c;
        }
        else out += c;
    }
    out += "\"";
    return out;
}

inline std::string autostartPath() {
    const char* home = getenv("HOME");
    std::string base = home ? home : ".";
    mkdir((base + "/.config").c_str(), 0755);
    std::string dir = base + "/.config/autostart";
    mkdir(dir.c_str(), 0755);
    return dir + "/monix.desktop";
}

inline bool autostartEnabled() {
    std::ifstream f(autostartPath());
    return f.good();
}

inline bool setAutostart(bool enabled) {
    std::string path = autostartPath();
    if (!enabled) return remove(path.c_str()) == 0 || errno == ENOENT;

    std::ofstream f(path);
    if (!f) return false;
    f << "[Desktop Entry]\n";
    f << "Type=Application\n";
    f << "Name=Monix\n";
    f << "Comment=Audient iD control panel\n";
    f << "Exec=" << desktopQuote(currentExecutablePath()) << " --start-hidden\n";
    f << "Icon=monix\n";
    f << "Terminal=false\n";
    f << "Categories=Audio;AudioVideo;Mixer;\n";
    f << "X-GNOME-Autostart-enabled=true\n";
    return true;
}

inline std::string launcherPath() {
    const char* home = getenv("HOME");
    std::string base = home ? home : ".";
    mkdir((base + "/.local").c_str(), 0755);
    mkdir((base + "/.local/share").c_str(), 0755);
    std::string dir = base + "/.local/share/applications";
    mkdir(dir.c_str(), 0755);
    return dir + "/monix.desktop";
}

// Directory holding the shipped monix.svg / monix-<size>.png, or "" if none.
// Checked next to the executable first (build tree / bundle), then the source.
inline std::string iconSourceDir() {
    auto isDir = [](const std::string& d){ struct stat st; return !d.empty() && stat(d.c_str(), &st) == 0 && S_ISDIR(st.st_mode); };
    std::string exe = currentExecutablePath();
    size_t slash = exe.find_last_of('/');
    if (slash != std::string::npos) {
        std::string d = exe.substr(0, slash) + "/icons";
        if (isDir(d)) return d;
    }
#ifdef MONIX_SOURCE_DIR
    { std::string d = std::string(MONIX_SOURCE_DIR) + "/app/icons"; if (isDir(d)) return d; }
#endif
    return "";
}

inline bool copyFile(const std::string& from, const std::string& to) {
    std::ifstream in(from, std::ios::binary); if (!in) return false;
    std::ofstream out(to, std::ios::binary | std::ios::trunc); if (!out) return false;
    out << in.rdbuf();
    return out.good();
}

// Install the Monix icon into the per-user icon theme so a launcher with
// Icon=monix resolves even when install.sh (system-wide) was never run.
inline bool installLauncherIcons() {
    std::string src = iconSourceDir();
    const char* home = getenv("HOME");
    if (src.empty() || !home) return false;
    const std::string dataHome = std::string(home) + "/.local/share";
    const std::string hicolor  = dataHome + "/icons/hicolor";
    mkdir((std::string(home) + "/.local").c_str(), 0755);
    mkdir(dataHome.c_str(), 0755);
    mkdir((dataHome + "/icons").c_str(), 0755);
    mkdir(hicolor.c_str(), 0755);
    mkdir((hicolor + "/scalable").c_str(), 0755);
    mkdir((hicolor + "/scalable/apps").c_str(), 0755);
    copyFile(src + "/monix.svg", hicolor + "/scalable/apps/monix.svg");
    bool any = false;
    for (int s : {16, 22, 24, 32, 48, 64, 128, 256, 512}) {
        char sz[8]; snprintf(sz, sizeof sz, "%d", s);
        std::string tx = std::string(sz) + "x" + sz;
        mkdir((hicolor + "/" + tx).c_str(), 0755);
        mkdir((hicolor + "/" + tx + "/apps").c_str(), 0755);
        if (copyFile(src + "/monix-" + sz + ".png", hicolor + "/" + tx + "/apps/monix.png")) any = true;
    }
    return any;
}

inline bool installLauncher() {
    std::ofstream f(launcherPath());
    if (!f) return false;
    f << "[Desktop Entry]\n";
    f << "Type=Application\n";
    f << "Name=Monix\n";
    f << "Comment=Audient iD control panel\n";
    f << "Exec=" << desktopQuote(currentExecutablePath()) << "\n";
    f << "Icon=monix\n";
    f << "Terminal=false\n";
    f << "Categories=Audio;AudioVideo;Mixer;\n";
    f.close();                       // flush before refreshing the caches
    installLauncherIcons();          // best-effort: per-user icon theme
    const char* home = getenv("HOME") ? getenv("HOME") : ".";
    std::string data = std::string(home) + "/.local/share";
    std::system(("gtk-update-icon-cache -q -t -f " + desktopQuote(data + "/icons/hicolor") + " 2>/dev/null").c_str());
    std::system(("update-desktop-database " + desktopQuote(data + "/applications") + " 2>/dev/null").c_str());
    return true;
}
