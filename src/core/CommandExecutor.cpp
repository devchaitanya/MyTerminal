#include "gui/TerminalWindow.hpp"
#include "gui/Tab.hpp"

#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <cerrno>
#include <cstring>
#include <vector>
#include <string>
#include <pwd.h>
#include <limits.h>
#include <cstdlib>
#include <pty.h>
#include <termios.h>
#include <glob.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <ctime>
#include <fstream>
#include <sys/stat.h>
#include <poll.h>
// Filter specific Xlib shutdown noise when a nested GUI client exits while
// its X connection is being torn down (e.g., parent UI closing). We don’t want
// this low-level diagnostic to pollute the shell output.
static bool is_x_shutdown_noise(const std::string& s) {
    return (s.find("X connection to ") != std::string::npos) &&
           (s.find("broken (explicit kill or server shutdown)") != std::string::npos);
}

// Globals used by multiWatch child for signal cleanup
static std::vector<pid_t> mw_pids;
static std::vector<std::string> mw_tempfiles;
static void mw_sweep_tempfiles() {
    glob_t gb; memset(&gb, 0, sizeof(gb));
    if (glob("temp/.temp.*.txt", 0, nullptr, &gb) == 0) {
        for (size_t i=0; i<gb.gl_pathc; ++i) {
            unlink(gb.gl_pathv[i]);
        }
    }
    globfree(&gb);
}
static void mw_cleanup() {
    for (pid_t p : mw_pids) {
        if (p > 0) kill(p, SIGKILL);
    }
    for (const auto& f : mw_tempfiles) {
        unlink(f.c_str());
    }
}
static void mw_signal_handler(int) {
    mw_cleanup();
    _exit(0);
}

static std::string cx_get_user() {
    if (const char* u = getenv("USER")) return u;
    if (passwd* pw = getpwuid(getuid())) return pw->pw_name;
    return "user";
}
static std::string cx_get_host() {
    char b[256]; if (gethostname(b, sizeof(b))==0) return b; return "host";
}
static std::string cx_get_cwd() {
    char b[PATH_MAX]; if (getcwd(b, sizeof(b))) return b; return "?";
}
static std::string ubuntu_prompt() {
    return cx_get_user()+"@"+cx_get_host()+":"+cx_get_cwd()+"$ ";
}

