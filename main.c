/* MiaSeriaPordo - GTK3 serial port monitor
 *
 * Build: make
 * Settings are saved with libconfig to ~/.config/miaseriapordo.cfg
 */

#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <glib-unix.h>
#include <libconfig.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define CFG_DIR    ".config"
#define CFG_NAME   "miaseriapordo.cfg"

#ifndef MIASERIAPORDO_DATA_DIR
#define MIASERIAPORDO_DATA_DIR "."
#endif
#define STREAM_LIMIT   (4u << 20)   /* trim stream history beyond 4 MiB */
#define READ_CHUNK 4096

/* ------------------------------------------------------------------ */
/* Widgets                                                             */

static GtkWidget     *win;
static GtkWidget     *combo_port, *combo_baud, *combo_databits,
                     *combo_parity, *combo_stopbits, *combo_newline;
static GtkWidget     *btn_connect, *btn_disconnect;
static GtkWidget     *textview, *entry_send;
static GtkWidget     *btn_send, *btn_clear, *btn_refresh, *check_autoconnect,
                     *check_hex;
static GtkTextBuffer *tbuf;

/* ------------------------------------------------------------------ */
/* State                                                               */

static GByteArray *history;          /* everything displayed: received + sent  */
static int         sfd = -1;    /* serial fd                          */
static guint       watch_id = 0;

static const struct {
    int value;
    int constant;               /* termios speed constant             */
} baud_table[] = {
    { 300,    B300    }, { 600,    B600    }, { 1200,   B1200   },
    { 2400,   B2400   }, { 4800,   B4800   }, { 9600,   B9600   },
    { 19200,  B19200  }, { 38400,  B38400  }, { 57600,  B57600  },
    { 115200, B115200 }, { 230400, B230400 }, { 460800, B460800 },
    { 500000, B500000 }, { 921600, B921600 },
};

/* line-ending appended to sent data */
static const struct {
    const char *label;           /* as shown in the combo box        */
    const char *bytes;           /* what actually gets appended      */
} nl_table[] = {
    { "\\n",    "\n"   },
    { "\\n\\r", "\n\r" },
    { "\\r\\n", "\r\n" },
    { "\\r",    "\r"   },
};

/* ------------------------------------------------------------------ */
/* Callbacks (forward declarations)                                    */

static void on_btn_connect_clicked(GtkButton *b, gpointer u);
static void on_btn_disconnect_clicked(GtkButton *b, gpointer u);

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */

/* Find an item in a combo box by text and select it.
 * Returns TRUE when found. */
static gboolean
combo_select_text(GtkComboBoxText *c, const gchar *text)
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

static void
scroll_to_end(void)
{
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(tbuf, &end);
    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(textview), &end,
                                 0.0, TRUE, 0.0, 1.0);
}

static void
append_text(const char *s, gssize len)
{
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(tbuf, &end);
    gtk_text_buffer_insert(tbuf, &end, s, len);
}

/* Insert an escape token (e.g. "\r\n") highlighted with the "esc" tag. */
static void
append_escape(const char *tok)
{
    GtkTextIter end, start;
    GtkTextMark *mark;

    gtk_text_buffer_get_end_iter(tbuf, &end);
    /* left-gravity mark survives the insert and keeps pointing at
     * the beginning of the token - safe across buffer modification */
    mark = gtk_text_buffer_create_mark(tbuf, NULL, &end, TRUE);
    gtk_text_buffer_insert(tbuf, &end, tok, -1);

    gtk_text_buffer_get_iter_at_mark(tbuf, &start, mark);
    gtk_text_buffer_get_end_iter(tbuf, &end);
    gtk_text_buffer_apply_tag_by_name(tbuf, "esc", &start, &end);
    gtk_text_buffer_delete_mark(tbuf, mark);
}

/* C-style escape text for a control byte: '\n' -> "\n", '\1' -> "\x01" ... */
static const char *
ctrl_escape(uint8_t c, char out[8])
{
    switch (c) {
    case '\n': return "\\n";
    case '\r': return "\\r";
    case '\t': return "\\t";
    case 0x00: return "\\0";
    default:
        g_snprintf(out, 8, (c == 0x7F) ? "\\x7F" : "\\x%02X", c);
        return out;
    }
}

