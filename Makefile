CC = gcc
CFLAGS = -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror
LDLIBS = -lm
.PHONY: all demo test clean
all: build/freezedoctor
build/freezedoctor: src/freezedoctor.c
	mkdir -p build
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)
demo: all
	mkdir -p results
	./build/freezedoctor --report results/report.json --csv results/samples.csv
test: all
	python3 -m unittest discover -s tests -v
clean:
	rm -rf build results
