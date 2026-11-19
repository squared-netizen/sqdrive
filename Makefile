# sqdrive: the Virtual Cartridge Drive, as a C++23 module (sq.drive) and a
# command-line tool (sqdrive).
#
#   make              libsqdrive.a, sqdrive and the tests
#   make test         build and run the tests
#   make clean
#
# Needs clang and libc++ with `import std;` (23 is what sqterm and CI use):
#
#   make CXX=/path/to/clang++
#
# Offline: sq.cart and what it builds with are vendored in
# third_party/sqcart (tools/vendor_sqcart.sh), and built here first, with
# the std module, into build/sqcart.

CXX      ?= clang++
BUILD    ?= build
OPT      ?= -O2 -g
STDLIB   := -stdlib=libc++
WARN     := -Wall -Wextra -Wpedantic -Wconversion -Wno-sign-conversion -Wno-missing-designated-field-initializers
CART_DIR := third_party/sqcart/v3
CART     := $(abspath $(BUILD))/sqcart
CXXFLAGS := -std=c++23 $(STDLIB) $(OPT) $(WARN) -fprebuilt-module-path=$(BUILD)/pcm -fprebuilt-module-path=$(CART)/pcm \
            -fmodule-file=std=$(CART)/pcm/std.pcm
LDFLAGS  := $(STDLIB) -lpthread

READ_LIBS  := $(CART)/libsqcart.a $(CART)/libsqcart-unzstd.a
WRITE_LIBS := $(CART)/libsqcart-write.a $(CART)/libsqcart.a $(CART)/libsqcart-zstd.a
CART_OUT   := $(READ_LIBS) $(CART)/libsqcart-write.a $(CART)/libsqcart-zstd.a $(CART)/pcm/sq.cart.pcm $(CART)/pcm/sq.cart.write.pcm
TESTS      := $(patsubst tests/%.cpp,$(BUILD)/%,$(wildcard tests/test_*.cpp))

.PHONY: all test clean
all: $(BUILD)/libsqdrive.a $(BUILD)/sqdrive $(TESTS)

# sq.cart, by its own Makefile, its outputs in build/sqcart.
$(CART_OUT) &: $(wildcard $(CART_DIR)/src/*) $(CART_DIR)/Makefile
	$(MAKE) -C $(CART_DIR) CXX="$(CXX)" BUILD=$(CART) OPT="$(OPT)" $(CART_OUT)

$(BUILD)/pcm/sq.drive.pcm $(BUILD)/obj/drive.o &: src/drive.cppm $(CART)/pcm/sq.cart.pcm $(CART)/pcm/sq.cart.write.pcm
	@mkdir -p $(BUILD)/pcm $(BUILD)/obj
	$(CXX) $(CXXFLAGS) -c $< -fmodule-output=$(BUILD)/pcm/sq.drive.pcm -o $(BUILD)/obj/drive.o

$(BUILD)/libsqdrive.a: $(BUILD)/obj/drive.o
	ar rcs $@ $^

# The tool writes too (through sq.cart.write).
$(BUILD)/sqdrive: tools/sqdrive.cpp $(BUILD)/pcm/sq.drive.pcm $(BUILD)/libsqdrive.a $(WRITE_LIBS)
	$(CXX) $(CXXFLAGS) $< $(BUILD)/libsqdrive.a $(WRITE_LIBS) $(LDFLAGS) -o $@

# The tests write cartridges with sq.cart.write.
$(BUILD)/test_%: tests/test_%.cpp $(BUILD)/pcm/sq.drive.pcm $(BUILD)/libsqdrive.a $(WRITE_LIBS) $(CART)/pcm/sq.cart.write.pcm
	$(CXX) $(CXXFLAGS) $< $(BUILD)/libsqdrive.a $(WRITE_LIBS) $(LDFLAGS) -o $@

test: all
	@fail=0; for t in $(TESTS); do ./$$t || fail=1; done; \
	if [ $$fail -ne 0 ]; then echo "FAILED"; exit 1; fi; echo "all tests passed"

clean:
	rm -rf $(BUILD)
