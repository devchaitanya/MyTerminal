CXX = g++
CXXFLAGS = -std=gnu++17 -Wall -Wextra -O2 -g -DUSE_PANGO_CAIRO
PANGO_CFLAGS = $(shell pkg-config --cflags pangocairo 2>/dev/null)
PANGO_LIBS = $(shell pkg-config --libs pangocairo 2>/dev/null)
LIBS = -lX11 $(PANGO_LIBS)

SRC = \
	src/gui/TerminalWindow.cpp \
	src/gui/Tab.cpp \
	src/core/CommandExecutor.cpp \
	src/core/History.cpp \
	src/app/main.cpp

INC = -Iinclude

myshell: $(SRC)
	$(CXX) $(CXXFLAGS) $(PANGO_CFLAGS) -o $@ $(SRC) $(INC) $(LIBS)

clean:
	rm -f myshell
