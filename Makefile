CC = gcc
CFLAGS = -Wall -Wextra
LDFLAGS = -lavformat -lavcodec -lavutil -lswscale -lSDL2

TARGET = pixelflix

$(TARGET): pixelflix.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

clean:
	rm -f $(TARGET)
