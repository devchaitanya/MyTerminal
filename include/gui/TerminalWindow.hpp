#pragma once
#include <X11/Xlib.h>
#ifdef USE_PANGO_CAIRO
#include <cairo/cairo-xlib.h>
#include <pango/pangocairo.h>
#endif
#include <string>
#include <vector>
#include <memory>
#include "core/History.hpp"

namespace myterm {

class Tab;

class TerminalWindow {
public:
    TerminalWindow(int width = 1000, int height = 700);
    ~TerminalWindow();

    // Non-copyable
    TerminalWindow(const TerminalWindow&) = delete;
    TerminalWindow& operator=(const TerminalWindow&) = delete;

    void run(); // event loop

    // For future: API to add tabs, etc.
    void newTab();
    void closeTab(int index);

private:
    void initX11();
    void allocateColors();
    void selectFont();
    void initHistory();
    void addHistoryEntry(const std::string& cmd);
    void redraw();
    void drawTabBar();
    void drawTextArea();
    void drawScrollBar(int totalLines, int viewportLines, int beginLine);
    void handleKeyPress(XKeyEvent* e);
    void handleButton(XButtonEvent* e);
    void handleMotion(XMotionEvent* e);
    void handleButtonRelease(XButtonEvent* e);
    void drawColoredPromptLine(int x, int y, const std::string& line);
    void drawMaybeColoredPromptLine(int x, int y, const std::string& line, bool gridMode=false);
    void drawAnsiTextWithParsing(int x, int y, const std::string& text);
    // Clipboard / paste
    void requestPaste(Atom selection);
    void handleSelectionNotify(XSelectionEvent* e);
    void handlePaste(const std::string& text);
    // Input helpers
    void submitInputLine(Tab& t, bool triggerRedraw = true);
    void runNextCommand(Tab& t);
    bool executeSingleCommand(Tab& t, const std::string& line, bool echoPromptAndCmd);
    // Autocomplete helpers
    void autocomplete(Tab& t);
    // Command execution
    void executeLine(const std::string& line);
    void executeLineInternal(const std::string& line, bool echoPromptAndCmd);
    void printPromptForCurrentTab(bool continuation);
    void spawnProcess(const std::vector<std::string>& argv);
    void pumpChildOutput();
    void drainBackgroundJobs();
    static std::vector<std::string> splitArgs(const std::string& s);
    static bool isWhitespaceOnly(const std::string& s);
    std::string sanitizeAndApplyANSI(struct Tab& t, const char* data, size_t n);

    // ANSI color helpers
    unsigned long ansiColorToPixel(int code, bool fg) const;
    void drawAnsiText(int x, int y, const std::string& text, unsigned long fgColor = 0, unsigned long bgColor = 0);
    int drawTextAdvance(int x, int y, const std::string& text, unsigned long fgColor = 0, unsigned long bgColor = 0);
    int measureTextWidth(const std::string& text);

#ifdef USE_PANGO_CAIRO
    void ensureCairoSurface();
    void destroyCairoObjects();
    void drawTextPango(int x, int y, const std::string& utf8, unsigned long fgPixel);
    int measureTextPango(const std::string& utf8);
#endif

    // Helpers
    int charWidth() const;
    // Persistent history
    History history_{};
    std::string historyPath_{};

    // Inline search (Ctrl+R) state: keep input intact, capture term separately
    bool searchActive_ = false;
    std::string searchTerm_{};
    size_t searchSavedCursor_ = 0;
    std::string searchSavedInput_{};

    // Autocomplete choice prompt state
    bool autocompleteChoiceActive_ = false;
    std::vector<std::string> autocompleteChoices_{};
    size_t acReplaceStart_ = 0;
    size_t acReplaceEnd_ = 0;
    std::string acDirPrefix_{}; // token directory prefix to keep when replacing
    size_t acScrollbackMark_ = (size_t)-1; // start offset in scrollback to erase choices

    Display* dpy_ = nullptr;
    int screen_ = 0;
    Window win_{};
    GC gc_{};
    XFontStruct* font_ = nullptr;
    Colormap cmap_{};

    // XIM/XIC for proper UTF-8 keyboard input
    XIM xim_ = nullptr;
    XIC xic_ = nullptr;

#ifdef USE_PANGO_CAIRO
    // Pango/Cairo for Unicode text rendering
    cairo_surface_t* cairoSurface_ = nullptr;
    cairo_t* cr_ = nullptr;
    PangoLayout* pangoLayout_ = nullptr;
    PangoFontDescription* pangoFontDesc_ = nullptr;
    int cairoW_ = 0, cairoH_ = 0; // cached surface size
    int cellW_ = 8; // width of one cell measured via Pango (monospace)
    // Pango-derived metrics (pixels)
    int pangoAscent_ = 0;
    int pangoDescent_ = 0;
#else
    int cellW_ = 8;
#endif

    // Clipboard atoms
    Atom clipboardAtom_ = None;
    Atom utf8Atom_ = None;
    Atom pasteProperty_ = None;

    struct ColorTheme {
        unsigned long bg = 0;     // background
        unsigned long fg = 0;     // foreground (text)
        unsigned long green = 0;  // user@host
        unsigned long blue = 0;   // cwd
        unsigned long gray = 0;   // UI lines
        unsigned long cursor = 0; // caret
        unsigned long tabActiveBg = 0;
        unsigned long tabInactiveBg = 0;
        unsigned long accent = 0; // accent line for active tab
        unsigned long scrollTrack = 0; // scrollbar track
        unsigned long scrollThumb = 0; // scrollbar thumb
        unsigned long scrollThumbHover = 0; // thumb hover color
        unsigned long tabHoverBg = 0; // hover color for tabs
        unsigned long newTabBg = 0; // color for new tab button
        std::vector<unsigned long> ansiFgColors;
        std::vector<unsigned long> ansiBgColors;
    } theme_{};

    int width_ = 0;
    int height_ = 0;
    int lineH_ = 18;
    bool focused_ = true;
    std::string cwdCache_{}; // cache current working directory for prompt

    // Scrollbar interaction
    bool draggingScrollbar_ = false;
    int dragStartY_ = 0;           // pixel Y where drag started
    int dragStartBeginLine_ = 0;   // begin line index at drag start
    bool hoverScrollbarThumb_ = false; // hover state for thumb

    // Tab hover/UI state
    int hoverTabIndex_ = -1;
    bool hoverNewTab_ = false;

    std::vector<std::unique_ptr<Tab>> tabs_;
    int activeTab_ = 0;

    bool cursorOn_ = true;
    // Blink timing
    int blinkMs_ = 600;
    int blinkCountdownMs_ = 600;
    int tickMs_ = 16; // ~60 FPS for smooth animations
    unsigned long long lastBlinkMs_ = 0; // monotonic ms at last update

    // Scrollbar geometry cache for hover checks
    int lastThumbY_ = -1;
    int lastThumbH_ = 0;
};

} // namespace myterm
