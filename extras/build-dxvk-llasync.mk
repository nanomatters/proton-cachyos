##
## dxvk-llasync
##

ifeq ($(findstring dxvk-llasync,$(WITHOUT_EXTRAS)),)

# wine builds DLLs with the same names, we need to differentiate the timestamps
DXVK_LLASYNC_i386_SOURCE_DATE_EPOCH := $(shell expr $(i386_SOURCE_DATE_EPOCH) - 1)
DXVK_LLASYNC_x86_64_SOURCE_DATE_EPOCH := $(shell expr $(x86_64_SOURCE_DATE_EPOCH) - 1)
DXVK_LLASYNC_arm64ec_SOURCE_DATE_EPOCH := $(shell expr $(arm64ec_SOURCE_DATE_EPOCH) - 1)

DXVK_LLASYNC_SOURCE_ARGS = \
  --exclude version.h.in \

DXVK_LLASYNC_MESON_ARGS = -Db_ndebug=true --force-fallback-for=libdisplay-info
DXVK_LLASYNC_i386_MESON_ARGS = --bindir=$(DXVK_LLASYNC_i386_DST)/lib/wine/dxvk-llasync/i386-windows
DXVK_LLASYNC_x86_64_MESON_ARGS = --bindir=$(DXVK_LLASYNC_x86_64_DST)/lib/wine/dxvk-llasync/x86_64-windows
DXVK_LLASYNC_arm64ec_MESON_ARGS = --bindir=$(DXVK_LLASYNC_arm64ec_DST)/lib/wine/dxvk-llasync/aarch64-windows
DXVK_LLASYNC_HOST_DEPENDS = glslang

DXVK_LLASYNC_i386_CFLAGS = -O3 -mno-avx $(i386_SANITY_FLAGS)
DXVK_LLASYNC_x86_64_CFLAGS = -O3 -mno-avx
DXVK_LLASYNC_CPPFLAGS = -msse -msse2
DXVK_LLASYNC_LDFLAGS = -static -static-libgcc -static-libstdc++

$(eval $(call rules-source,dxvk-llasync,$(SRCDIR)/extras//dxvk-llasync))
$(eval $(call rules-meson,dxvk-llasync,i386,windows))
$(eval $(call rules-meson,dxvk-llasync,x86_64,windows))
$(eval $(call rules-meson,dxvk-llasync,arm64ec,windows))

$(OBJ)/.dxvk-llasync-post-source: patches-source
	$(foreach p,$(shell find $(PATCHES_SRC)/extras/dxvk-llasync/ -name "*.patch" | sort),patch -d $(DXVK_LLASYNC_SRC) -Np1 -i $(p) &&) true
	sed -re 's#@VCS_TAG@#$(shell git -C $(SRCDIR)/extras//dxvk-llasync describe --always --abbrev=15 --dirty=0)#' \
	    $(SRCDIR)/extras/dxvk-llasync/version.h.in > $(DXVK_LLASYNC_SRC)/version.h.in
	mkdir -p $(DST_LIBDIR)/wine/dxvk-llasync
	rm -rf $(DST_LIBDIR)/wine/dxvk-llasync/version
	echo "$(shell git -C $(SRCDIR) submodule status -- extras/dxvk-llasync)" > $(DST_LIBDIR)/wine/dxvk-llasync/version
	touch $@

default_pfx: dxvk-llasync

endif # WITHOUT_EXTRAS
