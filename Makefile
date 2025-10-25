TARGET	= mpudp.out
CC		= g++
CPPFLAGS	= -Wall -O3
OBJS	= main.o network.o print.o client.o server.o

$(TARGET): $(OBJS) Makefile
	$(CC) $(OBJS) -g -pthread -o $@

%.o: %.cpp Makefile
	$(CC) $(CPPFLAGS) -g -c -o $@ $<

.PHONY: all
all:
	$(MAKE)	$(TARGET)

.PHONY: clean
clean:
	rm $(TARGET)
	rm *.o
