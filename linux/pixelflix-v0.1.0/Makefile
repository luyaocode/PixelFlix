CC = gcc
CFLAGS = -Wall -Wextra -g
LDFLAGS = -lavformat -lavcodec -lavutil -lswscale -lswresample -lSDL2 -lpthread

SRC=$(wildcard *.c */*.c)
TARGET = pixelflix
$(TARGET): $(SRC)
	$(CC) -g $(CFLAGS) $^ -o $@ $(LDFLAGS)

clean:
	rm -f $(TARGET)
