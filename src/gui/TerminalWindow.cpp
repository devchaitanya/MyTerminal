#include "gui/TerminalWindow.hpp"
#include "gui/Tab.hpp"

#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/Xatom.h>
#ifdef USE_PANGO_CAIRO
#include <X11/Xlocale.h>
#endif
#include <locale.h>
#include <unistd.h>
#include <pwd.h>
#include <sys/select.h>
#include <sys/time.h>
#include <limits.h>
#include <time.h>

#include <vector>
#include <string>
#include <dirent.h>
#include <memory>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <cctype>

namespace myterm {

// UTF-8 helpers for proper Unicode handling
static inline bool utf8_is_cont(unsigned char b) { return (b & 0xC0) == 0x80; }

#ifndef USE_PANGO_CAIRO
[[maybe_unused]] static size_t utf8_char_len_from_lead(unsigned char b) {
    if (b < 0x80) return 1;
    if ((b & 0xE0) == 0xC0) {
        if (b < 0xC2) return 1; // invalid/overlong
        return 2;
    }
    if ((b & 0xF0) == 0xE0) return 3;
    if ((b & 0xF8) == 0xF0) {
        if (b > 0xF4) return 1; // invalid
        return 4;
    }
    return 1; // invalid
}
#endif

#ifndef USE_PANGO_CAIRO
static size_t utf8_next_len(const std::string& s, size_t off) {
    if (off >= s.size()) return 0;
    unsigned char lead = (unsigned char)s[off];
    size_t len = utf8_char_len_from_lead(lead);
    if (off + len > s.size()) return s.size() - off;
    // Validate continuation bytes
    for (size_t i = 1; i < len; ++i) {
        if (!utf8_is_cont((unsigned char)s[off + i])) return 1; // fallback to single byte
    }
    return len;
}
#endif

// Replace invalid UTF-8 sequences with U+FFFD
static std::string sanitize_to_valid_utf8(const std::string& s) {
    std::string out; out.reserve(s.size());
    size_t off = 0;
    const char rep[3] = {(char)0xEF, (char)0xBF, (char)0xBD}; // U+FFFD
    while (off < s.size()) {
        unsigned char b = (unsigned char)s[off];
        size_t len = 0;
        if (b < 0x80) {
            len = 1;
        } else if ((b & 0xE0) == 0xC0) {
            if (b < 0xC2) { // overlong
                out.append(rep, 3); off += 1; continue;
            }
            len = 2;
        } else if ((b & 0xF0) == 0xE0) {
            len = 3;
        } else if ((b & 0xF8) == 0xF0) {
            if (b > 0xF4) { // > U+10FFFF
                out.append(rep, 3); off += 1; continue;
            }
            len = 4;
        } else {
            // invalid lead
            out.append(rep, 3); off += 1; continue;
        }
        if (off + len > s.size()) {
            // incomplete sequence
            out.append(rep, 3); off += 1; continue;
        }
        // Check continuations
        bool valid = true;
        for (size_t i = 1; i < len; ++i) {
            if (!utf8_is_cont((unsigned char)s[off + i])) {
                valid = false; break;
            }
        }
        if (!valid) {
            out.append(rep, 3); off += 1; continue;
        }
        // Additional checks for overlong in 3/4 byte
        if (len == 3) {
            unsigned char b1 = (unsigned char)s[off + 1];
            if ((b == 0xE0 && b1 < 0xA0) || (b == 0xED && b1 > 0x9F)) { // overlong or surrogate
                out.append(rep, 3); off += 1; continue;
            }
        } else if (len == 4) {
            unsigned char b1 = (unsigned char)s[off + 1];
            if ((b == 0xF0 && b1 < 0x90) || (b == 0xF4 && b1 > 0x8F)) { // overlong
                out.append(rep, 3); off += 1; continue;
            }
        }
        out.append(s, off, len); off += len;
    }
    return out;
}

// --- Grapheme cluster helpers using Pango/GLib (for accurate Unicode shaping) ---
#ifdef USE_PANGO_CAIRO
static std::vector<size_t> utf8_grapheme_boundaries_bytes(PangoLayout* layout, const std::string& s) {
    std::vector<size_t> bounds;
    if (!layout) return bounds;
    std::string safe = sanitize_to_valid_utf8(s);
    pango_layout_set_text(layout, safe.c_str(), (int)safe.size());
    PangoLogAttr* attrs = nullptr; int n_attrs = 0;
    pango_layout_get_log_attrs(layout, &attrs, &n_attrs);
    if (!attrs || n_attrs == 0) {
        bounds.push_back(0); bounds.push_back(safe.size());
        if (attrs) g_free(attrs);
        return bounds;
    }
    int n_chars = (int)g_utf8_strlen(safe.c_str(), (gssize)safe.size());
    bounds.push_back(0);
    int start_char = 0;
    for (int pos = 1; pos <= n_chars; ++pos) {
        if (!attrs[pos].is_cursor_position) continue;
        const char* start_ptr = g_utf8_offset_to_pointer(safe.c_str(), start_char);
        const char* end_ptr   = g_utf8_offset_to_pointer(safe.c_str(), pos);
        (void)start_ptr; // unused, but documented logic
        bounds.push_back((size_t)(end_ptr - safe.c_str()));
        start_char = pos;
    }
    if (attrs) g_free(attrs);
    if (bounds.back() != safe.size()) bounds.back() = safe.size();
    return bounds;
}

static size_t utf8_grapheme_count(PangoLayout* layout, const std::string& s) {
    auto b = utf8_grapheme_boundaries_bytes(layout, s);
    if (b.empty()) return 0;
    return b.size() - 1;
}

static size_t utf8_grapheme_index_upto(PangoLayout* layout, const std::string& s, size_t byte_off) {
    auto b = utf8_grapheme_boundaries_bytes(layout, s);
    if (b.empty()) return 0;
    std::string safe = sanitize_to_valid_utf8(s);
    if (byte_off > safe.size()) byte_off = safe.size();
    // find first boundary > byte_off; index is that-1
    size_t idx = 0;
    while (idx + 1 < b.size() && b[idx + 1] <= byte_off) idx++;
    return idx; // number of clusters fully before or at offset
}

static std::string utf8_substr_grapheme(PangoLayout* layout, const std::string& s, size_t start_g, size_t len_g) {
    auto b = utf8_grapheme_boundaries_bytes(layout, s);
    if (b.empty()) return std::string();
    size_t total = b.size() - 1;
    if (start_g > total) start_g = total;
    size_t end_g = std::min(total, start_g + len_g);
    size_t start_b = b[start_g];
    size_t end_b = b[end_g];
    // Return slice from a sanitized string to ensure valid UTF-8 is passed to Pango downstream
    std::string safe = sanitize_to_valid_utf8(s);
    if (start_b > safe.size()) start_b = safe.size();
    if (end_b > safe.size()) end_b = safe.size();
    return safe.substr(start_b, end_b - start_b);
}
#endif

// Provide non-Pango fallbacks so IntelliSense (or non-Pango builds) see valid symbols
#ifndef USE_PANGO_CAIRO
// Forward declarations for fallback helpers used below
static size_t utf8_count_codepoints(const std::string& s);
static size_t utf8_count_codepoints_upto(const std::string& s, size_t byte_off);
static std::string utf8_substr_cp(const std::string& s, size_t start_cp, size_t len_cp);

static std::vector<size_t> utf8_grapheme_boundaries_bytes(void*, const std::string& s) {
    // Treat each Unicode code point as a grapheme cluster
    std::vector<size_t> bounds; bounds.reserve(s.size()+1);
    bounds.push_back(0);
    size_t i = 0;
    while (i < s.size()) {
        size_t len = utf8_next_len(s, i);
        if (len == 0) break;
        i += len;
        bounds.push_back(i);
    }
    if (bounds.back() != s.size()) bounds.push_back(s.size());
    return bounds;
}
static size_t utf8_grapheme_count(void*, const std::string& s) {
    return utf8_count_codepoints(s);
}
static size_t utf8_grapheme_index_upto(void*, const std::string& s, size_t byte_off) {
    return utf8_count_codepoints_upto(s, byte_off);
}
static std::string utf8_substr_grapheme(void*, const std::string& s, size_t start_g, size_t len_g) {
    return utf8_substr_cp(s, start_g, len_g);
}
#endif

// Macro abstraction for layout pointer used in grapheme helpers
#ifdef USE_PANGO_CAIRO
#define MYTERM_LAYOUT pangoLayout_
#else
#define MYTERM_LAYOUT nullptr
#endif

#ifndef USE_PANGO_CAIRO
// Count Unicode code points in a UTF-8 string (non-Pango fallback build only)
static size_t utf8_count_codepoints(const std::string& s) {
    size_t count = 0;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char b = (unsigned char)s[i];
        if ((b & 0xC0) != 0x80) count++;
        size_t len = utf8_next_len(s, i);
        if (len == 0) break;
        i += len;
    }
    return count;
}

// Count code points from start up to a given byte offset (clamped)
static size_t utf8_count_codepoints_upto(const std::string& s, size_t byte_off) {
    if (byte_off > s.size()) byte_off = s.size();
    size_t count = 0;
    size_t i = 0;
    while (i < byte_off) {
        unsigned char b = (unsigned char)s[i];
        if ((b & 0xC0) != 0x80) count++;
        size_t len = utf8_next_len(s, i);
        if (len == 0) break;
        i += len;
    }
    return count;
}

// Compute the byte offset at which a given code point index starts
static size_t utf8_byte_offset_for_codepoints(const std::string& s, size_t cp_index) {
    size_t i = 0;
    size_t cp = 0;
    while (i < s.size() && cp < cp_index) {
        size_t len = utf8_next_len(s, i);
        if (len == 0) break;
        i += len;
        cp++;
    }
    return i; // may be == s.size()
}

// Take a substring by code point count (start_cp, len_cp)
static std::string utf8_substr_cp(const std::string& s, size_t start_cp, size_t len_cp) {
    size_t start_b = utf8_byte_offset_for_codepoints(s, start_cp);
    size_t end_b = utf8_byte_offset_for_codepoints(s, start_cp + len_cp);
    if (start_b > s.size()) start_b = s.size();
    if (end_b > s.size()) end_b = s.size();
    if (end_b < start_b) end_b = start_b;
    return s.substr(start_b, end_b - start_b);
}
#endif // !USE_PANGO_CAIRO