/* ------------------------------------------------------------------ */
/* Newline handling                                                    */

/* The sequence chosen in the combo box drives both views:
 * - hex mode:     break the hex stream when it is detected
 * - raw text mode: replace it by exactly one real line break,
 *   so e.g. devices sending bare \r wrap correctly              */
static guint8 nl_bytes[2];      /* selected terminator bytes          */
static gsize  nl_len = 0;

/* hex-mode matcher state (runs across chunk boundaries) */
static gsize  hx_nl_match = 0;

/* text-mode matcher state (runs across chunk boundaries) */
static gsize  sp_nl_match = 0;

/* (re)load the selected newline sequence into the matchers */
static void
hex_reset(void)
{
    gint idx = gtk_combo_box_get_active(GTK_COMBO_BOX(combo_newline));
    const gchar *nl;

    idx = (idx < 0 || (guint)idx >= G_N_ELEMENTS(nl_table)) ? 0 : idx;
    nl = nl_table[idx].bytes;
    nl_len = strlen(nl);
    memcpy(nl_bytes, nl, nl_len);
    hx_nl_match = 0;
    sp_nl_match = 0;
}

static void
hex_append(const uint8_t *data, gsize len)
{
    GString *acc = g_string_sized_new(len * 3 + 16);

    for (gsize i = 0; i < len; i++) {
        guint8 c = data[i];

        g_string_append_printf(acc, "%02X ", c);

        /* stream the terminator matcher across chunk boundaries */
        if (nl_len && c == nl_bytes[hx_nl_match]) {
            hx_nl_match++;
        } else if (nl_len && c == nl_bytes[0]) {
            hx_nl_match = 1;
        } else {
            hx_nl_match = 0;
        }
        if (nl_len && hx_nl_match == nl_len) {
            hx_nl_match = 0;
            g_string_append_c(acc, '\n');
        }
    }

    append_text(acc->str, (gssize)acc->len);
    g_string_free(acc, TRUE);
}

/* Convert one raw chunk for display.
 * Normal text mode: control bytes are always shown as highlighted
 * literal text ("\n", "\r", "\x01" ...), and the selected newline
 * sequence is the only thing that starts a real new line.           */
