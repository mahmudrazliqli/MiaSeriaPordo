#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <libconfig.h>
#include <locale.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <glib-unix.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#endif

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <sys/stat.h>
#endif

#define CFG_DIR    ".config"
#define CFG_NAME   TARGET ".cfg"

#define RES_GLADE  "/org/" TARGET "/window1.glade"
#define RES_CSS    "/org/" TARGET "/style.css"

#define STREAM_LIMIT (4u << 20)   /* trim stream history beyond 4 MiB */
#define READ_CHUNK   4096

/* ------------------------------------------------------------------ */
/* App state                                                           */

typedef struct {
    GtkWidget *window;
    GtkWidget *log_container;
    GtkWidget *status_label;
    GtkWidget *combo_port;
    GtkWidget *combo_baud;
    GtkWidget *combo_databits;
    GtkWidget *combo_parity;
    GtkWidget *combo_stopbits;
    GtkWidget *combo_newline;
    GtkWidget *hex_check;
    GtkWidget *connect_button;
    GtkWidget *refresh_button;
    GtkWidget *clear_button;
    GtkWidget *auto_check;
    GtkWidget *pause_button;
    GtkWidget *show_descriptor_button;
    GtkWidget *font_spin;
    GtkWidget *send_entry;
    GtkWidget *send_button;

    /* Text view log */
    GtkWidget     *textview;
    GtkTextBuffer *tbuf;
    GtkWidget     *log_scroll;

    GByteArray *history;    /* everything displayed: received + sent */

    int sfd;                /* serial fd (Unix) */

#ifndef _WIN32
    guint watch_id;
#else
    HANDLE   hComm;
    HANDLE   hReadThread;
    gboolean thread_running;
#endif

    gint log_font_size;
    gint hex_mode;
    gint paused;
    gint shutting_down;
} App;

static App app;

/* ------------------------------------------------------------------ */
/* Tables                                                              */

static const struct {
    const char *label;
    const char *bytes;
} nl_table[] = {
    { "\\n",    "\n"   },
    { "\\n\\r", "\n\r" },
    { "\\r\\n", "\r\n" },
    { "\\r",    "\r"   },
};

#ifdef _WIN32
#ifndef CBR_230400
#define CBR_230400 230400
#endif
#ifndef CBR_460800
#define CBR_460800 460800
#endif
#ifndef CBR_500000
#define CBR_500000 500000
#endif
#ifndef CBR_921600
#define CBR_921600 921600
#endif

static const DWORD baud_table_win[] = {
    CBR_300, CBR_600, CBR_1200, CBR_2400, CBR_4800, CBR_9600,
    CBR_19200, CBR_38400, CBR_57600, CBR_115200,
    CBR_230400, CBR_460800, CBR_500000, CBR_921600
};

static const int baud_table[] = {
    300, 600, 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200,
    230400, 460800, 500000, 921600
};
#else
static const struct {
    int value;
    int constant;
} baud_table[] = {
    { 300,    B300 },
    { 600,    B600 },
    { 1200,   B1200 },
    { 2400,   B2400 },
    { 4800,   B4800 },
    { 9600,   B9600 },
    { 19200,  B19200 },
    { 38400,  B38400 },
    { 57600,  B57600 },
    { 115200, B115200 },
#if defined(B230400)
    { 230400, B230400 },
#endif
#if defined(B460800)
    { 460800, B460800 },
#endif
#if defined(B500000)
    { 500000, B500000 },
#endif
#if defined(B921600)
    { 921600, B921600 },
#endif
};
#endif

/* newline matcher state (persists across chunk boundaries) */
static guint8 nl_bytes[2];
static gsize  nl_len = 0;
static gsize  hx_nl_match = 0;
static gsize  sp_nl_match = 0;

/* forward */
static void push_stream(const uint8_t *data, gsize len);
static void close_port(void);
static void redraw_all(void);

/* ------------------------------------------------------------------ */
/* Font-size CSS                                                       */

static GtkCssProvider *log_font_css = NULL;

static void ensure_log_css_provider(void)
{
    if (log_font_css) return;
    log_font_css = gtk_css_provider_new();
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(log_font_css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
}

static void apply_log_font_size(int size)
{
    if (size < 6)  size = 6;
    if (size > 48) size = 48;
    g_atomic_int_set(&app.log_font_size, size);

    ensure_log_css_provider();

    char *css = g_strdup_printf(
        "#textview_data {"
        "  font-family: 'DejaVu Sans Mono', monospace;"
        "  font-size: %dpt;"
        "}", size);
    gtk_css_provider_load_from_data(log_font_css, css, -1, NULL);
    g_free(css);
}

/* ------------------------------------------------------------------ */
/* Text view helpers                                                   */

static void scroll_to_end(void)
{
    if (!app.tbuf || !app.textview) return;
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(app.tbuf, &end);
    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(app.textview), &end,
                                 0.0, TRUE, 0.0, 1.0);
}

static void append_text(const char *s, gssize len)
{
    if (!app.tbuf) return;
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(app.tbuf, &end);
    gtk_text_buffer_insert(app.tbuf, &end, s, len);
}

/* Insert an escape token (e.g. "\r\n") highlighted with the "esc" tag. */
static void append_escape(const char *tok)
{
    if (!app.tbuf) return;

    GtkTextIter end, start;
    GtkTextMark *mark;

    gtk_text_buffer_get_end_iter(app.tbuf, &end);
    mark = gtk_text_buffer_create_mark(app.tbuf, NULL, &end, TRUE);
    gtk_text_buffer_insert(app.tbuf, &end, tok, -1);

    gtk_text_buffer_get_iter_at_mark(app.tbuf, &start, mark);
    gtk_text_buffer_get_end_iter(app.tbuf, &end);
    gtk_text_buffer_apply_tag_by_name(app.tbuf, "esc", &start, &end);
    gtk_text_buffer_delete_mark(app.tbuf, mark);
}

