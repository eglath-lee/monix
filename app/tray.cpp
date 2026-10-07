#include "tray.h"

#if defined(MONIX_HAVE_TRAY)
#include <gtk/gtk.h>
#include <string>
#include <cstdlib>
#include <sys/stat.h>
#if defined(MONIX_USE_AYATANA_APPINDICATOR)
#include <libayatana-appindicator/app-indicator.h>
#else
#include <libappindicator/app-indicator.h>
#endif

namespace {
AppIndicator* indicator = nullptr;
GtkWidget* showItem = nullptr;
GtkWidget* hideItem = nullptr;
bool showRequested = false;
bool hideRequested = false;
bool quitRequested = false;
bool trayAvailable = false;

// Best available Monix icon file, or "" if none found (the caller then falls back
// to the theme name "monix"). An absolute file path is preferred so we ship a
// branded icon instead of the generic system "audio-card" one.
std::string resolveIconPath() {
    auto exists = [](const char* p) { struct stat st; return p && stat(p, &st) == 0; };
    if (const char* e = getenv("MONIX_ICON")) if (exists(e)) return e;
    const char* rel = "icons/hicolor/scalable/apps/monix.svg";
    std::string p;
    if (const char* x = getenv("XDG_DATA_HOME")) { p = std::string(x) + "/" + rel; if (exists(p.c_str())) return p; }
    if (const char* h = getenv("HOME"))           { p = std::string(h) + "/.local/share/" + rel; if (exists(p.c_str())) return p; }
#ifdef MONIX_SOURCE_DIR
    p = std::string(MONIX_SOURCE_DIR) + "/app/icons/monix.svg"; if (exists(p.c_str())) return p;
#endif
    for (const char* base : { "/usr/local/share/", "/usr/share/" }) {
        p = std::string(base) + rel; if (exists(p.c_str())) return p;
    }
    return "";
}

void onShow(GtkMenuItem*, gpointer) { showRequested = true; }
void onHide(GtkMenuItem*, gpointer) { hideRequested = true; }
void onQuit(GtkMenuItem*, gpointer) { quitRequested = true; }
#if defined(MONIX_USE_AYATANA_APPINDICATOR)
void onActivate(AppIndicator*, gint, gint, gpointer) { showRequested = true; }
#endif
} // namespace

namespace monixtray {

bool init() {
    if (trayAvailable) return true;
    gtk_disable_setlocale();
    if (!gtk_init_check(nullptr, nullptr)) return false;

    indicator = app_indicator_new("monix",
                                  "monix",
                                  APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
    if (!indicator) return false;

    app_indicator_set_title(indicator, "Monix");
    std::string iconPath = resolveIconPath();
    if (!iconPath.empty()) app_indicator_set_icon_full(indicator, iconPath.c_str(), "Monix");
    app_indicator_set_status(indicator, APP_INDICATOR_STATUS_ACTIVE);
#if defined(MONIX_USE_AYATANA_APPINDICATOR)
    g_signal_connect(indicator, APP_INDICATOR_SIGNAL_ACTIVATE_EVENT, G_CALLBACK(onActivate), nullptr);
#endif

    GtkWidget* menu = gtk_menu_new();
    showItem = gtk_menu_item_new_with_label("Open Monix");
    hideItem = gtk_menu_item_new_with_label("Hide to Tray");
    GtkWidget* sep = gtk_separator_menu_item_new();
    GtkWidget* quitItem = gtk_menu_item_new_with_label("Quit Monix");

    g_signal_connect(showItem, "activate", G_CALLBACK(onShow), nullptr);
    g_signal_connect(hideItem, "activate", G_CALLBACK(onHide), nullptr);
    g_signal_connect(quitItem, "activate", G_CALLBACK(onQuit), nullptr);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), showItem);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), hideItem);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), sep);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), quitItem);
    gtk_widget_show_all(menu);
    app_indicator_set_menu(indicator, GTK_MENU(menu));

    trayAvailable = true;
    return true;
}

void shutdown() {
    if (indicator) app_indicator_set_status(indicator, APP_INDICATOR_STATUS_PASSIVE);
    indicator = nullptr;
    trayAvailable = false;
}

void poll() {
    if (!trayAvailable) return;
    while (gtk_events_pending()) gtk_main_iteration_do(FALSE);
}

bool available() { return trayAvailable; }

bool takeShowRequest() {
    bool r = showRequested;
    showRequested = false;
    return r;
}

bool takeHideRequest() {
    bool r = hideRequested;
    hideRequested = false;
    return r;
}

bool takeQuitRequest() {
    bool r = quitRequested;
    quitRequested = false;
    return r;
}

void setVisibleState(bool visible) {
    if (!trayAvailable) return;
    if (showItem) gtk_widget_set_sensitive(showItem, !visible);
    if (hideItem) gtk_widget_set_sensitive(hideItem, visible);
}

} // namespace monixtray

#else

namespace monixtray {
bool init() { return false; }
void shutdown() {}
void poll() {}
bool available() { return false; }
bool takeShowRequest() { return false; }
bool takeHideRequest() { return false; }
bool takeQuitRequest() { return false; }
void setVisibleState(bool) {}
} // namespace monixtray

#endif