static void
display_chunk(const uint8_t *data, gsize len)
{
    GString *acc;
    char esc[8];

    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(check_hex))) {
        hex_append(data, len);
        return;
    }

    acc = g_string_sized_new(len * 3 + 16);
    for (gsize i = 0; i < len; i++) {
        guint8 c = data[i];

        if ((c < 0x20) || c == 0x7F) {
            /* flush the plain run first, then add the highlighted token */
            if (acc->len) {
                append_text(acc->str, (gssize)acc->len);
                g_string_truncate(acc, 0);
            }
            append_escape(ctrl_escape(c, esc));
        } else {
            g_string_append_c(acc, (gchar)c);
        }

        /* advance the terminator matcher on EVERY byte (printables
         * must reset a half-matched state too); the real line break
         * happens only when the selected sequence completes */
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

/* Full redraw from history (used after Clear / toggle / trim). */
static void
redraw_all(void)
{
    gtk_text_buffer_set_text(tbuf, "", -1);
    hex_reset();                    /* restart offsets in hex mode */
    if (history->len)
        display_chunk(history->data, history->len);
    scroll_to_end();
}

/* ------------------------------------------------------------------ */
/* Settings via libconfig                                              */

static void
settings_path(char *buf, size_t n)
{
    const char *home = g_get_home_dir();
    g_snprintf(buf, n, "%s/%s/%s", home, CFG_DIR, CFG_NAME);
}

static void
save_settings(void)
{
    config_t cfg;
    config_setting_t *root, *grp, *s;
    char path[512];
    const gchar *port;
    FILE *f;

    settings_path(path, sizeof(path));

    config_init(&cfg);
    root = config_root_setting(&cfg);

    grp = config_setting_add(root, "serial", CONFIG_TYPE_GROUP);

    port = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo_port));
    s = config_setting_add(grp, "port", CONFIG_TYPE_STRING);
    if (s) config_setting_set_string(s, port ? port : "");
    g_free((gpointer)port);

    {
        int idx = gtk_combo_box_get_active(GTK_COMBO_BOX(combo_baud));
        int baud = (idx >= 0 && (guint)idx < G_N_ELEMENTS(baud_table))
                       ? baud_table[idx].value : 115200;
        s = config_setting_add(grp, "baud", CONFIG_TYPE_INT);
        if (s) config_setting_set_int(s, baud);
    }

    s = config_setting_add(grp, "databits", CONFIG_TYPE_INT);
    if (s) config_setting_set_int(s,
        gtk_combo_box_get_active(GTK_COMBO_BOX(combo_databits)) + 5);

    s = config_setting_add(grp, "parity", CONFIG_TYPE_INT);
    if (s) config_setting_set_int(s,
        gtk_combo_box_get_active(GTK_COMBO_BOX(combo_parity)));

    s = config_setting_add(grp, "stopbits", CONFIG_TYPE_INT);
    if (s) config_setting_set_int(s,
        gtk_combo_box_get_active(GTK_COMBO_BOX(combo_stopbits)) + 1);

    s = config_setting_add(grp, "newline", CONFIG_TYPE_INT);
    if (s) {
        gint idx = gtk_combo_box_get_active(GTK_COMBO_BOX(combo_newline));
        config_setting_set_int(s, idx < 0 ? 0 : idx);
    }

    s = config_setting_add(grp, "autoconnect", CONFIG_TYPE_INT);
    if (s) config_setting_set_int(s,
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(check_autoconnect)) ? 1 : 0);

    s = config_setting_add(grp, "hexview", CONFIG_TYPE_INT);
    if (s) config_setting_set_int(s,
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(check_hex)) ? 1 : 0);

    {
        gchar *dir = g_path_get_dirname(path);
        g_mkdir_with_parents(dir, 0755);
        g_free(dir);
    }
    if ((f = fopen(path, "w")) != NULL) {
        config_write(&cfg, f);
        fclose(f);
    } else {
        g_warning("cannot write %s: %s", path, g_strerror(errno));
    }

    config_destroy(&cfg);
}

static void
load_settings(void)
{
    config_t cfg;
    const char *port = NULL;
    int baud = 0, databits = -1, parity = -1, stopbits = -1,
        newline = -1, hexview = -1, autoconnect = -1;
    char path[512];

    settings_path(path, sizeof(path));

    config_init(&cfg);
    if (!config_read_file(&cfg, path)) {
        config_destroy(&cfg);
        return;                          /* first run: keep defaults */
    }

    config_lookup_string(&cfg, "serial.port", &port);
    config_lookup_int(&cfg, "serial.baud", &baud);
    config_lookup_int(&cfg, "serial.databits", &databits);
    config_lookup_int(&cfg, "serial.parity", &parity);
    config_lookup_int(&cfg, "serial.stopbits", &stopbits);
    config_lookup_int(&cfg, "serial.newline", &newline);
    config_lookup_int(&cfg, "serial.autoconnect", &autoconnect);
    config_lookup_int(&cfg, "serial.hexview", &hexview);

    if (port && *port) {
        GtkComboBoxText *c = GTK_COMBO_BOX_TEXT(combo_port);
        if (!combo_select_text(c, port)) {
            /* not in the device list: type it into the editable entry */
            GtkEntry *e = GTK_ENTRY(gtk_bin_get_child(GTK_BIN(c)));
            gtk_entry_set_text(e, port);
        }
    }

    if (baud > 0)
        for (guint i = 0; i < G_N_ELEMENTS(baud_table); i++)
            if (baud_table[i].value == baud) {
                gtk_combo_box_set_active(GTK_COMBO_BOX(combo_baud), (gint)i);
                break;
            }

    if (databits >= 5 && databits <= 8)
        gtk_combo_box_set_active(GTK_COMBO_BOX(combo_databits), databits - 5);
    if (parity >= 0 && parity <= 2)
        gtk_combo_box_set_active(GTK_COMBO_BOX(combo_parity), parity);
    if (stopbits == 1 || stopbits == 2)
        gtk_combo_box_set_active(GTK_COMBO_BOX(combo_stopbits), stopbits - 1);
    if (newline >= 0 && (guint)newline < G_N_ELEMENTS(nl_table))
        gtk_combo_box_set_active(GTK_COMBO_BOX(combo_newline), newline);
    if (autoconnect == 0 || autoconnect == 1)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check_autoconnect), autoconnect == 1);
    if (hexview == 0 || hexview == 1)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check_hex), hexview == 1);

    config_destroy(&cfg);
}