/* Simple log helper used by button handlers to print status lines. */
static void log_write(const char *prefix, const char *text)
{
    if (!app.tbuf) return;
    char *line = g_strdup_printf("[%s] %s\n", prefix, text);
    append_text(line, -1);
    g_free(line);
    scroll_to_end();
}

/* ------------------------------------------------------------------ */
/* Control-char escape text                                            */

static const char *ctrl_escape(uint8_t c, char out[8])
{
    switch (c) {
    case '\n': return "\\n";
    case '\r': return "\\r";
    case '\t': return "\\t";
    case 0x00: return "\\0";
    default:
        g_snprintf(out, 8, (c == 0x7F) ? "\\7F" : "\\%02X", c);
        return out;
    }
}

/* ------------------------------------------------------------------ */
/* Newline handling                                                    */

static void hex_reset(void)
{
    gint idx = gtk_combo_box_get_active(GTK_COMBO_BOX(app.combo_newline));
    const gchar *nl;

    idx = (idx < 0 || (guint)idx >= G_N_ELEMENTS(nl_table)) ? 0 : idx;
    nl = nl_table[idx].bytes;
    nl_len = strlen(nl);
    memcpy(nl_bytes, nl, nl_len);
    hx_nl_match = 0;
    sp_nl_match = 0;
}

static void hex_append(const uint8_t *data, gsize len)
{
    GString *acc = g_string_sized_new(len * 3 + 16);

    for (gsize i = 0; i < len; i++) {
        guint8 c = data[i];

        g_string_append_printf(acc, "%02X ", c);

        if (nl_len && c == nl_bytes[hx_nl_match])
            hx_nl_match++;
        else if (nl_len && c == nl_bytes[0])
            hx_nl_match = 1;
        else
            hx_nl_match = 0;

        if (nl_len && hx_nl_match == nl_len) {
            hx_nl_match = 0;
            g_string_append_c(acc, '\n');
        }
    }

    append_text(acc->str, (gssize)acc->len);
    g_string_free(acc, TRUE);
}

static void display_chunk(const uint8_t *data, gsize len)
{
    GString *acc;
    char esc[8];

    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app.hex_check))) {
        hex_append(data, len);
        return;
    }

    acc = g_string_sized_new(len * 3 + 16);
    for (gsize i = 0; i < len; i++) {
        guint8 c = data[i];

        if ((c < 0x20) || c == 0x7F) {
            if (acc->len) {
                append_text(acc->str, (gssize)acc->len);
                g_string_truncate(acc, 0);
            }
            append_escape(ctrl_escape(c, esc));
        } else {
            g_string_append_c(acc, (gchar)c);
        }

        if (c == nl_bytes[sp_nl_match]) {
            if (++sp_nl_match == nl_len) {
                sp_nl_match = 0;
                append_text("\n", 1);
            }
        } else {
            sp_nl_match = (c == nl_bytes[0]) ? 1 : 0;
        }
    }
    if (acc->len)
        append_text(acc->str, (gssize)acc->len);
    g_string_free(acc, TRUE);
}

static void redraw_all(void)
{
    if (!app.tbuf) return;
    gtk_text_buffer_set_text(app.tbuf, "", -1);
    hex_reset();
    if (app.history && app.history->len)
        display_chunk(app.history->data, app.history->len);
    scroll_to_end();
}

/* ------------------------------------------------------------------ */
/* Settings (libconfig)                                                */

static char *cfg_path = NULL;

static const char *get_config_path(void)
{
    if (cfg_path) return cfg_path;
    const char *home = g_get_home_dir();
    if (!home || !*home) home = ".";
    char *dir = g_build_filename(home, CFG_DIR, NULL);
    g_mkdir_with_parents(dir, 0755);
    cfg_path = g_build_filename(dir, CFG_NAME, NULL);
    g_free(dir);
    return cfg_path;
}

/* Select a combo box row by text; returns TRUE if found. */
static gboolean combo_select_text(GtkComboBoxText *c, const gchar *text)
{
    GtkTreeModel *m = gtk_combo_box_get_model(GTK_COMBO_BOX(c));
    GtkTreeIter it;
    gint idx = 0;

    if (!gtk_tree_model_get_iter_first(m, &it))
        return FALSE;
    do {
        gchar *s = NULL;
        gtk_tree_model_get(m, &it, 0, &s, -1);
        gboolean match = (s && g_strcmp0(s, text) == 0);
        g_free(s);
        if (match) {
            gtk_combo_box_set_active(GTK_COMBO_BOX(c), idx);
            return TRUE;
        }
        idx++;
    } while (gtk_tree_model_iter_next(m, &it));
    return FALSE;
}

