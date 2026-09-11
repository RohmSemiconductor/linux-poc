################################################################################
#
# sampler
#
################################################################################

SAMPLER_VERSION = 1.0
SAMPLER_SITE = ../sampler/src
SAMPLER_SITE_METHOD = local

define SAMPLER_INSTALL_TARGET_CMDS
	$(INSTALL) -m 0755 -D $(@D)/setupiio $(TARGET_DIR)/bin/setupiio
endef

$(eval $(generic-package))
