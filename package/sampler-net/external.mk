################################################################################
#
# sampler net
#
################################################################################

SAMPLER_NET_VERSION = 1.0
SAMPLER_NET_SITE = ../package/sampler-net/src
SAMPLER_NET_SITE_METHOD = local

define SAMPLER_NET_INSTALL_TARGET_CMDS
	$(INSTALL) -m 0644 -D $(@D)/interfaces $(TARGET_DIR)/etc/network/interfaces
	$(INSTALL) -m 0644 -D $(@D)/sshd_config $(TARGET_DIR)/etc/ssh/sshd_config
endef

$(eval $(generic-package))