static void load_config(void)
{
    config_t cfg;
    const char *port = NULL;
    int baud = 0, databits = -1, parity = -1, stopbits = -1,
        newline = -1, hexview = -1, autoconnect = -1;
    int w = 0, h = 0, x = -1, y = -1, fs = -1;

    config_init(&cfg);
    if (!config_read_file(&cfg, get_config_path())) {
        config_destroy(&cfg);
        return;
    }

    /* serial */
    config_lookup_string(&cfg, "serial.port", &port);
    config_lookup_int(&cfg, "serial.baud", &baud);
    config_lookup_int(&cfg, "serial.databits", &databits);
    config_lookup_int(&cfg, "serial.parity", &parity);
    config_lookup_int(&cfg, "serial.stopbits", &stopbits);
    config_lookup_int(&cfg, "serial.newline", &newline);
    config_lookup_int(&cfg, "serial.autoconnect", &autoconnect);
    config_lookup_int(&cfg, "serial.hexview", &hexview);

    /* window */
    int has_w = config_lookup_int(&cfg, "window_width",  &w);
    int has_h = config_lookup_int(&cfg, "window_height", &h);
    int has_x = config_lookup_int(&cfg, "window_x",      &x);
    int has_y = config_lookup_int(&cfg, "window_y",      &y);

    if (app.window) {
        if (has_w && has_h && w > 0 && h > 0)
            gtk_window_resize(GTK_WINDOW(app.window), w, h);
        if (has_x && has_y && x >= 0 && y >= 0)
            gtk_window_move(GTK_WINDOW(app.window), x, y);
    }

    if (port && *port) {
        GtkComboBoxText *c = GTK_COMBO_BOX_TEXT(app.combo_port);
        if (!combo_select_text(c, port)) {
            GtkEntry *e = GTK_ENTRY(gtk_bin_get_child(GTK_BIN(c)));
            gtk_entry_set_text(e, port);
        }
    }

    if (baud > 0) {
#ifdef _WIN32
        for (guint i = 0; i < G_N_ELEMENTS(baud_table); i++)
            if (baud_table[i] == baud) {
                gtk_combo_box_set_active(GTK_COMBO_BOX(app.combo_baud), (gint)i);
                break;
            }
#else
        for (guint i = 0; i < G_N_ELEMENTS(baud_table); i++)
            if (baud_table[i].value == baud) {
                gtk_combo_box_set_active(GTK_COMBO_BOX(app.combo_baud), (gint)i);
                break;
            }
#endif
    }

    if (databits >= 5 && databits <= 8)
        gtk_combo_box_set_active(GTK_COMBO_BOX(app.combo_databits), databits - 5);
    if (parity >= 0 && parity <= 2)
        gtk_combo_box_set_active(GTK_COMBO_BOX(app.combo_parity), parity);
    if (stopbits == 1 || stopbits == 2)
        gtk_combo_box_set_active(GTK_COMBO_BOX(app.combo_stopbits), stopbits - 1);
    if (newline >= 0 && (guint)newline < G_N_ELEMENTS(nl_table))
        gtk_combo_box_set_active(GTK_COMBO_BOX(app.combo_newline), newline);
    if (autoconnect == 0 || autoconnect == 1)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.auto_check),
                                     autoconnect == 1);
    if (hexview == 0 || hexview == 1)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.hex_check),
                                     hexview == 1);

    /* font size */
    if (config_lookup_int(&cfg, "log_font_size", &fs) && app.font_spin) {
        if (fs < 6)  fs = 6;
        if (fs > 48) fs = 48;
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(app.font_spin), fs);
    }

    config_destroy(&cfg);
}

static void save_config(void)
{
    config_t cfg;
    config_setting_t *root, *grp, *s;
    const gchar *port;

    config_init(&cfg);
    root = config_root_setting(&cfg);

    grp = config_setting_add(root, "serial", CONFIG_TYPE_GROUP);

    port = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(app.combo_port));
    s = config_setting_add(grp, "port", CONFIG_TYPE_STRING);
    if (s) config_setting_set_string(s, port ? port : "");
    g_free((gpointer)port);

    {
        int idx = gtk_combo_box_get_active(GTK_COMBO_BOX(app.combo_baud));
        int baud;
#ifdef _WIN32
        baud = (idx >= 0 && (guint)idx < G_N_ELEMENTS(baud_table))
                   ? baud_table[idx] : 115200;
#else
        baud = (idx >= 0 && (guint)idx < G_N_ELEMENTS(baud_table))
                   ? baud_table[idx].value : 115200;
#endif
        s = config_setting_add(grp, "baud", CONFIG_TYPE_INT);
        if (s) config_setting_set_int(s, baud);
    }

    s = config_setting_add(grp, "databits", CONFIG_TYPE_INT);
    if (s) config_setting_set_int(s,
        gtk_combo_box_get_active(GTK_COMBO_BOX(app.combo_databits)) + 5);

    s = config_setting_add(grp, "parity", CONFIG_TYPE_INT);
    if (s) config_setting_set_int(s,
        gtk_combo_box_get_active(GTK_COMBO_BOX(app.combo_parity)));

    s = config_setting_add(grp, "stopbits", CONFIG_TYPE_INT);
    if (s) config_setting_set_int(s,
        gtk_combo_box_get_active(GTK_COMBO_BOX(app.combo_stopbits)) + 1);

    s = config_setting_add(grp, "newline", CONFIG_TYPE_INT);
    if (s) {
        gint idx = gtk_combo_box_get_active(GTK_COMBO_BOX(app.combo_newline));
        config_setting_set_int(s, idx < 0 ? 0 : idx);
    }

    s = config_setting_add(grp, "autoconnect", CONFIG_TYPE_INT);
    if (s) config_setting_set_int(s,
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app.auto_check)) ? 1 : 0);

    s = config_setting_add(grp, "hexview", CONFIG_TYPE_INT);
    if (s) config_setting_set_int(s,
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app.hex_check)) ? 1 : 0);

    /* window */
    int w = 0, h = 0, x = -1, y = -1;
    if (app.window) {
        gtk_window_get_size(GTK_WINDOW(app.window), &w, &h);
        gtk_window_get_position(GTK_WINDOW(app.window), &x, &y);
    }
    s = config_setting_add(root, "window_width",  CONFIG_TYPE_INT); config_setting_set_int(s, w);
    s = config_setting_add(root, "window_height", CONFIG_TYPE_INT); config_setting_set_int(s, h);
    s = config_setting_add(root, "window_x",      CONFIG_TYPE_INT); config_setting_set_int(s, x);
    s = config_setting_add(root, "window_y",      CONFIG_TYPE_INT); config_setting_set_int(s, y);

    s = config_setting_add(root, "log_font_size", CONFIG_TYPE_INT);
    config_setting_set_int(s, g_atomic_int_get(&app.log_font_size));

    config_write_file(&cfg, get_config_path());
    config_destroy(&cfg);
}

