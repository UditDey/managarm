
$(call standard_dirs)

$c_CXX = x86_64-managarm-g++

$c_PKGCONF := PKG_CONFIG_SYSROOT_DIR=$(SYSROOT_PATH) \
	PKG_CONFIG_LIBDIR=$(SYSROOT_PATH)/usr/lib/pkgconfig pkg-config

$c_INCLUDES := -I$($c_GENDIR) -I$(TREE_PATH)/frigg/include \
	$(shell $($c_PKGCONF) --cflags protobuf-lite cairo freetype2)

$c_CXXFLAGS := $(CXXFLAGS) $($c_INCLUDES)
$c_CXXFLAGS += -std=c++1y -Wall -O3
$c_CXXFLAGS += -DFRIGG_HAVE_LIBC

$c_LIBS := -lbragi_mbus \
	$(shell $($c_PKGCONF) --libs protobuf-lite cairo freetype2)

$(call make_exec,bochs_vga,main.o input.pb.o)
$(call compile_cxx,$($c_SRCDIR),$($c_OBJDIR))

# compile protobuf files
gen-$c: $($c_GENDIR)/input.pb.tag

$(call gen_protobuf_cpp,$(TREE_PATH)/bragi/proto,$($c_GENDIR))
$(call compile_cxx,$($c_GENDIR),$($c_OBJDIR))


