################################################################################
#
# adcdemo ui
#
################################################################################

ADCDEMO_UI_VERSION = 1.0
ADCDEMO_UI_SITE = ../package/adcdemo-ui/src
ADCDEMO_UI_SITE_METHOD = local

ADCDEMO_UI_DEPENDENCIES = beagleVFire_adcdemo python3 python-aiohttp host-nodejs

# The mvaring Python extension is built directly with the target compiler
# (payload agnostic: the server tells the item size at run time).
# TODO: proper python-package packaging; the C sources are a copy of
# common/mvaring and common/shmem.
define ADCDEMO_UI_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -std=gnu11 -O2 -fPIC -shared \
		-I$(STAGING_DIR)/usr/include/python$(PYTHON3_VERSION_MAJOR) \
		-I$(@D)/common/mvaring/inc -I$(@D)/common/shmem/inc \
		$(TARGET_LDFLAGS) -o $(@D)/mvaring.so \
		$(@D)/py_mvaring/mvaring_ext.c \
		$(@D)/common/mvaring/src/mvaring.c \
		$(@D)/common/shmem/src/shmem.c -lrt

	$(TARGET_CC) $(TARGET_CFLAGS) -std=gnu11 -O2 \
		-I$(@D)/c-server -I$(@D)/bvf_inc \
		-I$(@D)/common/mvaring/inc -I$(@D)/common/shmem/inc \
		$(TARGET_LDFLAGS) -o $(@D)/bvf_adc_server \
		$(@D)/c-server/adc_server.c $(@D)/c-server/ws.c \
		$(@D)/common/mvaring/src/mvaring.c \
		$(@D)/common/shmem/src/shmem.c -lpthread -lrt

	$(NPM) --prefix $(@D)/react-frontend-webgl2 install
	$(NPM) --prefix $(@D)/react-frontend-webgl2 run build
endef

define ADCDEMO_UI_INSTALL_TARGET_CMDS
	$(INSTALL) -m 0755 -D $(@D)/bvf_adc_server $(TARGET_DIR)/usr/bin/bvf_adc_server
	$(INSTALL) -m 0755 -D $(@D)/adc_server.py $(TARGET_DIR)/ui/adc_server.py
	$(INSTALL) -m 0755 -D $(@D)/mvaring.so \
		$(TARGET_DIR)/usr/lib/python$(PYTHON3_VERSION_MAJOR)/site-packages/mvaring.so

	mkdir -p $(TARGET_DIR)/ui/react-frontend-webgl2/dist
	rsync -au $(@D)/react-frontend-webgl2/dist/* \
		$(TARGET_DIR)/ui/react-frontend-webgl2/dist/
	$(INSTALL) -m 0755 -D $(@D)/S50adcdemo_ui $(TARGET_DIR)/etc/init.d/S50adcdemo_ui
endef

$(eval $(generic-package))
