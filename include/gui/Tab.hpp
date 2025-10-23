#pragma once
#include <string>
#include <deque>
#include <utility>
#include <sys/types.h>
#include <vector>

namespace myterm {

struct BackgroundJob {
    pid_t pid;
    pid_t pgid;
    int outFd;
    int errFd;
    std::string cmd;
    bool isPty = false; // true when outFd refers to a PTY master (interactive job)
};

class Tab {
public:
    std::string scrollback; // accumulated output
    std::string input;      // current line
    size_t cursor = 0;      // cursor index
    int scrollOffsetLines = 0; // number of lines scrolled up from bottom
    int scrollOffsetTargetLines = 0; // target for smooth scrolling

    bool scrollToBottom = false; // flag to scroll to bottom on next draw

    std::deque<std::string> history;

    // Child process execution (per tab)
    pid_t childPid = -1; // >0 when a process is running in this tab
    pid_t childPgid = -1; // process group ID for signal handling
    int outFd = -1;      // stdout pipe read end
    int errFd = -1;      // stderr pipe read end
    int inFdWrite = -1;  // stdin pipe write end (from terminal to child)

    // ANSI parsing state (for chunked reads)
    enum { ANSI_TEXT=0, ANSI_ESC=1, ANSI_CSI=2 } ansiState = ANSI_TEXT;
    std::string ansiSeq;

    // Continuation input state (for unmatched quotes)
    bool contActive = false;        // true when waiting for closing quote
    std::string contBuffer;         // accumulated lines including closing quote
    // Continuation semantics: when true, last consumed line ended with a trailing '\\'
    // and was joined without inserting a newline. We recompute this each time from input,
    // but keep this for potential UI decisions.
    bool contJoinNoNewline = false;

    // Queue of pending commands to execute sequentially: pair<cmd, echoPromptAndCmd>
    std::deque<std::pair<std::string,bool>> pendingCmds;

    std::vector<BackgroundJob> backgroundJobs;

    // multiWatch state: save/restore original terminal content
    bool watchActive = false;                 // true while multiWatch is running
    std::string savedScrollbackBeforeWatch;   // previous scrollback to restore on completion

    void appendOutput(const std::string& s, size_t cap = 1<<20);
};

} // namespace myterm
