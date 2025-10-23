#include "gui/TerminalWindow.hpp"
#include <glob.h>
#include <unistd.h>
#include <cstring>

static void sweep_on_exit() {
    glob_t gb; memset(&gb, 0, sizeof(gb));
    if (glob("temp/.temp.*.txt", 0, nullptr, &gb) == 0) {
        for (size_t i=0; i<gb.gl_pathc; ++i) {
            unlink(gb.gl_pathv[i]);
        }
    }
    globfree(&gb);
}

int main() {
    atexit(sweep_on_exit);
    myterm::TerminalWindow app(1000, 700);
    app.run();
    return 0;
}
