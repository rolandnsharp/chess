CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -std=c11 -D_GNU_SOURCE
PKGS    = sdl3 cairo pangocairo librsvg-2.0 libcurl alsa
GUI_CFLAGS = $(shell pkg-config --cflags $(PKGS))
GUI_LIBS   = $(shell pkg-config --libs $(PKGS)) -lpthread -lm

CORE = src/chess.c
APP  = src/main.c src/render.c src/engine.c src/ai.c src/drill.c src/puzzles.c src/review.c src/json.c src/lichess.c src/sound.c

all: chess test_chess

chess: $(APP) $(CORE) src/*.h
	$(CC) $(CFLAGS) $(GUI_CFLAGS) -o $@ $(APP) $(CORE) $(GUI_LIBS)

test_chess: tests/test_chess.c $(CORE) src/chess.h
	$(CC) $(CFLAGS) -O3 -o $@ tests/test_chess.c $(CORE)

test: test_chess
	./test_chess $(wildcard data/chesscom/all.pgn data/lichess/all.pgn)

SF_TAG = sf_17.1
SF_ARCH ?= armv8

stockfish:
	test -d third_party/Stockfish || git clone --depth 1 --branch $(SF_TAG) https://github.com/official-stockfish/Stockfish.git third_party/Stockfish
	$(MAKE) -C third_party/Stockfish/src -j4 build ARCH=$(SF_ARCH) COMP=gcc

PY = .venv/bin/python

# fetch new games, analyse the newest, rebuild drills and report
update:
	$(PY) analysis/fetch.py
	$(PY) analysis/analyse.py --games 300 --nodes 25000 --procs 4
	$(PY) analysis/threats.py --procs 4
	$(PY) analysis/report.py > /dev/null
	@echo "report: data/analysis/report.md"

clean:
	rm -f chess test_chess

.PHONY: all test clean stockfish update
