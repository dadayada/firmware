################################################################################
#
# v4l2loopback-openipc
#
################################################################################

# v0.12.7, pinned by commit. Same version buildroot 2024.02 ships.
V4L2LOOPBACK_OPENIPC_VERSION = 1ecf810f0d687b647caa3050ae30cf51b5902afd
V4L2LOOPBACK_OPENIPC_SITE = $(call github,umlaeute,v4l2loopback,$(V4L2LOOPBACK_OPENIPC_VERSION))
# Named after the upstream repository, not this package, so the hash file
# and the download cache carry the same tarball name GitHub serves.
V4L2LOOPBACK_OPENIPC_SOURCE = v4l2loopback-$(V4L2LOOPBACK_OPENIPC_VERSION).tar.gz
V4L2LOOPBACK_OPENIPC_LICENSE = GPL-2.0+
V4L2LOOPBACK_OPENIPC_LICENSE_FILES = COPYING

# No LINUX_CONFIG_FIXUPS on purpose: see Config.in. The board kernel config
# is the one place that decides whether the V4L2 core is built in or a module.

$(eval $(kernel-module))
$(eval $(generic-package))
