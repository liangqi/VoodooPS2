/*
 * Copyright (c) 2002 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * The contents of this file constitute Original Code as defined in and
 * are subject to the Apple Public Source License Version 1.2 (the
 * "License").  You may not use this file except in compliance with the
 * License.  Please obtain a copy of the License at
 * http://www.apple.com/publicsource and read it before using this file.
 * 
 * This Original Code and all software distributed under the License are
 * distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE OR NON-INFRINGEMENT.  Please see the
 * License for the specific language governing rights and limitations
 * under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */

#ifndef _APPLEPS2ELANTOUCHPAD_H
#define _APPLEPS2ELANTOUCHPAD_H

#include "ApplePS2MouseDevice.h"
#include <IOKit/hidsystem/IOHIPointing.h>

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
// ApplePS2ElanTrackpad Class Declaration
//

// copy from elan linux driver, drivers/input/mouse/elantech.h -- begin
/*
 * Command values for Synaptics style queries
 */
#define ETP_FW_ID_QUERY			0x00
#define ETP_FW_VERSION_QUERY		0x01
#define ETP_CAPABILITIES_QUERY		0x02
#define ETP_SAMPLE_QUERY		0x03
#define ETP_RESOLUTION_QUERY		0x04

/*
 * Command values for register reading or writing
 */
#define ETP_REGISTER_READ		0x10
#define ETP_REGISTER_WRITE		0x11
#define ETP_REGISTER_READWRITE		0x00

/*
 * Hardware version 2 custom PS/2 command value
 */
#define ETP_PS2_CUSTOM_COMMAND		0xf8

/*
 * Times to retry a ps2_command and millisecond delay between tries
 */
#define ETP_PS2_COMMAND_TRIES		3
#define ETP_PS2_COMMAND_DELAY		500

/*
 * Times to try to read back a register and millisecond delay between tries
 */
#define ETP_READ_BACK_TRIES		5
#define ETP_READ_BACK_DELAY		2000

/*
 * Register bitmasks for hardware version 1
 */
#define ETP_R10_ABSOLUTE_MODE		0x04
#define ETP_R11_4_BYTE_MODE		0x02

/*
 * Capability bitmasks
 */
#define ETP_CAP_HAS_ROCKER		0x04

/*
 * One hard to find application note states that X axis range is 0 to 576
 * and Y axis range is 0 to 384 for harware version 1.
 * Edge fuzz might be necessary because of bezel around the touchpad
 */
#define ETP_EDGE_FUZZ_V1		32

#define ETP_XMIN_V1			(  0 + ETP_EDGE_FUZZ_V1)
#define ETP_XMAX_V1			(576 - ETP_EDGE_FUZZ_V1)
#define ETP_YMIN_V1			(  0 + ETP_EDGE_FUZZ_V1)
#define ETP_YMAX_V1			(384 - ETP_EDGE_FUZZ_V1)

/*
 * The resolution for older v2 hardware doubled.
 * (newer v2's firmware provides command so we can query)
 */
#define ETP_XMIN_V2			0
#define ETP_XMAX_V2			1152
#define ETP_YMIN_V2			0
#define ETP_YMAX_V2			768

#define ETP_PMIN_V2			0
#define ETP_PMAX_V2			255
#define ETP_WMIN_V2			0
#define ETP_WMAX_V2			15

/*
 * v3 hardware has 2 kinds of packet types,
 * v4 hardware has 3.
 */
#define PACKET_UNKNOWN			0x01
#define PACKET_DEBOUNCE			0x02
#define PACKET_V3_HEAD			0x03
#define PACKET_V3_TAIL			0x04
#define PACKET_V4_HEAD			0x05
#define PACKET_V4_MOTION		0x06
#define PACKET_V4_STATUS		0x07

/*
 * track up to 5 fingers for v4 hardware
 */
#define ETP_MAX_FINGERS			5

/*
 * weight value for v4 hardware
 */
