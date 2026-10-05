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

# No _DEPENDENCIES on fpvcam or msposd, on purpose: Config.in selects them,
# which is what puts them in the image. A build dependency would also seed
# this package's per-package directory with a copy of their files, and that
# copy is merged into the final tree after theirs ("o" sorts after "f"). When
# fpvcam alone was rebuilt, the image kept shipping the old binary from here:
# two images in a row were flashed with fixes that were not in them.

# Nothing here may share a path with another package's install. With
# per-package directories the final tree is assembled in alphabetical package
# order, so a dependency gives no say in who wins a collision: this profile's
# v4l2rtspserver.conf lost to v4l2rtspserver's default on the first image.
# fpvcam ships its own init script and keeps its settings in a file it writes
# itself; what is installed for it from here is fpvcam.defaults, the values
# that are facts about this board, which fpvcam reads and never writes. No
# module list is installed:
# /etc/modules belongs to the shared overlay, which is copied over the target
# after every package, so the one this profile used to install never reached
# an image. S95fpvcam loads uvcvideo itself.
define OPENIPC_USBFPV_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(OPENIPC_USBFPV_PKGDIR)/files/msposd.conf $(TARGET_DIR)/etc/msposd.conf
	$(INSTALL) -D -m 0644 $(OPENIPC_USBFPV_PKGDIR)/files/fpvcam.defaults $(TARGET_DIR)/etc/fpvcam.defaults
	$(INSTALL) -D -m 0755 $(OPENIPC_USBFPV_PKGDIR)/files/S99msposd $(TARGET_DIR)/etc/init.d/S99msposd
endef

$(eval $(generic-package))

# Installed straight from this directory, and reinstalled when a file in it
# changes: the version above never moves, and buildroot would otherwise keep
# the copy it took on the first build. See the same rule in fpvcam.mk.
$(OPENIPC_USBFPV_TARGET_INSTALL_TARGET): $(wildcard $(OPENIPC_USBFPV_PKGDIR)/files/*)
