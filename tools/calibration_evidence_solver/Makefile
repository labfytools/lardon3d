CXX ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -Wconversion -Wshadow
PKG_CFLAGS := $(shell pkg-config --cflags opencv5 openssl)
PKG_LIBS := $(shell pkg-config --libs opencv5 openssl)

calibration_evidence_solver: calibration_evidence_solver.cpp
	$(CXX) $(CXXFLAGS) $(PKG_CFLAGS) $< -o $@ $(PKG_LIBS)

test: calibration_evidence_solver
	./calibration_evidence_solver --self-test

asan:
	$(CXX) -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $(PKG_CFLAGS) calibration_evidence_solver.cpp -o calibration_evidence_solver_asan $(PKG_LIBS)
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 ./calibration_evidence_solver_asan --self-test

clean:
	rm -f calibration_evidence_solver calibration_evidence_solver_asan
