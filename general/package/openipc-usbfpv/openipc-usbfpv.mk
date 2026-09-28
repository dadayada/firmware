################################################################################
#
# openipc-usbfpv
#
# Profile files for a majestic-free FPV image: Divinus on the sensor, a UVC
# camera transcoded by ffmpeg into v4l2loopback and served by v4l2rtspserver,
# MSP forwarded to the ground by msposd.
#
################################################################################

OPENIPC_USBFPV_VERSION = 1.0
OPENIPC_USBFPV_SITE_METHOD = local
OPENIPC_USBFPV_SITE = $(BR2_EXTERNAL_GENERAL_PATH)/package/openipc-usbfpv
OPENIPC_USBFPV_LICENSE = MIT

OPENIPC_USBFPV_DEPENDENCIES = divinus msposd v4l2rtspserver usbmjpeg ffmpeg-openipc v4l2loopback-openipc

# Nothing here may share a path with another package's install. With
# per-package directories the final tree is assembled in alphabetical package
# order, so a dependency gives no say in who wins a collision: the profile's
# v4l2rtspserver.conf lost to v4l2rtspserver's default on the first image.
# The two files that override another package's default (divinus.yaml,
# v4l2rtspserver.conf) therefore ship from overlay/ through
# scripts/late-overlays.list, which lands after every package.
define OPENIPC_USBFPV_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(@D)/files/msposd.conf $(TARGET_DIR)/etc/msposd.conf
	$(INSTALL) -D -m 0644 $(@D)/files/modules $(TARGET_DIR)/etc/modules
	$(INSTALL) -D -m 0644 $(@D)/files/usbtranscode.conf $(TARGET_DIR)/etc/usbtranscode.conf
	$(INSTALL) -D -m 0755 $(@D)/files/usbtranscode $(TARGET_DIR)/usr/sbin/usbtranscode
	$(INSTALL) -D -m 0755 $(@D)/files/S93usbtranscode $(TARGET_DIR)/etc/init.d/S93usbtranscode
	$(INSTALL) -D -m 0755 $(@D)/files/S95divinus $(TARGET_DIR)/etc/init.d/S95divinus
	$(INSTALL) -D -m 0755 $(@D)/files/S99msposd $(TARGET_DIR)/etc/init.d/S99msposd
endef

$(eval $(generic-package))
