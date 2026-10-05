# Builds fpvcam out of tree. The package's .mk passes:
#   O        where objects and the binary go
#   SDK_INC  the SigmaStar MI headers for this SoC family
#   SDK_LIB  the directory holding the libmi_*.so the image ships
#
# The MI libraries are linked by name rather than dlopen()ed. --no-as-needed
# matters: libcus3a and libispalgo are never called from here, but libmi_isp
# resolves its 3A algorithms from them at run time and names neither as a
# dependency of its own, so a linker that drops "unused" libraries leaves an
# ISP with no auto-exposure.

SRC := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
O ?= $(SRC)build

OBJS := main.o config.o sdk.o usb.o night.o rtsp.o http.o page.o
MI_LIBS := -lmi_isp -lmi_sensor -lmi_sys -lmi_venc -lmi_vif -lmi_vpe \
	-lcus3a -lispalgo -lcam_os_wrapper

# Kept apart from CFLAGS: buildroot passes CFLAGS on the command line, and a
# command-line variable silently wins over anything a makefile adds to it.
FPV_CFLAGS := -std=gnu99 -Wall -Wextra -I$(SDK_INC)

all: $(O)/fpvcam

$(O)/fpvcam: $(addprefix $(O)/,$(OBJS))
	$(CC) $(CFLAGS) $(FPV_CFLAGS) $(LDFLAGS) -o $@ $^ -L$(SDK_LIB) \
		-Wl,--no-as-needed $(MI_LIBS) -Wl,--as-needed -ljpeg -lpthread -ldl

# The vendor headers are not ours to fix, so their warnings are not shown.
$(O)/sdk.o: FPV_CFLAGS += -isystem $(SDK_INC)

$(O)/%.o: $(SRC)%.c $(SRC)fpvcam.h | $(O)
	$(CC) $(CFLAGS) $(FPV_CFLAGS) -c -o $@ $<

$(O)/page.o: $(SRC)page.S $(SRC)page.html | $(O)
	$(CC) -c -DPAGE_FILE='"$(SRC)page.html"' -o $@ $<

$(O):
	mkdir -p $@

.PHONY: all
