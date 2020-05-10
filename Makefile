CC = gcc
CFLAGS = -I.
LDFLAGS = -lpthread
DEPS =
OBJS = udpgen2.o

%.o: %.c $(DEPS)
	$(CC) -c -o $@ $< $(CFLAGS)

udpgen2: $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS)

