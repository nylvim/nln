main: main.c
	gcc -std=c23 -Wall -Wextra -Wno-stringop-truncation -O2 -o nln main.c

run: main
	./nln

clean:
	rm -f nln
