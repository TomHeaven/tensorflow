TARGET_TOOLCHAIN_ROOT := %{TARGET_TOOLCHAIN_ROOT}%
TARGET_TOOLCHAIN_PREFIX := %{TARGET_TOOLCHAIN_PREFIX}%

# These are microcontroller-specific rules for converting the ELF output
# of the linker into a binary image that can be loaded directly.
CXX             := '$(TARGET_TOOLCHAIN_ROOT)$(TARGET_TOOLCHAIN_PREFIX)g++'
CC              := '$(TARGET_TOOLCHAIN_ROOT)$(TARGET_TOOLCHAIN_PREFIX)gcc'
AS              := '$(TARGET_TOOLCHAIN_ROOT)$(TARGET_TOOLCHAIN_PREFIX)as'
AR              := '$(TARGET_TOOLCHAIN_ROOT)$(TARGET_TOOLCHAIN_PREFIX)ar' -r
LD              := '$(TARGET_TOOLCHAIN_ROOT)$(TARGET_TOOLCHAIN_PREFIX)ld'
NM              := '$(TARGET_TOOLCHAIN_ROOT)$(TARGET_TOOLCHAIN_PREFIX)nm'
OBJDUMP         := '$(TARGET_TOOLCHAIN_ROOT)$(TARGET_TOOLCHAIN_PREFIX)objdump'
OBJCOPY         := '$(TARGET_TOOLCHAIN_ROOT)$(TARGET_TOOLCHAIN_PREFIX)objcopy'
SIZE            := '$(TARGET_TOOLCHAIN_ROOT)$(TARGET_TOOLCHAIN_PREFIX)size'

RM = rm -f
ARFLAGS := -cs
SRCS := \
%{SRCS}%

OBJS := \
$(patsubst %.cc,%.o,$(patsubst %.c,%.o,$(SRCS)))

CXXFLAGS += %{CXX_FLAGS}%
CCFLAGS += %{CC_FLAGS}%

LDFLAGS += %{LINKER_FLAGS}%


# library to be generated
MICROLITE_LIB = libtensorflow-microlite.a

%.o: %.cc
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

%.o: %.c
	$(CC) $(CCFLAGS) $(INCLUDES) -c $< -o $@

%{EXECUTABLE}% : $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS) $(LDFLAGS)


# Gathers together all the objects we've compiled into a single '.a' archive.
$(MICROLITE_LIB): tensorflow/lite/schema/schema_generated.h $(OBJS)
	@mkdir -p $(dir $@)
	$(AR) $(ARFLAGS) $(MICROLITE_LIB) $(OBJS)

all: %{EXECUTABLE}%

lib: $(MICROLITE_LIB)

clean:
	-$(RM) $(OBJS)
	-$(RM) %{EXECUTABLE}%
	-$(RM) MICROLITE_LIB_PATH
