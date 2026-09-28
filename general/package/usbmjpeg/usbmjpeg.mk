################################################################################
#
# usbmjpeg
#
# V4L2 YUYV capture -> libjpeg -> v4l2loopback MJPEG. See src/usbmjpeg.c.
#
################################################################################

USBMJPEG_VERSION = 1.0
USBMJPEG_SITE_METHOD = local
USBMJPEG_SITE = $(BR2_EXTERNAL_GENERAL_PATH)/package/usbmjpeg
USBMJPEG_LICENSE = MIT

# "jpeg" is buildroot's virtual package: libjpeg-turbo where the CPU has
# SIMD, IJG libjpeg otherwise. The program builds against either.
USBMJPEG_DEPENDENCIES = jpeg

define USBMJPEG_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -Wall -Wextra \
		-o $(@D)/usbmjpeg $(@D)/src/usbmjpeg.c -ljpeg
endef

define USBMJPEG_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/usbmjpeg $(TARGET_DIR)/usr/bin/usbmjpeg
endef

$(eval $(generic-package))