// Spawn a tiny helper that will begin draining the given fd only after this UI
// process exits. This avoids competing reads while the UI is alive, but keeps
// the fd open and prevents SIGHUP/EOF when the UI is gone.
static void spawnFdKeeperDelayed(int fdToKeep) {
    if (fdToKeep < 0) return;
    int dupFd = dup(fdToKeep);
    if (dupFd < 0) return;
    pid_t pid = fork();
    if (pid < 0) { close(dupFd); return; }
    if (pid == 0) {
        // Child: become its own session, ignore SIGHUP, drain forever until EOF
        setsid();
        signal(SIGHUP, SIG_IGN);
        signal(SIGPIPE, SIG_IGN);
        pid_t ppid_initial = getppid();
        // Wait until parent exits (then we're adopted by init, ppid==1)
        while (getppid() == ppid_initial) { usleep(100000); }
        // Close all fds except dupFd (best-effort)
        for (int fd = 0; fd < 256; ++fd) { if (fd != dupFd) close(fd); }
        char buf[4096];
        while (true) {
            ssize_t n = read(dupFd, buf, sizeof(buf));
            if (n > 0) {
                // discard; prevents writer from blocking
                continue;
            }
            if (n == 0) break; // EOF, child closed side
            if (errno == EAGAIN || errno == EWOULDBLOCK) { usleep(50000); continue; }
            // On error, exit
            break;
        }
        close(dupFd);
        _exit(0);
    }
    // Parent: keep going; dupFd now owned by keeper
}
static std::string get_user() {
    if (const char* u = getenv("USER")) return u;
    if (passwd* pw = getpwuid(getuid())) return pw->pw_name;
    return "user";
}
static std::string get_host() {
    // For requested behavior, show username on both sides: user@user
    return get_user();
}
static std::string get_cwd() {
    char b[PATH_MAX];
    if (getcwd(b, sizeof(b))) {
        std::string cwd = b;
        const char* homeEnv = getenv("HOME");
        std::string home;
        if (homeEnv && *homeEnv) {
            home = homeEnv;
        } else if (passwd* pw = getpwuid(getuid())) {
            if (pw->pw_dir) home = pw->pw_dir;
        }
        if (!home.empty()) {
            // Normalize: remove any trailing slash from HOME except root
            if (home.size() > 1 && home.back() == '/') home.pop_back();
            if (cwd == home) {
                return "~";
            } else if (cwd.rfind(home + "/", 0) == 0) {
                // Starts with HOME + '/'
                return std::string("~") + cwd.substr(home.size());
            }
        }
        return cwd;
    }
    return "?";
}

TerminalWindow::TerminalWindow(int w, int h): width_(w), height_(h) {
    setlocale(LC_ALL, "");
    tabs_.emplace_back(std::make_unique<Tab>());
}

TerminalWindow::~TerminalWindow() {
#ifdef USE_PANGO_CAIRO
    destroyCairoObjects();
#endif
    if (xic_) XDestroyIC(xic_);
    if (xim_) XCloseIM(xim_);
    if (font_) XFreeFont(dpy_, font_);
    if (gc_) XFreeGC(dpy_, gc_);
    if (win_) XDestroyWindow(dpy_, win_);
    if (dpy_) XCloseDisplay(dpy_);
}

void TerminalWindow::initX11() {
    dpy_ = XOpenDisplay(nullptr);
    if (!dpy_) { fprintf(stderr, "Failed to open X display\n"); _exit(1); }
    screen_ = DefaultScreen(dpy_);
    win_ = XCreateSimpleWindow(dpy_, RootWindow(dpy_, screen_), 100, 100, width_, height_, 0,
                               BlackPixel(dpy_, screen_), WhitePixel(dpy_, screen_));
    XStoreName(dpy_, win_, "MyTerminal");
    XSelectInput(dpy_, win_, ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | StructureNotifyMask | FocusChangeMask | PropertyChangeMask);
    XMapWindow(dpy_, win_);

    selectFont();
    gc_ = XCreateGC(dpy_, win_, 0, nullptr);
    if (font_) XSetFont(dpy_, gc_, font_->fid);
    cmap_ = DefaultColormap(dpy_, screen_);
    allocateColors();
    XSetWindowBackground(dpy_, win_, theme_.bg);
    XSetForeground(dpy_, gc_, theme_.fg);

    // Initialize input method for UTF-8 keyboard input
    if (!XSupportsLocale()) {
        // best-effort: still try
    }
    const char* lm = XSetLocaleModifiers("");
    (void)lm;
    xim_ = XOpenIM(dpy_, nullptr, nullptr, nullptr);
    if (!xim_) {
        // try fallback modifiers
        XSetLocaleModifiers("@im=local");
        xim_ = XOpenIM(dpy_, nullptr, nullptr, nullptr);
    }
    if (!xim_) {
        XSetLocaleModifiers("@im=none");
        xim_ = XOpenIM(dpy_, nullptr, nullptr, nullptr);
    }
    if (xim_) {
        xic_ = XCreateIC(xim_,
                         XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
                         XNClientWindow, win_,
                         XNFocusWindow, win_,
                         nullptr);
    }

    // Clipboard atoms
    clipboardAtom_ = XInternAtom(dpy_, "CLIPBOARD", False);
    utf8Atom_ = XInternAtom(dpy_, "UTF8_STRING", False);
    pasteProperty_ = XInternAtom(dpy_, "MYTERM_PASTE", False);
}
void TerminalWindow::selectFont() {
    // Try a cascade of reliable core X11 fonts (visible size change), then fallback
    const char* fonts[] = {
        "10x20",     // large, readable
        "12x24",     // larger
        "9x15",      // common monospace
        "fixed"      // fallback
    };
    for (auto f: fonts) {
        font_ = XLoadQueryFont(dpy_, f);
        if (font_) break;
    }
    if (!font_) font_ = XLoadQueryFont(dpy_, "fixed");
    // Set line height based on font metrics
    if (font_) lineH_ = font_->ascent + font_->descent + 2; else lineH_ = 18;
}
void TerminalWindow::allocateColors() {
    auto alloc = [&](const char* name, unsigned long& out) {
        XColor color, exact;
        if (XAllocNamedColor(dpy_, cmap_, name, &color, &exact)) out = color.pixel;
    };
    // Dark theme similar to Ubuntu terminal
    alloc("#1e1e1e", theme_.bg);        // background
    alloc("#e5e5e5", theme_.fg);        // foreground
    alloc("#4ec9b0", theme_.green);     // user@host (teal-ish)
    alloc("#569cd6", theme_.blue);      // cwd (blue)
    alloc("#606060", theme_.gray);      // UI separators
    alloc("#dcdcaa", theme_.cursor);    // caret (soft yellow)
    alloc("#2d2d2d", theme_.tabInactiveBg);
    alloc("#333333", theme_.tabActiveBg);
    alloc("#c586c0", theme_.accent);    // accent/magenta for active underline
    alloc("#252525", theme_.scrollTrack);
    alloc("#555555", theme_.scrollThumb);
    alloc("#6a6a6a", theme_.scrollThumbHover);
    alloc("#3a3a3a", theme_.tabHoverBg);
    alloc("#2a2a2a", theme_.newTabBg);

    // ANSI 16 colors
    const char* ansiColors[16] = {
        "#000000", "#cd0000", "#00cd00", "#cdcd00", "#0000cd", "#cd00cd", "#00cdcd", "#e5e5e5",
        "#4d4d4d", "#ff0000", "#00ff00", "#ffff00", "#0000ff", "#ff00ff", "#00ffff", "#ffffff"
    };
    theme_.ansiFgColors.resize(16);
    theme_.ansiBgColors.resize(16);
    for (int i=0; i<16; i++) {
        alloc(ansiColors[i], theme_.ansiFgColors[i]);
        theme_.ansiBgColors[i] = theme_.ansiFgColors[i];
    }
    // Xft not used
}

unsigned long TerminalWindow::ansiColorToPixel(int code, bool fg) const {
    if (code >= 0 && code < 16) {
        return fg ? theme_.ansiFgColors[code] : theme_.ansiBgColors[code];
    } else if (code >= 16 && code < 232) {
        // 6x6x6 cube
        int r = ((code - 16) / 36) * 51;
        int g = (((code - 16) % 36) / 6) * 51;
        int b = ((code - 16) % 6) * 51;
        XColor color;
        color.red = r << 8;
        color.green = g << 8;
        color.blue = b << 8;
        XAllocColor(dpy_, cmap_, &color);
        return color.pixel;
    } else if (code >= 232 && code < 256) {
        // grayscale
        int gray = (code - 232) * 10 + 8;
        XColor color;
        color.red = color.green = color.blue = gray << 8;
        XAllocColor(dpy_, cmap_, &color);
        return color.pixel;
    }
    return fg ? theme_.fg : theme_.bg;
}

void TerminalWindow::drawAnsiText(int x, int y, const std::string& text, unsigned long fgColor, unsigned long bgColor) {
    // For simplicity, assume no background color for now, just fg
    (void)bgColor; // suppress unused parameter warning for now
#ifdef USE_PANGO_CAIRO
    drawTextPango(x, y, text, fgColor);
#else
    XSetForeground(dpy_, gc_, fgColor);
    XDrawString(dpy_, win_, gc_, x, y, text.c_str(), (int)text.size());
#endif
}

int TerminalWindow::drawTextAdvance(int x, int y, const std::string& text, unsigned long fgColor, unsigned long bgColor) {
    (void)bgColor;
#ifdef USE_PANGO_CAIRO
    std::string safe = sanitize_to_valid_utf8(text);
    drawTextPango(x, y, safe, fgColor);
    return measureTextPango(safe);
#else
    XSetForeground(dpy_, gc_, fgColor);
    XDrawString(dpy_, win_, gc_, x, y, text.c_str(), (int)text.size());
    return (int)text.size() * charWidth();
#endif
}

#ifdef USE_PANGO_CAIRO
[[maybe_unused]] static int compute_pango_cell_width(PangoLayout* layout, PangoFontDescription* desc) {
    // Measure width of 100 ASCII 'M' characters and divide by 100
    const char* probe = "MMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMM"; // 100 M
    pango_layout_set_text(layout, probe, -1);
    pango_layout_set_font_description(layout, desc);
    PangoRectangle ink, logical; pango_layout_get_pixel_extents(layout, &ink, &logical);
    return std::max(1, logical.width / 100);
}
#endif

int TerminalWindow::charWidth() const {
#ifdef USE_PANGO_CAIRO
    return cellW_ > 0 ? cellW_ : (font_ ? font_->max_bounds.width : 8);
#else
    return font_ ? font_->max_bounds.width : 8;
#endif
}

int TerminalWindow::measureTextWidth(const std::string& text) {
#ifdef USE_PANGO_CAIRO
    return measureTextPango(text);
#else
    if (!font_) return (int)text.size() * 8;
    return XTextWidth(font_, text.c_str(), (int)text.size());
#endif
}

