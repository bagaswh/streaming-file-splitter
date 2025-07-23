all: main.c
	clang -Wall -Wextra -Werror --std=c23 -O2 -g -static main.c -o build/main