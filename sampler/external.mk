################################################################################
#
# sampler
#
################################################################################

SAMPLER_VERSION = 1.0
SAMPLER_SITE = ../sampler/src
SAMPLER_SITE_METHOD = local

define SAMPLER_BUILD_CMDS
	$(MAKE) -C $(@D)
endef

# LIBIIO_WRAPPER = libiio_wrapper_but_python_shouldnt_import_this.so

define SAMPLER_INSTALL_TARGET_CMDS
	$(INSTALL) -m 0755 -D $(@D)/setupiio $(TARGET_DIR)/bin/setupiio

	# Install LibIIO wrapper
	$(INSTALL) -m 0755 -D $(@D)/*.so $(TOPDIR)/../ui/
endef

$(eval $(generic-package))