#define ETP_WEIGHT_VALUE		5

#define ETP_TAPTOCLICK_DIST     32

struct elantech_data {
	unsigned char reg_07;
	unsigned char reg_10;
	unsigned char reg_11;
	unsigned char reg_20;
	unsigned char reg_21;
	unsigned char reg_22;
	unsigned char reg_23;
	unsigned char reg_24;
	unsigned char reg_25;
	unsigned char reg_26;
	unsigned char debug;
	unsigned char capabilities[3];
	bool paritycheck;
	bool jumpy_cursor;
	bool reports_pressure;
	unsigned char hw_version;
	unsigned int fw_version;
	unsigned int single_finger_reports;
	unsigned int y_max;
	unsigned int width;
	struct IOGPoint mt[ETP_MAX_FINGERS];
	unsigned char parity[256];
	bool send_cmd;
};
// copy from elan linux driver, drivers/input/mouse/elantech.h -- end

class ApplePS2ElanTrackpad : public IOHIPointing 
{
    OSDeclareDefaultStructors( ApplePS2ElanTrackpad );

private:
    ApplePS2MouseDevice * _device;
    UInt32                _interruptHandlerInstalled:1;
    UInt32                _powerControlHandlerInstalled:1;
    UInt8                 _packetBuffer[6];
    UInt32                _packetByteCount;
    IOFixed               _resolution;
    elantech_data         e_data;
    elantech_data         *etd;
    UInt8                 pktsize;
    IOGBounds             bounds;
    unsigned int          last_fingers;
    bool                  tapToClick;
    bool                  tapInRange[ETP_MAX_FINGERS];
    AbsoluteTime          now;
    IOGPoint              lastPoint;
    bool                  validLastPoint;
    IOGPoint              startPoint[ETP_MAX_FINGERS];
    bool                  validStartPoint[ETP_MAX_FINGERS];

protected:
	virtual IOItemCount buttonCount();
	virtual IOFixed     resolution();
    virtual void   free();
    virtual void   interruptOccurred( UInt8 data );
    virtual void   setDevicePowerState(UInt32 whatToDo);
    virtual void   setCommandByte( UInt8 setBits, UInt8 clearBits );
    virtual void   setTouchPadEnable( bool enable );

private:
    bool elantech_is_signature_valid( const UInt8 *param );
    int synaptics_send_cmd( IOService * provider, UInt8 c, UInt8 *param );
    int elantech_send_cmd( IOService * provider, UInt8 c, UInt8 *param );
    int send_cmd( IOService * provider, UInt8 c, UInt8 *param, bool s );
    //int elantech_ps2_command(IOService * provider, UInt8 *param, UInt command);
    int elantech_write_reg( IOService * provider, UInt8 reg, UInt8 val );
    int elantech_read_reg( IOService * provider, UInt8 reg, UInt8 *val );
    int psmouse_sliced_command( PS2Request * request, int &j, UInt command );
    int elantech_set_absolute_mode( IOService * provider );
    int elantech_set_range(IOService * provider,
                           unsigned int *x_min, unsigned int *y_min,
                           unsigned int *x_max, unsigned int *y_max,
                           unsigned int *width);
    unsigned int elantech_convert_res(unsigned int val);
    int elantech_get_resolution_v4(IOService * provider,
                                   unsigned int *x_res,
                                   unsigned int *y_res);
    void elantech_packet_dump( UInt8 data );
    void elantech_report_absolute_v3(UInt8* packet, UInt32 packetSize, int packet_type);
    
public:
    virtual bool init( OSDictionary * properties );
    virtual ApplePS2ElanTrackpad * probe( IOService * provider,
                                               SInt32 *    score );
    virtual bool start( IOService * provider );
    virtual void stop( IOService * provider );
    virtual UInt32 deviceType();
    virtual UInt32 interfaceID();
};

#endif /* _APPLEPS2ELANTOUCHPAD_H */
