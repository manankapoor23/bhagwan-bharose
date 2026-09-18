CC = clang

CFLAGS = -Wall -Wextra -Wpedantic \
         -O2 \
         -I./src/motion

FRAMEWORKS = \
    -framework Cocoa \
    -framework IOKit \
    -framework CoreFoundation

SOURCES = \
    src/app/main.m \
    src/motion/imu.c

TARGET = bhagwan-bharose

all:
	$(CC) $(CFLAGS) $(SOURCES) $(FRAMEWORKS) -lm -o $(TARGET)

run: all
	sudo ./$(TARGET)

clean:
	rm -f $(TARGET)