/* ------------------------------------------------------------------ */
/* Serial port                                                         */

#ifdef _WIN32
static DWORD baud_to_win32(int value)
{
    for (guint i = 0; i < G_N_ELEMENTS(baud_table); i++) {
        if (baud_table[i] == value && i < G_N_ELEMENTS(baud_table_win))
            return baud_table_win[i];
    }
    return CBR_115200;
}

static void apply_win32_comm_state(HANDLE hComm)
{
    DCB dcb = {0};
    COMMTIMEOUTS timeouts = {0};
    int bits, parity, stops;

    dcb.DCBlength = sizeof(DCB);

    if (!GetCommState(hComm, &dcb)) {
        g_warning("Failed to get comm state");
        return;
    }

    gint idx = gtk_combo_box_get_active(GTK_COMBO_BOX(app.combo_baud));
    int speed_val = (idx >= 0 && (guint)idx < G_N_ELEMENTS(baud_table))
                        ? baud_table[idx] : 115200;
    dcb.BaudRate = baud_to_win32(speed_val);

    bits = gtk_combo_box_get_active(GTK_COMBO_BOX(app.combo_databits));
    dcb.ByteSize = bits + 5;

    parity = gtk_combo_box_get_active(GTK_COMBO_BOX(app.combo_parity));
    switch (parity) {
    case 1:  dcb.Parity = ODDPARITY;  dcb.fParity = TRUE;  break;
    case 2:  dcb.Parity = EVENPARITY; dcb.fParity = TRUE;  break;
    default: dcb.Parity = NOPARITY;   dcb.fParity = FALSE; break;
    }

    stops = gtk_combo_box_get_active(GTK_COMBO_BOX(app.combo_stopbits));
    dcb.StopBits = (stops == 1) ? TWOSTOPBITS : ONESTOPBIT;

    dcb.fOutxCtsFlow = FALSE;
    dcb.fRtsControl  = RTS_CONTROL_DISABLE;
    dcb.fDtrControl  = DTR_CONTROL_DISABLE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fTXContinueOnXoff = FALSE;

    if (!SetCommState(hComm, &dcb)) {
        g_warning("Failed to set comm state");
        return;
    }

    timeouts.ReadIntervalTimeout         = 10;
    timeouts.ReadTotalTimeoutMultiplier  = 0;
    timeouts.ReadTotalTimeoutConstant    = 10;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant   = 100;
    SetCommTimeouts(hComm, &timeouts);
}

static gboolean push_stream_wrapper(gpointer data)
{
    GByteArray *arr = (GByteArray *)data;
    push_stream(arr->data, arr->len);
    g_byte_array_unref(arr);
    return FALSE;
}

static DWORD WINAPI read_thread_func(LPVOID lpParam)
{
    HANDLE hComm = (HANDLE)lpParam;
    uint8_t buf[READ_CHUNK];
    DWORD bytes_read = 0;
    COMSTAT comstat;
    DWORD errors;

    while (app.thread_running) {
        if (!ClearCommError(hComm, &errors, &comstat)) {
            if (GetLastError() == ERROR_OPERATION_ABORTED) break;
            Sleep(10);
            continue;
        }
        if (comstat.cbInQue == 0) { Sleep(1); continue; }

        DWORD to_read = min(comstat.cbInQue, READ_CHUNK);
        if (ReadFile(hComm, buf, to_read, &bytes_read, NULL)) {
            if (bytes_read > 0) {
                GByteArray *arr = g_byte_array_new();
                g_byte_array_append(arr, buf, bytes_read);
                g_idle_add_full(G_PRIORITY_DEFAULT,
                                push_stream_wrapper, arr,
                                (GDestroyNotify)g_byte_array_unref);
            }
        } else {
            if (GetLastError() == ERROR_OPERATION_ABORTED) break;
        }
    }
    return 0;
}
#else /* Unix */
static int baud_to_const(int value)
{
    for (guint i = 0; i < G_N_ELEMENTS(baud_table); i++)
        if (baud_table[i].value == value)
            return baud_table[i].constant;
    return B115200;
}

static void apply_termios(int fd)
{
    struct termios tio;
    int bits, parity, stops, speed_val;

    tcgetattr(fd, &tio);
    cfmakeraw(&tio);

    {
        gint idx = gtk_combo_box_get_active(GTK_COMBO_BOX(app.combo_baud));
        speed_val = (idx >= 0 && (guint)idx < G_N_ELEMENTS(baud_table))
                        ? baud_table[idx].value : 115200;
    }
    cfsetispeed(&tio, baud_to_const(speed_val));
    cfsetospeed(&tio, baud_to_const(speed_val));

    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cflag &= ~CSIZE;

    bits = gtk_combo_box_get_active(GTK_COMBO_BOX(app.combo_databits));
    switch (bits) {
    case 0:  tio.c_cflag |= CS5; break;
    case 1:  tio.c_cflag |= CS6; break;
    case 2:  tio.c_cflag |= CS7; break;
    default: tio.c_cflag |= CS8; break;
    }

    parity = gtk_combo_box_get_active(GTK_COMBO_BOX(app.combo_parity));
    switch (parity) {
    case 1:
        tio.c_cflag |= PARENB | PARODD;
        tio.c_iflag |= INPCK;
        break;
    case 2:
        tio.c_cflag |= PARENB;
        tio.c_cflag &= ~PARODD;
        tio.c_iflag |= INPCK;
        break;
    default:
        tio.c_cflag &= ~PARENB;
        tio.c_iflag &= ~INPCK;
        break;
    }

    stops = gtk_combo_box_get_active(GTK_COMBO_BOX(app.combo_stopbits));
    if (stops == 1) tio.c_cflag |= CSTOPB;
    else            tio.c_cflag &= ~CSTOPB;

    tio.c_cc[VMIN]  = 0;
    tio.c_cc[VTIME] = 0;

    tcsetattr(fd, TCSANOW, &tio);
    tcflush(fd, TCIOFLUSH);
}
#endif

