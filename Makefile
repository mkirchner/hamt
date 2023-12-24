BUILD_DIR ?= ./build
SRC_DIRS ?= ./src ./test ./include
INC_DIRS := $(shell find $(SRC_DIRS) -type d)
INC_FLAGS := $(addprefix -I,$(INC_DIRS))

LIB_SRCS := \
	src/hamt.c \
	src/murmur3.c \
	src/uh.c

LIB_OBJS := $(LIB_SRCS:%=$(BUILD_DIR)/%.o)
LIB_DEPS := $(LIB_OBJS:.o=.d)

TEST_HAMT_SRCS := \
	src/murmur3.c \
	src/uh.c \
	test/test_hamt.c \
	test/utils.c \
	test/words.c

TEST_HAMT_OBJS := $(TEST_HAMT_SRCS:%=$(BUILD_DIR)/%.o)
TEST_HAMT_DEPS := $(TEST_HAMT_OBJS:.o=.d)

TEST_MURMUR_SRCS := test/test_murmur.c

TEST_MURMUR_OBJS := $(TEST_MURMUR_SRCS:%=$(BUILD_DIR)/%.o)
TEST_MURMUR_DEPS := $(TEST_MURMUR_OBJS:.o=.d)

CPPFLAGS ?= $(INC_FLAGS) -MMD -MP -O0  -g # -DWITH_TABLE_CACHE -DWITH_TABLE_CACHE_STATS 

lib: $(BUILD_DIR)/src/libhamt.dylib

$(BUILD_DIR)/src/libhamt.dylib: $(LIB_OBJS)
	$(CC) $(LIB_OBJS) -dynamiclib -o $@

build_test: $(BUILD_DIR)/test/test_hamt $(BUILD_DIR)/test/test_murmur

test: build_test
	$(BUILD_DIR)/test/test_murmur
	$(BUILD_DIR)/test/test_hamt

$(BUILD_DIR)/test/test_hamt: $(TEST_HAMT_OBJS)
	$(CC) $(TEST_HAMT_OBJS) -o $@ $(LDFLAGS)

$(BUILD_DIR)/test/test_murmur: $(TEST_MURMUR_OBJS)
	$(CC) $(TEST_MURMUR_OBJS) -o $@ $(LDFLAGS)

# c source
$(BUILD_DIR)/%.c.o: %.c
	$(MKDIR_P) $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

.PHONY: clean

clean:
	$(RM) -r $(BUILD_DIR)

-include $(LIB_DEPS)
-include $(TEST_HAMT_DEPS)
-include $(TEST_MURMUR_DEPS)

MKDIR_P ?= mkdir -p

