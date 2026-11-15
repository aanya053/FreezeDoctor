CXX = g++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror
.PHONY: all demo test clean
all: build/freezedoctor
build/freezedoctor: src/freezedoctor.cpp
	mkdir -p build
	$(CXX) $(CXXFLAGS) $< -o $@
demo: all
	mkdir -p results
	./build/freezedoctor --report results/report.json --csv results/samples.csv
test: all
	python3 -m unittest discover -s tests -v
clean:
	rm -rf build results