namespace myterm {

// Print a visual separator when multiple commands were queued, to avoid mixing outputs
static inline void append_sep_if_queued(Tab& t) {
    if (!t.pendingCmds.empty()) {
        t.appendOutput("-------------------------------------------------------------\n");
    }
}

bool TerminalWindow::isWhitespaceOnly(const std::string& s) {
    for (char c: s) {
        if (!isspace((unsigned char)c)) return false;
    }
    return true;
}

std::vector<std::string> TerminalWindow::splitArgs(const std::string& s) {
    std::vector<std::string> out; std::string cur; bool inSingle=false, inDouble=false;
    for (size_t i=0;i<s.size();++i) {
        char c = s[i];
        if (c=='"' && !inSingle) { inDouble = !inDouble; continue; }
        if (c=='\'' && !inDouble) { inSingle = !inSingle; continue; }
        // Delimit only on space or tab when not quoted; preserve newlines verbatim
        if (!inSingle && !inDouble && (c==' ' || c=='\t')) {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

static std::vector<std::string> expandGlobs(const std::vector<std::string>& args) {
    std::vector<std::string> expanded;
    for (const auto& arg : args) {
        glob_t globbuf;
        int flags = GLOB_NOCHECK | GLOB_TILDE;
        if (glob(arg.c_str(), flags, nullptr, &globbuf) == 0) {
            for (size_t i = 0; i < globbuf.gl_pathc; ++i) {
                expanded.push_back(globbuf.gl_pathv[i]);
            }
        } else {
            expanded.push_back(arg);
        }
        globfree(&globbuf);
    }
    return expanded;
}

static std::string expandVars(const std::string& s) {
    if (!s.empty() && s[0] == '~') {
        const char* home = getenv("HOME");
        return std::string(home ? home : "") + s.substr(1);
    }
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '$') {
            size_t j = i + 1;
            while (j < s.size() && (isalnum(s[j]) || s[j] == '_')) ++j;
            std::string var = s.substr(i + 1, j - i - 1);
            const char* val = getenv(var.c_str());
            if (val) out += val;
            i = j - 1;
        } else {
            out += s[i];
        }
    }
    return out;
}

// removed unused splitCommands helper (submitInputLine provides its own semicolon-aware splitter)

void TerminalWindow::pumpChildOutput() {
    if (tabs_.empty()) return;
    Tab& t = *tabs_[activeTab_];
    char buf[4096]; bool readSomething=false;
    for (int fdIdx=0; fdIdx<2; ++fdIdx) {
        int fd = (fdIdx==0)?t.outFd:t.errFd; if (fd<0) continue;
        while (true) {
            ssize_t n = read(fd, buf, sizeof(buf));
            if (n>0) {
                std::string chunk = sanitizeAndApplyANSI(t, buf, (size_t)n);
                if (!chunk.empty() && !is_x_shutdown_noise(chunk)) t.appendOutput(chunk);
                readSomething=true;
            }
            else if (n==0) { close(fd); if (fdIdx==0) t.outFd=-1; else t.errFd=-1; break; }
            else { if (errno==EAGAIN || errno==EWOULDBLOCK) break; else break; }
        }
    }
    if (readSomething) { t.scrollOffsetTargetLines = t.scrollOffsetLines; redraw(); }
    // Reap if finished
    if (t.childPid>0) {
        int status=0; pid_t r = waitpid(t.childPid, &status, WNOHANG);
        if (r==t.childPid) {
            t.childPid=-1; t.childPgid=-1; if (t.inFdWrite>=0){close(t.inFdWrite); t.inFdWrite=-1;}
            // Add a separator if more commands are queued
            append_sep_if_queued(t);
            // If multiWatch was active, restore previous scrollback
            if (t.watchActive) {
                t.scrollback = t.savedScrollbackBeforeWatch;
                t.watchActive = false;
                t.scrollOffsetLines = 0; t.scrollOffsetTargetLines = 0;
                redraw();
            }
            runNextCommand(t);
            if (t.childPid <= 0) redraw();
        }
    }
}

void TerminalWindow::drainBackgroundJobs() {
    if (tabs_.empty()) return;
    Tab& t = *tabs_[activeTab_];
    char buf[4096]; bool readSomething = false;
    for (auto it = t.backgroundJobs.begin(); it != t.backgroundJobs.end(); ) {
        for (int fdIdx=0; fdIdx<2; ++fdIdx) {
            int fd = (fdIdx==0)?it->outFd:it->errFd; if (fd<0) continue;
            while (true) {
                ssize_t n = read(fd, buf, sizeof(buf));
                if (n>0) {
                    std::string chunk = sanitizeAndApplyANSI(t, buf, (size_t)n);
                    if (!chunk.empty() && !is_x_shutdown_noise(chunk)) t.appendOutput(chunk);
                    readSomething=true;
                }
                else if (n==0) { close(fd); if (fdIdx==0) it->outFd=-1; else it->errFd=-1; break; }
                else { if (errno==EAGAIN || errno==EWOULDBLOCK) break; else break; }
            }
        }
        // Check if dead
        int status=0; pid_t r = waitpid(it->pid, &status, WNOHANG);
        if (r == it->pid) {
            if (it->outFd >=0) close(it->outFd);
            if (it->errFd >=0) close(it->errFd);
            it = t.backgroundJobs.erase(it);
            continue;
        }
        ++it;
    }
    if (readSomething) { t.scrollOffsetTargetLines = t.scrollOffsetLines; redraw(); }
}

void TerminalWindow::spawnProcess(const std::vector<std::string>& argv) {
    if (tabs_.empty() || argv.empty()) return;
    Tab& t = *tabs_[activeTab_];
    int outPipe[2]; int errPipe[2]; int inPipe[2];
    if (pipe(outPipe)<0 || pipe(errPipe)<0 || pipe(inPipe)<0) { t.appendOutput("pipe() failed\n"); return; }
    pid_t pid = fork();
    if (pid<0) { t.appendOutput("fork() failed\n"); close(outPipe[0]); close(outPipe[1]); close(errPipe[0]); close(errPipe[1]); return; }
    if (pid==0) {
        // child
        setpgid(0, 0);
        dup2(inPipe[0], STDIN_FILENO);
        dup2(outPipe[1], STDOUT_FILENO);
        dup2(errPipe[1], STDERR_FILENO);
        close(inPipe[0]); close(inPipe[1]);
        close(outPipe[0]); close(outPipe[1]); close(errPipe[0]); close(errPipe[1]);
        // Expand globs
        auto expanded_argv = expandGlobs(argv);
        // Build argv for execvp
        std::vector<char*> cargv; cargv.reserve(expanded_argv.size()+1);
        for (auto& s: expanded_argv) cargv.push_back(const_cast<char*>(s.c_str()));
        cargv.push_back(nullptr);
        execvp(cargv[0], cargv.data());
        // If execvp fails, report error to stderr
        {
            std::string msg = std::string(cargv[0]) + ": " + (errno==ENOENT?"command not found":strerror(errno)) + "\n";
                (void)!write(STDERR_FILENO, msg.c_str(), msg.size()); // Cast return value to void
        }
        _exit(127);
    } else {
        // parent
        t.childPid = pid;
        t.childPgid = pid;
        close(inPipe[0]);
        close(outPipe[1]); close(errPipe[1]);
        t.outFd = outPipe[0]; t.errFd = errPipe[0];
        t.inFdWrite = inPipe[1];
        fcntl(t.outFd, F_SETFL, O_NONBLOCK);
        fcntl(t.errFd, F_SETFL, O_NONBLOCK);
    }
}

// Build a simple pipeline and redirections without invoking a shell.
// Supports: cmds separated by '|', and redirections: >, >>, < on the first/last stage appropriately.
// No globbing/var expansion.
static std::vector<std::string> splitPipeline(const std::string& s) {
    std::vector<std::string> parts; std::string cur; bool inS=false,inD=false;
    for (size_t i=0;i<s.size();++i) {
        char c=s[i];
        if (c=='"' && !inS){inD=!inD;continue;} if (c=='\'' && !inD){inS=!inS;continue;}
        if (!inS && !inD && c=='|'){ if(!cur.empty()){parts.push_back(cur);cur.clear();} }
        else cur.push_back(c);
    }
    if(!cur.empty()) parts.push_back(cur);
    return parts;
}

struct Redir { std::string in; std::string out; bool append=false; std::string errOut; bool errAppend=false; };
static std::vector<std::string> splitArgsLoose(const std::string& s) {
    std::vector<std::string> out; std::string cur; bool inS=false,inD=false;
    for (size_t i=0;i<s.size();++i) {
        char c=s[i];
        if (c=='"' && !inS){inD=!inD;continue;}
        if (c=='\'' && !inD){inS=!inS;continue;}
        if (!inS && !inD) {
            // Delimit only on space or tab; preserve newlines inside arguments
            if (c==' ' || c=='\t') {
                if(!cur.empty()){out.push_back(cur);cur.clear();}
                continue;
            }
            // Handle redirection tokens even without surrounding spaces
            if (c=='<' || c=='>') {
                if(!cur.empty()){out.push_back(cur);cur.clear();}
                if (c=='>' && i+1<s.size() && s[i+1]=='>') { out.emplace_back(">>"); ++i; }
                else { out.emplace_back(std::string(1,c)); }
                continue;
            }
        }
        cur.push_back(c);
    }
    if(!cur.empty()) out.push_back(cur);
    return out;
}
static std::vector<std::string> parseCmdWithRedir(const std::string& s, Redir& r) {
    std::vector<std::string> tokens = splitArgsLoose(s);
    std::vector<std::string> argv; for (size_t i=0;i<tokens.size();++i) {
        if (tokens[i]==">" || tokens[i]==">>") { r.append = (tokens[i]==">>"); if (i+1<tokens.size()) { r.out=tokens[i+1]; ++i; } continue; }
        if (tokens[i]=="2>" || tokens[i]=="2>>") { r.errAppend = (tokens[i]=="2>>"); if (i+1<tokens.size()) { r.errOut=tokens[i+1]; ++i; } continue; }
        if (tokens[i]=="<") { if (i+1<tokens.size()) { r.in=tokens[i+1]; ++i; } continue; }
        argv.push_back(tokens[i]);
    }
    return argv;
}

bool quotesBalanced(const std::string& s) {
    bool inS=false, inD=false;
    for (char c: s) {
        if (c=='"' && !inS) inD = !inD;
        else if (c=='\'' && !inD) inS = !inS;
    }
    return !inS && !inD;
}

// Minimal ANSI handler: clear screen (CSI 2J) and strip/ignore others
std::string TerminalWindow::sanitizeAndApplyANSI(struct Tab& t, const char* data, size_t n) {
    std::string out; out.reserve(n);
    auto clearScreen = [&]() {
        t.scrollback.clear();
        t.scrollOffsetLines = 0;
        t.scrollOffsetTargetLines = 0;
    };
    for (size_t i=0;i<n;i++) {
        unsigned char c = (unsigned char)data[i];
        if (t.ansiState==Tab::ANSI_TEXT) {
            if (c==0x1B) { t.ansiState=Tab::ANSI_ESC; t.ansiSeq = "\x1B"; }
            else if (c=='\r') {
                // Convert CR or CRLF to a single newline; avoid doubling on CRLF
                if (!(i+1<n && (unsigned char)data[i+1]=='\n')) out.push_back('\n');
            }
            else if (c=='\n') { out.push_back('\n'); }
            else if (c=='\t') {
                // Expand tabs for readability (fixed width)
                out.append("    "); // 4 spaces
            }
            else if (c==0x07) { /* BEL - ignore */ }
            else if (c>=0x20 || c=='\n' || c=='\t') { out.push_back((char)c); }
            else { /* drop other control chars */ }
        } else if (t.ansiState==Tab::ANSI_ESC) {
            t.ansiSeq.push_back((char)c);
            if (c=='[') { t.ansiState=Tab::ANSI_CSI; }
            else { out.append(t.ansiSeq); t.ansiSeq.clear(); t.ansiState=Tab::ANSI_TEXT; }
        } else if (t.ansiState==Tab::ANSI_CSI) {
            t.ansiSeq.push_back((char)c);
            if ((unsigned char)c==0x07) { out.append(t.ansiSeq); t.ansiSeq.clear(); t.ansiState=Tab::ANSI_TEXT; continue; }
            if (c>='@' && c<='~') {
                char final = (char)c;
                if (final=='J') {
                    if (t.ansiSeq.find('2') != std::string::npos) clearScreen();
                }
                out.append(t.ansiSeq);
                t.ansiSeq.clear(); t.ansiState=Tab::ANSI_TEXT;
            }
        }
    }
    return out;
}

void TerminalWindow::printPromptForCurrentTab(bool continuation) {
    if (tabs_.empty()) return;
    Tab& t = *tabs_[activeTab_];
    if (!continuation) {
        t.appendOutput(ubuntu_prompt());
    } else {
        t.appendOutput("> ");
    }
}

void TerminalWindow::initHistory() {
    // Determine history file path: ~/.myterm_history
    const char* home = getenv("HOME");
    if (!home) {
        if (passwd* pw = getpwuid(getuid())) home = pw->pw_dir;
    }
    if (home) historyPath_ = std::string(home) + "/.myterm_history";
    if (!historyPath_.empty()) history_.loadFromFile(historyPath_);
}

void TerminalWindow::addHistoryEntry(const std::string& cmd) {
    if (cmd.empty()) return;
    // Skip if same as the most recent command (avoid consecutive duplicates)
    const auto& dq = history_.data();
    if (!dq.empty() && dq.back() == cmd) return;
    history_.add(cmd);
    if (!historyPath_.empty()) {
        // If we exceeded cap, rewrite file with last cap entries to enforce limit
        if (history_.data().size() > 10000) {
            history_.saveToFile(historyPath_);
        } else {
            history_.appendToFile(historyPath_, cmd);
        }
    }
}

void TerminalWindow::executeLine(const std::string& line) {
    executeLineInternal(line, true);
}

void TerminalWindow::executeLineInternal(const std::string& line, bool echoPromptAndCmd) {
    if (tabs_.empty()) return;
    Tab& t = *tabs_[activeTab_];
    if (isWhitespaceOnly(line)) return;
    auto args = splitArgs(line);
    bool background = false;
    if (!args.empty() && args.back() == "&") {
        background = true;
        args.pop_back();
    }
    for (auto& arg : args) arg = expandVars(arg);
    // IMPORTANT: keep the original line (with quotes/newlines) for pipeline/redirection parsing
    // and for echoing what the user typed. Rejoining args would drop quotes and turn newlines into spaces.
    std::string cmd_line = line;
    // echo the command exactly as typed
    if (echoPromptAndCmd) t.appendOutput(ubuntu_prompt()+line+"\n");
    if (args.empty()) return;
    // Built-in: echo (interpret C-like escapes when quoted input contains them)
    if (!args.empty() && args[0]=="echo") {
        // Reconstruct payload from original line after the leading 'echo'
        // Find first occurrence of "echo" and take remainder
        std::string payload;
        {
            size_t pos = line.find("echo");
            if (pos != std::string::npos) {
                payload = line.substr(pos + 4);
                // trim leading spaces/tabs
                while (!payload.empty() && (payload.front()==' ' || payload.front()=='\t')) payload.erase(payload.begin());
            } else {
                // fallback: join args[1..]
                for (size_t i=1;i<args.size();++i){ if(i>1) payload.push_back(' '); payload += args[i]; }
            }
        }
        // If payload is wrapped in matching single or double quotes, strip them
        if (payload.size()>=2 && ((payload.front()=='"' && payload.back()=='"') || (payload.front()=='\'' && payload.back()=='\''))) {
            payload = payload.substr(1, payload.size()-2);
        }
        // Unescape sequences: \n, \t, \\ and \"\' inside quoted forms
        std::string out; out.reserve(payload.size());
        for (size_t i=0;i<payload.size(); ++i) {
            char c = payload[i];
            if (c=='\\' && i+1<payload.size()) {
                char n = payload[i+1];
                // remove a backslash that was used as line continuation before a literal newline from transcript
                if (n=='\n') { ++i; continue; }
                if (n=='n') { out.push_back('\n'); ++i; continue; }
                if (n=='t') { out.append("    "); ++i; continue; }
                if (n=='\"') { out.push_back('"'); ++i; continue; }
                if (n=='\'') { out.push_back('\''); ++i; continue; }
                if (n=='\\') { out.push_back('\\'); ++i; continue; }
            }
            out.push_back(c);
        }
    t.appendOutput(out + "\n");
    redraw();
    // Continue with any queued commands, add a separator if more remain
    append_sep_if_queued(t);
    runNextCommand(t);
    return;
    }

    // Built-in: history
    if (args[0]=="history") {
        // Clear history: history -c | history --clear | history clear
        if (args.size()>=2 && (args[1]=="-c" || args[1]=="--clear" || args[1]=="clear")) {
            history_.clear();
            if (!historyPath_.empty()) {
                // Truncate file to empty
                history_.saveToFile(historyPath_);
            }
            t.appendOutput("History cleared\n");
            redraw();
            append_sep_if_queued(t);
            runNextCommand(t);
            return;
        }
        // Default: print last 1000 commands
        const auto& dq = history_.data();
        int count = (int)dq.size();
        int start = std::max(0, count - 1000);
    for (int i=start;i<count;++i) t.appendOutput(dq[i] + "\n");
    redraw();
    append_sep_if_queued(t);
    runNextCommand(t);
    return;
    }

    // Built-in: cd
    if (args[0]=="cd") {
        const char* target = nullptr;
        if (args.size()==1) target = getenv("HOME");
        else if (args[1]=="~") target = getenv("HOME");
        else if (args[1].size() && args[1][0]=='~') {
            std::string p = std::string(getenv("HOME")) + args[1].substr(1);
            target = p.c_str();
            if (chdir(target)==0) { /* success */ }
            else { t.appendOutput("cd: no such file or directory\n"); }
            redraw(); return;
        }
        if (!target && args.size()>=2) target = args[1].c_str();
        if (!target) target = getenv("HOME");
        if (chdir(target)!=0) {
            t.appendOutput("cd: no such file or directory\n");
        }
        if (t.inFdWrite>=0) { close(t.inFdWrite); t.inFdWrite=-1; }
    redraw();
    append_sep_if_queued(t);
    runNextCommand(t);
        return;
    }
    // Built-in: clear (works even when child stdout is not a TTY)
    if (args[0]=="clear") {
    t.scrollback.clear();
    t.scrollOffsetLines = 0;
    t.scrollOffsetTargetLines = 0;
    t.ansiState = Tab::ANSI_TEXT;
    t.ansiSeq.clear();
    redraw();
    append_sep_if_queued(t);
    runNextCommand(t);
    return;
    }
    // Built-in: bgpids (list background pids with commands)
    if (args[0]=="bgpids") {
        if (t.backgroundJobs.empty()) {
            t.appendOutput("No background jobs\n");
        } else {
            for (const auto &job : t.backgroundJobs) {
                t.appendOutput("PID=" + std::to_string(job.pid) +
                               (job.pgid>0? (" PGID=" + std::to_string(job.pgid)) : std::string("")) +
                               " CMD=" + job.cmd + "\n");
            }
        }
    redraw();
    append_sep_if_queued(t);
    runNextCommand(t);
        return;
    }
    // Built-in: killprocess [-9] PID [PID...]
    if (args[0]=="killprocess") {
        int sig = SIGTERM;
        size_t i = 1;
        if (args.size()>=2 && args[1].size()>1 && args[1][0]=='-' ) {
            if (args[1] == "-9") sig = SIGKILL;
            else if (args[1] == "-15") sig = SIGTERM;
            ++i;
        }
        if (i>=args.size()) { t.appendOutput("usage: killprocess [-9] PID [PID ...]\n"); redraw(); return; }
        for (; i<args.size(); ++i) {
            const std::string &spid = args[i];
            char *end=nullptr; long v = strtol(spid.c_str(), &end, 10);
            if (!spid.empty() && end && *end=='\0' && v>0) {
                pid_t pid = (pid_t)v;
                bool found=false;
                for (auto it = t.backgroundJobs.begin(); it != t.backgroundJobs.end(); ++it) {
                    if (it->pid == pid || it->pgid == pid) {
                        if (it->pgid > 0) {
                            if (killpg(it->pgid, sig) == 0) {
                                t.appendOutput("killed process group " + std::to_string(it->pgid) + " (sig " + std::to_string(sig) + ")\n");
                            } else {
                                t.appendOutput("killpg(" + std::to_string(it->pgid) + ") failed: " + std::string(strerror(errno)) + "\n");
                            }
                        } else {
                            if (kill(it->pid, sig) == 0) {
                                t.appendOutput("killed pid " + std::to_string(it->pid) + " (sig " + std::to_string(sig) + ")\n");
                            } else {
                                t.appendOutput("kill(" + std::to_string(it->pid) + ") failed: " + std::string(strerror(errno)) + "\n");
                            }
                        }
                        if (it->outFd>=0) { close(it->outFd); }
                        if (it->errFd>=0) { close(it->errFd); }
                        t.backgroundJobs.erase(it);
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    if (kill(pid, sig)==0) {
                        t.appendOutput("killed pid " + std::to_string(pid) + " (sig " + std::to_string(sig) + ")\n");
                    } else {
                        t.appendOutput("kill(" + std::to_string(pid) + ") failed: " + std::string(strerror(errno)) + "\n");
                    }
                }
            } else {
                t.appendOutput("killprocess: invalid pid '" + spid + "'\n");
            }
        }
    redraw();
    runNextCommand(t);
    return;
    }
    // Built-in: kill [-9] PID [PID...]
    if (args[0]=="kill") {
        int sig = SIGTERM;
        size_t i = 1;
        if (args.size()>=2 && args[1].size()>1 && args[1][0]=='-' ) {
            if (args[1] == "-9") sig = SIGKILL;
            else if (args[1] == "-15") sig = SIGTERM;
            // You can add more signals as needed
            ++i;
        }
        if (i>=args.size()) { t.appendOutput("usage: kill [-9] PID [PID ...]\n"); redraw(); return; }
        for (; i<args.size(); ++i) {
            const std::string &spid = args[i];
            char *end=nullptr; long v = strtol(spid.c_str(), &end, 10);
            if (!spid.empty() && end && *end=='\0' && v>0) {
                pid_t pid = (pid_t)v;
                // Try to match background job to prefer killing process group
                bool found=false;
                for (auto it = t.backgroundJobs.begin(); it != t.backgroundJobs.end(); ++it) {
                    if (it->pid == pid || it->pgid == pid) {
                        // Prefer
                        if (it->pgid > 0) {
                            if (killpg(it->pgid, sig) == 0) {
                                t.appendOutput("killed process group " + std::to_string(it->pgid) + " (sig " + std::to_string(sig) + ")\n");
                            } else {
                                t.appendOutput("killpg(" + std::to_string(it->pgid) + ") failed: " + std::string(strerror(errno)) + "\n");
                            }
                        } else {
                            if (kill(it->pid, sig) == 0) {
                                t.appendOutput("killed pid " + std::to_string(it->pid) + " (sig " + std::to_string(sig) + ")\n");
                            } else {
                                t.appendOutput("kill(" + std::to_string(it->pid) + ") failed: " + std::string(strerror(errno)) + "\n");
                            }
                        }
                        // Close any FDs and remove job
                        if (it->outFd>=0) { close(it->outFd); }
                        if (it->errFd>=0) { close(it->errFd); }
                        t.backgroundJobs.erase(it);
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    // Attempt a direct kill as a fallback
                    if (kill(pid, sig)==0) {
                        t.appendOutput("killed pid " + std::to_string(pid) + " (sig " + std::to_string(sig) + ")\n");
                    } else {
                        t.appendOutput("kill(" + std::to_string(pid) + ") failed: " + std::string(strerror(errno)) + "\n");
                    }
                }
            } else {
                t.appendOutput("kill: invalid pid '" + spid + "'\n");
            }
        }
        redraw();
        return;
    }
    // Built-in: multiWatch [interval] ["cmd1", "cmd2", ...] OR multiWatch [interval] cmd1 cmd2 ...
    if (!args.empty() && args[0] == "multiWatch") {
        int interval = 2; // default seconds
        size_t argStart = 1;
        if (args.size() > 1) {
            try {
                int v = std::stoi(args[1]);
                if (v > 0) { interval = v; argStart = 2; }
            } catch (...) {}
        }
        std::vector<std::string> cmds;
        // Try to parse list form: everything after args[0] joined, parse [ ... ] items respecting quotes
        if (args.size() > argStart) {
            std::string joined;
            for (size_t i = argStart; i < args.size(); ++i) {
                if (i > argStart) joined.push_back(' ');
                joined += args[i];
            }
            size_t lb = joined.find('[');
            size_t rb = joined.rfind(']');
            if (lb != std::string::npos && rb != std::string::npos && rb > lb) {
                std::string body = joined.substr(lb + 1, rb - lb - 1);
                std::string cur; bool inS=false, inD=false;
                for (size_t i=0;i<body.size();++i) {
                    char c = body[i];
                    if (c=='"' && !inS){ inD=!inD; continue; }
                    if (c=='\'' && !inD){ inS=!inS; continue; }
                    if (!inS && !inD && c==',') { if (!cur.empty()) { cmds.push_back(cur); cur.clear(); } continue; }
                    cur.push_back(c);
                }
                if (!cur.empty()) cmds.push_back(cur);
                // Trim surrounding spaces and quotes
                for (auto &s : cmds) {
                    while (!s.empty() && isspace((unsigned char)s.front())) s.erase(s.begin());
                    while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
                    if (s.size()>=2 && ((s.front()=='"' && s.back()=='"') || (s.front()=='\'' && s.back()=='\''))) {
                        s = s.substr(1, s.size()-2);
                    }
                }
            }
            if (cmds.empty()) {
                // Fallback: treat remaining args as commands directly
                for (size_t i=argStart;i<args.size();++i) cmds.push_back(args[i]);
            }
        }
        if (cmds.empty()) { t.appendOutput("multiWatch: no commands specified\n"); redraw(); return; }
        // Save and clear
        if (!t.watchActive) {
            t.savedScrollbackBeforeWatch = t.scrollback;
            t.scrollback.clear();
            t.scrollOffsetLines = 0;
            t.scrollOffsetTargetLines = 0;
            t.watchActive = true;
            redraw();
        }

        int outPipe[2]; if (pipe(outPipe)<0) {
            t.appendOutput("pipe() failed\n");
            if (t.watchActive) {
                t.scrollback = t.savedScrollbackBeforeWatch;
                t.watchActive = false;
                redraw();
            }
            return;
        }
        pid_t cpid = fork();
        if (cpid<0) { t.appendOutput("fork() failed\n"); close(outPipe[0]); close(outPipe[1]);
            if (t.watchActive) {
                t.scrollback = t.savedScrollbackBeforeWatch;
                t.watchActive = false;
                redraw();
            }
            return; }
        if (cpid==0) {
            // multiWatch worker (child)
            dup2(outPipe[1], STDOUT_FILENO); close(outPipe[0]); close(outPipe[1]);
            // Put worker in its own process group so Ctrl+C can target the entire job
            setpgid(0, 0);
            signal(SIGINT, mw_signal_handler);
            signal(SIGTERM, mw_signal_handler);
            signal(SIGHUP, mw_signal_handler);
            signal(SIGQUIT, mw_signal_handler);
            // Remove any stale temp FIFOs from previous abnormal runs
            mw_sweep_tempfiles();
            while (true) {
                mw_pids.clear(); mw_tempfiles.clear();
                std::vector<pid_t> pidsForIndex(cmds.size(), -1);
                std::vector<int> exitCodes(cmds.size(), -999);
                std::vector<std::string> paths(cmds.size());
                std::vector<struct pollfd> pfds;
                pfds.reserve(cmds.size());
                // Fork children, each writing to its own FIFO at .temp.<PID>.txt
                // Ensure temp directory exists
                mkdir("temp", 0755);
                for (size_t i=0;i<cmds.size(); ++i) {
                    pid_t p = fork();
                    if (p==0) {
                        // Child: open write end of FIFO once parent creates and opens read end
                        pid_t self = getpid();
                        std::string tf = std::string("temp/.temp.") + std::to_string(self) + ".txt";
                        int wfd = -1;
                        // Try non-blocking open until reader is ready
                        while (true) {
                            wfd = open(tf.c_str(), O_WRONLY | O_NONBLOCK);
                            if (wfd >= 0) break;
                            if (errno == ENOENT || errno == ENXIO) {
                                struct timespec ts{0, 10*1000*1000}; // 10ms
                                nanosleep(&ts, nullptr);
                                continue;
                            }
                            _exit(127);
                        }
                        // Redirect and exec
                        dup2(wfd, STDOUT_FILENO);
                        dup2(wfd, STDERR_FILENO);
                        close(wfd);
                        execlp("sh", "sh", "-c", cmds[i].c_str(), (char*)nullptr);
                        _exit(127);
                    } else if (p>0) {
                        // Parent: create FIFO, open read-end nonblocking
                        mw_pids.push_back(p);
                        pidsForIndex[i] = p;
                        std::string tf = std::string("temp/.temp.") + std::to_string(p) + ".txt";
                        // Ensure no stale FIFO/file remains, then create
                        unlink(tf.c_str());
                        mkfifo(tf.c_str(), 0644);
                        mw_tempfiles.push_back(tf);
                        paths[i] = tf;
                        int rfd = -1;
                        // Open read end non-blocking; if ENOENT/ENXIO, retry until available
                        while (true) {
                            rfd = open(tf.c_str(), O_RDONLY | O_NONBLOCK);
                            if (rfd >= 0) break;
                            if (errno == ENOENT || errno == ENXIO) {
                                struct timespec ts{0, 10*1000*1000}; // 10ms
                                nanosleep(&ts, nullptr);
                                continue;
                            }
                            break;
                        }
                        if (rfd >= 0) {
                            struct pollfd pd{}; pd.fd = rfd; pd.events = POLLIN | POLLHUP | POLLERR; pd.revents = 0;
                            pfds.push_back(pd);
                        }
                    }
                }
                // Stream data as it becomes available via poll
                size_t openCount = pfds.size();
                std::vector<size_t> fdIndexToCmdIdx; fdIndexToCmdIdx.reserve(pfds.size());
                {
                    // Build mapping by re-opening in the same order pfds were pushed
                    size_t pushed = 0;
                    for (size_t i=0;i<cmds.size() && pushed < pfds.size(); ++i) {
                        if (!paths[i].empty()) {
                            fdIndexToCmdIdx.push_back(i);
                            ++pushed;
                        }
                    }
                }
                const size_t BUF_SZ = 4096; char buf[BUF_SZ];
                // Track formatting state per fd: header/trailer printed
                std::vector<bool> headerPrinted(pfds.size(), false);
                std::vector<bool> trailerPrinted(pfds.size(), false);
                while (openCount > 0 && !pfds.empty()) {
                    int rc = poll(pfds.data(), (nfds_t)pfds.size(), 200);
                    if (rc < 0) {
                        if (errno == EINTR) continue; // interrupted by signal
                        break;
                    }
                    if (rc == 0) goto after_poll_io; // timeout, loop again
                    for (size_t j=0;j<pfds.size(); ++j) {
                        auto &pd = pfds[j];
                        if (pd.fd < 0) continue;
                        if (pd.revents & (POLLIN)) {
                            ssize_t n = read(pd.fd, buf, BUF_SZ);
                            if (n > 0) {
                                if (!headerPrinted[j]) {
                                    time_t now = time(nullptr);
                                    size_t ci = fdIndexToCmdIdx[j];
                                    std::string header = std::string("\"") + cmds[ci] + "\" , current_time: " + std::to_string(now) + " :\n";
                                    const char* sep = "----------------------------------------------------\n";
                                    (void)!write(STDOUT_FILENO, header.c_str(), header.size());
                                    (void)!write(STDOUT_FILENO, sep, strlen(sep));
                                    headerPrinted[j] = true;
                                }
                                (void)!write(STDOUT_FILENO, buf, (size_t)n);
                            } else if (n == 0) {
                                // Writer closed; print trailer if needed and close
                                if (!headerPrinted[j]) {
                                    time_t now = time(nullptr);
                                    size_t ci = fdIndexToCmdIdx[j];
                                    std::string header = std::string("\"") + cmds[ci] + "\" , current_time: " + std::to_string(now) + " :\n";
                                    const char* sep = "----------------------------------------------------\n";
                                    (void)!write(STDOUT_FILENO, header.c_str(), header.size());
                                    (void)!write(STDOUT_FILENO, sep, strlen(sep));
                                    headerPrinted[j] = true;
                                }
                                if (!trailerPrinted[j]) {
                                    const char* sep = "----------------------------------------------------\n";
                                    (void)!write(STDOUT_FILENO, sep, strlen(sep));
                                    trailerPrinted[j] = true;
                                }
                                // Close and unlink this FIFO immediately
                                size_t ci = fdIndexToCmdIdx[j];
                                std::string tf = paths[ci];
                                close(pd.fd); pd.fd = -1; --openCount;
                                if (!tf.empty()) unlink(tf.c_str());
                            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                                close(pd.fd); pd.fd = -1; --openCount;
                            }
                        }
                        if (pd.revents & (POLLHUP | POLLERR)) {
                            // Drain any remaining data
                            while (true) {
                                ssize_t n = read(pd.fd, buf, BUF_SZ);
                                if (n > 0) {
                                    if (!headerPrinted[j]) {
                                        time_t now = time(nullptr);
                                        size_t ci = fdIndexToCmdIdx[j];
                                        std::string header = std::string("\"") + cmds[ci] + "\" , " + std::to_string(now) + " :\n";
                                        const char* sep = "----------------------------------------------------\n";
                                        (void)!write(STDOUT_FILENO, header.c_str(), header.size());
                                        (void)!write(STDOUT_FILENO, sep, strlen(sep));
                                        headerPrinted[j] = true;
                                    }
                                    (void)!write(STDOUT_FILENO, buf, (size_t)n);
                                } else {
                                    break;
                                }
                            }
                            if (!headerPrinted[j]) {
                                // No output at all; still print header and separators
                                time_t now = time(nullptr);
                                size_t ci = fdIndexToCmdIdx[j];
                                std::string header = std::string("\"") + cmds[ci] + "\" , current_time: " + std::to_string(now) + " :\n";
                                const char* sep = "----------------------------------------------------\n";
                                (void)!write(STDOUT_FILENO, header.c_str(), header.size());
                                (void)!write(STDOUT_FILENO, sep, strlen(sep));
                                headerPrinted[j] = true;
                            }
                            if (!trailerPrinted[j]) {
                                const char* sep = "----------------------------------------------------\n";
                                (void)!write(STDOUT_FILENO, sep, strlen(sep));
                                trailerPrinted[j] = true;
                            }
                            if (pd.fd >= 0) {
                                size_t ci = fdIndexToCmdIdx[j];
                                std::string tf = paths[ci];
                                close(pd.fd); pd.fd = -1; --openCount;
                                if (!tf.empty()) unlink(tf.c_str());
                            }
                        }
                        pd.revents = 0;
                    }
after_poll_io:
                    ;
                }
                // Reap children and collect exit codes (optional)
                for (size_t i=0;i<pidsForIndex.size(); ++i) {
                    pid_t pid = pidsForIndex[i];
                    if (pid>0) {
                        int st=0; if (waitpid(pid, &st, 0) > 0) {
                            if (WIFEXITED(st)) exitCodes[i] = WEXITSTATUS(st);
                            else if (WIFSIGNALED(st)) exitCodes[i] = 128 + WTERMSIG(st);
                            else exitCodes[i] = -1;
                        }
                    }
                }
                // Cleanup FIFOs
                for (const auto &f: mw_tempfiles) unlink(f.c_str());
                // Sleep interval seconds (interruptible via SIGINT)
                for (int s=0; s<interval; ++s) {
                    struct timeval tv; tv.tv_sec=1; tv.tv_usec=0;
                    select(0,nullptr,nullptr,nullptr,&tv);
                }
            }
            _exit(0);
        } else {
            // parent: connect worker stdout to GUI
            // Ensure worker is leader of its own process group for Ctrl+C (killpg)
            setpgid(cpid, cpid);
            close(outPipe[1]);
            t.childPid = cpid; t.childPgid = cpid; t.outFd = outPipe[0]; t.errFd = -1; t.inFdWrite = -1;
            fcntl(t.outFd, F_SETFL, O_NONBLOCK);
            return;
        }
    }

    // Build pipeline without shell
    // Prepare stdin write fd state
    if (t.inFdWrite>=0) { close(t.inFdWrite); t.inFdWrite=-1; }
    auto stages = splitPipeline(cmd_line);
    if (stages.empty()) return;
    int n = (int)stages.size();
    // If it's a single stage with no explicit redirections, run under a PTY for interactive I/O
    if (n==1) {
        Redir rprobe{}; auto argvProbe = parseCmdWithRedir(stages[0], rprobe);
        if (!argvProbe.empty() && rprobe.in.empty() && rprobe.out.empty()) {
            int masterFd=-1, slaveFd=-1;
            struct winsize ws{};
            // Set a reasonable window size in characters; rows based on viewport estimated from lineH_ isn't available here; use defaults.
            ws.ws_row = 24; ws.ws_col = 80;
            if (openpty(&masterFd, &slaveFd, nullptr, nullptr, &ws)==0) {
                pid_t pid = fork();
                if (pid==0) {
                    // child attaches to slave as controlling TTY
                    close(masterFd);
                    setsid();
                    ioctl(slaveFd, TIOCSCTTY, 0);
                    // Put terminal in canonical mode with echo so std::cin prompts behave like a normal TTY
                    struct termios tio{};
                    if (tcgetattr(slaveFd, &tio) == 0) {
                        tio.c_lflag |= (ICANON | ECHO);
                        tio.c_iflag |= (ICRNL);
                        tio.c_oflag |= (OPOST | ONLCR);
                        tio.c_cc[VMIN] = 1; tio.c_cc[VTIME] = 0;
                        tcsetattr(slaveFd, TCSANOW, &tio);
                    }
                    dup2(slaveFd, STDIN_FILENO);
                    dup2(slaveFd, STDOUT_FILENO);
                    dup2(slaveFd, STDERR_FILENO);
                    close(slaveFd);
                    auto expanded_argv = expandGlobs(argvProbe);
                    std::vector<char*> cargv; for (auto& s: expanded_argv) cargv.push_back(const_cast<char*>(s.c_str())); cargv.push_back(nullptr);
                    execvp(cargv[0], cargv.data());
                    std::string msg = std::string(cargv[0]) + ": " + (errno==ENOENT?"command not found":strerror(errno)) + "\n";
                    (void)!write(STDERR_FILENO, msg.c_str(), msg.size());
                    _exit(127);
                } else if (pid>0) {
                    // parent connects master side to UI
                    close(slaveFd);
                    t.childPid = pid;
                    t.childPgid = pid;
                    t.outFd = masterFd; t.errFd = -1; t.inFdWrite = masterFd;
                    fcntl(t.outFd, F_SETFL, O_NONBLOCK);
                    return;
                } else {
                    // fork failed
                    close(masterFd); close(slaveFd);
                }
            }
            // Fallthrough to pipe-based execution if PTY path fails
        }
    }
    // For now we only attach stdout/stderr of the LAST stage to GUI; intermediate stages run to/from pipes
    // Create pipes between stages
    std::vector<int> pipesFD; pipesFD.resize((n-1)*2, -1);
    for (int i=0;i<n-1;i++) { int fds[2]; if (pipe(fds)<0) { t.appendOutput("pipe() failed\n"); return; } pipesFD[i*2]=fds[0]; pipesFD[i*2+1]=fds[1]; }

    // Optional interactive stdin for stage 0 when no explicit '<'
    int stdinPipe[2] = {-1,-1}; bool haveInteractiveStdin = false;
    {
        Redir r0{}; parseCmdWithRedir(stages[0], r0);
        if (r0.in.empty()) {
            if (pipe(stdinPipe) == 0) {
                haveInteractiveStdin = true;
                // We'll keep stdinPipe[1] open in parent as t.inFdWrite and dup stdinPipe[0] to stage 0 STDIN
            }
        }
    }

    int outPipe[2]; int errPipe[2];
    if (pipe(outPipe)<0 || pipe(errPipe)<0) { t.appendOutput("pipe() failed\n"); for(size_t k=0;k<pipesFD.size();++k) if(pipesFD[k]>=0) close(pipesFD[k]); return; }

    pid_t lastPid = -1;
    pid_t firstPid = -1;
    for (int i=0;i<n;i++) {
        Redir rinfo{}; auto argv = parseCmdWithRedir(stages[i], rinfo);
        if (argv.empty()) { t.appendOutput("invalid command\n"); return; }
        pid_t pid = fork();
        if (pid<0) { t.appendOutput("fork() failed\n"); return; }
        if (pid==0) {
            // child
            if (i==0) setpgid(0, 0);
            else setpgid(0, firstPid);
            // stdin
            if (!rinfo.in.empty()) {
                int fd = open(rinfo.in.c_str(), O_RDONLY);
                if (fd>=0) { dup2(fd, STDIN_FILENO); close(fd); }
                else { std::string msg = rinfo.in + ": " + strerror(errno) + "\n"; (void)!write(STDERR_FILENO, msg.c_str(), msg.size()); _exit(1); }
            } else if (i==0 && haveInteractiveStdin) {
                dup2(stdinPipe[0], STDIN_FILENO);
            } else if (i>0) {
                dup2(pipesFD[(i-1)*2], STDIN_FILENO);
            }
            // stdout
            if (!rinfo.out.empty()) {
                int fd = open(rinfo.out.c_str(), rinfo.append?(O_WRONLY|O_CREAT|O_APPEND):(O_WRONLY|O_CREAT|O_TRUNC), 0666);
                if (fd>=0) { dup2(fd, STDOUT_FILENO); close(fd); }
                else { std::string msg = rinfo.out + ": " + strerror(errno) + "\n"; (void)!write(STDERR_FILENO, msg.c_str(), msg.size()); _exit(1); }
            } else if (i<n-1) {
                dup2(pipesFD[i*2+1], STDOUT_FILENO);
            } else {
                dup2(outPipe[1], STDOUT_FILENO);
            }
            // stderr
            if (!rinfo.errOut.empty()) {
                int fd = open(rinfo.errOut.c_str(), rinfo.errAppend?(O_WRONLY|O_CREAT|O_APPEND):(O_WRONLY|O_CREAT|O_TRUNC), 0666);
                if (fd>=0) { dup2(fd, STDERR_FILENO); close(fd); }
                else { std::string msg = rinfo.errOut + ": " + strerror(errno) + "\n"; (void)!write(STDERR_FILENO, msg.c_str(), msg.size()); _exit(1); }
            } else {
                // stderr: route all stages' stderr to GUI error pipe
                dup2(errPipe[1], STDERR_FILENO);
            }

            // close all pipe fds
            for (size_t k=0;k<pipesFD.size();++k) if (pipesFD[k]>=0) close(pipesFD[k]);
            if (stdinPipe[0]>=0) close(stdinPipe[0]);
            if (stdinPipe[1]>=0) close(stdinPipe[1]);
            close(outPipe[0]); close(outPipe[1]); close(errPipe[0]); close(errPipe[1]);

            // Expand globs
            auto expanded_argv = expandGlobs(argv);
            // exec
            std::vector<char*> cargv; for (auto& s: expanded_argv) cargv.push_back(const_cast<char*>(s.c_str())); cargv.push_back(nullptr);
            execvp(cargv[0], cargv.data());
            // If exec fails, report
            {
                std::string msg = std::string(cargv[0]) + ": " + (errno==ENOENT?"command not found":strerror(errno)) + "\n";
                (void)!write(STDERR_FILENO, msg.c_str(), msg.size());
            }
            _exit(127);
        } else {
            lastPid = pid;
            if (i==0) firstPid = pid;
        }
    }
    // parent
    for (size_t k=0;k<pipesFD.size();++k) if (pipesFD[k]>=0) close(pipesFD[k]);
    if (stdinPipe[0]>=0) close(stdinPipe[0]);
    close(outPipe[1]); close(errPipe[1]);
    t.childPid = lastPid; // track last stage
    t.childPgid = firstPid;
    t.outFd = outPipe[0]; t.errFd = errPipe[0];
    // We don't support interactive typing into running processes anymore; close write end here.
    if (haveInteractiveStdin) { t.inFdWrite = -1; close(stdinPipe[1]); }
    fcntl(t.outFd, F_SETFL, O_NONBLOCK);
    fcntl(t.errFd, F_SETFL, O_NONBLOCK);
    if (background) {
        BackgroundJob bj {t.childPid, t.childPgid, t.outFd, t.errFd, cmd_line};
        t.backgroundJobs.push_back(bj);
        t.childPid = -1;
        t.childPgid = -1;
        t.outFd = -1;
        t.errFd = -1;
        t.inFdWrite = -1;
        append_sep_if_queued(t);
        runNextCommand(t);
    }
}

} // namespace myterm