static void close_port(void)
{
#ifdef _WIN32
    if (app.hReadThread) {
        app.thread_running = FALSE;
        if (app.hComm != INVALID_HANDLE_VALUE) {
            CancelIoEx(app.hComm, NULL);
            SetCommMask(app.hComm, 0);
        }
        WaitForSingleObject(app.hReadThread, 1000);
        CloseHandle(app.hReadThread);
        app.hReadThread = NULL;
    }
    if (app.hComm != INVALID_HANDLE_VALUE) {
        CloseHandle(app.hComm);
        app.hComm = INVALID_HANDLE_VALUE;
    }
#else
    if (app.watch_id) {
        g_source_remove(app.watch_id);
        app.watch_id = 0;
    }
    if (app.sfd >= 0) {
        close(app.sfd);
        app.sfd = -1;
    }
#endif
}

static void push_stream(const uint8_t *data, gsize len)
{
    if (!app.history) return;
    g_byte_array_append(app.history, data, (guint)len);

    if (app.history->len > STREAM_LIMIT) {
        g_byte_array_remove_range(app.history, 0, app.history->len / 2);
        redraw_all();
    } else if (!g_atomic_int_get(&app.paused)) {
        display_chunk(data, len);
    }
    scroll_to_end();
}

#ifndef _WIN32
static gboolean on_serial_data(gint fd, GIOCondition cond, gpointer user_data)
{
    uint8_t buf[READ_CHUNK];
    ssize_t n;
    (void)cond; (void)user_data;

    while ((n = read(fd, buf, sizeof(buf))) > 0)
        push_stream(buf, (gsize)n);

    if (n < 0 && errno == EAGAIN)
        return TRUE;

    if (n < 0 && errno != EINTR) {
        close_port();
        gtk_button_set_label(GTK_BUTTON(app.connect_button), "Connect");
        log_write("SERR", g_strerror(errno));
        if (app.status_label)
            gtk_label_set_text(GTK_LABEL(app.status_label), "Read error");
        return FALSE;
    }
    return TRUE;
}
#endif

static void open_port(void)
{
    gchar *path;
    GtkWidget *dlg;

    path = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(app.combo_port));
    if (!path || !*path) {
        dlg = gtk_message_dialog_new(GTK_WINDOW(app.window), GTK_DIALOG_MODAL,
                                     GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
                                     "Please select a serial port first.");
        gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
        g_free(path);
        return;
    }

#ifdef _WIN32
    char port_name[256];
    if (g_str_has_prefix(path, "/dev/ttyS")) {
        g_snprintf(port_name, sizeof(port_name), "\\\\.\\COM%s",
                   path + strlen("/dev/ttyS"));
    } else {
        g_snprintf(port_name, sizeof(port_name), "\\\\.\\%s", path);
    }

    app.hComm = CreateFile(port_name,
                           GENERIC_READ | GENERIC_WRITE,
                           0, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);

    if (app.hComm == INVALID_HANDLE_VALUE) {
        char errmsg[256];
        DWORD err = GetLastError();
        g_snprintf(errmsg, sizeof(errmsg),
                   "Cannot open %s: Error %ld", path, (long)err);
        dlg = gtk_message_dialog_new(GTK_WINDOW(app.window), GTK_DIALOG_MODAL,
                                     GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
                                     "%s", errmsg);
        gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
        g_free(path);
        return;
    }

    apply_win32_comm_state(app.hComm);

    app.thread_running = TRUE;
    app.hReadThread = CreateThread(NULL, 0, read_thread_func,
                                   app.hComm, 0, NULL);
    if (!app.hReadThread) {
        CloseHandle(app.hComm);
        app.hComm = INVALID_HANDLE_VALUE;
        dlg = gtk_message_dialog_new(GTK_WINDOW(app.window), GTK_DIALOG_MODAL,
                                     GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
                                     "Failed to create read thread");
        gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
        g_free(path);
        return;
    }
#else
    app.sfd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (app.sfd < 0) {
        dlg = gtk_message_dialog_new(GTK_WINDOW(app.window), GTK_DIALOG_MODAL,
                                     GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
                                     "Cannot open %s:\n%s",
                                     path, g_strerror(errno));
        gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
        g_free(path);
        return;
    }

    apply_termios(app.sfd);
    app.watch_id = g_unix_fd_add(app.sfd, G_IO_IN, on_serial_data, NULL);
#endif

    save_config();
    gtk_button_set_label(GTK_BUTTON(app.connect_button), "Disconnect");
    if (app.status_label) {
        char msg[256];
        g_snprintf(msg, sizeof(msg), "Connected to %s", path);
        gtk_label_set_text(GTK_LABEL(app.status_label), msg);
    }
    g_free(path);
}

/* ------------------------------------------------------------------ */
/* Port enumeration                                                    */

static void refresh_ports(void)
{
    gchar *sel;

    sel = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(app.combo_port));
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(app.combo_port));

