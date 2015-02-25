/*
 *  Copyright © 2015 Broadcom
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/types.h>

enum bcm_mbox_property_status {
	bcm_mbox_status_request = 0,
	bcm_mbox_status_success = 0x80000000,
	bcm_mbox_status_error =   0x80000001,
};

struct bcm_mbox_property_tag_header {
	/* One of enum_mbox_property_tag. */
	u32 tag;

	/* The number of bytes in the value buffer following this
	 * struct.
	 */
	u32 buf_size;

	/*
	 * On submit, the length of the request (though it doesn't
	 * appear to be currently used by the firmware).  On return,
	 * the length of the response (always 4 byte aligned), with
	 * the low bit set.
	 */
	u32 req_resp_size;
};

enum bcm_mbox_property_tag {
	bcm_mbox_property_end = 0,
	bcm_mbox_get_firmware_revision =                  0x00000001,

	bcm_mbox_set_cursor_info =                        0x00008010,
	bcm_mbox_set_cursor_state =                       0x00008011,

	bcm_mbox_get_board_model =                        0x00010001,
	bcm_mbox_get_board_revision =                     0x00010002,
	bcm_mbox_get_board_mac_address =                  0x00010003,
	bcm_mbox_get_board_serial =                       0x00010004,
	bcm_mbox_get_arm_memory =                         0x00010005,
	bcm_mbox_get_vc_memory =                          0x00010006,
	bcm_mbox_get_clocks =                             0x00010007,
	bcm_mbox_get_power_state =                        0x00020001,
	bcm_mbox_get_timing =                             0x00020002,
	bcm_mbox_set_power_state =                        0x00028001,
	bcm_mbox_get_clock_state =                        0x00030001,
	bcm_mbox_get_clock_rate =                         0x00030002,
	bcm_mbox_get_voltage =                            0x00030003,
	bcm_mbox_get_max_clock_rate =                     0x00030004,
	bcm_mbox_get_max_voltage =                        0x00030005,
	bcm_mbox_get_temperature =                        0x00030006,
	bcm_mbox_get_min_clock_rate =                     0x00030007,
	bcm_mbox_get_min_voltage =                        0x00030008,
	bcm_mbox_get_turbo =                              0x00030009,
	bcm_mbox_get_max_temperature =                    0x0003000a,
	bcm_mbox_allocate_memory =                        0x0003000c,
	bcm_mbox_lock_memory =                            0x0003000d,
	bcm_mbox_unlock_memory =                          0x0003000e,
	bcm_mbox_release_memory =                         0x0003000f,
	bcm_mbox_execute_code =                           0x00030010,
	bcm_mbox_execute_qpu =                            0x00030011,
	bcm_mbox_set_enable_qpu =                         0x00030012,
	bcm_mbox_get_dispmanx_resource_mem_handle =       0x00030014,
	bcm_mbox_get_edid_block =                         0x00030020,
	bcm_mbox_set_clock_state =                        0x00038001,
	bcm_mbox_set_clock_rate =                         0x00038002,
	bcm_mbox_set_voltage =                            0x00038003,
	bcm_mbox_set_turbo =                              0x00038009,

	/* Dispmanx tags */
	bcm_mbox_framebuffer_allocate =                   0x00040001,
	bcm_mbox_framebuffer_blank =                      0x00040002,
	bcm_mbox_framebuffer_get_physical_width_height =  0x00040003,
	bcm_mbox_framebuffer_get_virtual_width_height =   0x00040004,
	bcm_mbox_framebuffer_get_depth =                  0x00040005,
	bcm_mbox_framebuffer_get_pixel_order =            0x00040006,
	bcm_mbox_framebuffer_get_alpha_mode =             0x00040007,
	bcm_mbox_framebuffer_get_pitch =                  0x00040008,
	bcm_mbox_framebuffer_get_virtual_offset =         0x00040009,
	bcm_mbox_framebuffer_get_overscan =               0x0004000a,
	bcm_mbox_framebuffer_get_palette =                0x0004000b,
	bcm_mbox_framebuffer_release =                    0x00048001,
	bcm_mbox_framebuffer_test_physical_width_height = 0x00044003,
	bcm_mbox_framebuffer_test_virtual_width_height =  0x00044004,
	bcm_mbox_framebuffer_test_depth =                 0x00044005,
	bcm_mbox_framebuffer_test_pixel_order =           0x00044006,
	bcm_mbox_framebuffer_test_alpha_mode =            0x00044007,
	bcm_mbox_framebuffer_test_virtual_offset =        0x00044009,
	bcm_mbox_framebuffer_test_overscan =              0x0004400a,
	bcm_mbox_framebuffer_test_palette =               0x0004400b,
	bcm_mbox_framebuffer_set_physical_width_height =  0x00048003,
	bcm_mbox_framebuffer_set_virtual_width_height =   0x00048004,
	bcm_mbox_framebuffer_set_depth =                  0x00048005,
	bcm_mbox_framebuffer_set_pixel_order =            0x00048006,
	bcm_mbox_framebuffer_set_alpha_mode =             0x00048007,
	bcm_mbox_framebuffer_set_virtual_offset =         0x00048009,
	bcm_mbox_framebuffer_set_overscan =               0x0004800a,
	bcm_mbox_framebuffer_set_palette =                0x0004800b,

	bcm_mbox_get_command_line =                       0x00050001,
	bcm_mbox_get_dma_channels =                       0x00060001,
};

int bcm_mbox_property(void *data, size_t tag_size);
