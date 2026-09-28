################################################################################
#
# sampler libiio
#
################################################################################

SAMPLER_LIBIIO_VERSION = 7c2377612404b66950afacb5b145ea436104c745
SAMPLER_LIBIIO_SITE = https://github.com/analogdevicesinc/libiio.git
SAMPLER_LIBIIO_SITE_METHOD = git
SAMPLER_LIBIIO_LICENSE = LGPL-2.1+
SAMPLER_LIBIIO_LICENSE_FILES = COPYING.txt
SAMPLER_LIBIIO_DEPENDENCIES = libxml2 libaio host-flex host-bison

SAMPLER_LIBIIO_CONF_OPTS = \
	-DWITH_LOCAL_BACKEND=ON \
	-DWITH_AIO=ON \
	-DWITH_IIOD=OFF \
	-DWITH_NETWORK_BACKEND=OFF \
	-DWITH_USB_BACKEND=OFF \
	-DWITH_SERIAL_BACKEND=OFF \
	-DHAVE_DNS_SD=OFF \
	-DWITH_TESTS=OFF \
	-DWITH_DOC=OFF \
	-DINSTALL_UDEV_RULE=OFF \
	-DLIBIIO_COMPAT=OFF

$(eval $(cmake-package))
