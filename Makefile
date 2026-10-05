CC = clang
CFLAGS = -Wall -Wextra -Wpedantic -std=c23 -g

TARGET = microaudio
SRC = src/main.c src/microaudio.c
OBJ = $(SRC:.c=.o)
LDLIBS = -lasound -lm

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)


%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) $(TARGET)

.PHONY: all clean
