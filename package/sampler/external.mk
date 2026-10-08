################################################################################
#
# sampler
#
################################################################################

SAMPLER_VERSION = 1.0
SAMPLER_SITE = ../package/sampler/src
SAMPLER_SITE_METHOD = local
SAMPLER_DEPENDENCIES += sampler-net sampler-ui

define SAMPLER_INSTALL_TARGET_CMDS
	$(INSTALL) -m 0755 -D $(@D)/S99sampler $(TARGET_DIR)/etc/init.d/S99sampler
endef

$(eval $(generic-package))