#ifdef USE_PANGO_CAIRO
int TerminalWindow::measureTextPango(const std::string& utf8) {
    ensureCairoSurface();
    if (!cr_) return (int)utf8.size() * 8;
    if (!pangoLayout_) pangoLayout_ = pango_cairo_create_layout(cr_);

    std::string safeUtf8 = sanitize_to_valid_utf8(utf8);
    // Use grapheme clusters for terminal cell count
    int clusters = (int)utf8_grapheme_count(pangoLayout_, safeUtf8);
    return clusters * charWidth();
}
void TerminalWindow::ensureCairoSurface() {
    if (!cairoSurface_ || cairoW_ != width_ || cairoH_ != height_) {
        if (cairoSurface_) cairo_surface_destroy(cairoSurface_);
        cairoSurface_ = cairo_xlib_surface_create(dpy_, win_, DefaultVisual(dpy_, screen_), width_, height_);
        cairoW_ = width_; cairoH_ = height_;
    }
    if (!cr_) cr_ = cairo_create(cairoSurface_);
    if (!pangoLayout_) pangoLayout_ = pango_cairo_create_layout(cr_);
    if (!pangoFontDesc_) {
        pangoFontDesc_ = pango_font_description_new();
        pango_font_description_set_family(pangoFontDesc_, "Monospace");
        // Match the X11 font size more precisely
        if (font_) {
            int font_size = font_->ascent + font_->descent;
            // Reduce font size a bit more so text isn’t oversized
            int desired = std::max(6, font_size - 7);
            pango_font_description_set_size(pangoFontDesc_, desired * PANGO_SCALE);
        } else {
            pango_font_description_set_size(pangoFontDesc_, 12 * PANGO_SCALE);
        }
        
        // Configure layout for terminal consistency
        pango_layout_set_spacing(pangoLayout_, 0);
        pango_layout_set_single_paragraph_mode(pangoLayout_, TRUE);
        pango_layout_set_width(pangoLayout_, -1); // no wrapping
        
        // Set font options for crisp terminal rendering
        cairo_font_options_t *font_options = cairo_font_options_create();
        cairo_font_options_set_antialias(font_options, CAIRO_ANTIALIAS_GRAY);
        cairo_font_options_set_hint_style(font_options, CAIRO_HINT_STYLE_FULL);
        cairo_font_options_set_hint_metrics(font_options, CAIRO_HINT_METRICS_ON);
        pango_cairo_context_set_font_options(pango_layout_get_context(pangoLayout_), font_options);
        cairo_font_options_destroy(font_options);
    }
    // After creating layout and font description, compute metrics and cell width once
    PangoContext* ctx = pango_layout_get_context(pangoLayout_);
    PangoFontMetrics* m = pango_context_get_metrics(ctx, pangoFontDesc_, pango_language_get_default());
    pangoAscent_ = pango_font_metrics_get_ascent(m) / PANGO_SCALE;
    pangoDescent_ = pango_font_metrics_get_descent(m) / PANGO_SCALE;
    pango_font_metrics_unref(m);
    cellW_ = compute_pango_cell_width(pangoLayout_, pangoFontDesc_);
    // Slightly tighten horizontal spacing for Unicode clusters
    if (cellW_ > 1) cellW_ -= 1;
    // Align overall line height to Pango metrics with minimal extra padding to avoid clipping
    int pangoLine = pangoAscent_ + pangoDescent_;
    lineH_ = std::max(lineH_, pangoLine + 4);
}

void TerminalWindow::drawTextPango(int x, int y, const std::string& utf8, unsigned long fgPixel) {
    ensureCairoSurface();
    // Convert X11 pixel to RGB components (approximate)
    XColor c; c.pixel = fgPixel; XQueryColor(dpy_, cmap_, &c);
    double r = c.red / 65535.0, g = c.green / 65535.0, b = c.blue / 65535.0;
    cairo_set_source_rgb(cr_, r, g, b);

    std::string safeUtf8 = sanitize_to_valid_utf8(utf8);

    // Render by grapheme clusters so complex scripts (e.g., Devanagari) shape correctly
    pango_layout_set_text(pangoLayout_, safeUtf8.c_str(), (int)safeUtf8.size());
    pango_layout_set_font_description(pangoLayout_, pangoFontDesc_);

    PangoLogAttr* attrs = nullptr; int n_attrs = 0;
    pango_layout_get_log_attrs(pangoLayout_, &attrs, &n_attrs);
    int n_chars = (int)g_utf8_strlen(safeUtf8.c_str(), (gssize)safeUtf8.size());
    if (!attrs || n_attrs == 0) {
        // Fallback: draw as single run
        int char_width = charWidth();
        PangoRectangle logical; pango_layout_get_pixel_extents(pangoLayout_, nullptr, &logical);
        int offset_x = std::max(0, (char_width - logical.width) / 2);
        // Use global ascent/descent for consistent baseline and to avoid clipping descenders
        int top_y = y - pangoAscent_;
        cairo_save(cr_);
        cairo_rectangle(cr_, x, top_y - 1, char_width, pangoAscent_ + pangoDescent_ + 4);
        cairo_clip(cr_);
        cairo_move_to(cr_, x + offset_x, top_y);
        pango_cairo_show_layout(cr_, pangoLayout_);
        cairo_restore(cr_);
        return;
    }

    int current_x = x;
    int char_width = charWidth();
    int start_char = 0;
    for (int pos = 1; pos <= n_chars; ++pos) {
        if (!attrs[pos].is_cursor_position) continue; // not a grapheme boundary
        // Map char indices to byte offsets
        const char* start_ptr = g_utf8_offset_to_pointer(safeUtf8.c_str(), start_char);
        const char* end_ptr   = g_utf8_offset_to_pointer(safeUtf8.c_str(), pos);
        std::string cluster(start_ptr, end_ptr - start_ptr);

        pango_layout_set_text(pangoLayout_, cluster.c_str(), (int)cluster.size());
        PangoRectangle logical; pango_layout_get_pixel_extents(pangoLayout_, nullptr, &logical);
        int offset_x = std::max(0, (char_width - logical.width) / 2);
    // Use global ascent/descent for consistent baseline and avoid per-glyph baseline jitter
    int top_y = y - pangoAscent_; // baseline at y
        cairo_save(cr_);
    // Minimal vertical padding to ensure underscores/descenders are not clipped
    cairo_rectangle(cr_, current_x, top_y - 1, char_width, pangoAscent_ + pangoDescent_ + 4);
        cairo_clip(cr_);
        cairo_move_to(cr_, current_x + offset_x, top_y);
        pango_cairo_show_layout(cr_, pangoLayout_);
        cairo_restore(cr_);
        current_x += char_width;
        start_char = pos;
    }
    if (attrs) g_free(attrs);
}

void TerminalWindow::destroyCairoObjects() {
    if (pangoLayout_) { g_object_unref(pangoLayout_); pangoLayout_ = nullptr; }
    if (pangoFontDesc_) { pango_font_description_free(pangoFontDesc_); pangoFontDesc_ = nullptr; }
    if (cr_) { cairo_destroy(cr_); cr_ = nullptr; }
    if (cairoSurface_) { cairo_surface_destroy(cairoSurface_); cairoSurface_ = nullptr; }
}
#endif

void TerminalWindow::drawTabBar() {
    int tabH = 26;
    int tabW = 140;
    int tabSpacing = 4;
    for (int i=0;i<(int)tabs_.size();++i) {
        int x = 8 + i*(tabW + tabSpacing);
        bool hover = (hoverTabIndex_ == i);
        unsigned long bg = (i==activeTab_) ? theme_.tabActiveBg : (hover && theme_.tabHoverBg ? theme_.tabHoverBg : theme_.tabInactiveBg);
        XSetForeground(dpy_, gc_, bg);
        XFillRectangle(dpy_, win_, gc_, x, 6, tabW, tabH);
        if (i==activeTab_) {
            XSetForeground(dpy_, gc_, theme_.accent);
            XFillRectangle(dpy_, win_, gc_, x, 6+tabH, tabW, 2);
        }
        XSetForeground(dpy_, gc_, theme_.gray);
        XDrawRectangle(dpy_, win_, gc_, x, 6, tabW, tabH);
        std::string label = std::string("Tab ") + std::to_string(i+1);
        int textX = x + 8;
        int textY = 6 + tabH - 6;
    XSetForeground(dpy_, gc_, theme_.fg); XDrawString(dpy_, win_, gc_, textX, textY, label.c_str(), (int)label.size());
        // Draw close button
        int closeW = 16; int closeH = 16;
        int closeX = x + tabW - closeW - 4;
        int closeY = 6 + (tabH - closeH)/2;
        XSetForeground(dpy_, gc_, theme_.scrollThumb);
        XFillRectangle(dpy_, win_, gc_, closeX, closeY, closeW, closeH);
        XSetForeground(dpy_, gc_, theme_.gray);
        XDrawRectangle(dpy_, win_, gc_, closeX, closeY, closeW, closeH);
        XSetForeground(dpy_, gc_, theme_.fg);
        XDrawString(dpy_, win_, gc_, closeX + 6, closeY + 12, "x", 1);
    }
    // New tab button
    int xPlus = 8 + (int)tabs_.size()*(tabW + tabSpacing);
    int plusW = 28; int plusH = tabH;
    unsigned long plusBg = hoverNewTab_ && theme_.tabHoverBg ? theme_.tabHoverBg : (theme_.newTabBg ? theme_.newTabBg : theme_.tabInactiveBg);
    XSetForeground(dpy_, gc_, plusBg);
    XFillRectangle(dpy_, win_, gc_, xPlus, 6, plusW, plusH);
    XSetForeground(dpy_, gc_, theme_.gray);
    XDrawRectangle(dpy_, win_, gc_, xPlus, 6, plusW, plusH);
    // draw '+' centered
    int textX = xPlus + (plusW - charWidth())/2;
    int asc =
#ifdef USE_PANGO_CAIRO
    (pangoAscent_ ? pangoAscent_ : (font_ ? font_->ascent : 10));
#else
    (font_ ? font_->ascent : 10);
#endif
    int desc =
#ifdef USE_PANGO_CAIRO
    (pangoDescent_ ? pangoDescent_ : (font_ ? font_->descent : 2));
#else
    (font_ ? font_->descent : 2);
#endif
    // Center baseline vertically inside the button rect: top padding + center of (asc+desc)
    int textY = 6 + ((plusH - (asc + desc)) / 2) + asc;
#ifdef USE_PANGO_CAIRO
    drawTextAdvance(textX, textY, std::string("+"), theme_.fg, 0);
#else
    XSetForeground(dpy_, gc_, theme_.fg);
    XDrawString(dpy_, win_, gc_, textX, textY, "+", 1);
#endif
}