/* ------------------------------------------------------------------ */
/* Serial port                                                         */

static int
baud_to_const(int value)
{
    for (guint i = 0; i < G_N_ELEMENTS(baud_table); i++)
        if (baud_table[i].value == value)
            return baud_table[i].constant;
    return B115200;
}

static void
apply_termios(int fd)
{
    struct termios tio;
    int bits, parity, stops, speed_val;

    tcgetattr(fd, &tio);
    cfmakeraw(&tio);

    speed_val = 0;
    {
        gint idx = gtk_combo_box_get_active(GTK_COMBO_BOX(combo_baud));
        speed_val = (idx >= 0 && (guint)idx < G_N_ELEMENTS(baud_table))
                        ? baud_table[idx].value : 115200;
    }
    cfsetispeed(&tio, baud_to_const(speed_val));
    cfsetospeed(&tio, baud_to_const(speed_val));

    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cflag &= ~CSIZE;

    bits = gtk_combo_box_get_active(GTK_COMBO_BOX(combo_databits)); /* 0..3 -> 5..8 */
    switch (bits) {
    case 0:  tio.c_cflag |= CS5; break;
    case 1:  tio.c_cflag |= CS6; break;
    case 2:  tio.c_cflag |= CS7; break;
    default: tio.c_cflag |= CS8; break;
    }

    parity = gtk_combo_box_get_active(GTK_COMBO_BOX(combo_parity));
    switch (parity) {
    case 1:                                    /* odd  */
        tio.c_cflag |= PARENB | PARODD;
        tio.c_iflag |= INPCK;
        break;
    case 2:                                    /* even */
        tio.c_cflag |= PARENB;
        tio.c_cflag &= ~PARODD;
        tio.c_iflag |= INPCK;
        break;
    default:                                   /* none */
        tio.c_cflag &= ~PARENB;
        tio.c_iflag &= ~INPCK;
        break;
    }

    stops = gtk_combo_box_get_active(GTK_COMBO_BOX(combo_stopbits));
    if (stops == 1)
        tio.c_cflag |= CSTOPB;                 /* 2 stop bits */
    else
        tio.c_cflag &= ~CSTOPB;                /* 1 stop bit  */

    tio.c_cc[VMIN]  = 0;
    tio.c_cc[VTIME] = 0;                       /* fully non-blocking */

    tcsetattr(fd, TCSANOW, &tio);
    tcflush(fd, TCIOFLUSH);
}

static void
close_port(void)
{
    if (watch_id) {
        g_source_remove(watch_id);
        watch_id = 0;
    }
    if (sfd >= 0) {
        close(sfd);
        sfd = -1;
    }
}

/* Reflect connection state on the Connect/Disconnect buttons. */
static void
set_connected(gboolean on)
{
    gtk_widget_set_sensitive(btn_connect, !on);
    gtk_widget_set_sensitive(btn_disconnect, on);
}

/* Add bytes to the shared history and show them - used for both
 * received and sent data so they appear identically in the view. */
