
executable('gfx_intel', ['src/main.cpp'],
	dependencies: [
		clang_coroutine_dep,
		libarch_dep, lib_helix_dep, hw_protocol_dep,
		libmbus_protocol_dep, proto_lite_dep],
	cpp_args: ['-DFRIGG_HAVE_LIBC'],
	install: true)

