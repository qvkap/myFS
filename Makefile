CC = gcc
CFLAGS = -Wall -Wextra -Wpedantic -std=gnu11 -O2 -g -D_GNU_SOURCE
LDFLAGS = -lzstd -lcrypto -luuid
INCLUDES = -Isrc

SRCDIR = src
OBJDIR = obj

SRCS = $(filter-out $(SRCDIR)/$(MOUNT_HELPER).c, $(wildcard $(SRCDIR)/*.c))
OBJS = $(SRCS:$(SRCDIR)/%.c=$(OBJDIR)/%.o)

TARGET = spermfs
MOUNT_HELPER = spermfs_mount_helper

.PHONY: all clean test

all: $(TARGET) $(MOUNT_HELPER)

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(MOUNT_HELPER): $(SRCDIR)/$(MOUNT_HELPER).c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -rf $(OBJDIR) $(TARGET) $(TARGET).img $(MOUNT_HELPER)

test: $(TARGET) $(MOUNT_HELPER)
	# Create a test image
	dd if=/dev/zero of=$(TARGET).img bs=1M count=64 2>/dev/null
	# Format it
	./$(TARGET) $(TARGET).img --mkfs --blocksize 4096 --size 16384
	# Show info
	./$(TARGET) --version
	@echo "Test image created: $(TARGET).img"
	@echo "To mount: ./$(TARGET) $(TARGET).img /mnt/spermfs -f"

test-full: $(TARGET)
	dd if=/dev/zero of=$(TARGET).img bs=1M count=256 2>/dev/null
	./$(TARGET) $(TARGET).img --mkfs --blocksize 65536 --size 4096 --compress 2
	./$(TARGET) $(TARGET).img --mkfs --blocksize 4096 --size 65536 --compress 1 --encrypt 0
	@echo "Full test image created."

valgrind: $(TARGET)
	dd if=/dev/zero of=$(TARGET).img bs=1M count=64 2>/dev/null
	./$(TARGET) $(TARGET).img --mkfs --blocksize 4096 --size 16384

debug: CFLAGS += -DDEBUG -g -O0 -fsanitize=address
debug: $(TARGET)

# Show all source files
src-list:
	@echo "Source files:"
	@for f in $(SRCS); do echo "  $$f"; done
	@echo "Object files:"
	@for f in $(OBJS); do echo "  $$f"; done