void TerminalWindow::drawTextArea() {
    #ifdef USE_PANGO_CAIRO
    ensureCairoSurface();
    #endif
    int y = 40 + lineH_; // below tab bar
    Tab& t = *tabs_[activeTab_];

    // Build lines from scrollback plus live prompt+input (which may be multi-line)
    size_t start=0; std::vector<std::string> lines;
    for (size_t i=0;i<t.scrollback.size();++i) if (t.scrollback[i]=='\n') { lines.emplace_back(t.scrollback.substr(start,i-start)); start=i+1; }
    if (start<t.scrollback.size()) lines.emplace_back(t.scrollback.substr(start));

    // Soft-wrap scrollback lines
    std::vector<std::string> wrappedLines;
    int wrapCols = std::max(1, (width_ - 20) / charWidth());
    for (const std::string& line : lines) {
        int graphemeCount = (int)utf8_grapheme_count(MYTERM_LAYOUT, line);
        for (int s = 0; s < graphemeCount; s += wrapCols) {
            int len = std::min(wrapCols, graphemeCount - s);
            std::string sub = utf8_substr_grapheme(MYTERM_LAYOUT, line, (size_t)s, (size_t)len);
            wrappedLines.push_back(sub);
        }
    }
    lines = std::move(wrappedLines);

    // Only honor auto-scroll to bottom if we're already at the bottom; if user scrolled up, don't snap
    if (t.scrollToBottom && t.scrollOffsetLines == 0) { t.scrollOffsetTargetLines = 0; t.scrollToBottom = false; }

    // Compute live input split. If a child is running, show raw input (no PS1/PS2).
    std::vector<std::string> inputParts; inputParts.reserve(4);
    {
        size_t s=0,p=0; while ((p = t.input.find('\n', s)) != std::string::npos) { inputParts.emplace_back(t.input.substr(s, p-s)); s=p+1; }
        inputParts.emplace_back(t.input.substr(s));
    }
    int firstLiveIdx = (int)lines.size();
    bool searchActive = (searchActive_ && t.childPid <= 0);
    int searchLineIdx = -1;
    // Build live input lines and map caret in one consistent pass (handles wrap and newlines)
    int liveLineIdxForCursor = -1;
    int cursorColForLive = 0;
    if (t.childPid <= 0 && !autocompleteChoiceActive_) {
        const int charW = charWidth();
        const int maxCols = std::max(1, (width_ - 20) / charW);
        const std::string u = get_user();
        const std::string h = get_host();
        const std::string cwdstr = get_cwd();
        const std::string uh = u+"@"+h+":";
        const std::string ps1_prefix = u+"@"+h+":"+cwdstr+"$ ";
        const std::string ps2_prefix = "> ";
    const int ps1Cols = (int)utf8_grapheme_count(MYTERM_LAYOUT, uh) + (int)utf8_grapheme_count(MYTERM_LAYOUT, cwdstr) + 2;
        const int ps2Cols = 2;

        // Grapheme boundaries across the full input; caret index in graphemes
    const auto bounds = utf8_grapheme_boundaries_bytes(MYTERM_LAYOUT, t.input);
        const size_t totalG = bounds.empty() ? 0 : (bounds.size()-1);
    const size_t caretG = utf8_grapheme_index_upto(MYTERM_LAYOUT, t.input, t.cursor);

    int currentCols = 0;
    std::string currentLine;
    int currentPromptCols = t.contActive ? ps2Cols : ps1Cols;
    std::string currentPromptPrefix = t.contActive ? ps2_prefix : ps1_prefix;
        bool caretPlaced = false;
        int producedLiveLines = 0;

    // Initialize first line with PS1 or PS2 prompt depending on continuation state
        currentLine = currentPromptPrefix;
        currentCols = currentPromptCols;

        for (size_t gi = 0; gi < totalG; ++gi) {
            // Extract this grapheme
            size_t b = bounds[gi], e = bounds[gi+1];
            std::string g = t.input.substr(b, e-b);
            bool isNewline = (g.size() == 1 && g[0] == '\n');

            if (caretG == gi && !caretPlaced) {
                cursorColForLive = currentCols; // caret before this grapheme
                liveLineIdxForCursor = firstLiveIdx + producedLiveLines;
                caretPlaced = true;
            }

            if (isNewline) {
                // Push current line and start a new one with PS2
                lines.emplace_back(currentLine);
                producedLiveLines++;
                currentLine = ps2_prefix;
                currentPromptCols = ps2Cols;
                currentCols = currentPromptCols;
                continue;
            }

            // Wrap if needed
            if (currentCols >= maxCols) {
                lines.emplace_back(currentLine);
                producedLiveLines++;
                currentLine = ps2_prefix;
                currentPromptCols = ps2Cols;
                currentCols = currentPromptCols;
            }
            currentLine += g;
            currentCols += 1; // one cell per grapheme in live grid
        }

        // If caret not set yet, it's at end of current line (after prompt)
        if (!caretPlaced) {
            cursorColForLive = currentCols;
            liveLineIdxForCursor = firstLiveIdx + producedLiveLines;
            caretPlaced = true;
        }
        // Push the last line if it has content (prompt always present)
        if (!currentLine.empty()) {
            lines.emplace_back(currentLine);
            producedLiveLines++;
        }
    }
    // When searching, show a separate search prompt line after the live prompt
    if (searchActive) {
        std::string prompt = "Enter search term: ";
        lines.emplace_back(prompt + searchTerm_);
        searchLineIdx = (int)lines.size() - 1;
        // Place caret at end of search term
        if (t.childPid <= 0) {
            cursorColForLive = (int)utf8_grapheme_count(MYTERM_LAYOUT, prompt) + (int)utf8_grapheme_count(MYTERM_LAYOUT, searchTerm_);
            liveLineIdxForCursor = searchLineIdx;
        }
    }


    // Viewport height: reserve only the top margin (lineH_) below the tab bar, no extra bottom padding
    int viewportLines = std::max(1,(height_ - 40 - lineH_)/lineH_);
    // Snap scroll to target immediately for responsiveness
    if (t.scrollOffsetTargetLines < 0) t.scrollOffsetTargetLines = 0;
    t.scrollOffsetLines = t.scrollOffsetTargetLines;
    int bottomStart = std::max(0,(int)lines.size()-viewportLines);
    int begin = std::max(0, bottomStart - std::max(0, t.scrollOffsetLines));
    int end = std::min((int)lines.size(), begin + viewportLines);

    // If mapping failed or produced an off-screen line, fall back to last live line
    if (t.childPid <= 0 && (liveLineIdxForCursor < firstLiveIdx || liveLineIdxForCursor >= (int)lines.size())) {
    #ifdef USE_PANGO_CAIRO
    int cols = (int)utf8_grapheme_count(MYTERM_LAYOUT, lines.empty()?std::string():lines.back());
    #else
    int cols = (int)utf8_count_codepoints(lines.empty()?std::string():lines.back());
    #endif
    liveLineIdxForCursor = std::max(firstLiveIdx, (int)lines.size()-1);
    cursorColForLive = cols;
    }

    // Compute horizontal pan (in columns) for the live line so the cursor stays visible
    int liveHScrollCols = 0;
    if (liveLineIdxForCursor >= begin && liveLineIdxForCursor < end) {
        int charW = charWidth();
        int maxCols = std::max(1, (width_ - 20) / charW);
        // Pan so the caret is always inside the viewport (place near the right edge if needed)
        liveHScrollCols = std::max(0, cursorColForLive - (maxCols - 1));
    }

    for (int i=begin;i<end;++i) {
#ifdef USE_PANGO_CAIRO
        // Clip rendering to the text area width to avoid overflow, ensuring descenders are visible
        ensureCairoSurface();
        cairo_save(cr_);
    cairo_rectangle(cr_, 10, y - pangoAscent_ - 1, std::max(0, width_ - 20), pangoAscent_ + pangoDescent_ + 2);
        cairo_clip(cr_);
    int drawX = 10;
    if (i == liveLineIdxForCursor && liveHScrollCols > 0) {
            drawX -= liveHScrollCols * charWidth();
        }
    bool isLiveGrid = (t.childPid <= 0 && !autocompleteChoiceActive_ && i >= firstLiveIdx);
    drawMaybeColoredPromptLine(drawX, y, lines[i], isLiveGrid);
        cairo_restore(cr_);
#else
        int drawX = 10;
        if (i == liveLineIdxForCursor && liveHScrollCols > 0) drawX -= liveHScrollCols * charWidth();
    bool isLiveGrid = (t.childPid <= 0 && !autocompleteChoiceActive_ && i >= firstLiveIdx);
    drawMaybeColoredPromptLine(drawX, y, lines[i], isLiveGrid);
#endif
        y+=lineH_;
    }

    // Visual scrollbar reflects total lines including live prompt line
    drawScrollBar((int)lines.size(), viewportLines, begin);

    // Draw cursor only if the cursor's live line is visible within the current viewport
    if (t.childPid <= 0 && !autocompleteChoiceActive_ && (focused_ ? cursorOn_ : true)) {
        if (liveLineIdxForCursor >= begin && liveLineIdxForCursor < end) {
            // If cursor is on the last line, ensure we scroll to bottom for visibility
            if (liveLineIdxForCursor == (int)lines.size() - 1 && t.scrollOffsetLines == 0) {
                t.scrollOffsetTargetLines = 0;
                t.scrollOffsetLines = 0;
            }
            int yLine = 40 + lineH_ + (liveLineIdxForCursor - begin) * lineH_;
            int charW = charWidth();
            // cursorCol already includes prefix columns
            int baseX = 10; // drawing starts at x=10
            int cellLeftX = baseX + (cursorColForLive - liveHScrollCols)*charW;
            // Draw caret at the LEFT edge of the current cell (between characters)
            int drawW = 2; // slim caret width
            int caretX = cellLeftX; // boundary position between cells
            int ascent =
#ifdef USE_PANGO_CAIRO
                (pangoAscent_ ? pangoAscent_ : (font_ ? font_->ascent : (lineH_ - 4)));
#else
                (font_ ? font_->ascent : (lineH_ - 4));
#endif
            int descent =
#ifdef USE_PANGO_CAIRO
                (pangoDescent_ ? pangoDescent_ : (font_ ? font_->descent : 2));
#else
                (font_ ? font_->descent : 2);
#endif
            int top = yLine - ascent;
            int height = ascent + descent;
            XSetForeground(dpy_, gc_, theme_.cursor);
            if (focused_) {
                // Slim solid bar at right cell edge
                XFillRectangle(dpy_, win_, gc_, caretX, top, drawW, height);
                // Subtle underline at the baseline for extra visibility
                int baselineY = yLine; // baseline is yLine
                XFillRectangle(dpy_, win_, gc_, caretX, baselineY, drawW, 1);
            } else {
                // Unfocused: even slimmer bar
                XFillRectangle(dpy_, win_, gc_, caretX, top, 2, height);
            }
            XSetForeground(dpy_, gc_, theme_.fg);
        } else {
            // If caret would be off-screen, only auto-reveal when at bottom; if user scrolled up, don't force jump
            if (t.scrollOffsetLines == 0) {
                int viewportLines2 = std::max(1,(height_ - 40 - lineH_)/lineH_);
                int targetBegin = std::max(0, liveLineIdxForCursor - (viewportLines2 - 1));
                int bottomStart2 = std::max(0,(int)lines.size()-viewportLines2);
                t.scrollOffsetTargetLines = std::max(0, bottomStart2 - targetBegin);
                cursorOn_ = true;
            }
        }
    }
}

void TerminalWindow::drawColoredPromptLine(int x, int y, const std::string& line) {
    // Split and draw: user@host: in green, cwd in blue, "$ " and input in fg.
    std::string u = get_user();
    std::string h = get_host();
    std::string cwd = get_cwd();
    std::string dollar = "$ ";
    int advance = 0;

    std::string uh = u+"@"+h+":";
    // user@host:
    advance += drawTextAdvance(x + advance, y, uh, theme_.green, 0);
    // cwd
    advance += drawTextAdvance(x + advance, y, cwd, theme_.blue, 0);
    // "$ "
    advance += drawTextAdvance(x + advance, y, dollar, theme_.fg, 0);

    // Draw remainder after "$ " from the provided line (assumes structure matches)
    size_t pos_after_prompt = uh.size() + cwd.size() + dollar.size();
    if (line.size() > pos_after_prompt) {
        std::string rest = line.substr(pos_after_prompt);
        // User-typed text remains regular (not bold)
        advance += drawTextAdvance(x + advance, y, rest, theme_.fg, 0);
    }
}

void TerminalWindow::drawMaybeColoredPromptLine(int x, int y, const std::string& line, bool gridMode) {
    // Detect lines that look like our prompt: user@host:cwd$ rest
    std::string u = get_user();
    std::string h = get_host();
    std::string uh = u+"@"+h+":";
    if (line.rfind(uh, 0) == 0) {
        size_t pos_after_uh = uh.size();
        size_t pos_dollar = line.find("$ ", pos_after_uh);
        if (pos_dollar != std::string::npos) {
            std::string cwd = line.substr(pos_after_uh, pos_dollar - pos_after_uh);
            std::string rest = line.substr(pos_dollar + 2);

            int advance = 0;
            // user@host:
            advance += drawTextAdvance(x + advance, y, uh, theme_.green, 0);
            // cwd
            advance += drawTextAdvance(x + advance, y, cwd, theme_.blue, 0);
            // "$ "
            advance += drawTextAdvance(x + advance, y, std::string("$ "), theme_.fg, 0);
            // rest (may include ANSI sequences if from scrollback)
            if (!rest.empty()) {
                if (gridMode) {
                    // Grid mode: measure as terminal cells; avoid natural advance variance
                    advance += drawTextAdvance(x + advance, y, rest, theme_.fg, 0);
                } else {
                    drawAnsiTextWithParsing(x + advance, y, rest);
                }
            }
            return;
        }
    }
    // For non-prompt lines or unrecognized
    if (gridMode) {
        drawTextAdvance(x, y, line, theme_.fg, 0);
    } else {
        drawAnsiTextWithParsing(x, y, line);
    }
}

