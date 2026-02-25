; =============================================================================
; BareMetal -- a 64-bit OS written in Assembly for x86-64 systems
; Copyright (C) 2008-2026 Return Infinity -- see LICENSE.TXT
;
; Driver Includes
; =============================================================================


; Internal
%include "drivers/apic.asm"
%include "drivers/ioapic.asm"
%include "drivers/msi.asm"
%include "drivers/ps2.asm"
%include "drivers/serial.asm"
%include "drivers/timer.asm"
%include "drivers/virtio.asm"

; Bus
%include "drivers/bus/pcie.asm"
%include "drivers/bus/pci.asm"

; Non-volatile Storage
%include "drivers/nvs/virtio-blk.asm"

; Network
%include "drivers/net/virtio-net.asm"

; Video
%ifndef NO_LFB
%include "drivers/lfb/lfb.asm"
%endif
%ifndef NO_VGA
%include "drivers/vga.asm"
%endif

NIC_DeviceVendor_ID:	; The supported list of NICs

; Virtio
%ifndef NO_VIRTIO
dw 0x1AF4		; Driver ID
dw 0x1AF4		; Vendor ID
dw 0x1000		; Device ID - legacy
dw 0x1041		; Device ID - v1.0
dw 0x0000
%endif

; End of list
dw 0x0000
dw 0x0000


; =============================================================================
; EOF
