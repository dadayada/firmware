################################################################################
#
# fpvcam
#
# Sensor and UVC camera to RTSP, with a settings page. See src/fpvcam.h.
#
################################################################################

# What is downloaded is not fpvcam: its source is src/ in this package. It is
# OpenIPC/research, for the SigmaStar MI headers under star/include/, which
# this tree does not carry. The version is therefore that repository's commit,
# pinned in full, and bumping it changes the headers and nothing else.
FPVCAM_VERSION = 30e6fa0a546e218f0a83eae6e48c17f91a76081c
FPVCAM_SITE = $(call github,openipc,research,$(FPVCAM_VERSION))
FPVCAM_LICENSE = MIT

# "jpeg" is buildroot's virtual package: libjpeg-turbo where the CPU has
# SIMD, which is what makes the MJPEG fallback for the USB camera affordable.
# The osdrv package is a dependency for its libmi_*.so, which the program is
# linked against straight from that package's files/lib.
FPVCAM_DEPENDENCIES = jpeg sigmastar-osdrv-infinity6e

define FPVCAM_BUILD_CMDS
	$(MAKE) $(TARGET_CONFIGURE_OPTS) -C $(FPVCAM_PKGDIR)/src \
		O=$(@D)/build \
		SDK_INC=$(@D)/star/include/infinity6e \
		SDK_LIB=$(SIGMASTAR_OSDRV_INFINITY6E_PKGDIR)/files/lib
endef

define FPVCAM_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/build/fpvcam $(TARGET_DIR)/usr/bin/fpvcam
	$(INSTALL) -D -m 0755 $(FPVCAM_PKGDIR)/files/S95fpvcam $(TARGET_DIR)/etc/init.d/S95fpvcam
endef

$(eval $(generic-package))