void TerminalWindow::drawAnsiTextWithParsing(int x, int y, const std::string& text) {
    unsigned long currentFg = theme_.fg;
    unsigned long currentBg = theme_.bg;
    int currentX = x;
    size_t i = 0;
    auto drawChunkNatural = [&](const std::string& chunk){
#ifdef USE_PANGO_CAIRO
        ensureCairoSurface();
        // Convert X11 pixel to RGB components (approximate)
        XColor c; c.pixel = currentFg; XQueryColor(dpy_, cmap_, &c);
        double r = c.red / 65535.0, g = c.green / 65535.0, b = c.blue / 65535.0;
        cairo_set_source_rgb(cr_, r, g, b);
        std::string safe = sanitize_to_valid_utf8(chunk);
    pango_layout_set_text(pangoLayout_, safe.c_str(), (int)safe.size());
    pango_layout_set_font_description(pangoLayout_, pangoFontDesc_);
        int baseline_px = pango_layout_get_baseline(pangoLayout_) / PANGO_SCALE;
        int top_y = y - baseline_px;
        cairo_move_to(cr_, currentX, top_y);
        // optional background fill for the chunk
        PangoRectangle logical; pango_layout_get_pixel_extents(pangoLayout_, nullptr, &logical);
        if (currentBg != theme_.bg) {
            // Approximate bg behind text
            XSetForeground(dpy_, gc_, currentBg);
            XFillRectangle(dpy_, win_, gc_, currentX, y - (pango_layout_get_baseline(pangoLayout_) / PANGO_SCALE), logical.width, pangoAscent_ + pangoDescent_);
            XSetForeground(dpy_, gc_, theme_.fg);
            // Redraw text atop bg
            cairo_move_to(cr_, currentX, y - (pango_layout_get_baseline(pangoLayout_) / PANGO_SCALE));
            pango_cairo_show_layout(cr_, pangoLayout_);
        } else {
            pango_cairo_show_layout(cr_, pangoLayout_);
        }
        // advance by natural pixel width
        currentX += logical.width;
#else
    // Approximate background fill for ANSI bg colors in core X11 path
    if (currentBg != theme_.bg) {
        XSetForeground(dpy_, gc_, currentBg);
        int w = (int)chunk.size() * charWidth();
        int asc = font_ ? font_->ascent : (lineH_ - 4);
        int desc = font_ ? font_->descent : 2;
        XFillRectangle(dpy_, win_, gc_, currentX, y - asc, w, asc + desc);
    }
    XSetForeground(dpy_, gc_, currentFg);
    XDrawString(dpy_, win_, gc_, currentX, y, chunk.c_str(), (int)chunk.size());
    currentX += (int)chunk.size() * charWidth();
#endif
    };
    while (i < text.size()) {
        if (text[i] == '\x1B' && i+1 < text.size() && text[i+1] == '[') {
            // ANSI CSI
            size_t end = i+2;
            while (end < text.size() && text[end] != 'm') end++;
            if (end < text.size()) {
                std::string seq = text.substr(i+2, end - (i+2));
                // Parse SGR
                std::vector<int> codes;
                size_t pos = 0;
                while (pos < seq.size()) {
                    size_t next = seq.find(';', pos);
                    if (next == std::string::npos) next = seq.size();
                    if (pos < next) {
                        codes.push_back(std::stoi(seq.substr(pos, next - pos)));
                    }
                    pos = next + 1;
                }
                for (int code : codes) {
                    if (code == 0) { currentFg = theme_.fg; currentBg = theme_.bg; }
                    else if (code >= 30 && code <= 37) currentFg = ansiColorToPixel(code - 30, true);
                    else if (code >= 40 && code <= 47) currentBg = ansiColorToPixel(code - 40, false);
                    else if (code >= 90 && code <= 97) currentFg = ansiColorToPixel(code - 82, true); // bright
                    else if (code >= 100 && code <= 107) currentBg = ansiColorToPixel(code - 92, false);
                    // Skip 38/48 for now
                }
                (void)currentBg; // silence potential set-but-not-used in analysis
                i = end + 1;
                continue;
            }
        }
        // Find next escape or end
        size_t nextEsc = text.find('\x1B', i);
        if (nextEsc == std::string::npos) nextEsc = text.size();
        std::string chunk = text.substr(i, nextEsc - i);
        // Scrollback/output lines may contain mixed-width scripts; draw with natural spacing to avoid overlaps.
        drawChunkNatural(chunk);
        i = nextEsc;
    }
}

void TerminalWindow::redraw() {
    // Create double buffer to prevent flickering
    Pixmap pixmap = XCreatePixmap(dpy_, win_, width_, height_, DefaultDepth(dpy_, screen_));
    GC pixGC = XCreateGC(dpy_, pixmap, 0, nullptr);
    if (font_) XSetFont(dpy_, pixGC, font_->fid);
    
    // Clear background
    XSetForeground(dpy_, pixGC, theme_.bg);
    XFillRectangle(dpy_, pixmap, pixGC, 0, 0, width_, height_);
    
    // Temporarily switch drawing context to pixmap
    Window oldWin = win_;
    GC oldGC = gc_;
    win_ = pixmap;
    gc_ = pixGC;
    
#ifdef USE_PANGO_CAIRO
    // Recreate Cairo surface for double buffer
    if (cairoSurface_) cairo_surface_destroy(cairoSurface_);
    cairoSurface_ = cairo_xlib_surface_create(dpy_, pixmap, DefaultVisual(dpy_, screen_), width_, height_);
    if (cr_) cairo_destroy(cr_);
    cr_ = cairo_create(cairoSurface_);
#endif
    
    drawTabBar();
    drawTextArea();
    
    // Copy buffer to window
    win_ = oldWin;
    gc_ = oldGC;
    XCopyArea(dpy_, pixmap, win_, gc_, 0, 0, width_, height_, 0, 0);
    
    // Cleanup
    XFreeGC(dpy_, pixGC);
    XFreePixmap(dpy_, pixmap);
    
#ifdef USE_PANGO_CAIRO
    // Restore Cairo surface for window
    if (cairoSurface_) cairo_surface_destroy(cairoSurface_);
    cairoSurface_ = cairo_xlib_surface_create(dpy_, win_, DefaultVisual(dpy_, screen_), width_, height_);
    if (cr_) cairo_destroy(cr_);
    cr_ = cairo_create(cairoSurface_);
#endif
    
    XFlush(dpy_);
}

// Xft helper removed in revert

