// Local builds embed the generated, transformed HLOD pack in libsavr.so.  This
// keeps deployment split-only: no base APK or 1.4 GB data split replacement is
// needed, and the original retail assets are never modified on the device.
// native/CMakeLists.txt explicitly tracks the incbin payload; otherwise Ninja
// can relink a new mod while silently retaining a stale HLOD object.
    .section .rodata.savr_aircraft_hlod,"a",@progbits
    .balign 16
    .global savr_aircraft_hlod_data
    .hidden savr_aircraft_hlod_data
savr_aircraft_hlod_data:
    .incbin "../aircraft-hlod-global/aircraft.hlod"
    .global savr_aircraft_hlod_data_end
    .hidden savr_aircraft_hlod_data_end
savr_aircraft_hlod_data_end:
    .byte 0