static void
push_stream(const uint8_t *data, gsize len)
{
    g_byte_array_append(history, data, (guint)len);

    if (history->len > STREAM_LIMIT) {         /* keep memory bounded */
        g_byte_array_remove_range(history, 0, history->len / 2);
        redraw_all();
    } else {
        display_chunk(data, len);
    }
    scroll_to_end();
}

static gboolean
on_serial_data(gint fd, GIOCondition cond, gpointer user_data)
{
    uint8_t buf[READ_CHUNK];
    ssize_t n;
    (void)cond;
    (void)user_data;

    while ((n = read(fd, buf, sizeof(buf))) > 0)
        push_stream(buf, (gsize)n);

    if (n < 0 && errno == EAGAIN)
        return TRUE;                           /* nothing more for now */

    /* A 0-byte wakeup is NOT an EOF on a serial port - USB-serial
     * drivers (FTDI, CDC-ACM, ...) often flag the fd readable for
     * status updates with no payload. Only real errors disconnect. */
    if (n < 0 && errno != EINTR) {
        close_port();
        set_connected(FALSE);
        g_warning("serial read error: %s", g_strerror(errno));
        return FALSE;
    }
    return TRUE;
}

static void
open_port(void)
{
    gchar *path;
    GtkWidget *dlg;

    path = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo_port));
    if (!path || !*path) {
        dlg = gtk_message_dialog_new(GTK_WINDOW(win), GTK_DIALOG_MODAL,
                                     GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
                                     "Please select a serial port first.");
        gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
        g_free(path);
        return;
    }

    sfd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (sfd < 0) {
        dlg = gtk_message_dialog_new(GTK_WINDOW(win), GTK_DIALOG_MODAL,
                                     GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
                                     "Cannot open %s:\n%s", path, g_strerror(errno));
        gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
        g_free(path);
        return;
    }

    apply_termios(sfd);
    watch_id = g_unix_fd_add(sfd, G_IO_IN, on_serial_data, NULL);
    save_settings();
    set_connected(TRUE);
    g_free(path);
}

/* ------------------------------------------------------------------ */
/* Callbacks                                                           */

static void
refresh_ports(void)
{
    GDir *dir;
    const gchar *name;
    gchar *sel;

    sel = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo_port));
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(combo_port));

    dir = g_dir_open("/dev", 0, NULL);
    if (dir) {
        while ((name = g_dir_read_name(dir)) != NULL) {
            if (g_str_has_prefix(name, "ttyS") ||
                g_str_has_prefix(name, "ttyUSB") ||
                g_str_has_prefix(name, "ttyACM") ||
                g_str_has_prefix(name, "ttyAMA") ||
                g_str_has_prefix(name, "rfcomm")) {
                gchar *full = g_strdup_printf("/dev/%s", name);
                gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_port), full);
                g_free(full);
            }
        }
        g_dir_close(dir);
    }

    if (sel && *sel)
        combo_select_text(GTK_COMBO_BOX_TEXT(combo_port), sel);
    g_free(sel);
}

static void
on_btn_refresh_clicked(GtkButton *b, gpointer u)
{
    (void)b; (void)u;
    refresh_ports();
}

static void
send_data(void)
{
    const gchar *txt;
    GtkWidget *dlg;

    if (sfd < 0) {
        dlg = gtk_message_dialog_new(GTK_WINDOW(win), GTK_DIALOG_MODAL,
                                     GTK_MESSAGE_WARNING, GTK_BUTTONS_OK,
                                     "Port is not open.");
        gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
        return;
    }

    txt = gtk_entry_get_text(GTK_ENTRY(entry_send));
    if (*txt) {
        const char *nl = nl_table[0].bytes;
        gint idx = gtk_combo_box_get_active(GTK_COMBO_BOX(combo_newline));
        GString *out;
        gsize sent = 0;

        if (idx >= 0 && (guint)idx < G_N_ELEMENTS(nl_table))
            nl = nl_table[idx].bytes;

        out = g_string_new(txt);
        g_string_append(out, nl);

        /* write everything (handles partial writes), then echo the
         * bytes that actually made it onto the wire */
        while (sent < out->len) {
            ssize_t w = write(sfd, out->str + sent, out->len - sent);
            if (w < 0) {
                if (errno == EINTR || errno == EAGAIN)
                    continue;
                g_warning("write failed: %s", g_strerror(errno));
                break;
            }
            sent += (gsize)w;
        }

        if (sent)
            push_stream((const uint8_t *)out->str, sent);

        g_string_free(out, TRUE);
        gtk_editable_delete_text(GTK_EDITABLE(entry_send), 0, -1);
    }
}

