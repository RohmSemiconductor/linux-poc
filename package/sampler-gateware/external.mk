################################################################################
#
# sampler gateware
#
################################################################################

SAMPLER_GATEWARE_VERSION = beaglev-adc-fpga-v1.0
SAMPLER_GATEWARE_SITE = https://github.com/RohmSemiconductor/linux-poc.git
SAMPLER_GATEWARE_SITE_METHOD = git

define SAMPLER_GATEWARE_INSTALL_TARGET_CMDS
	unzip -u $(@D)/LinuxProgramming.zip -d $(TARGET_DIR)
endef

$(eval $(generic-package))
