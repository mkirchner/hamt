BUILD_DIR ?= ./build
SRC_DIRS ?= ./src ./test ./include
INC_DIRS := $(shell find $(SRC_DIRS) -type d)
INC_FLAGS := $(addprefix -I,$(INC_DIRS))

LIB_SRCS := \
	src/mem.c \
	src/hamt.c \
	src/murmur3.c

LIB_OBJS := $(LIB_SRCS:%=$(BUILD_DIR)/%.o)
LIB_DEPS := $(LIB_OBJS:.o=.d)

TEST_SRCS := \
	src/mem.c \
	src/murmur3.c \
	test/test_hamt.c \
	test/utils.c

TEST_OBJS := $(TEST_SRCS:%=$(BUILD_DIR)/%.o)
TEST_DEPS := $(TEST_OBJS:.o=.d)

PERF_SRCS := \
	src/hamt.c \
	src/mem.c \
	src/murmur3.c \
	test/perf.c \
	test/utils.c

PERF_OBJS := $(PERF_SRCS:%=$(BUILD_DIR)/%.o)
PERF_DEPS := $(PERF_OBJS:.o=.d)

CPPFLAGS ?= $(INC_FLAGS) -MMD -MP -g -O0

lib: $(BUILD_DIR)/src/libhamt.dylib

$(BUILD_DIR)/src/libhamt.dylib: $(LIB_OBJS)
	$(CC) $(LIB_OBJS) -dynamiclib -o $@

test: $(BUILD_DIR)/test/test_hamt
	$(BUILD_DIR)/test/test_hamt

$(BUILD_DIR)/test/test_hamt: $(TEST_OBJS)
	$(CC) $(TEST_OBJS) -o $@ $(LDFLAGS)

perf: $(BUILD_DIR)/test/perf
	$(BUILD_DIR)/test/perf | tee $(BUILD_DIR)/test/perf.csv
	python test/perf.py

$(BUILD_DIR)/test/perf: $(PERF_OBJS)
	$(CC) $(PERF_OBJS) -o $@ $(LDFLAGS)

# c source
$(BUILD_DIR)/%.c.o: %.c
	$(MKDIR_P) $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

.PHONY: clean

clean:
	$(RM) -r $(BUILD_DIR)

-include $(LIB_DEPS)
-include $(TEST_DEPS)

MKDIR_P ?= mkdir -p

