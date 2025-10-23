#include "gui/Tab.hpp"

namespace myterm {

// Revert: remove explicit constructor and rely on default member initializers in Tab.hpp

void Tab::appendOutput(const std::string& s, size_t cap) {
    if (scrollback.size() + s.size() > cap) {
        size_t drop = scrollback.size() + s.size() - cap;
        if (drop >= scrollback.size()) scrollback.clear(); else scrollback.erase(0, drop);
    }
    scrollback += s;
    scrollToBottom = true; // always auto-scroll on new output (original behavior)
}

} // namespace myterm