static void
on_btn_send_clicked(GtkButton *b, gpointer u)
{
    (void)b; (void)u;
    send_data();
}

static void
on_entry_send_activate(GtkEntry *e, gpointer u)
{
    (void)e; (void)u;
    send_data();
}

static void
on_btn_clear_clicked(GtkButton *b, gpointer u)
{
    (void)b; (void)u;
    g_byte_array_set_size(history, 0);
    redraw_all();
}

static void
on_check_autoconnect_toggled(GtkToggleButton *b, gpointer u)
{
    (void)b; (void)u;
    save_settings();                 /* remember preference right away */
}

/* deferred startup connection for the Auto connect checkbox */
static gboolean
autoconnect_cb(gpointer u)
{
    (void)u;
    if (sfd < 0)
        gtk_button_clicked(GTK_BUTTON(btn_connect));
    return FALSE;
}

static void
on_check_hex_toggled(GtkToggleButton *b, gpointer u)
{
    (void)b; (void)u;
    save_settings();
    redraw_all();
}

static void
on_combo_newline_changed(GtkComboBox *c, gpointer u)
{
    (void)c; (void)u;
    save_settings();
    redraw_all();               /* both raw & hex views follow selection */
}

static void
on_btn_connect_clicked(GtkButton *b, gpointer u)
{
    (void)b; (void)u;
    if (sfd < 0)
        open_port();
}

static void
on_btn_disconnect_clicked(GtkButton *b, gpointer u)
{
    (void)b; (void)u;
    if (sfd >= 0) {
        close_port();
        set_connected(FALSE);
        save_settings();
    }
}

static gboolean
on_window1_delete_event(GtkWidget *w, GdkEvent *e, gpointer u)
{
    (void)w; (void)e; (void)u;
    close_port();
    save_settings();
    return FALSE;                    /* allow destroy */
}

/* ------------------------------------------------------------------ */
/* Startup                                                             */

static void
fill_combos(void)
{
    static const char *parities[] = { "None", "Odd", "Even" };

    for (guint i = 0; i < G_N_ELEMENTS(baud_table); i++) {
        gchar *s = g_strdup_printf("%d", baud_table[i].value);
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_baud), s);
        g_free(s);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo_baud), 9);      /* 115200 */

    for (int b = 5; b <= 8; b++) {
        gchar *s = g_strdup_printf("%d", b);
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_databits), s);
        g_free(s);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo_databits), 3);  /* 8 */

    for (guint i = 0; i < G_N_ELEMENTS(parities); i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_parity),
                                       parities[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo_parity), 0);    /* none */

    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_stopbits), "1");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_stopbits), "2");
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo_stopbits), 0);  /* 1 */

    for (guint i = 0; i < G_N_ELEMENTS(nl_table); i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_newline),
                                       nl_table[i].label);
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo_newline), 0);   /* \n */
}

/* Monospace font that covers the Unicode Control Pictures block
 * (so \n, \1 ... render as real glyphs when the checkbox is on). */