#ifdef _WIN32
    for (int i = 1; i <= 256; i++) {
        char port_name[32];
        g_snprintf(port_name, sizeof(port_name), "COM%d", i);
        HANDLE h = CreateFile(port_name, GENERIC_READ, 0, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app.combo_port),
                                           port_name);
        }
    }
#else
    GDir *dir = g_dir_open("/dev", 0, NULL);
    if (dir) {
        const gchar *name;
        while ((name = g_dir_read_name(dir)) != NULL) {
            if (g_str_has_prefix(name, "ttyS")   ||
                g_str_has_prefix(name, "ttyUSB") ||
                g_str_has_prefix(name, "ttyACM") ||
                g_str_has_prefix(name, "ttyAMA") ||
                g_str_has_prefix(name, "rfcomm")) {
                gchar *full = g_strdup_printf("/dev/%s", name);
                gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app.combo_port),
                                               full);
                g_free(full);
            }
        }
        g_dir_close(dir);
    }
#endif

    if (sel && *sel)
        combo_select_text(GTK_COMBO_BOX_TEXT(app.combo_port), sel);
    g_free(sel);
}

/* ------------------------------------------------------------------ */
/* Combos                                                              */

static void fill_combos(void)
{
    static const char *parities[] = { "None", "Odd", "Even" };

#ifdef _WIN32
    for (guint i = 0; i < G_N_ELEMENTS(baud_table); i++) {
        gchar *s = g_strdup_printf("%d", baud_table[i]);
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app.combo_baud), s);
        g_free(s);
    }
#else
    for (guint i = 0; i < G_N_ELEMENTS(baud_table); i++) {
        gchar *s = g_strdup_printf("%d", baud_table[i].value);
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app.combo_baud), s);
        g_free(s);
    }
#endif
    gtk_combo_box_set_active(GTK_COMBO_BOX(app.combo_baud), 9);   /* 115200 */

    for (int b = 5; b <= 8; b++) {
        gchar *s = g_strdup_printf("%d", b);
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app.combo_databits), s);
        g_free(s);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(app.combo_databits), 3);  /* 8 */

    for (guint i = 0; i < G_N_ELEMENTS(parities); i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app.combo_parity),
                                       parities[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(app.combo_parity), 0);

    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app.combo_stopbits), "1");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app.combo_stopbits), "2");
    gtk_combo_box_set_active(GTK_COMBO_BOX(app.combo_stopbits), 0);

    for (guint i = 0; i < G_N_ELEMENTS(nl_table); i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app.combo_newline),
                                       nl_table[i].label);
    gtk_combo_box_set_active(GTK_COMBO_BOX(app.combo_newline), 0);
}

/* ------------------------------------------------------------------ */
/* Send                                                                */

static void send_data(void)
{
    const gchar *txt;
    GtkWidget *dlg;

#ifdef _WIN32
    if (app.hComm == INVALID_HANDLE_VALUE) {
#else
    if (app.sfd < 0) {
#endif
        dlg = gtk_message_dialog_new(GTK_WINDOW(app.window), GTK_DIALOG_MODAL,
                                     GTK_MESSAGE_WARNING, GTK_BUTTONS_OK,
                                     "Port is not open.");
        gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
        return;
    }

    txt = gtk_entry_get_text(GTK_ENTRY(app.send_entry));
    if (!txt || !*txt) return;

    const char *nl = nl_table[0].bytes;
    gint idx = gtk_combo_box_get_active(GTK_COMBO_BOX(app.combo_newline));
    if (idx >= 0 && (guint)idx < G_N_ELEMENTS(nl_table))
        nl = nl_table[idx].bytes;

    GString *out = g_string_new(txt);
    g_string_append(out, nl);

    gsize sent = 0;

#ifdef _WIN32
    DWORD written = 0;
    if (!WriteFile(app.hComm, out->str, (DWORD)out->len, &written, NULL))
        g_warning("write failed: error %lu", GetLastError());
    else
        sent = (gsize)written;
#else
    while (sent < out->len) {
        ssize_t w = write(app.sfd, out->str + sent, out->len - sent);
        if (w < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            g_warning("write failed: %s", g_strerror(errno));
            break;
        }
        sent += (gsize)w;
    }
#endif

    if (sent)
        push_stream((const uint8_t *)out->str, sent);

    g_string_free(out, TRUE);
}

/* ------------------------------------------------------------------ */
/* Ctrl+Scroll font size                                               */

static gboolean on_view_scroll(GtkWidget *w, GdkEvent *event, gpointer data)
{
    (void)w; (void)data;

    if (!event || event->type != GDK_SCROLL) return FALSE;
    if (!(event->scroll.state & GDK_CONTROL_MASK)) return FALSE;
    if (!app.font_spin) return FALSE;

    GdkScrollDirection dir = event->scroll.direction;
    int delta = 0;
    if (dir == GDK_SCROLL_UP)        delta =  1;
    else if (dir == GDK_SCROLL_DOWN) delta = -1;
    else return FALSE;

    GtkAdjustment *adj = gtk_spin_button_get_adjustment(GTK_SPIN_BUTTON(app.font_spin));
    int cur = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app.font_spin));
    int lo  = (int)gtk_adjustment_get_lower(adj);
    int hi  = (int)gtk_adjustment_get_upper(adj);
    int nv  = cur + delta;
    if (nv < lo) nv = lo;
    if (nv > hi) nv = hi;
    if (nv != cur)
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(app.font_spin), nv);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Log view construction                                               */

