CC = gcc
CFLAGS = -Wall -Wextra
LDFLAGS = -lavformat -lavcodec -lavutil -lswscale -lSDL2

TARGET = ffplayer

$(TARGET): ffplayer.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

clean:
	rm -f $(TARGET)
