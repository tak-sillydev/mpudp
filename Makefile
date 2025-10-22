TARGET	= mpudp.out
CC		= g++
CPPFLAGS	= -Wall
OBJS	= main.o network.o print.o client.o server.o

$(TARGET): $(OBJS) Makefile
	$(CC) $(OBJS) -g -o $@

%.o: %.cpp Makefile
	$(CC) $(CPPFLAGS) -g -c -o $@ $<

.PHONY: all
all:
	$(MAKE)	$(TARGET)

.PHONY: clean
clean:
	rm $(TARGET)
	rm *.o
