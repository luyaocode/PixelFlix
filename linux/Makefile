CC = gcc
CFLAGS = -Wall -Wextra -g
LDFLAGS = -lavformat -lavcodec -lavutil -lswscale -lswresample -lSDL2 -lpthread

TARGET = pixelflix

$(TARGET): pixelflix-v0.0.3.c
	$(CC) -g $(CFLAGS) $^ -o $@ $(LDFLAGS)

clean:
	rm -f $(TARGET)
