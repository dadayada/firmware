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

# Listed so this installs after them: divinus and v4l2rtspserver both ship a
# default config at the same /etc path, and the profile's copy is the one the
# image has to boot with.
OPENIPC_USBFPV_DEPENDENCIES = divinus msposd v4l2rtspserver ffmpeg-openipc v4l2loopback-openipc

define OPENIPC_USBFPV_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(@D)/files/divinus.yaml $(TARGET_DIR)/etc/divinus.yaml
	$(INSTALL) -D -m 0644 $(@D)/files/msposd.conf $(TARGET_DIR)/etc/msposd.conf
	$(INSTALL) -D -m 0644 $(@D)/files/modules $(TARGET_DIR)/etc/modules
	$(INSTALL) -D -m 0644 $(@D)/files/usbtranscode.conf $(TARGET_DIR)/etc/usbtranscode.conf
	$(INSTALL) -D -m 0644 $(@D)/files/v4l2rtspserver.conf $(TARGET_DIR)/etc/v4l2rtspserver.conf
	$(INSTALL) -D -m 0755 $(@D)/files/usbtranscode $(TARGET_DIR)/usr/sbin/usbtranscode
	$(INSTALL) -D -m 0755 $(@D)/files/S93usbtranscode $(TARGET_DIR)/etc/init.d/S93usbtranscode
	$(INSTALL) -D -m 0755 $(@D)/files/S95divinus $(TARGET_DIR)/etc/init.d/S95divinus
	$(INSTALL) -D -m 0755 $(@D)/files/S99msposd $(TARGET_DIR)/etc/init.d/S99msposd
endef

$(eval $(generic-package))
