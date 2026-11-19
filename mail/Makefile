CC ?= cc
CFLAGS ?= -O2 -pipe
CFLAGS += -Wall -Wextra -std=gnu11 -pthread -D_GNU_SOURCE
LDLIBS = -lcrypt -lcurl -pthread
OBJ = main.o util.o mime.o store.o smtp.o web.o

sekmaild: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

$(OBJ): util.h mime.h store.h smtp.h

clean:
	rm -f sekmaild $(OBJ)
