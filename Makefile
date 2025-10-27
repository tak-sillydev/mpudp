TARGET	= mpudp.out
CC		= g++
CPPFLAGS	= -Wall -O3 -g
OBJS	= main.o network.o print.o client.o server.o mpudp.o
INCS	= network.h server.h print.h ringbuf.h mpudp.h

$(TARGET): $(OBJS) Makefile
	$(CC) $(OBJS) -g -pthread -o $@

%.o: %.cpp $(INCS) Makefile
	$(CC) $(CPPFLAGS) -c -o $@ $<

.PHONY: all
all:
	$(MAKE)	$(TARGET)

.PHONY: clean
clean:
	rm $(TARGET)
	rm *.o
