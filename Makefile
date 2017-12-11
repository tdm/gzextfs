# Makefile

### Compatibility checks

LIBEXT2FS_INCLUDE_ROOT := /usr/include/ext2fs

test_file := $(LIBEXT2FS_INCLUDE_ROOT)/ext2fs.h
test_result := $(shell grep ext2fs_dirent_name_len $(test_file) | wc -l)
ifneq ($(test_result),0)
LIBEXT2_CFLAGS += -DHAVE_EXT2FS_DIRENT_NAME_LEN
endif

test_file := $(LIBEXT2FS_INCLUDE_ROOT)/ext2_io.h
test_result := $(shell grep cache_readahead $(test_file) | wc -l)
ifneq ($(test_result),0)
LIBEXT2_CFLAGS += -DHAVE_EXT2FS_CACHE_READAHEAD
endif

test_file := $(LIBEXT2FS_INCLUDE_ROOT)/ext2_io.h
test_result := $(shell grep zeroout $(test_file) | wc -l)
ifneq ($(test_result),0)
LIBEXT2_CFLAGS += -DHAVE_EXT2FS_ZEROOUT
endif

### Tools and flags

CC := gcc
CFLAGS := -g -Wall $(LIBEXT2_CFLAGS)

CXX := g++
CXXFLAGS := $(CFLAGS)

LD := g++
LDFLAGS := -g

STATIC_LIBS :=
SHARED_LIBS := -lfuse -lext2fs -lz

DESTDIR ?= /usr/local
BINDIR ?= $(DESTDIR)/bin

### Rules

MODULE := gzextfs

SRC_FILES := \
	gzextio.cxx \
	gzextfs.cxx

all: $(MODULE)
.PHONY: all

obj_dir := .obj

c_sources := $(filter %.c,$(SRC_FILES))
c_objects := $(addprefix $(obj_dir)/,$(c_sources:.c=.o))

cxx_sources := $(filter %.cxx,$(SRC_FILES))
cxx_objects := $(addprefix $(obj_dir)/,$(cxx_sources:.cxx=.o))

module_objects := $(c_objects) $(cxx_objects)

ifeq (,$(filter clean,$(MAKECMDGOALS)))
-include $(obj_dir)/*.d
endif

$(MODULE): $(module_objects)
	$(LD) $(LDFLAGS) -o $@ $^ $(STATIC_LIBS) $(SHARED_LIBS)

$(obj_dir)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -MD -MP -MF $(@D)/$(@F:.o=.d) -c -o $@ $<

$(obj_dir)/%.o: %.cxx
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -MD -MP -MF $(@D)/$(@F:.o=.d) -c -o $@ $<

.PHONY: clean
clean:
	rm -rf $(obj_dir) $(MODULE)

.PHONY: install
install: $(MODULE)
	install -m 0755 $(MODULE) $(BINDIR)
