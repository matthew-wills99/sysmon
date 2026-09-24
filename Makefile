CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=gnu11 -Iinclude
LDLIBS  += -lm -ldl

SRC = $(wildcard src/*.c)
OBJ = $(SRC:.c=.o)

sysmon: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) sysmon

install: sysmon
	install -d /usr/local/bin
	install -m 755 sysmon /usr/local/bin/sysmon

uninstall:
	rm -f /usr/local/bin/sysmon

.PHONY: clean install uninstall