void TerminalWindow::handleKeyPress(XKeyEvent* e) {
    KeySym ks = XLookupKeysym(e, 0);

    if (e->state & Mod1Mask) { // Alt key combinations
        if (ks >= XK_1 && ks <= XK_9) {
            int tabIdx = ks - XK_1;
            if (tabIdx < (int)tabs_.size()) {
                activeTab_ = tabIdx;
                redraw();
            }
            return;
        }
    }

    // Active tab reference
    Tab& t = *tabs_[activeTab_];

    // Translate keypress to UTF-8 text (for regular input), also update KeySym via XIM when available
    char txt[64] = {0};
    int n = 0;
    {
        KeySym sym = 0;
        if (xic_) {
            Status status;
            n = Xutf8LookupString(xic_, e, txt, (int)sizeof(txt), &sym, &status);
        } else {
            n = XLookupString(e, txt, (int)sizeof(txt), &sym, nullptr);
        }
        if (sym) ks = sym; // prefer the symbol resolved by lookup
    }

    // Scrolling keys
    if (ks == XK_Page_Up)   { t.scrollOffsetTargetLines = t.scrollOffsetLines + 10; redraw(); return; }
    if (ks == XK_Page_Down) { t.scrollOffsetTargetLines = std::max(0, t.scrollOffsetLines - 10); redraw(); return; }
    if ((ks == XK_Home) && (e->state & ControlMask)) { t.scrollOffsetTargetLines = 1000000; redraw(); return; }
    if ((ks == XK_End)  && (e->state & ControlMask)) { t.scrollOffsetTargetLines = 0; redraw(); return; }

    // Ctrl+R: enter inline history search (keep normal input intact)
    if (n==1 && txt[0]==18 && t.childPid <= 0) { // Ctrl+R
        if (!searchActive_) {
            searchActive_ = true;
            searchSavedInput_ = t.input;
            searchSavedCursor_ = t.cursor;
            searchTerm_.clear();
            t.scrollOffsetLines = 0; t.scrollOffsetTargetLines = 0;
            redraw();
            return;
        }
    }

    bool searchActive = (searchActive_ && t.childPid <= 0);

    // Foreground lock: block input except Ctrl+C, Ctrl+Z, and scrolling
    if (t.childPid > 0 && !(n==1 && (txt[0]==3 || txt[0]==26))) return;

    // Ctrl keys
    if (n==1 && txt[0]==1) { t.cursor = 0; redraw(); return; } // Ctrl+A
    if (n==1 && txt[0]==5) { t.cursor = t.input.size(); redraw(); return; } // Ctrl+E
    if (n==1 && txt[0]==20) { // Ctrl+T -> new tab
        newTab(); activeTab_ = (int)tabs_.size()-1; redraw(); return;
    }
    if (n==1 && txt[0]==17) { // Ctrl+Q -> close current tab
        closeTab(activeTab_);
        return;
    }
    if (n==1 && txt[0]==12) { // Ctrl+L -> clear screen
        t.scrollback.clear();
        t.scrollOffsetLines = 0; t.scrollOffsetTargetLines = 0;
        t.ansiState = Tab::ANSI_TEXT; t.ansiSeq.clear();
        redraw(); return;
    }
    if (n==1 && txt[0]==3) { // Ctrl+C -> interrupt foreground job or cancel current input
        if (t.childPgid > 0) {
            killpg(t.childPgid, SIGINT);
        } else {
            // Cancel current input line like typical shells:
            // 1) Preserve the prompt+partial input that was visible
            {
                std::string ps1 = get_user()+"@"+get_host()+":"+get_cwd()+"$ ";
                t.appendOutput(ps1 + t.input + "\n");
            }
            // 2) Print ^C on the next line
            t.appendOutput("^C\n");
            t.input.clear();
            t.cursor = 0;
            t.contActive = false; t.contBuffer.clear(); t.contJoinNoNewline = false;
            t.scrollOffsetLines = 0; t.scrollOffsetTargetLines = 0;
            redraw();
        }
        return;
    }
    if (n==1 && txt[0]==26) { // Ctrl+Z -> detach foreground job (continue running in background, keep printing)
        if (t.childPgid > 0) {
            // Determine if this job is PTY-backed so we can keep the PTY master open beyond UI lifetime
            bool isPty = (t.inFdWrite >= 0 && t.inFdWrite == t.outFd);
            if (isPty && t.outFd >= 0) {
                // Keep fd alive past parent exit without competing reads now
                spawnFdKeeperDelayed(t.outFd);
            }
            // Move to background so we keep draining and printing output continuously
            if (t.outFd >= 0 || t.errFd >= 0) {
                BackgroundJob bj { t.childPid, t.childPgid, t.outFd, t.errFd, "[detached]", isPty };
                t.backgroundJobs.push_back(bj);
            }
            // Mark no active foreground job; clear foreground fds but do NOT close them here
            t.childPid = -1;
            t.childPgid = -1;
            t.outFd = -1;
            t.errFd = -1;
            // Important: clear inFdWrite so starting a new command won't close the detached job's PTY
            // The FD stays alive via bj.outFd (for PTY both read/write refer to same master fd)
            t.inFdWrite = -1;
            runNextCommand(t);
        }
        return;
    }
    // Ctrl+C and Ctrl+D behavior unchanged for shell (no child streaming now)
    if (ks == XK_Return) {
        // If awaiting autocomplete numeric choice
        if (autocompleteChoiceActive_) {
            // Nothing to do on Enter alone, ignore (require numeric selection)
            return;
        }
        if (searchActive) {
            std::string term = searchTerm_;
            // Echo the prompt line as it appeared when search began
            {
                std::string ps1 = get_user()+"@"+get_host()+":"+get_cwd()+"$ ";
                t.appendOutput(ps1 + searchSavedInput_ + "\n");
            }
            // Persist the search prompt line into scrollback
            t.appendOutput(std::string("Enter search term: ") + term + "\n");
            // Perform search and print results (exact match first, then best substring matches)
            if (!term.empty()) {
                const auto& dq = history_.data();
                int exactIdx = -1;
                for (int i=(int)dq.size()-1;i>=0;--i){ if(dq[i]==term){ exactIdx=i; break; }}
                std::vector<std::string> results;
                if (exactIdx>=0) results.push_back(dq[exactIdx]);
                auto matches = history_.bestSubstringMatches(term);
                for (const auto& s : matches) {
                    if (results.size()>=20) break; // cap output
                    if (results.empty() || s != results[0]) results.push_back(s);
                }
                if (!results.empty()) {
                    for (const auto& s : results) t.appendOutput(s + "\n");
                } else {
                    t.appendOutput("No match for search term in history\n");
                }
            } else {
                t.appendOutput("No match for search term in history\n");
            }
            // Restore prior input state and exit search
            searchActive_ = false; searchTerm_.clear();
            t.input = searchSavedInput_; t.cursor = searchSavedCursor_;
            // Snap view to bottom so results are visible
            t.scrollOffsetLines = 0; t.scrollOffsetTargetLines = 0;
            redraw();
            return;
        } else {
            submitInputLine(t); return;
        }
    }

    // (reverted) Up/Down multiline navigation within input removed

    // ESC cancels search and restores input without output
    if (ks == XK_Escape && searchActive) {
        searchActive_ = false;
        searchTerm_.clear();
        t.input = searchSavedInput_; t.cursor = searchSavedCursor_;
        redraw(); return;
    }
    if (ks == XK_Escape && autocompleteChoiceActive_) {
        // Cancel choice prompt
        if (acScrollbackMark_ != (size_t)-1 && acScrollbackMark_ <= t.scrollback.size()) {
            t.scrollback.erase(acScrollbackMark_);
        }
        autocompleteChoiceActive_ = false;
        autocompleteChoices_.clear();
        acScrollbackMark_ = (size_t)-1;
        redraw(); return;
    }

    // Paste shortcuts: Ctrl+V, Shift+Insert
    if ((ks == XK_v || ks == XK_V) && (e->state & ControlMask)) { requestPaste(clipboardAtom_ ? clipboardAtom_ : XA_PRIMARY); return; }
    if (ks == XK_Insert && (e->state & ShiftMask)) { requestPaste(clipboardAtom_ ? clipboardAtom_ : XA_PRIMARY); return; }
    if (ks == XK_BackSpace) {
        if (searchActive) {
            if (!searchTerm_.empty()) searchTerm_.pop_back();
            t.scrollOffsetLines = 0; t.scrollOffsetTargetLines = 0;
            redraw(); return;
        } else {
            if (t.cursor>0) { t.input.erase(t.input.begin()+t.cursor-1); t.cursor--; }
            redraw(); return;
        }
    }
    if (ks == XK_Left)  { if (!searchActive && !autocompleteChoiceActive_ && t.cursor>0) t.cursor--; redraw(); return; }
    if (ks == XK_Right) { if (!searchActive && !autocompleteChoiceActive_ && t.cursor<t.input.size()) t.cursor++; redraw(); return; }
    if (ks == XK_Home)  { if (!searchActive) t.cursor=0; redraw(); return; }
    if (ks == XK_End)   { if (!searchActive) t.cursor=t.input.size(); redraw(); return; }

    // Tab for filename autocomplete (only when no child running and not in search)
    if (ks == XK_Tab && t.childPid <= 0 && !searchActive) {
        autocomplete(t);
        return;
    }

    // Regular text
    if (n>0) {
        // If we're in autocomplete numeric choice mode, accept digits 1..N
        if (autocompleteChoiceActive_) {
            for (int i=0;i<n;i++) {
                char c = txt[i];
                if (c>='1' && c<='9') {
                    int idx = (c - '1');
                    if (idx >= 0 && idx < (int)autocompleteChoices_.size()) {
                        std::string choice = autocompleteChoices_[idx];
                        // Append trailing slash if the choice is a directory
                        auto is_dir_choice = [&](const std::string& nm) -> bool {
                            std::string base = ".";
                            // If acDirPrefix_ points to an absolute path or relative dir, resolve it from current input token
                            if (!acDirPrefix_.empty()) {
                                // Build full dir path from prefix
                                if (acDirPrefix_[0] == '/') base = acDirPrefix_;
                                else base = acDirPrefix_;
                                // Remove trailing slash to avoid doubling
                                if (!base.empty() && base.back() == '/') base.pop_back();
                            }
                            struct stat st{};
                            std::string path = base;
                            if (path.empty()) path = ".";
                            if (path != "/") path += "/";
                            path += nm;
                            if (stat(path.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
                            return false;
                        };
                        if (is_dir_choice(choice)) choice += "/";
                        // Replace token range
                        std::string before = t.input.substr(0, acReplaceStart_);
                        std::string after  = t.input.substr(acReplaceEnd_);
                        t.input = before + acDirPrefix_ + choice + after;
                        t.cursor = (before + acDirPrefix_ + choice).size();
                        if (acScrollbackMark_ != (size_t)-1 && acScrollbackMark_ <= t.scrollback.size()) {
                            t.scrollback.erase(acScrollbackMark_);
                        }
                        autocompleteChoiceActive_ = false; autocompleteChoices_.clear(); acScrollbackMark_ = (size_t)-1;
                        redraw();
                        return;
                    }
                }
            }
            // Ignore other text while prompting
            return;
        }
        if (searchActive) {
            for (int i=0;i<n;i++) searchTerm_.push_back(txt[i]);
            t.scrollOffsetLines = 0; t.scrollOffsetTargetLines = 0;
            redraw();
        } else {
            for (int i=0;i<n;i++) { t.input.insert(t.input.begin()+t.cursor, txt[i]); t.cursor++; }
            t.scrollOffsetLines = 0; t.scrollOffsetTargetLines = 0; // editing snaps back to bottom like terminals
            redraw();
        }
    }
}

// Compute autocomplete candidates and update input or show prompt to choose
void TerminalWindow::autocomplete(Tab& t) {
    // Identify the token at the cursor (from last space to cursor)
    size_t start = t.input.rfind(' ', t.cursor == 0 ? 0 : t.cursor - 1);
    if (start == std::string::npos) start = 0; else start += 1;
    size_t end = t.cursor;
    std::string token = t.input.substr(start, end - start);
    acReplaceStart_ = start; acReplaceEnd_ = end;
    // Determine directory part and prefix
    std::string dir = ".";
    std::string prefix = token;
    acDirPrefix_.clear();
    size_t slash = token.rfind('/');
    if (slash != std::string::npos) {
        std::string dirpart = token.substr(0, slash);
        if (dirpart.empty()) dir = "/"; else dir = dirpart;
        prefix = token.substr(slash + 1);
        acDirPrefix_ = token.substr(0, slash + 1);
    }

    // Read directory entries
    std::vector<std::string> matches;
    DIR* d = opendir(dir.c_str());
    if (d) {
        struct dirent* ent;
        while ((ent = readdir(d)) != nullptr) {
            std::string name = ent->d_name;
            if (name == "." || name == "..") continue;
            if (prefix.empty() || name.rfind(prefix, 0) == 0) {
                matches.push_back(name);
            }
        }
        closedir(d);
    }
    // Helper to check if a candidate is a directory on disk
    auto is_dir = [&](const std::string& nm) -> bool {
        std::string base = dir.empty() ? std::string(".") : dir;
        std::string path = base;
        if (path != "/") path += "/";
        path += nm;
        struct stat st{};
        if (stat(path.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
        return false;
    };
    if (matches.empty()) {
        // nothing to do
        return;
    }
    if (matches.size() == 1) {
        // Single: complete fully
        std::string before = t.input.substr(0, acReplaceStart_);
        std::string after  = t.input.substr(acReplaceEnd_);
        bool dirp = is_dir(matches[0]);
        std::string insert = acDirPrefix_ + matches[0] + (dirp ? "/" : "");
        t.input = before + insert + after;
        t.cursor = (before + insert).size();
        redraw();
        return;
    }
    // Multiple: compute longest common prefix among matches relative to current prefix
    auto lcp = [&](const std::vector<std::string>& v){
        if (v.empty()) return std::string();
        std::string pref = v[0];
        for (size_t i=1;i<v.size();++i) {
            size_t k=0; while (k<pref.size() && k<v[i].size() && pref[k]==v[i][k]) k++;
            pref.resize(k);
            if (pref.empty()) break;
        }
        return pref;
    };
    std::string common = lcp(matches);
    if (common.size() > prefix.size()) {
        // Expand to common prefix and stop
        std::string before = t.input.substr(0, acReplaceStart_);
        std::string after  = t.input.substr(acReplaceEnd_);
        t.input = before + acDirPrefix_ + common + after;
        t.cursor = (before + acDirPrefix_ + common).size();
        redraw();
        return;
    }
    // Still ambiguous: present numbered choices
    autocompleteChoiceActive_ = true;
    autocompleteChoices_ = matches;
    // Echo current prompt+input to scrollback so choices appear after it
    {
        std::string ps1 = get_user()+"@"+get_host()+":"+get_cwd()+"$ ";
        size_t mark = t.scrollback.size();
        t.appendOutput(ps1 + t.input + "\n");
        acScrollbackMark_ = mark;
    }
    t.appendOutput("Select a file: ");
    for (size_t i=0;i<matches.size(); ++i) {
        bool dirp = is_dir(matches[i]);
        std::string disp = matches[i] + (dirp?"/":"");
        t.appendOutput(std::to_string(i+1) + ". " + disp + (i+1<matches.size()?" ":"\n"));
    }
    t.scrollOffsetLines = 0; t.scrollOffsetTargetLines = 0;
    redraw();
}

void TerminalWindow::handleButton(XButtonEvent* e) {
    // Tab bar interactions first (includes new tab button)
    int tabH = 26;
    int tabW = 140;
    int tabSpacing = 4;
    int closeW = 16;
    if (e->y>=6 && e->y<=6+tabH) {
        int xPlus = 8 + (int)tabs_.size()*(tabW + tabSpacing);
        if (e->button==Button1 && e->x>=xPlus && e->x<=xPlus+28) { newTab(); activeTab_ = (int)tabs_.size()-1; redraw(); return; }
        // Check tabs hit test precisely
        for (int i=0;i<(int)tabs_.size();++i) {
            int xStart = 8 + i*(tabW + tabSpacing);
            if (e->x>=xStart && e->x<=xStart+tabW) {
                if (e->button==Button1) {
                    int closeX = xStart + tabW - closeW - 4;
                    int closeY = 6 + (tabH - 16)/2;
                    if (e->x >= closeX && e->x <= closeX + closeW && e->y >= closeY && e->y <= closeY + 16) {
                        closeTab(i);
                    } else {
                        activeTab_=i; redraw();
                    }
                }
                return;
            }
        }
        return; // clicks in tab bar but not on elements: ignore
    }
    // Scrolling via mouse wheel
    Tab& t = *tabs_[activeTab_];
    // Middle-click paste from PRIMARY selection
    if (e->button == Button2) { requestPaste(XA_PRIMARY); return; }
    if (e->button == Button4) { t.scrollOffsetTargetLines = t.scrollOffsetLines + 3; redraw(); return; }
    if (e->button == Button5) { t.scrollOffsetTargetLines = std::max(0, t.scrollOffsetLines - 3); redraw(); return; }
    // Scrollbar interactions
    const int sbW = 12; int trackX = width_ - sbW - 2; int trackTop = 40; int trackH = height_ - 40 - lineH_;
    if (e->button == Button1 && e->x >= trackX) {
        // Rebuild lines
        size_t start=0; std::vector<std::string> lines;
        for (size_t i=0;i<t.scrollback.size();++i) if (t.scrollback[i]=='\n') { lines.emplace_back(t.scrollback.substr(start,i-start)); start=i+1; }
        if (start<t.scrollback.size()) lines.emplace_back(t.scrollback.substr(start));
        int total = (int)lines.size();
        int viewportLines = std::max(1,(height_ - 40 - 2*lineH_)/lineH_);
        int bottomStart = std::max(0,(int)lines.size()-viewportLines);
        int begin = std::max(0, bottomStart - std::max(0, t.scrollOffsetLines));

        // Determine current thumb geometry
        double thumbHpx = std::max(20.0, (double)trackH * (double)viewportLines / std::max(1,total));
        int maxBegin = std::max(0, total - viewportLines);
        double frac = maxBegin ? (double)begin / (double)maxBegin : 0.0;
        double trackMovable = (double)trackH - thumbHpx;
        int thumbY = trackTop + (int)(frac * std::max(0.0, trackMovable));
        int thumbBottom = thumbY + (int)thumbHpx;

        // If click is inside thumb: begin drag. Else: page jump towards click position.
        if (e->y >= thumbY && e->y <= thumbBottom) {
            draggingScrollbar_ = true;
            dragStartY_ = e->y;
            dragStartBeginLine_ = begin;
            return;
        } else {
            // Map click position to target begin line proportionally
            double clickFrac = 0.0;
            if (trackH > thumbHpx) clickFrac = std::clamp((double)(e->y - trackTop) / (double)(trackH - thumbHpx), 0.0, 1.0);
            int targetBegin = (int)(clickFrac * maxBegin);
            int targetOffsetFromBottom = std::max(0, bottomStart - targetBegin);
            t.scrollOffsetTargetLines = targetOffsetFromBottom;
            redraw();
            return;
        }
    }
    // Right-click on track: page up/down by a viewport
    if (e->button == Button3 && e->x >= trackX) {
        // Page up/down by one viewport relative to current thumb position
        int viewportLines = std::max(1,(height_ - 40 - 2*lineH_)/lineH_);
        int thumbY = lastThumbY_;
        if (e->y < thumbY) {
            t.scrollOffsetTargetLines = t.scrollOffsetLines + viewportLines;
        } else {
            t.scrollOffsetTargetLines = std::max(0, t.scrollOffsetLines - viewportLines);
        }
        redraw();
        return;
    }
}

void TerminalWindow::handleMotion(XMotionEvent* e) {
    Tab& t = *tabs_[activeTab_];
    // Tab hover
    int tabW=120; int tabH=26;
    if (e->y>=6 && e->y<=6+tabH) {
        int xPlus = 8 + (int)tabs_.size()*(tabW+8);
        bool hn = (e->x>=xPlus && e->x<=xPlus+28);
        int hi = -1;
        if (!hn) {
            for (int i=0;i<(int)tabs_.size();++i) { int xStart=8+i*(tabW+8); if (e->x>=xStart && e->x<=xStart+tabW) { hi=i; break; } }
        }
        if (hoverNewTab_!=hn || hoverTabIndex_!=hi) { hoverNewTab_=hn; hoverTabIndex_=hi; redraw(); }
    } else {
        if (hoverNewTab_||hoverTabIndex_!=-1) { hoverNewTab_=false; hoverTabIndex_=-1; redraw(); }
    }
    // Hover detection for scrollbar thumb
    const int sbW = 12; int x = width_ - sbW - 2; int trackTop = 40; int trackH = height_ - 40 - lineH_;
    bool overScroll = (e->x >= x && e->y >= trackTop && e->y <= trackTop+trackH);
    bool overThumb = overScroll && lastThumbY_>=0 && e->y>=lastThumbY_ && e->y<=lastThumbY_+lastThumbH_;
    if (hoverScrollbarThumb_ != overThumb) { hoverScrollbarThumb_ = overThumb; redraw(); }

    if (!draggingScrollbar_) return;
    // reconstruct lines
    size_t start=0; std::vector<std::string> lines;
    for (size_t i=0;i<t.scrollback.size();++i) if (t.scrollback[i]=='\n') { lines.emplace_back(t.scrollback.substr(start,i-start)); start=i+1; }
    if (start<t.scrollback.size()) lines.emplace_back(t.scrollback.substr(start));
    int total = (int)lines.size();
    int viewportLines = std::max(1,(height_ - 40 - 2*lineH_)/lineH_);
    int dy = e->y - dragStartY_;
    double thumbHpx = std::max(20.0, (double)trackH * (double)viewportLines / std::max(1,total));
    double trackMovable = (double)trackH - thumbHpx;
    double frac = 0.0; if (trackMovable>1) frac = (double)dy / trackMovable;
    int maxBegin = std::max(0, total - viewportLines);
    int begin = dragStartBeginLine_ + (int)(frac * maxBegin);
    begin = std::clamp(begin, 0, maxBegin);
    int bottomStart = std::max(0, total - viewportLines);
    t.scrollOffsetTargetLines = std::max(0, bottomStart - begin);
    redraw();
}

void TerminalWindow::handleButtonRelease(XButtonEvent*) {
    draggingScrollbar_ = false;
}

void TerminalWindow::newTab() {
    tabs_.emplace_back(std::make_unique<Tab>());
}

void TerminalWindow::closeTab(int index) {
    if (index >= 0 && index < (int)tabs_.size()) {
        bool closingLast = (tabs_.size() == 1);
        if (closingLast) {
            Tab& t = *tabs_[index];
            // For a running foreground PTY-backed job, preserve PTY master past UI exit
            if (t.childPid > 0 && t.outFd >= 0 && t.inFdWrite >= 0 && t.inFdWrite == t.outFd) {
                spawnFdKeeperDelayed(t.outFd);
            }
            // For background PTY-backed jobs, also preserve
            for (auto &bj : t.backgroundJobs) {
                if (bj.isPty && bj.outFd >= 0) {
                    spawnFdKeeperDelayed(bj.outFd);
                }
            }
        }
        tabs_.erase(tabs_.begin() + index);
        if (activeTab_ > index) {
            activeTab_--;
        } else if (activeTab_ == index) {
            activeTab_ = std::max(0, index - 1);
        }
        if (tabs_.empty()) { exit(0); }
        redraw();
    }
}

void TerminalWindow::run() {
    initHistory();
    initX11();

    int x11fd = ConnectionNumber(dpy_);
    struct timeval tv;
    // initialize blink timestamp
    timespec ts{}; clock_gettime(CLOCK_MONOTONIC, &ts);
    lastBlinkMs_ = (unsigned long long)ts.tv_sec*1000ull + ts.tv_nsec/1000000ull;
    while (true) {
        fd_set rfds; FD_ZERO(&rfds); FD_SET(x11fd, &rfds);
        int maxfd = x11fd;
        // add child pipes for active tab (non-blocking read)
        if (!tabs_.empty()) {
            Tab& t = *tabs_[activeTab_];
            if (t.outFd>=0) { FD_SET(t.outFd, &rfds); if (t.outFd>maxfd) maxfd=t.outFd; }
            if (t.errFd>=0) { FD_SET(t.errFd, &rfds); if (t.errFd>maxfd) maxfd=t.errFd; }
            // add background jobs
            for (auto& bj : t.backgroundJobs) {
                if (bj.outFd>=0) { FD_SET(bj.outFd, &rfds); if (bj.outFd>maxfd) maxfd=bj.outFd; }
                if (bj.errFd>=0) { FD_SET(bj.errFd, &rfds); if (bj.errFd>maxfd) maxfd=bj.errFd; }
            }
        }
        tv.tv_sec = 0; tv.tv_usec = tickMs_ * 1000; // ~60fps
        int r = select(maxfd+1, &rfds, nullptr, nullptr, &tv);
        // compute elapsed time for blinking regardless of select wake reason
        clock_gettime(CLOCK_MONOTONIC, &ts);
        unsigned long long nowMs = (unsigned long long)ts.tv_sec*1000ull + ts.tv_nsec/1000000ull;
        unsigned long long elapsed = nowMs - lastBlinkMs_;
        if (elapsed >= (unsigned long long)tickMs_) {
            lastBlinkMs_ = nowMs;
            blinkCountdownMs_ -= (int)elapsed;
            if (blinkCountdownMs_ <= 0) { cursorOn_ = !cursorOn_; blinkCountdownMs_ = blinkMs_; redraw(); }
        }
        if (r>0) {
            // Check child pipes
            if (!tabs_.empty()) {
                Tab& t = *tabs_[activeTab_];
                if (t.outFd>=0 && FD_ISSET(t.outFd, &rfds)) pumpChildOutput();
                if (t.errFd>=0 && FD_ISSET(t.errFd, &rfds)) pumpChildOutput();
                // Check background jobs
                for (auto& bj : t.backgroundJobs) {
                    if (bj.outFd>=0 && FD_ISSET(bj.outFd, &rfds)) drainBackgroundJobs();
                    if (bj.errFd>=0 && FD_ISSET(bj.errFd, &rfds)) drainBackgroundJobs();
                }
            }
        }
        // Even if no FDs were readable, poll child exit occasionally to avoid being stuck when
        // the child closes pipes earlier or produces no output (no more FD events).
        if (!tabs_.empty()) {
            Tab& t = *tabs_[activeTab_];
            if (t.childPid > 0) pumpChildOutput();
            drainBackgroundJobs();
        }
        // Smooth scrolling: if any tab is animating, redraw
        bool anim = false;
        for (auto& pt : tabs_) {
            if (pt->scrollOffsetLines != pt->scrollOffsetTargetLines) { anim = true; break; }
        }
        if (anim) redraw();
        while (XPending(dpy_)) {
            XEvent ev; XNextEvent(dpy_, &ev);
            switch (ev.type) {
                case Expose: redraw(); break;
                case KeyPress: cursorOn_ = true; blinkCountdownMs_ = blinkMs_; handleKeyPress(&ev.xkey); break;
                case ButtonPress:
                    if (ev.xbutton.button == 4 || ev.xbutton.button == 5) { // Scroll wheel
                        if (activeTab_ >= 0) {
                            Tab& t = *tabs_[activeTab_];
                            int direction = (ev.xbutton.button == 4) ? 1 : -1;
                            int scrollAmount = 3; // lines
                            t.scrollOffsetTargetLines = std::max(0, t.scrollOffsetLines + direction * scrollAmount);
                            redraw();
                        }
                    } else {
                        handleButton(&ev.xbutton);
                    }
                    break;
                case ButtonRelease: handleButtonRelease(&ev.xbutton); break;
                case MotionNotify: handleMotion(&ev.xmotion); break;
                case ConfigureNotify: width_=ev.xconfigure.width; height_=ev.xconfigure.height; redraw(); break;
                case FocusIn: focused_ = true; cursorOn_ = true; blinkCountdownMs_ = blinkMs_; redraw(); break;
                case FocusOut: focused_ = false; cursorOn_ = false; redraw(); break;
                case SelectionNotify: handleSelectionNotify(&ev.xselection); break;
            }
        }
    }
}
void TerminalWindow::drawScrollBar(int totalLines, int viewportLines, int beginLine) {
    if (totalLines <= viewportLines) return; // no scrollbar needed
    const int sbW = 12;
    int x = width_ - sbW - 2;
    int trackTop = 40;
    int trackH = height_ - 40 - lineH_;
    if (trackH <= 0) return;

    // track
    XSetForeground(dpy_, gc_, theme_.scrollTrack);
    XFillRectangle(dpy_, win_, gc_, x, trackTop, sbW, trackH);

    // thumb size/position
    double thumbHpx = std::max(20.0, (double)trackH * (double)viewportLines / std::max(1,totalLines));
    int maxBegin = std::max(0, totalLines - viewportLines);
    double frac = maxBegin ? (double)beginLine / (double)maxBegin : 0.0;
    double trackMovable = (double)trackH - thumbHpx;
    int thumbY = trackTop + (int)(frac * std::max(0.0, trackMovable));

    // cache thumb geometry for hover
    lastThumbY_ = thumbY; lastThumbH_ = (int)thumbHpx;

    XSetForeground(dpy_, gc_, hoverScrollbarThumb_ && theme_.scrollThumbHover ? theme_.scrollThumbHover : theme_.scrollThumb);
    XFillRectangle(dpy_, win_, gc_, x, thumbY, sbW, (unsigned int)thumbHpx);
    // border
    XSetForeground(dpy_, gc_, theme_.gray);
    XDrawRectangle(dpy_, win_, gc_, x, trackTop, sbW, trackH);
}



void TerminalWindow::requestPaste(Atom selection) {
    if (!dpy_) return;
    Atom target = utf8Atom_ ? utf8Atom_ : XA_STRING;
    if (!pasteProperty_) pasteProperty_ = XInternAtom(dpy_, "MYTERM_PASTE", False);
    XConvertSelection(dpy_, selection, target, pasteProperty_, win_, CurrentTime);
}

void TerminalWindow::handleSelectionNotify(XSelectionEvent* e) {
    if (!e || e->property == None) return; // conversion failed
    Atom actual_type = None; int actual_format = 0; unsigned long nitems = 0, bytes_after = 0; unsigned char* prop = nullptr;
    std::string result;
    do {
        if (prop) { XFree(prop); prop = nullptr; }
        if (XGetWindowProperty(dpy_, win_, e->property, 0, 1024, True, AnyPropertyType, &actual_type, &actual_format, &nitems, &bytes_after, &prop) != Success) {
            break;
        }
        if (actual_format == 8 && prop && nitems>0) {
            result.append(reinterpret_cast<char*>(prop), reinterpret_cast<char*>(prop) + nitems);
        }
    } while (bytes_after > 0);
    if (prop) XFree(prop);
    if (!result.empty()) handlePaste(result);
}

static std::string normalize_paste_text(const std::string& in) {
    std::string out; out.reserve(in.size());
    for (size_t i=0;i<in.size();++i) {
        unsigned char c = (unsigned char)in[i];
        if (c=='\r') {
            // Convert CR or CRLF to single \n; skip if next is \n to avoid doubling
            if (!(i+1<in.size() && (unsigned char)in[i+1]=='\n')) out.push_back('\n');
            continue;
        }
        if (c=='\n' || c=='\t' || c>=0x20) { // allow printable and UTF-8 bytes
            if (c==0x7F) continue; // drop DEL
            out.push_back((char)c);
        }
        // drop other control characters (NUL, BEL, etc.)
    }
    return out;
}

// Return true when the string has balanced single and double quotes.
static bool quotes_balanced_simple(const std::string& s) {
    bool inS = false, inD = false;
    for (char c : s) {
        if (c=='"' && !inS) inD = !inD; else if (c=='\'' && !inD) inS = !inS;
    }
    return !inS && !inD;
}

// Split by newlines that are NOT inside quotes. Preserves newlines that are
// within quotes (so a multi-line quoted string stays a single command).
static std::vector<std::string> split_lines_respecting_quotes(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    bool inS=false, inD=false;
    for (char c : s) {
        if (c=='"' && !inS) { inD = !inD; cur.push_back(c); continue; }
        if (c=='\'' && !inD) { inS = !inS; cur.push_back(c); continue; }
        if (c=='\n' && !inS && !inD) {
            // newline as separator
            // trim whitespace-only cur
            bool onlyWs = true; for (char k: cur) { if (!isspace((unsigned char)k)) { onlyWs=false; break; } }
            if (!cur.empty() && !onlyWs) out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    bool onlyWs = true; for (char k: cur) { if (!isspace((unsigned char)k)) { onlyWs=false; break; } }
    if (!cur.empty() && !onlyWs) out.push_back(cur);
    return out;
}

void TerminalWindow::handlePaste(const std::string& text) {
    if (tabs_.empty()) return;
    Tab& t = *tabs_[activeTab_];
    std::string cleaned = normalize_paste_text(text);
    if (cleaned.empty()) return;

    // Bracketed paste semantics: insert entire block into input at cursor; do NOT submit.
    std::string before = t.input.substr(0, t.cursor);
    std::string after = (t.cursor < t.input.size()) ? t.input.substr(t.cursor) : std::string();
    t.input = before + cleaned + after;
    t.cursor = before.size() + cleaned.size();
    // Snap to bottom and make caret visible immediately after paste
    t.scrollOffsetLines = 0; t.scrollOffsetTargetLines = 0;
    cursorOn_ = true; blinkCountdownMs_ = blinkMs_;
    redraw();
}

void TerminalWindow::submitInputLine(Tab& t, bool triggerRedraw) {
    // Continuation mode for unterminated quotes OR backslash-newline joins
    auto ends_with_backslash = [](const std::string& s){ return !s.empty() && s.back()=='\\'; };
    if (!t.contActive) {
        if (!t.input.empty() && !isWhitespaceOnly(t.input)) {
            bool unbalanced = !quotes_balanced_simple(t.input);
            bool bscont = ends_with_backslash(t.input);
            if (unbalanced || bscont) {
                t.contActive = true;
                std::string typed = t.input; // keep exactly what the user saw (including trailing '\\' if present)
                if (bscont) {
                    // Strip trailing backslash in backend buffer (join w/o newline to next part)
                    t.contBuffer = typed.substr(0, typed.size()-1);
                    t.contJoinNoNewline = true;
                } else {
                    t.contBuffer = typed;
                    t.contJoinNoNewline = false;
                }
                t.input.clear(); t.cursor = 0;
                // Echo PS1 + first part
                std::string ps1 = get_user()+"@"+get_host()+":"+get_cwd()+"$ ";
                // Echo without the trailing backslash (continuation marker shouldn't appear in transcript)
                std::string visible = bscont && !typed.empty() ? typed.substr(0, typed.size()-1) : typed;
                t.appendOutput(ps1 + visible + "\n");
                if (triggerRedraw) redraw();
                return;
            }
        }
    } else {
        // Already in continuation: append current input and test again
        std::string add = t.input;
        bool bscont = ends_with_backslash(add);
        std::string addVisible = bscont && !add.empty() ? add.substr(0, add.size()-1) : add;
        // If previous line ended with backslash, join without inserting a newline
        if (t.contJoinNoNewline) {
            t.contBuffer += addVisible;
        } else {
            t.contBuffer += "\n" + addVisible;
        }
        // Commit the PS2 line so it stays visible in transcript, without the trailing backslash
        t.appendOutput(std::string("> ") + addVisible + "\n");
        // Determine whether we remain in continuation
        t.input.clear(); t.cursor = 0;
        // If the line we just added ends with a backslash, set join-without-newline for the next part
        if (bscont) {
            t.contJoinNoNewline = true;
        } else {
            t.contJoinNoNewline = false;
        }
        bool unbalanced = !quotes_balanced_simple(t.contBuffer);
        if (unbalanced || bscont) {
            if (triggerRedraw) redraw();
            return;
        }
        // fallthrough to execution with contBuffer
    }

    // Build command list: bracketed paste can include multiple commands separated by newlines
    std::vector<std::string> cmds;
    if (t.contActive) {
        // Single logical command containing embedded newlines
        cmds.push_back(t.contBuffer);
    } else if (!t.input.empty() && !isWhitespaceOnly(t.input)) {
        cmds = split_lines_respecting_quotes(t.input);
    }

    if (!cmds.empty()) {
        // For continuation, we've already echoed PS1 first line and each PS2 line as user hit Enter.
        // For non-continuation (e.g., bracketed paste), echo once now.
        if (!t.contActive) {
            std::string ps1 = get_user()+"@"+get_host()+":"+get_cwd()+"$ ";
            t.appendOutput(ps1 + cmds[0] + "\n");
            for (size_t i=1;i<cmds.size(); ++i) t.appendOutput(std::string("> ") + cmds[i] + "\n");
        }
        // Queue commands without echoing again (respect semicolons outside quotes)
        auto split_by_semicolon = [](const std::string& s){
            std::vector<std::string> out; std::string cur; bool inS=false, inD=false;
            for (size_t i=0;i<s.size();++i){ char c=s[i];
                if (c=='"' && !inS) inD=!inD; else if (c=='\'' && !inD) inS=!inS;
                else if (!inS && !inD && c==';') { if (!cur.empty()) { out.push_back(cur); cur.clear(); } continue; }
                cur.push_back(c);
            }
            if (!cur.empty()) out.push_back(cur);
            return out;
        };
        for (auto& c : cmds) {
            auto pieces = split_by_semicolon(c);
            if (pieces.empty()) continue;
            for (auto& p : pieces) {
                if (!isWhitespaceOnly(p)) t.pendingCmds.emplace_back(p, false);
            }
            // Keep history as the original line (like shells do)
            addHistoryEntry(c);
        }
        runNextCommand(t);
    }
    // reset state for next prompt
    t.scrollOffsetLines = 0; t.scrollOffsetTargetLines = 0;
    t.input.clear(); t.cursor=0;
    if (t.contActive) { t.contActive=false; t.contBuffer.clear(); }
    t.contJoinNoNewline = false;
    if (triggerRedraw) redraw();
}

void TerminalWindow::runNextCommand(Tab& t) {
    if (t.childPid>0) return; // busy
    if (t.pendingCmds.empty()) return;
    auto pair = t.pendingCmds.front();
    std::string line = pair.first; bool echo = pair.second;
    t.pendingCmds.pop_front();
    executeLineInternal(line, echo);
}

bool TerminalWindow::executeSingleCommand(Tab& t, const std::string& line, bool echoPromptAndCmd) {
    if (line.empty() || isWhitespaceOnly(line)) return true;
    t.pendingCmds.emplace_back(line, echoPromptAndCmd);
    runNextCommand(t);
    return true;
}

} // namespace myterm
