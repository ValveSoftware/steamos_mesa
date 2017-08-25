if HAVE_GALLIUM_V3D

TARGET_DRIVERS += v3d
TARGET_CPPFLAGS += -DGALLIUM_V3D
TARGET_LIB_DEPS += \
	$(top_builddir)/src/gallium/winsys/vc5/drm/libv3ddrm.la \
	$(top_builddir)/src/gallium/drivers/vc5/libv3d.la \
	$(top_builddir)/src/broadcom/libbroadcom.la

if !HAVE_GALLIUM_VC4
TARGET_LIB_DEPS += $(top_builddir)/src/broadcom/cle/libbroadcom_cle.la
endif

endif
