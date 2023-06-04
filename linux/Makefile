CC = gcc
CFLAGS = -Wall -Wextra
LDFLAGS = -lavformat -lavcodec -lavutil -lswscale -lswresample -lSDL2 -lpthread

TARGET = pixelflix

$(TARGET): pixelflix-v0.0.2.5.c
	$(CC) -g $(CFLAGS) $^ -o $@ $(LDFLAGS)

clean:
	rm -f $(TARGET)