static GtkWidget *build_log_view(void)
{
    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

    GtkWidget *tv = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(tv), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(tv), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tv), GTK_WRAP_WORD_CHAR);
    gtk_widget_set_name(tv, "textview_data");
    gtk_container_add(GTK_CONTAINER(scrolled), tv);

    app.textview   = tv;
    app.log_scroll = scrolled;
    app.tbuf       = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));

    gtk_text_buffer_create_tag(app.tbuf, "esc",
        "foreground", "#117dd4",
        "weight",     PANGO_WEIGHT_THIN,
        "size",       7 * PANGO_SCALE,
        NULL);

    gtk_widget_add_events(scrolled, GDK_SCROLL_MASK);
    g_signal_connect(scrolled, "scroll-event",
                     G_CALLBACK(on_view_scroll), NULL);

    return scrolled;
}

/* ------------------------------------------------------------------ */
/* Callbacks                                                           */

static void on_refresh_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    refresh_ports();
    if (app.status_label)
        gtk_label_set_text(GTK_LABEL(app.status_label), "Port list refreshed");
}

static void on_connect_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;

#ifdef _WIN32
    gboolean connected = (app.hComm != INVALID_HANDLE_VALUE);
#else
    gboolean connected = (app.sfd >= 0);
#endif

    if (connected) {
        close_port();
        gtk_button_set_label(GTK_BUTTON(app.connect_button), "Connect");
        log_write("CONN", "Disconnected");
        if (app.status_label)
            gtk_label_set_text(GTK_LABEL(app.status_label), "Disconnected");
    } else {
        open_port();
    }
    save_config();
}

static void on_send_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    send_data();
}

static void on_clear_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    if (app.history) g_byte_array_set_size(app.history, 0);
    redraw_all();
    if (app.status_label)
        gtk_label_set_text(GTK_LABEL(app.status_label), "Logs cleared");
}

static void on_show_info(GtkButton *b, gpointer d)
{
    (void)b; (void)d;

    gchar *port = gtk_combo_box_text_get_active_text(
                      GTK_COMBO_BOX_TEXT(app.combo_port));
    gint bidx = gtk_combo_box_get_active(GTK_COMBO_BOX(app.combo_baud));
#ifdef _WIN32
    int baud = (bidx >= 0 && (guint)bidx < G_N_ELEMENTS(baud_table))
                   ? baud_table[bidx] : 115200;
#else
    int baud = (bidx >= 0 && (guint)bidx < G_N_ELEMENTS(baud_table))
                   ? baud_table[bidx].value : 115200;
#endif
    gint db = gtk_combo_box_get_active(GTK_COMBO_BOX(app.combo_databits)) + 5;
    gint par = gtk_combo_box_get_active(GTK_COMBO_BOX(app.combo_parity));
    gint sb = gtk_combo_box_get_active(GTK_COMBO_BOX(app.combo_stopbits)) + 1;

    char info[512];
    g_snprintf(info, sizeof(info),
               "Port: %s   Baud: %d   Data: %d   Parity: %d   Stop: %d   History: %u bytes",
               port ? port : "(none)", baud, db, par, sb,
               app.history ? app.history->len : 0);
    g_free(port);

    log_write("INFO", info);
    if (app.status_label)
        gtk_label_set_text(GTK_LABEL(app.status_label), "Port info logged");
}

static void update_pause_button(void)
{
    if (!app.pause_button) return;

    gint paused = g_atomic_int_get(&app.paused);
    GtkWidget *img = gtk_bin_get_child(GTK_BIN(app.pause_button));
    if (img && GTK_IS_IMAGE(img)) {
        gtk_image_set_from_icon_name(
            GTK_IMAGE(img),
            paused ? "media-playback-start" : "media-playback-pause",
            GTK_ICON_SIZE_BUTTON);
    }
}

static void on_pause_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    gint paused = !g_atomic_int_get(&app.paused);
    g_atomic_int_set(&app.paused, paused);
    update_pause_button();

    if (!paused) redraw_all();
    if (app.status_label)
        gtk_label_set_text(GTK_LABEL(app.status_label),
                           paused ? "Display paused" : "Display resumed");
}

static void on_hex_toggled(GtkToggleButton *b, gpointer data)
{
    (void)data;
    int hex = gtk_toggle_button_get_active(b);
    g_atomic_int_set(&app.hex_mode, hex);
    save_config();
    redraw_all();
    if (app.status_label)
        gtk_label_set_text(GTK_LABEL(app.status_label),
                           hex ? "View: HEX" : "View: ASCII");
}

static void on_newline_changed(GtkComboBox *c, gpointer u)
{
    (void)c; (void)u;
    save_config();
    redraw_all();
}

static void on_autoconnect_toggled(GtkToggleButton *b, gpointer u)
{
    (void)b; (void)u;
    save_config();
}

static void on_font_size_changed(GtkSpinButton *spin, gpointer data)
{
    (void)data;
    apply_log_font_size(gtk_spin_button_get_value_as_int(spin));
}

static void on_window_destroy(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    if (g_atomic_int_get(&app.shutting_down)) return;
    g_atomic_int_set(&app.shutting_down, 1);

    save_config();
    close_port();
    gtk_main_quit();
}

/* ------------------------------------------------------------------ */
/* Auto-connect                                                        */

