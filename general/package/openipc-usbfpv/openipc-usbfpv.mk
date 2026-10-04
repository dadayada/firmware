################################################################################
#
# openipc-usbfpv
#
# Profile files for a majestic-free FPV image: fpvcam on the sensor and on a
# UVC camera, MSP forwarded to the ground by msposd.
#
################################################################################

OPENIPC_USBFPV_VERSION = 1.0
OPENIPC_USBFPV_SITE_METHOD = local
OPENIPC_USBFPV_SITE = $(BR2_EXTERNAL_GENERAL_PATH)/package/openipc-usbfpv
OPENIPC_USBFPV_LICENSE = MIT

OPENIPC_USBFPV_DEPENDENCIES = fpvcam msposd

# Nothing here may share a path with another package's install. With
# per-package directories the final tree is assembled in alphabetical package
# order, so a dependency gives no say in who wins a collision: this profile's
# v4l2rtspserver.conf lost to v4l2rtspserver's default on the first image.
# fpvcam ships its own init script and keeps its settings in a file it writes
# itself, so nothing of its is installed from here. Nor is a module list:
# /etc/modules belongs to the shared overlay, which is copied over the target
# after every package, so the one this profile used to install never reached
# an image. S95fpvcam loads uvcvideo itself.
define OPENIPC_USBFPV_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(@D)/files/msposd.conf $(TARGET_DIR)/etc/msposd.conf
	$(INSTALL) -D -m 0755 $(@D)/files/S99msposd $(TARGET_DIR)/etc/init.d/S99msposd
endef

$(eval $(generic-package))