static void
apply_mono_font(void)
{
    GtkCssProvider *prov = gtk_css_provider_new();
    gtk_css_provider_load_from_data(prov,
        "#textview_data { font-family: 'DejaVu Sans Mono', monospace;"
        "                  font-size: 11pt; }", -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(prov),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(prov);
}

int
main(int argc, char **argv)
{
    GtkBuilder *builder;

    gtk_init(&argc, &argv);

    /* load the UI: installed data dir first, then the source directory */
    {
        static const char *ui_paths[] = {
            MIASERIAPORDO_DATA_DIR "/windows1.glade",
            "windows1.glade",
            NULL
        };
        GError *err = NULL;
        gboolean loaded = FALSE;

        builder = gtk_builder_new();
        for (guint i = 0; ui_paths[i] && !loaded; i++)
            if (g_file_test(ui_paths[i], G_FILE_TEST_EXISTS))
                loaded = gtk_builder_add_from_file(builder, ui_paths[i], &err);
        if (!loaded) {
            g_printerr("miaseriapordo: cannot load UI: %s\n",
                       err ? err->message : "file not found");
            return 1;
        }
    }

#define W(id) gtk_builder_get_object(builder, id)
    win           = GTK_WIDGET(W("window1"));
    combo_port    = GTK_WIDGET(W("combo_port"));
    combo_baud    = GTK_WIDGET(W("combo_baud"));
    combo_databits= GTK_WIDGET(W("combo_databits"));
    combo_parity  = GTK_WIDGET(W("combo_parity"));
    combo_stopbits= GTK_WIDGET(W("combo_stopbits"));
    combo_newline = GTK_WIDGET(W("combo_newline"));
    btn_connect    = GTK_WIDGET(W("btn_connect"));
    btn_disconnect = GTK_WIDGET(W("btn_disconnect"));
    textview      = GTK_WIDGET(W("textview_data"));
    entry_send    = GTK_WIDGET(W("entry_send"));
    btn_send      = GTK_WIDGET(W("btn_send"));
    btn_clear     = GTK_WIDGET(W("btn_clear"));
    btn_refresh   = GTK_WIDGET(W("btn_refresh"));
    check_autoconnect = GTK_WIDGET(W("check_autoconnect"));
    check_hex     = GTK_WIDGET(W("check_hex"));
#undef W

    tbuf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(textview));
    gtk_widget_set_name(textview, "textview_data");

    /* highlighted escape tokens (\n, \r ...) in text mode */
    gtk_text_buffer_create_tag(tbuf, "esc",
                               "foreground", "#d0483a",
                               "weight", PANGO_WEIGHT_BOLD,
                               NULL);

    history = g_byte_array_new();

    fill_combos();
    refresh_ports();
    load_settings();
    apply_mono_font();
    gtk_window_set_default_icon_name("miaseriapordo");

    /* connect signals manually (more reliable than builder auto-connect) */
    g_signal_connect(win, "delete-event", G_CALLBACK(on_window1_delete_event), NULL);
    g_signal_connect(btn_refresh, "clicked", G_CALLBACK(on_btn_refresh_clicked), NULL);
    g_signal_connect(btn_connect, "clicked", G_CALLBACK(on_btn_connect_clicked), NULL);
    g_signal_connect(btn_disconnect, "clicked", G_CALLBACK(on_btn_disconnect_clicked), NULL);
    g_signal_connect(entry_send, "activate", G_CALLBACK(on_entry_send_activate), NULL);
    g_signal_connect(btn_send, "clicked", G_CALLBACK(on_btn_send_clicked), NULL);
    g_signal_connect(btn_clear, "clicked", G_CALLBACK(on_btn_clear_clicked), NULL);
    g_signal_connect(check_autoconnect, "toggled", G_CALLBACK(on_check_autoconnect_toggled), NULL);
    g_signal_connect(check_hex, "toggled", G_CALLBACK(on_check_hex_toggled), NULL);
    g_signal_connect(combo_newline, "changed", G_CALLBACK(on_combo_newline_changed), NULL);

    hex_reset();                    /* init newline matcher */
    gtk_widget_show_all(win);

    /* Auto connect: open the configured port shortly after startup */
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(check_autoconnect))) {
        gchar *p = gtk_combo_box_text_get_active_text(
                       GTK_COMBO_BOX_TEXT(combo_port));
        if (p && *p)
            g_timeout_add(200, autoconnect_cb, NULL);
        g_free(p);
    }

    gtk_main();

    g_object_unref(builder);
    g_byte_array_free(history, TRUE);
    return 0;
}