static gboolean autoconnect_cb(gpointer u)
{
    (void)u;
#ifdef _WIN32
    if (app.hComm == INVALID_HANDLE_VALUE) {
#else
    if (app.sfd < 0) {
#endif
        gtk_button_clicked(GTK_BUTTON(app.connect_button));
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* main                                                                */

int main(int argc, char *argv[])
{
    setlocale(LC_ALL, "");

    app.sfd = -1;
    app.log_font_size = 11;
    app.hex_mode = 0;
    app.paused = 0;
    app.shutting_down = 0;
#ifdef _WIN32
    app.hComm = INVALID_HANDLE_VALUE;
#endif

    gtk_init(&argc, &argv);

    GError *err = NULL;
    GtkBuilder *builder = gtk_builder_new();
    if (!gtk_builder_add_from_resource(builder, RES_GLADE, &err)) {
        g_printerr("Failed to load %s: %s\n", RES_GLADE,
                   err ? err->message : "unknown error");
        if (err) g_error_free(err);
        g_object_unref(builder);
        return 1;
    }

    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_resource(css, RES_CSS);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    app.window         = GTK_WIDGET(gtk_builder_get_object(builder, "main_window"));
    app.log_container  = GTK_WIDGET(gtk_builder_get_object(builder, "log_container"));
    app.status_label   = GTK_WIDGET(gtk_builder_get_object(builder, "status_label"));
    app.combo_port     = GTK_WIDGET(gtk_builder_get_object(builder, "combo_port"));
    app.combo_baud     = GTK_WIDGET(gtk_builder_get_object(builder, "combo_baud"));
    app.combo_databits = GTK_WIDGET(gtk_builder_get_object(builder, "combo_databits"));
    app.combo_parity   = GTK_WIDGET(gtk_builder_get_object(builder, "combo_parity"));
    app.combo_stopbits = GTK_WIDGET(gtk_builder_get_object(builder, "combo_stopbits"));
    app.combo_newline  = GTK_WIDGET(gtk_builder_get_object(builder, "combo_newline"));
    app.hex_check      = GTK_WIDGET(gtk_builder_get_object(builder, "hex_check"));
    app.connect_button = GTK_WIDGET(gtk_builder_get_object(builder, "connect_button"));
    app.refresh_button = GTK_WIDGET(gtk_builder_get_object(builder, "refresh_button"));
    app.clear_button   = GTK_WIDGET(gtk_builder_get_object(builder, "clear_button"));
    app.auto_check     = GTK_WIDGET(gtk_builder_get_object(builder, "check_autoconnect"));
    app.pause_button   = GTK_WIDGET(gtk_builder_get_object(builder, "pause_button"));
    app.show_descriptor_button =
                         GTK_WIDGET(gtk_builder_get_object(builder, "show_descriptor_button"));
    app.font_spin      = GTK_WIDGET(gtk_builder_get_object(builder, "font_size_spin"));
    app.send_entry     = GTK_WIDGET(gtk_builder_get_object(builder, "send_entry"));
    app.send_button    = GTK_WIDGET(gtk_builder_get_object(builder, "send_button"));

    if (!app.window || !app.log_container || !app.status_label ||
        !app.combo_port || !app.combo_baud || !app.combo_databits ||
        !app.combo_parity || !app.combo_stopbits || !app.combo_newline ||
        !app.hex_check || !app.connect_button || !app.refresh_button ||
        !app.clear_button || !app.auto_check || !app.pause_button ||
        !app.show_descriptor_button || !app.font_spin ||
        !app.send_entry || !app.send_button) {
        g_printerr("Error: Failed to find required widgets in glade file\n");
        g_object_unref(builder);
        return 1;
    }

    gtk_window_set_title(GTK_WINDOW(app.window), WINTITLE);

    /* Combo cell renderer for combo_port (GtkComboBoxText already has one) */

    /* Log view */
    GtkWidget *log_scroll = build_log_view();
    gtk_box_pack_start(GTK_BOX(app.log_container), log_scroll, TRUE, TRUE, 0);
    gtk_widget_show_all(app.log_container);

    app.history = g_byte_array_new();

    /* Populate combos and ports, then load settings */
    fill_combos();
    refresh_ports();
    load_config();

    /* Initial font size */
    apply_log_font_size(gtk_spin_button_get_value_as_int(
                            GTK_SPIN_BUTTON(app.font_spin)));

    g_signal_connect(app.refresh_button, "clicked",
                     G_CALLBACK(on_refresh_clicked), NULL);
    g_signal_connect(app.connect_button, "clicked",
                     G_CALLBACK(on_connect_clicked), NULL);
    g_signal_connect(app.send_button, "clicked",
                     G_CALLBACK(on_send_clicked), NULL);
    g_signal_connect(app.send_entry, "activate",
                     G_CALLBACK(on_send_clicked), NULL);
    g_signal_connect(app.clear_button, "clicked",
                     G_CALLBACK(on_clear_clicked), NULL);
    g_signal_connect(app.auto_check, "toggled",
                     G_CALLBACK(on_autoconnect_toggled), NULL);
    g_signal_connect(app.hex_check, "toggled",
                     G_CALLBACK(on_hex_toggled), NULL);
    g_signal_connect(app.pause_button, "clicked",
                     G_CALLBACK(on_pause_clicked), NULL);
    g_signal_connect(app.show_descriptor_button, "clicked",
                     G_CALLBACK(on_show_info), NULL);
    g_signal_connect(app.combo_newline, "changed",
                     G_CALLBACK(on_newline_changed), NULL);
    g_signal_connect(app.font_spin, "value-changed",
                     G_CALLBACK(on_font_size_changed), NULL);
    g_signal_connect(app.window, "destroy",
                     G_CALLBACK(on_window_destroy), NULL);

    hex_reset();
    update_pause_button();
    gtk_widget_show_all(app.window);

    //log_write("APP ", "Ready — MiaSeriaPordo template");

    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app.auto_check))) {
        gchar *p = gtk_combo_box_text_get_active_text(
                       GTK_COMBO_BOX_TEXT(app.combo_port));
        if (p && *p)
            g_timeout_add(200, autoconnect_cb, NULL);
        g_free(p);
    }

    gtk_main();

    if (app.history) g_byte_array_free(app.history, TRUE);
    g_free(cfg_path);
    if (log_font_css) g_object_unref(log_font_css);
    g_object_unref(builder);
    return 0;
}