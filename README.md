# MyTerminal - Custom Terminal Emulator

MyTerminal is a lightweight graphical terminal application implemented with X11/Xlib, optionally accelerated by Pango/Cairo for robust UTF-8 rendering. It provides a shell-like environment supporting pipelines, redirections, history, tabs, background jobs, and a custom `multiWatch` command that executes multiple commands in parallel and streams their outputs with timestamps.

## Features

### Core Functionality
- **Graphical User Interface**: X11-based window with multiple tabs, each running an independent shell session.
- **Command Execution**: Support for external commands, pipelines (`|`), and redirections (`<`, `>`, `2>`).
- **Multiline Unicode Input**: Handles multiline input with unmatched quotes/backslashes and preserves Unicode encoding.
- **Background Jobs**: Detach jobs with Ctrl+Z; list with `bgpids`; kill with `killprocess`.
- **Signal Handling**: Ctrl+C interrupts foreground jobs; proper process group management.

### Advanced Features
- **multiWatch Command**: Executes multiple commands in parallel per period, streams outputs with UNIX timestamps and headers, using temp FIFOs per child PID. Cleans up on Ctrl+C and exit.
- **Shell History**: Persistent history of up to 10,000 commands; `history` command; Ctrl+R for inline search (exact match or longest substring).
- **Autocomplete**: Tab key for built-in commands, executables, and file paths. For files: single match completion, longest prefix for multiples, numbered selection prompt.
- **Line Editing**: Basic input editing; Ctrl+A (start of line) and Ctrl+E (end of line).
- **ANSI Rendering**: Colored output with optional Pango/Cairo for UTF-8 shaping.

## Prerequisites

- **Operating System**: Linux with X11.
- **Dependencies**:
  - X11 development libraries: `libx11-dev`
  - Optional: Pango/Cairo for Unicode rendering: `libpangocairo-1.0-0`, `libpango1.0-dev`
- **Compiler**: GCC with C++17 support.
- **Build Tools**: Make or CMake.

Install dependencies (Ubuntu/Debian):
```bash
sudo apt-get update
sudo apt-get install build-essential libx11-dev libpangocairo-1.0-0 libpango1.0-dev cmake
```

## Installation and Build

Clone the repository and navigate to the project directory.

### Build with Make
```bash
make          # Builds with X11; enables Pango/Cairo if available
make clean
```

### Build with CMake
```bash
cmake -S . -B build
cmake --build build
```

To disable Pango/Cairo, define `USE_PANGO_CAIRO=OFF` in CMake or modify Makefile accordingly.

## Usage

Run the terminal:
```bash
./myshell
```

### Example Commands

- External commands:
  ```bash
  ls -l
  gcc -o prog prog.c
  ./prog
  ```

- Pipelines and redirections:
  ```bash
  ls | grep txt > files.txt
  cat input.txt | sort | uniq > output.txt
  ```

- Multiline input:
  ```bash
  echo "Hello \
  World"
  ```

- multiWatch:
  ```bash
  multiWatch 10 ["date", "uptime"]
  multiWatch ["ps aux", "df -h"]
  ```

- Background jobs:
  ```bash
  sleep 100  # Then Ctrl+Z to background
  bgpids
  killprocess 1234
  ```

- History:
  ```bash
  history
  history clear
  # Ctrl+R for search
  ```

- Autocomplete:
  ```bash
  ls abc  # Press Tab: completes to abc.txt if single match
  ls def  # Press Tab: shows 1. def.txt 2. def.log, wait for number
  ```

### Keyboard Shortcuts

- **Ctrl+C**: Interrupt current foreground command.
- **Ctrl+Z**: Detach current job to background.
- **Tab**: Autocomplete commands/files.
- **Ctrl+A**: Move cursor to start of line.
- **Ctrl+E**: Move cursor to end of line.
- **Ctrl+R**: Search history.
- **Arrow Keys**: Basic navigation.
- **Backspace/Delete**: Edit text.

## Built-in Commands

MyTerminal includes several built-in commands for enhanced functionality beyond standard shell commands. These are handled internally and provide features like job management, history, and parallel monitoring.

### multiWatch
**Syntax**: `multiWatch [interval] ["cmd1", "cmd2", ...]` or `multiWatch [interval] cmd1 cmd2 ...`  
**Description**: Runs multiple commands in parallel each period, streaming their outputs with UNIX timestamps and formatted headers. Uses temporary FIFOs for each child process. Interval defaults to 5 seconds if omitted.  
**Examples**:
```bash
multiWatch 10 ["date", "uptime"]  # Run date and uptime every 10 seconds
multiWatch ["ps aux", "df -h"]    # Monitor processes and disk usage every 5 seconds
```
**Features**: Parallel execution, timestamped output, cleanup on Ctrl+C.

### bgpids
**Syntax**: `bgpids`  
**Description**: Lists all active background job PIDs with their command names.  
**Example**:
```bash
bgpids  # Shows: 1234: sleep 100
```

### killprocess
**Syntax**: `killprocess [-9] PID [PID ...]`  
**Description**: Sends SIGTERM (or SIGKILL if -9) to the specified process IDs.  
**Examples**:
```bash
killprocess 1234        # Terminate process 1234
killprocess -9 1234 5678  # Force kill multiple processes
```

### echo
**Syntax**: `echo [args ...]`  
**Description**: Prints arguments to stdout, separated by spaces.  
**Example**:
```bash
echo Hello World  # Outputs: Hello World
```

### history
**Syntax**: `history` or `history clear`  
**Description**: Shows the most recent commands from history (up to 1000 displayed). `clear` removes all history.  
**Examples**:
```bash
history      # Show recent commands
history clear  # Clear all history
```

### cd
**Syntax**: `cd [directory]`  
**Description**: Changes the current working directory. Defaults to home if no argument.  
**Examples**:
```bash
cd /home/user  # Change to /home/user
cd             # Change to home directory
```

### clear
**Syntax**: `clear`  
**Description**: Clears the terminal screen.  
**Example**:
```bash
clear  # Clear the display
```

### pwd
**Syntax**: `pwd`  
**Description**: Prints the current working directory.  
**Example**:
```bash
pwd  # Outputs: /home/user
```

## Project Structure

```
MyTerminal/
├── src/
│   ├── app/
│   │   └── main.cpp              # Entry point with exit cleanup
│   ├── core/
│   │   ├── CommandExecutor.cpp   # Command parsing, execution, multiWatch
│   │   └── History.cpp           # History persistence and search
│   └── gui/
│       ├── TerminalWindow.cpp    # X11 GUI, event loop, rendering
│       └── Tab.cpp               # Tab utilities
├── include/
│   └── gui/
│       ├── TerminalWindow.hpp    # GUI headers
│       └── Tab.hpp               # Tab state
├── temp/                         # Runtime FIFOs for multiWatch
├── design.tex                    # Detailed design document (LaTeX)
├── DESIGNDOC                     # Per-feature design notes
├── Makefile                      # Build script
├── Makefile.nopango              # Build script without Pango/Cairo
├── CMakeLists.txt                # CMake build
├── README.md                     # This file
└── build/                        # CMake build directory
```