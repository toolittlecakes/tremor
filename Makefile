CC      ?= clang
CFLAGS  ?= -O2
LDLIBS   = -framework CoreFoundation

build/tremor: src/tremor.c build/html_embed.h
	$(CC) $(CFLAGS) -Ibuild -o $@ src/tremor.c $(LDLIBS)

# Bake the page into a C header so the binary is fully self-contained.
build/html_embed.h: public/index.html
	mkdir -p build
	cd public && xxd -i index.html > ../build/html_embed.h

run: build/tremor
	./build/tremor

clean:
	rm -rf build

.PHONY: run clean
