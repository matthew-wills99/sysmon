CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=gnu11
LDLIBS  += -lm -ldl
 
SRC = main.c ui.c term.c graph.c cpu.c mem.c gpu.c disk.c net.c process.c utils.c
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