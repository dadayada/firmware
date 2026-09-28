################################################################################
#
# v4l2rtspserver
#
################################################################################

# v0.3.13, pinned by commit.
V4L2RTSPSERVER_VERSION = fafc8cbd1c93bf0f7453efa8b7718c68941fd5eb
V4L2RTSPSERVER_SITE = $(call github,mpromonet,v4l2rtspserver,$(V4L2RTSPSERVER_VERSION))
V4L2RTSPSERVER_LICENSE = Unlicense
V4L2RTSPSERVER_LICENSE_FILES = Unlicense.txt

# Upstream vendors its V4L2 wrapper as a git submodule and fetches live555 at
# configure time from an unversioned "latest" URL. A GitHub tarball carries no
# submodules and a build must not reach for the network, so both are pinned
# here and dropped into the tree where CMakeLists.txt expects them. live555
# comes from VideoLAN's contrib mirror, which keeps dated releases the way
# live555.com does not; it is the same source buildroot's own live555 package
# moved to.
V4L2RTSPSERVER_LIBV4L2CPP_VERSION = 4c90adc628719c89e48fbca75b53c13c96d8065b
V4L2RTSPSERVER_LIVE555_VERSION = 2025.10.13
V4L2RTSPSERVER_EXTRA_DOWNLOADS = \
	https://github.com/mpromonet/libv4l2cpp/archive/$(V4L2RTSPSERVER_LIBV4L2CPP_VERSION)/libv4l2cpp-$(V4L2RTSPSERVER_LIBV4L2CPP_VERSION).tar.gz \
	https://download.videolan.org/contrib/live555/live.$(V4L2RTSPSERVER_LIVE555_VERSION).tar.gz

define V4L2RTSPSERVER_UNPACK_BUNDLED
	rm -rf $(@D)/libv4l2cpp $(@D)/live
	mkdir -p $(@D)/libv4l2cpp
	$(TAR) --strip-components=1 -C $(@D)/libv4l2cpp \
		-xzf $(V4L2RTSPSERVER_DL_DIR)/libv4l2cpp-$(V4L2RTSPSERVER_LIBV4L2CPP_VERSION).tar.gz
	$(TAR) -C $(@D) -xzf $(V4L2RTSPSERVER_DL_DIR)/live.$(V4L2RTSPSERVER_LIVE555_VERSION).tar.gz
endef
V4L2RTSPSERVER_POST_EXTRACT_HOOKS += V4L2RTSPSERVER_UNPACK_BUNDLED

V4L2RTSPSERVER_DEPENDENCIES = host-pkgconf
V4L2RTSPSERVER_SUPPORTS_IN_SOURCE_BUILD = NO

# live555 is compiled into the binary and libstdc++ is linked statically, so
# the image carries one file and rootfs_script.sh's libstdc++ prune cannot
# break it. No pkg-config live555 is installed on purpose: finding one would
# make CMake link the shared library instead. CMAKE_DISABLE_FIND_PACKAGE_Git
# stops the configure step running `git submodule update` in a plain tarball.
V4L2RTSPSERVER_CONF_OPTS = \
	-DCMAKE_BUILD_TYPE=MinSizeRel \
	-DCMAKE_DISABLE_FIND_PACKAGE_Git=TRUE \
	-DVERSION=0.3.13 \
	-DWITH_SSL=OFF \
	-DALSA=OFF \
	-DLOG4CPP=OFF \
	-DSYSTEMD=OFF \
	-DSTATICSTDCPP=ON

define V4L2RTSPSERVER_INSTALL_TARGET_CMDS
	$(INSTALL) -m 755 -D $(V4L2RTSPSERVER_BUILDDIR)/v4l2rtspserver $(TARGET_DIR)/usr/bin/v4l2rtspserver
	$(INSTALL) -m 644 -D $(V4L2RTSPSERVER_PKGDIR)/files/v4l2rtspserver.conf $(TARGET_DIR)/etc/v4l2rtspserver.conf
	$(INSTALL) -m 755 -D $(V4L2RTSPSERVER_PKGDIR)/files/S96v4l2rtspserver $(TARGET_DIR)/etc/init.d/S96v4l2rtspserver
endef

$(eval $(cmake-package))
