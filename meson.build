project('wlgears', ['c'], license: 'MIT', version: '0.1.0')

cc = meson.get_compiler('c')

wayland_client = dependency('wayland-client', version: '>=1.10.0')
wayland_scanner = dependency('wayland-scanner', version: '>=1.10.0', required: false, native: true)
wayland_protocols = dependency('wayland-protocols', version: '>=1.16', required: false)
wayland_scanner_tool = find_program(wayland_scanner.get_variable('wayland_scanner'), native: true)

wl_protocol_dir = wayland_protocols.get_variable('pkgdatadir')

src = files('src/wlgears.c')

deps = [
    dependency('wayland-client'),
    dependency('wayland-cursor'),
    dependency('wayland-egl'),
    dependency('egl'),
    dependency('epoxy'),
	cc.find_library('m'),
]

protocols = [
	wl_protocol_dir / 'stable/xdg-shell/xdg-shell.xml',
]

wl_protos_src = []

foreach xml : protocols
	wl_protos_src += custom_target(
		xml.underscorify() + '_c',
		input: xml,
		output: '@BASENAME@-protocol.c',
		command: [wayland_scanner_tool, 'private-code', '@INPUT@', '@OUTPUT@'],
	)

	wl_protos_src += custom_target(
		xml.underscorify() + '_client_h',
		input: xml,
		output: '@BASENAME@-client-protocol.h',
		command: [wayland_scanner_tool, 'client-header', '@INPUT@', '@OUTPUT@'],
	)
endforeach

example = executable('wlgears',
    src, wl_protos_src,
    dependencies: deps,
	install: true,
)
