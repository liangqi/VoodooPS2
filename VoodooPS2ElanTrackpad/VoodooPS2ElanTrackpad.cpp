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

#include <IOKit/assert.h>
#include <IOKit/IOLib.h>
#include <IOKit/hidsystem/IOHIDParameter.h>
#include "VoodooPS2ElanTrackpad.h"

#define DEBUG 1

#if DEBUG
#define DEBUG_LOG(fmt, args...) IOLog("[%s] "fmt"\n", getName(), ## args)
#else
#define DEBUG_LOG(fmt, args...)
#endif

/*
 * determine hardware version and set some properties according to it.
 */
static int elantech_set_properties(struct elantech_data *etd)
{
	/* This represents the version of IC body. */
	int ver = (etd->fw_version & 0x0f0000) >> 16;
    
	/* Early version of Elan touchpads doesn't obey the rule. */
	if (etd->fw_version < 0x020030 || etd->fw_version == 0x020600)
		etd->hw_version = 1;
	else {
		switch (ver) {
            case 2:
            case 4:
                etd->hw_version = 2;
                break;
            case 5:
                etd->hw_version = 3;
                break;
            case 6:
                etd->hw_version = 4;
                break;
            default:
                return -1;
		}
	}
    
	/* decide which send_cmd we're gonna use early */
	etd->send_cmd = etd->hw_version >= 3 ? true : false;
    
	/* Turn on packet checking by default */
	etd->paritycheck = 1;
    
	/*
	 * This firmware suffers from misreporting coordinates when
	 * a touch action starts causing the mouse cursor or scrolled page
	 * to jump. Enable a workaround.
	 */
	etd->jumpy_cursor =
    (etd->fw_version == 0x020022 || etd->fw_version == 0x020600);
    
	if (etd->hw_version > 1) {
		/* For now show extra debug information */
		etd->debug = 2;
        
		if (etd->fw_version >= 0x020800)
			etd->reports_pressure = true;
	}
    
	return 0;
}

static int elantech_packet_check_v1(struct elantech_data *etd, UInt8* packet)
{
	unsigned char p1, p2, p3;
    
	/* Parity bits are placed differently */
	if (etd->fw_version < 0x020000) {
		/* byte 0:  D   U  p1  p2   1  p3   R   L */
		p1 = (packet[0] & 0x20) >> 5;
		p2 = (packet[0] & 0x10) >> 4;
	} else {
		/* byte 0: n1  n0  p2  p1   1  p3   R   L */
		p1 = (packet[0] & 0x10) >> 4;
		p2 = (packet[0] & 0x20) >> 5;
	}
    
	p3 = (packet[0] & 0x04) >> 2;
    
	return etd->parity[packet[1]] == p1 &&
           etd->parity[packet[2]] == p2 &&
           etd->parity[packet[3]] == p3;
}

/*
 * We check the constant bits to determine what packet type we get,
 * so packet checking is mandatory for v3 and later hardware.
 */
static int elantech_packet_check_v3(struct elantech_data *etd, UInt8* packet)
{
	const UInt8 debounce_packet[] = { 0xc4, 0xff, 0xff, 0x02, 0xff, 0xff };
    
	/*
	 * check debounce first, it has the same signature in byte 0
	 * and byte 3 as PACKET_V3_HEAD.
	 */
	if (!memcmp(packet, debounce_packet, sizeof(debounce_packet)))
		return PACKET_DEBOUNCE;
    
	if ((packet[0] & 0x0c) == 0x04 && (packet[3] & 0xcf) == 0x02)
		return PACKET_V3_HEAD;
    
	if ((packet[0] & 0x0c) == 0x0c && (packet[3] & 0xce) == 0x0c)
		return PACKET_V3_TAIL;
    
	return PACKET_UNKNOWN;
}

// =============================================================================
// ApplePS2ElanTrackpad Class Implementation
//

#define super IOHIPointing

OSDefineMetaClassAndStructors(ApplePS2ElanTrackpad, IOHIPointing);
UInt32 ApplePS2ElanTrackpad::deviceType() { return NX_EVS_DEVICE_TYPE_MOUSE; };
UInt32 ApplePS2ElanTrackpad::interfaceID(){ return NX_EVS_DEVICE_INTERFACE_BUS_ACE; };
IOItemCount ApplePS2ElanTrackpad::buttonCount() { return 2; };
IOFixed     ApplePS2ElanTrackpad::resolution()  { return _resolution; };

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

bool ApplePS2ElanTrackpad::init( OSDictionary * properties )
{
    //
    // Initialize this object's minimal state. This is invoked right after this
    // object is instantiated.
    //
    if (!super::init(properties))  return false;
    DEBUG_LOG("init");
    _device                    = 0;
    _packetByteCount           = 0;
    _resolution                = (100) << 16; // (100 dpi, 4 counts/mm) On init should be on default
    etd                        = &e_data;
    return true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

ApplePS2ElanTrackpad *
ApplePS2ElanTrackpad::probe( IOService * provider, SInt32 * score )
{
    // from elantech_detect
    UInt8 Byte[3];
    
    if (!super::probe(provider, score)) return 0;
    DEBUG_LOG("probe");
    
    _device = (ApplePS2MouseDevice *) provider;
    
    PS2Request * request = _device->allocateRequest();
    
    if ( !request ) return 0;
    
    // "Elantech magic knock"
    request->commands[0].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[0].inOrOut = kDP_SetDefaults;
    request->commands[1].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[1].inOrOut = kDP_SetDefaultsAndDisable;
    request->commands[2].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[2].inOrOut = kDP_SetMouseScaling1To1;
    request->commands[3].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[3].inOrOut = kDP_SetMouseScaling1To1;
    request->commands[4].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[4].inOrOut = kDP_SetMouseScaling1To1;
    request->commands[5].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[5].inOrOut = kDP_GetMouseInformation;
    request->commands[6].command = kPS2C_ReadDataPort;
    request->commands[6].inOrOut = 0;
    request->commands[7].command = kPS2C_ReadDataPort;
    request->commands[7].inOrOut = 0;
    request->commands[8].command = kPS2C_ReadDataPort;
    request->commands[8].inOrOut = 0;
    request->commandsCount = 9;
    _device->submitRequestAndBlock(request);
    
    // result is "magic knock"
    Byte[0] = request->commands[6].inOrOut;
    Byte[1] = request->commands[7].inOrOut;
    Byte[2] = request->commands[8].inOrOut;
    _device->freeRequest(request);
    DEBUG_LOG("Elantech magic knock: [ 0x%02x, 0x%02x, 0x%02x ]", Byte[0], Byte[1], Byte[2]);
    
    request = _device->allocateRequest();
    if (!request) return 0;

	if (Byte[0] != 0x3c || Byte[1] != 0x03 ||
	    (Byte[2] != 0xc8 && Byte[2] != 0x00)) {
        DEBUG_LOG("Unexpected Elantech magic knock!");
        return 0;
    }

    request = _device->allocateRequest();
    if (!request) return 0;
    
    // Now fetch "firmware version"
    synaptics_send_cmd(provider, ETP_FW_VERSION_QUERY, Byte);
    DEBUG_LOG("Elantech version query result : [ 0x%02x, 0x%02x, 0x%02x ]", Byte[0], Byte[1], Byte[2]);

	if (!elantech_is_signature_valid(Byte)) {
		DEBUG_LOG("Probably not a real Elantech touchpad. Aborting.");
        return 0;
    }
    
	etd->parity[0] = 1;
	for (int i = 1; i < 256; i++)
		etd->parity[i] = etd->parity[i & (i - 1)] ^ 1;

    etd->fw_version = (Byte[0] << 16) | (Byte[1] << 8) | Byte[2];
    
	if (elantech_set_properties(etd)) {
		DEBUG_LOG("unknown hardware version, aborting...");
        return 0;
	}
    
    DEBUG_LOG("assuming hardware version %d (with firmware version 0x%02x%02x%02x)",
              etd->hw_version, Byte[0], Byte[1], Byte[2]);
    
    //
    // Announce hardware properties.
    //
    
    IOLog("ApplePS2ElanTrackpad: Elan Trackpad hardware version %d (with firmware version 0x%02x%02x%02x)\n", etd->hw_version, Byte[0], Byte[1], Byte[2]);
    
    return this;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void ApplePS2ElanTrackpad::free()
{
    //
    // Release the pointer to the provider object.
    //
    
    if (_device)
    {
        _device->release();
        _device = 0;
    }
    
    super::free();
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
/*
 * (value from firmware) * 10 + 790 = dpi
 * we also have to convert dpi to dots/mm (*10/254 to avoid floating point)
 */

unsigned int ApplePS2ElanTrackpad::elantech_convert_res(unsigned int val)
{
	return (val * 10 + 790) * 10 / 254;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

int ApplePS2ElanTrackpad::elantech_get_resolution_v4(IOService * provider,
                               unsigned int *x_res,
                               unsigned int *y_res)
{
	UInt8 param[3];
    
	if (elantech_send_cmd(provider, ETP_RESOLUTION_QUERY, param))
		return -1;
    
	*x_res = elantech_convert_res(param[1] & 0x0f);
	*y_res = elantech_convert_res((param[1] & 0xf0) >> 4);
    
	return 0;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

int ApplePS2ElanTrackpad::elantech_set_range(IOService * provider,
                       unsigned int *x_min, unsigned int *y_min,
                       unsigned int *x_max, unsigned int *y_max,
                       unsigned int *width)
{
	UInt8 param[3];
	UInt8 traces;
    
	switch (etd->hw_version) {
        case 1:
            *x_min = ETP_XMIN_V1;
            *y_min = ETP_YMIN_V1;
            *x_max = ETP_XMAX_V1;
            *y_max = ETP_YMAX_V1;
            break;
            
        case 2:
            if (etd->fw_version == 0x020800 ||
                etd->fw_version == 0x020b00 ||
                etd->fw_version == 0x020030) {
                *x_min = ETP_XMIN_V2;
                *y_min = ETP_YMIN_V2;
                *x_max = ETP_XMAX_V2;
                *y_max = ETP_YMAX_V2;
            } else {
                int i;
                int fixed_dpi;
                
                i = (etd->fw_version > 0x020800 &&
                     etd->fw_version < 0x020900) ? 1 : 2;
                
                if (send_cmd(provider, ETP_FW_ID_QUERY, param, etd->send_cmd))
                    return -1;
                
                fixed_dpi = param[1] & 0x10;
                
                if (((etd->fw_version >> 16) == 0x14) && fixed_dpi) {
                    if (send_cmd(provider, ETP_SAMPLE_QUERY, param, etd->send_cmd))
                        return -1;
                    
                    *x_max = (etd->capabilities[1] - i) * param[1] / 2;
                    *y_max = (etd->capabilities[2] - i) * param[2] / 2;
                } else if (etd->fw_version == 0x040216) {
                    *x_max = 819;
                    *y_max = 405;
                } else if (etd->fw_version == 0x040219 || etd->fw_version == 0x040215) {
                    *x_max = 900;
                    *y_max = 500;
                } else {
                    *x_max = (etd->capabilities[1] - i) * 64;
                    *y_max = (etd->capabilities[2] - i) * 64;
                }
            }
            break;
            
        case 3:
            if (send_cmd(provider, ETP_FW_ID_QUERY, param, etd->send_cmd))
                return -1;
            
            *x_max = (0x0f & param[0]) << 8 | param[1];
            *y_max = (0xf0 & param[0]) << 4 | param[2];
            break;
            
        case 4:
            if (send_cmd(provider, ETP_FW_ID_QUERY, param, etd->send_cmd))
                return -1;
            
            *x_max = (0x0f & param[0]) << 8 | param[1];
            *y_max = (0xf0 & param[0]) << 4 | param[2];
            traces = etd->capabilities[1];
            if ((traces < 2) || (traces > *x_max))
                return -1;
            
            *width = *x_max / (traces - 1);
            break;
	}
    
	return 0;    
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

int ApplePS2ElanTrackpad::elantech_set_absolute_mode( IOService * provider )
{
	UInt8 val;
    int rc = 0;
    
	switch (etd->hw_version) {
        case 1:
            etd->reg_10 = 0x16;
            etd->reg_11 = 0x8f;
            if (elantech_write_reg(provider, 0x10, etd->reg_10) ||
                elantech_write_reg(provider, 0x11, etd->reg_11)) {
                rc = -1;
            }
            break;
            
        case 2:
            /* Windows driver values */
            etd->reg_10 = 0x54;
            etd->reg_11 = 0x88;	/* 0x8a */
            etd->reg_21 = 0x60;	/* 0x00 */
            if (elantech_write_reg(provider, 0x10, etd->reg_10) ||
                elantech_write_reg(provider, 0x11, etd->reg_11) ||
                elantech_write_reg(provider, 0x21, etd->reg_21)) {
                rc = -1;
            }
            break;
            
        case 3:
            etd->reg_10 = 0x0b;
            if (elantech_write_reg(provider, 0x10, etd->reg_10))
                rc = -1;
            
            break;
            
        case 4:
            etd->reg_07 = 0x01;
            if (elantech_write_reg(provider, 0x07, etd->reg_07))
                rc = -1;
            
            /* v4 has no reg 0x10 to read */
	}
    
    switch (etd->hw_version) {
        case 1 ... 3:
            if (rc == 0) {
                /*
                 * Read back reg 0x10. For hardware version 1 we must make
                 * sure the absolute mode bit is set. For hardware version 2
                 * the touchpad is probably initializing and not ready until
                 * we read back the value we just wrote.
                 */
                rc = elantech_read_reg(provider, 0x10, &val);
                if (rc) {
                    DEBUG_LOG("failed to read back register 0x10.");
                } else if (etd->hw_version == 1 &&
                           !(val & ETP_R10_ABSOLUTE_MODE)) {
                    DEBUG_LOG("touchpad refuses to switch to absolute mode.");
                    rc = -1;
                }
            }
            break;
        case 4:
            break;
	}

	if (rc)
		DEBUG_LOG("failed to initialise registers.");
    
	return rc;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

bool ApplePS2ElanTrackpad::elantech_is_signature_valid( const UInt8 *param )
{
	static const unsigned char rates[] = { 200, 100, 80, 60, 40, 20, 10 };
    int array_size = 7;
	int i;
    
	if (param[0] == 0)
		return false;
    
	if (param[1] == 0)
		return true;
    
	for (i = 0; i < array_size; i++)
		if (param[2] == rates[i])
			return false;
    
	return true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

bool ApplePS2ElanTrackpad::start( IOService * provider )
{
    UInt64 enabledProperty;
    
    //
    // If this is not here, then when it starts it takes mouse acceleration until changed
    // from trackpad preference pane
    //
    
    //setProperty(kIOHIDPointerAccelerationTypeKey, kIOHIDTrackpadAccelerationType);
    
    //
    // The driver has been instructed to start. This is called after a
    // successful probe and match.
    //
    
    if (!super::start(provider)) return false;
    DEBUG_LOG("start");
    
    //
    // Maintain a pointer to and retain the provider object.
    //
    
    _device = (ApplePS2MouseDevice *) provider;
    _device->retain();
    
    if (send_cmd(provider, ETP_CAPABILITIES_QUERY, etd->capabilities, etd->send_cmd)) {
        DEBUG_LOG("failed to query capabilities.");
        return 0;
    }
    
    DEBUG_LOG("Synaptics capabilities query result 0x%02x, 0x%02x, 0x%02x.",
              etd->capabilities[0], etd->capabilities[1],
              etd->capabilities[2]);
    
    if (elantech_set_absolute_mode(provider)) {
        DEBUG_LOG("failed to put touchpad into absolute mode.");
        return 0;
    }
    
    // port note: elantech_set_input_params was not ported yet, not sure works fine or not, but part of it was ported as bellow
	unsigned int x_min = 0, y_min = 0, x_max = 0, y_max = 0, width = 0;
	unsigned int x_res = 0, y_res = 0;
    // port note: elantech_set_range was ported
	if (elantech_set_range(provider, &x_min, &y_min, &x_max, &y_max, &width)) {
        DEBUG_LOG("failed to query touchpad range.");
        return 0;
    }
    
    DEBUG_LOG("Touchpad range query result %d, %d, %d, %d, %d.",
              x_min, y_min, x_max, y_max, width);
    
    bounds.minx = x_min;
    bounds.maxx = x_max;
    bounds.miny = y_min;
    bounds.maxy = y_max;
    
    last_fingers = 0;
    tapToClick = true;
    validLastPoint = false;
    for (int i = 0; i < ETP_MAX_FINGERS; ++i) {
        tapInRange[i] = true;
        validStartPoint[i] = false;
    }
    
    // port note: elantech_get_resolution_v4 was ported
	switch (etd->hw_version) {
        case 4:
            if (elantech_get_resolution_v4(provider, &x_res, &y_res)) {
                /*
                 * if query failed, print a warning and leave the values
                 * zero to resemble synaptics.c behavior.
                 */
                DEBUG_LOG("couldn't query resolution data.");
            } else {
                DEBUG_LOG("V4: resolution dataquery result %d, %d.",
                          x_res, y_res);
            }
            break;
    }

    etd->y_max = y_max;
	etd->width = width;
    
    pktsize = etd->hw_version > 1 ? 6 : 4;

    DEBUG_LOG("pktsize result %d.", pktsize);

    //
    // IntelliMouse Mode until we figure the new EC Absolute mode format...
    //
    
    //setIntelliMouseMode();
    
    //
    // Advertise some supported features (tapping, edge scrolling).
    //
    
    //enabledProperty = 1;
    
    // Enable tapping
    
    //setTapEnable( true );
    
    // Enable Absolute Mode
    /*
    if(_packetByteCount == 6)
    {
        setAbsoluteMode();
    }
     */
    
    //
    // Must add this property to let our superclass know that it should handle
    // trackpad acceleration settings from user space.  Without this, tracking
    // speed adjustments from the mouse prefs panel have no effect.
    //
    
    setProperty(kIOHIDPointerAccelerationTypeKey, kIOHIDTrackpadAccelerationType);
    
    //
    // Install our driver's interrupt handler, for asynchronous data delivery.
    //

    _device->installInterruptAction(this,
                                    OSMemberFunctionCast(PS2InterruptAction,this,&ApplePS2ElanTrackpad::interruptOccurred));
    _interruptHandlerInstalled = true;

    //
    // Enable the mouse clock (should already be so) and the mouse IRQ line.
    //
    
    setCommandByte( kCB_EnableMouseIRQ, kCB_DisableMouseClock );
    
    //
    // Finally, we enable the trackpad itself, so that it may start reporting
    // asynchronous events.
    //
    
    setTouchPadEnable(true);
    
    //
    // Install our power control handler.
    //

    _device->installPowerControlAction( this, OSMemberFunctionCast(PS2PowerControlAction,this,
                                                                   &ApplePS2ElanTrackpad::setDevicePowerState) );
    _powerControlHandlerInstalled = true;
    
    return true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void ApplePS2ElanTrackpad::setCommandByte( UInt8 setBits, UInt8 clearBits )
{
    //
    // Sets the bits setBits and clears the bits clearBits "atomically" in the
    // controller's Command Byte.   Since the controller does not provide such
    // a read-modify-write primitive, we resort to a test-and-set try loop.
    //
    // Do NOT issue this request from the interrupt/completion context.
    //
	
    UInt8        commandByte;
    UInt8        commandByteNew;
    PS2Request * request = _device->allocateRequest();
	
    if ( !request ) return;
	
    do
    {
        // (read command byte)
        request->commands[0].command = kPS2C_WriteCommandPort;
        request->commands[0].inOrOut = kCP_GetCommandByte;
        request->commands[1].command = kPS2C_ReadDataPort;
        request->commands[1].inOrOut = 0;
        request->commandsCount = 2;
        _device->submitRequestAndBlock(request);
		
        //
        // Modify the command byte as requested by caller.
        //
		
        commandByte    = request->commands[1].inOrOut;
        commandByteNew = (commandByte | setBits) & (~clearBits);
		
        // ("test-and-set" command byte)
        request->commands[0].command = kPS2C_WriteCommandPort;
        request->commands[0].inOrOut = kCP_GetCommandByte;
        request->commands[1].command = kPS2C_ReadDataPortAndCompare;
        request->commands[1].inOrOut = commandByte;
        request->commands[2].command = kPS2C_WriteCommandPort;
        request->commands[2].inOrOut = kCP_SetCommandByte;
        request->commands[3].command = kPS2C_WriteDataPort;
        request->commands[3].inOrOut = commandByteNew;
        request->commandsCount = 4;
        _device->submitRequestAndBlock(request);
		
        //
        // Repeat this loop if last command failed, that is, if the
        // old command byte was modified since we first read it.
        //
		
    } while (request->commandsCount != 4);
	
    _device->freeRequest(request);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void ApplePS2ElanTrackpad::interruptOccurred( UInt8 data )
{
    //DEBUG_LOG("interruptOccurred");

	if (etd->debug > 1)
		elantech_packet_dump(data);

    //
    // This will be invoked automatically from our device when asynchronous
    // events need to be delivered. Process the trackpad data. Do NOT issue
    // any BLOCKING commands to our device in this context.
    //
    // Ignore all bytes until we see the start of a packet, otherwise the
    // packets may get out of sequence and things will get very confusing.
    //

    _packetBuffer[_packetByteCount++] = data;

	if(_packetByteCount == pktsize) // Absolute mode
	{
        //DEBUG_LOG("pktsize = %d", pktsize);
        int packet_type;
        switch (etd->hw_version) {
            case 1:
                /*
                if (etd->paritycheck && !elantech_packet_check_v1(etd, _packetBuffer)) {
                    
                } else {
                    elantech_report_absolute_v1(psmouse);                    
                }
                 */
                
                break;
                
            case 2:
                /* ignore debounce */
                /*
                if (elantech_debounce_check_v2(psmouse))
                    return PSMOUSE_FULL_PACKET;
                
                if (etd->paritycheck && !elantech_packet_check_v2(psmouse))
                    return PSMOUSE_BAD_DATA;
                
                elantech_report_absolute_v2(psmouse);
                 */
                break;
                
            case 3:
                packet_type = elantech_packet_check_v3(etd, _packetBuffer);
                /* ignore debounce */
                if (packet_type != PACKET_DEBOUNCE && packet_type != PACKET_UNKNOWN)
                    elantech_report_absolute_v3(_packetBuffer, pktsize, packet_type);
                break;
                
            case 4:
                /*
                packet_type = elantech_packet_check_v4(psmouse);
                if (packet_type == PACKET_UNKNOWN)
                    return PSMOUSE_BAD_DATA;
                
                elantech_report_absolute_v4(psmouse, packet_type);
                 */
                break;
        }
        
		return;
	}
	return;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void ApplePS2ElanTrackpad::elantech_packet_dump( UInt8 data )
{
	static int i = 0;
    //DEBUG_LOG("%d: 0x%02x", i, data);
    i++;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
/*
 * Interpret complete data packets and report absolute mode input events for
 * hardware version 3. (12 byte packets for two fingers)
 */

void ApplePS2ElanTrackpad::elantech_report_absolute_v3(UInt8* packet, UInt32 packetSize, int packet_type)
{
    /*
    if (etd->debug > 1) {
        DEBUG_LOG("elantech_report_absolute_v3: packet_type = %d", packet_type);
        for (int i = 0; i * pktsize < packetSize; ++i) {
            DEBUG_LOG("elantech_report_absolute_v3: i = %d, data = 0x%02x, 0x%02x, 0x%02x, 0x%02x, 0x%02x, 0x%02x", i,
                      packet[i * pktsize], packet[i * pktsize + 1], packet[i * pktsize + 2], 
                      packet[i * pktsize + 3], packet[i * pktsize + 4], packet[i * pktsize + 5]);
        }
          
    }
     */
    
    unsigned int fingers = 0, x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    int x_diff, y_diff;
	unsigned int width = 0, pres = 0;
    
	UInt32       buttons = 0;
    
#if APPLESDK
	clock_get_uptime(&now);
#else
	clock_get_uptime((uint64_t*)&now);
#endif
    
	/* byte 0: n1  n0   .   .   .   .   R   L */
	fingers = (packet[0] & 0xc0) >> 6;
    
	pres = (packet[1] & 0xf0) | ((packet[4] & 0xf0) >> 4);
	width = ((packet[0] & 0x30) >> 2) | ((packet[3] & 0x30) >> 4);
    
    if ( (packet[0] & 0x1) ) buttons |= 0x1;  // left button   (bit 0 in packet)
    if ( (packet[0] & 0x2) ) buttons |= 0x2;  // right button  (bit 1 in packet)

	switch (fingers) {
        case 0:
            if (tapToClick && last_fingers <= 2 && last_fingers > 0) {
                DEBUG_LOG("old buttons 0x%02x", buttons);
                switch (last_fingers) {
                    case 1:
                        buttons |= 0x1;
                        break;
                    case 2:
                        buttons |= 0x2;
                        break;
                }
                DEBUG_LOG("new buttons 0x%02x", buttons);
                tapToClick = false;
            }
            
            dispatchRelativePointerEvent(0, 0, buttons, now);
            validLastPoint = false;
            for (int i = 0; i < ETP_MAX_FINGERS; ++i) {
                validStartPoint[i] = false;
                tapInRange[i] = true;
            }
            tapToClick = true;
            break;
        case 3:
            break;
        case 1:
            /*
             * byte 1:  .   .   .   .  x11 x10 x9  x8
             * byte 2: x7  x6  x5  x4  x4  x2  x1  x0
             */
            x1 = ((packet[1] & 0x0f) << 8) | packet[2];
            /*
             * byte 4:  .   .   .   .  y11 y10 y9  y8
             * byte 5: y7  y6  y5  y4  y3  y2  y1  y0
             */
            y1 = etd->y_max - (((packet[4] & 0x0f) << 8) | packet[5]);
            
            if (x1 != 0 && y1 != 0) {
                if (validLastPoint)
                    dispatchRelativePointerEvent(x1 - lastPoint.x, y1 - lastPoint.y, buttons, now);
                
                lastPoint.x = x1;
                lastPoint.y = y1;
                
                validLastPoint = true;

                if (validStartPoint[0]) {
                    if (tapInRange) {
                        x_diff = x1 - startPoint[0].x;
                        y_diff = y1 - startPoint[0].y;
                        DEBUG_LOG("x_diff %d, y_diff %d", x_diff, y_diff);
                    
                        DEBUG_LOG("before check tapToClick %d, tapInRange[0] %d", tapToClick, tapInRange[0]);
                        
                        if (((x_diff < -ETP_TAPTOCLICK_DIST) || (x_diff > ETP_TAPTOCLICK_DIST) || (y_diff < -ETP_TAPTOCLICK_DIST) || (y_diff > ETP_TAPTOCLICK_DIST))) {
                            DEBUG_LOG("tapInRange[0] is false now %d", ((x_diff < -ETP_TAPTOCLICK_DIST) || (x_diff > ETP_TAPTOCLICK_DIST) || (y_diff < -ETP_TAPTOCLICK_DIST) || (y_diff > ETP_TAPTOCLICK_DIST)));
                            tapInRange[0] = false;
                        }
                    
                        if (!tapInRange[0])
                            tapToClick = false;
                        
                        DEBUG_LOG("after check tapToClick %d, tapInRange[0] %d", tapToClick, tapInRange[0]);
                    }
                }
                
                if (!validStartPoint[0]) {
                    startPoint[0].x = x1;
                    startPoint[0].y = y1;
                    
                    validStartPoint[0] = true;
                    DEBUG_LOG("start point %d, x %d, y %d", validStartPoint[0], startPoint[0].x, startPoint[0].y);
                }
            }
            break;
            
        case 2:
            if (packet_type == PACKET_V3_HEAD) {
                /*
                 * byte 1:   .    .    .    .  ax11 ax10 ax9  ax8
                 * byte 2: ax7  ax6  ax5  ax4  ax3  ax2  ax1  ax0
                 */
                x1 = ((packet[1] & 0x0f) << 8) | packet[2];
                /*
                 * byte 4:   .    .    .    .  ay11 ay10 ay9  ay8
                 * byte 5: ay7  ay6  ay5  ay4  ay3  ay2  ay1  ay0
                 */
                y1 = etd->y_max - (((packet[4] & 0x0f) << 8) | packet[5]);
                
                if (x1 != 0 && y1 != 0) {
                    if (validStartPoint[0]) {
                        if (tapInRange[0]) {
                            x_diff = x1 - startPoint[0].x;
                            y_diff = y1 - startPoint[0].y;
                            DEBUG_LOG("x_diff %d, y_diff %d", x_diff, y_diff);
                        
                            DEBUG_LOG("before check tapToClick %d, tapInRange[0] %d", tapToClick, tapInRange[0]);
                            
                            if (((x_diff < -ETP_TAPTOCLICK_DIST) || (x_diff > ETP_TAPTOCLICK_DIST) || (y_diff < -ETP_TAPTOCLICK_DIST) || (y_diff > ETP_TAPTOCLICK_DIST))) {
                                DEBUG_LOG("tapInRange[0] is false now %d", ((x_diff < -ETP_TAPTOCLICK_DIST) || (x_diff > ETP_TAPTOCLICK_DIST) || (y_diff < -ETP_TAPTOCLICK_DIST) || (y_diff > ETP_TAPTOCLICK_DIST)));
                                tapInRange[0] = false;
                            }
                            
                            if (!tapInRange[0])
                                tapToClick = false;
                            
                            DEBUG_LOG("after check tapToClick %d, tapInRange[0] %d", tapToClick, tapInRange[0]);
                        }
                    }
                    
                    if (!validStartPoint[0]) {
                        startPoint[0].x = x1;
                        startPoint[0].y = y1;
                        
                        validStartPoint[0] = true;
                        DEBUG_LOG("start point 0: %d, x %d, y %d", validStartPoint[0], startPoint[0].x, startPoint[0].y);
                    }
                }
                
                /*
                 * wait for next packet
                 */
                etd->mt[0].x = x1;
                etd->mt[0].y = y1;
            } else {        
                /* packet_type == PACKET_V3_TAIL */
                x2 = ((packet[1] & 0x0f) << 8) | packet[2];
                y2 = etd->y_max - (((packet[4] & 0x0f) << 8) | packet[5]);
                
                if (x2 != 0 && y2 != 0) {
                    if (validStartPoint[1]) {
                        if (tapInRange[1]) {
                            x_diff = x2 - startPoint[1].x;
                            y_diff = y2 - startPoint[1].y;
                            DEBUG_LOG("x_diff %d, y_diff %d", x_diff, y_diff);
                            
                            DEBUG_LOG("before check tapToClick %d, tapInRange[1] %d", tapToClick, tapInRange[1]);
                            
                            if (((x_diff < -ETP_TAPTOCLICK_DIST) || (x_diff > ETP_TAPTOCLICK_DIST) || (y_diff < -ETP_TAPTOCLICK_DIST) || (y_diff > ETP_TAPTOCLICK_DIST))) {
                                DEBUG_LOG("tapInRange[0] is false now %d", ((x_diff < -ETP_TAPTOCLICK_DIST) || (x_diff > ETP_TAPTOCLICK_DIST) || (y_diff < -ETP_TAPTOCLICK_DIST) || (y_diff > ETP_TAPTOCLICK_DIST)));
                                tapInRange[1] = false;
                            }
                            
                            if (!tapInRange[1])
                                tapToClick = false;
                            
                            DEBUG_LOG("after check tapToClick %d, tapInRange[1] %d", tapToClick, tapInRange[1]);
                        }
                    }
                    
                    if (!validStartPoint[1]) {
                        startPoint[1].x = x2;
                        startPoint[1].y = y2;
                        
                        validStartPoint[1] = true;
                        DEBUG_LOG("start point 1: %d, x %d, y %d", validStartPoint[1], startPoint[1].x, startPoint[1].y);
                    }
                }
                
            }
            break;
	}

    if (etd->debug > 1) {
        DEBUG_LOG("fingers %d, x1 %d, y1 %d, x2 %d, y2 %d, width %d, pres %d",
                  fingers, x1, y1, x2, y2, width, pres);
    }
    
    last_fingers = fingers;
    _packetByteCount = 0;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void ApplePS2ElanTrackpad::setDevicePowerState( UInt32 whatToDo )
{
    switch ( whatToDo )
    {
        case kPS2C_DisableDevice:
            
            //
            // Disable touchpad.
            //
            
            setTouchPadEnable( false );
            break;
            
        case kPS2C_EnableDevice:
		case 2:  //Slice :)
			
			IOSleep(1000);
            
            //setTapEnable( _touchPadModeByte );
            
            
            //
            // Enable the mouse clock (should already be so) and the
            // mouse IRQ line.
            //
            
            setCommandByte( kCB_EnableMouseIRQ, kCB_DisableMouseClock );
            
			//DEBUG_LOG(" ABMod Waking up Touchpad setting setTapEnable to %d\n",_touchPadModeByte);
            
			//
			//This is the way devs hit things when they don't work
			//Hit false / Hit true - OK now we have a picture good sit back and relax- ab_73
			//
			setTouchPadEnable( true );
            
			setTouchPadEnable(false);
			setTouchPadEnable(true);
            
			//
            // Finally, we enable the trackpad itself, so that it may
            // start reporting asynchronous events.
            //
			
			//setTapEnable( _touchPadModeByte );
			//if(_packetByteCount == 6)
			//{
				//setAbsoluteMode();
			//}
            setTouchPadEnable( true );
            break;
	}
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void ApplePS2ElanTrackpad::stop( IOService * provider )
{
    //
    // The driver has been instructed to stop.  Note that we must break all
    // connections to other service objects now (ie. no registered actions,
    // no pointers and retains to objects, etc), if any.
    //
    
    assert(_device == provider);
    DEBUG_LOG("stop");
    //
    // Disable the mouse itself, so that it may stop reporting mouse events.
    //
    
    setTouchPadEnable(false);
    
    //
    // Disable the mouse clock and the mouse IRQ line.
    //
    
    setCommandByte( kCB_DisableMouseClock, kCB_EnableMouseIRQ );
    
    //
    // Uninstall the interrupt handler.
    //
    
    if ( _interruptHandlerInstalled )  _device->uninstallInterruptAction();
    _interruptHandlerInstalled = false;
    
    //
    // Uninstall the power control handler.
    //
    
    if ( _powerControlHandlerInstalled ) _device->uninstallPowerControlAction();
    _powerControlHandlerInstalled = false;
    
	super::stop(provider);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
int ApplePS2ElanTrackpad::psmouse_sliced_command( PS2Request * request, int &j, UInt command )
{
    if ( !request ) return -1;
    
    request->commands[j].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[j].inOrOut = kDP_SetMouseScaling1To1;
    for (int i = 6; i >= 0; i -= 2) {
        unsigned char d = (command >> i) & 3;
        j++;
        request->commands[j].command = kPS2C_SendMouseCommandAndCompareAck;
        request->commands[j].inOrOut = kDP_SetMouseResolution;
        j++;
        request->commands[j].command = kPS2C_SendMouseCommandAndCompareAck;
        request->commands[j].inOrOut = d;
    }
    return 0;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

int ApplePS2ElanTrackpad::send_cmd( IOService * provider, UInt8 c, UInt8 *param, bool s )
{
    if (s)
        return elantech_send_cmd(provider, c, param);
    else
        return synaptics_send_cmd(provider, c, param);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void ApplePS2ElanTrackpad::setTouchPadEnable( bool enable )
{
    //
    // Instructs the trackpad to start or stop the reporting of data packets.
    // It is safe to issue this request from the interrupt/completion context.
    //
    
    PS2Request * request = _device->allocateRequest();
    if ( !request ) return;
    
    // (mouse enable/disable command)
    request->commands[0].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[0].inOrOut = (enable)?kDP_Enable:kDP_SetDefaultsAndDisable;
    request->commandsCount = 1;
    _device->submitRequestAndBlock(request);
    _device->freeRequest(request);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
/*
 * A retrying version of ps2_command
 */
/*
int ApplePS2ElanTrackpad::elantech_ps2_command(IOService * provider, UInt8 *param, UInt command)
{
    _device = (ApplePS2MouseDevice *) provider;
    
    PS2Request * request = _device->allocateRequest();
    
    if ( !request ) return -1;
    
	int rc;
	int tries = ETP_PS2_COMMAND_TRIES;
    int j = 0;
    
	do {
        request->commands[j].command = kPS2C_SendMouseCommandAndCompareAck;
        request->commands[j].inOrOut = command;
        j++;
        request->commandsCount = j;
        _device->submitRequestAndBlock(request);

		rc = ps2_command(ps2dev, param, command);
		if (rc == 0)
			break;
		tries--;
		DEBUG_LOG("retrying ps2 command 0x%02x (%d).", command, tries);
        IOSleep(ETP_PS2_COMMAND_DELAY);
	} while (tries > 0);
    
	if (rc)
		DEBUG_LOG("ps2 command 0x%02x failed.", command);
    
	return rc;
    
}
 */
// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
// port note: elantech_ps2_command was not ported yet, not sure works fine or not

int ApplePS2ElanTrackpad::elantech_write_reg( IOService * provider, UInt8 reg, UInt8 val )
{
	if (reg < 0x07 || reg > 0x26)
		return -1;
    
	if (reg > 0x11 && reg < 0x20)
		return -1;
    
    _device = (ApplePS2MouseDevice *) provider;
    
    PS2Request * request = _device->allocateRequest();
    
    if ( !request ) return -1;
    DEBUG_LOG("elantech_write_reg: reg = 0x%02x, val = 0x%02x", reg, val);
    
    int j = 0;
	switch (etd->hw_version) {
        case 1:
            psmouse_sliced_command(request, j, ETP_REGISTER_WRITE); j++;
            psmouse_sliced_command(request, j, reg); j++;
            psmouse_sliced_command(request, j, val); j++;
            request->commands[j].command = kPS2C_SendMouseCommandAndCompareAck;
            request->commands[j].inOrOut = kDP_SetMouseScaling1To1;
            j++;
            request->commandsCount = j;
            _device->submitRequestAndBlock(request);
            
            break;
            
        case 2:
            request->commands[j].command = kPS2C_SendMouseCommandAndCompareAck;
            request->commands[j].inOrOut = ETP_PS2_CUSTOM_COMMAND;
            j++;
            request->commands[j].command = kPS2C_SendMouseCommandAndCompareAck;
            request->commands[j].inOrOut = ETP_REGISTER_WRITE;
            j++;
            request->commands[j].command = kPS2C_SendMouseCommandAndCompareAck;
            request->commands[j].inOrOut = ETP_PS2_CUSTOM_COMMAND;
            j++;
            request->commands[j].command = kPS2C_SendMouseCommandAndCompareAck;
            request->commands[j].inOrOut = reg;
            j++;
            request->commands[j].command = kPS2C_SendMouseCommandAndCompareAck;
            request->commands[j].inOrOut = ETP_PS2_CUSTOM_COMMAND;
            j++;
            request->commands[j].command = kPS2C_SendMouseCommandAndCompareAck;
            request->commands[j].inOrOut = val;
            j++;
            request->commands[j].command = kPS2C_SendMouseCommandAndCompareAck;
            request->commands[j].inOrOut = kDP_SetMouseScaling1To1;
            j++;
            request->commandsCount = j;
            _device->submitRequestAndBlock(request);
            
            break;
            
        case 3:
            request->commands[j].command = kPS2C_SendMouseCommandAndCompareAck;
            request->commands[j].inOrOut = ETP_PS2_CUSTOM_COMMAND;
            j++;
            request->commands[j].command = kPS2C_SendMouseCommandAndCompareAck;
            request->commands[j].inOrOut = ETP_REGISTER_READWRITE;
            j++;
            request->commands[j].command = kPS2C_SendMouseCommandAndCompareAck;
            request->commands[j].inOrOut = ETP_PS2_CUSTOM_COMMAND;
            j++;
            request->commands[j].command = kPS2C_SendMouseCommandAndCompareAck;
            request->commands[j].inOrOut = reg;
            j++;
            request->commands[j].command = kPS2C_SendMouseCommandAndCompareAck;
            request->commands[j].inOrOut = ETP_PS2_CUSTOM_COMMAND;
            j++;
            request->commands[j].command = kPS2C_SendMouseCommandAndCompareAck;
            request->commands[j].inOrOut = val;
            j++;
            request->commands[j].command = kPS2C_SendMouseCommandAndCompareAck;
            request->commands[j].inOrOut = kDP_SetMouseScaling1To1;
            j++;
            request->commandsCount = j;
            _device->submitRequestAndBlock(request);
            
            break;
            
        case 4:
            request->commands[j].command = kPS2C_SendMouseCommandAndCompareAck;
            request->commands[j].inOrOut = ETP_PS2_CUSTOM_COMMAND;
            j++;
            request->commands[j].command = kPS2C_SendMouseCommandAndCompareAck;
            request->commands[j].inOrOut = ETP_REGISTER_READWRITE;
            j++;
            request->commands[j].command = kPS2C_SendMouseCommandAndCompareAck;
            request->commands[j].inOrOut = ETP_PS2_CUSTOM_COMMAND;
            j++;
            request->commands[j].command = kPS2C_SendMouseCommandAndCompareAck;
            request->commands[j].inOrOut = reg;
            j++;
            request->commands[j].command = kPS2C_SendMouseCommandAndCompareAck;
            request->commands[j].inOrOut = ETP_PS2_CUSTOM_COMMAND;
            j++;
            request->commands[j].command = kPS2C_SendMouseCommandAndCompareAck;
            request->commands[j].inOrOut = ETP_REGISTER_READWRITE;
            j++;
            request->commands[j].command = kPS2C_SendMouseCommandAndCompareAck;
            request->commands[j].inOrOut = ETP_PS2_CUSTOM_COMMAND;
            j++;
            request->commands[j].command = kPS2C_SendMouseCommandAndCompareAck;
            request->commands[j].inOrOut = val;
            j++;
            request->commands[j].command = kPS2C_SendMouseCommandAndCompareAck;
            request->commands[j].inOrOut = kDP_SetMouseScaling1To1;
            j++;
            request->commandsCount = j;
            _device->submitRequestAndBlock(request);
            
            break;
	}
    
	return 0;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
// port note: elantech_ps2_command was not ported yet, not sure works fine or not

int ApplePS2ElanTrackpad::elantech_read_reg( IOService * provider, UInt8 reg, UInt8 *val )
{
	UInt8 param[3];
    
	if (reg < 0x07 || reg > 0x26)
		return -1;
    
	if (reg > 0x11 && reg < 0x20)
		return -1;

    _device = (ApplePS2MouseDevice *) provider;
    
    PS2Request * request = _device->allocateRequest();
    
    if ( !request ) return -1;
    DEBUG_LOG("elantech_read_reg: reg = 0x%02x", reg);
    
    int j = 0;
	switch (etd->hw_version) {
        case 1:
            psmouse_sliced_command(request, j, ETP_REGISTER_READ); j++;
            psmouse_sliced_command(request, j, reg); j++;

            break;
            
        case 2:
            request->commands[j].command = kPS2C_SendMouseCommandAndCompareAck;
            request->commands[j].inOrOut = ETP_PS2_CUSTOM_COMMAND;
            j++;
            request->commands[j].command = kPS2C_SendMouseCommandAndCompareAck;
            request->commands[j].inOrOut = ETP_REGISTER_READ;
            j++;
            request->commands[j].command = kPS2C_SendMouseCommandAndCompareAck;
            request->commands[j].inOrOut = ETP_PS2_CUSTOM_COMMAND;
            j++;
            request->commands[j].command = kPS2C_SendMouseCommandAndCompareAck;
            request->commands[j].inOrOut = reg;
            j++;

            break;
            
        case 3 ... 4:
            request->commands[j].command = kPS2C_SendMouseCommandAndCompareAck;
            request->commands[j].inOrOut = ETP_PS2_CUSTOM_COMMAND;
            j++;
            request->commands[j].command = kPS2C_SendMouseCommandAndCompareAck;
            request->commands[j].inOrOut = ETP_REGISTER_READWRITE;
            j++;
            request->commands[j].command = kPS2C_SendMouseCommandAndCompareAck;
            request->commands[j].inOrOut = ETP_PS2_CUSTOM_COMMAND;
            j++;
            request->commands[j].command = kPS2C_SendMouseCommandAndCompareAck;
            request->commands[j].inOrOut = reg;
            j++;

            break;
	}
    request->commands[j].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[j].inOrOut = kDP_GetMouseInformation;
    j++;
    request->commands[j].command = kPS2C_ReadDataPort;
    request->commands[j].inOrOut = 0;
    j++;
    request->commands[j].command = kPS2C_ReadDataPort;
    request->commands[j].inOrOut = 0;
    j++;
    request->commands[j].command = kPS2C_ReadDataPort;
    request->commands[j].inOrOut = 0;
    j++;
    request->commandsCount = j;
    _device->submitRequestAndBlock(request);
    
    param[0] = request->commands[j-3].inOrOut;
    param[1] = request->commands[j-2].inOrOut;
    param[2] = request->commands[j-1].inOrOut;
    _device->freeRequest(request);
    
    if (etd->hw_version != 4)
		*val = param[0];
	else
		*val = param[1];
    
    DEBUG_LOG("elantech_read_reg: val = 0x%02x", *val);
    
	return 0;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

int ApplePS2ElanTrackpad::synaptics_send_cmd( IOService * provider, UInt8 c, UInt8 *param )
{
    _device = (ApplePS2MouseDevice *) provider;
    
    PS2Request * request = _device->allocateRequest();
    
    if ( !request ) return -1;
    DEBUG_LOG("synaptics_send_cmd: cmd = 0x%02x", c);
    
    int j = 0;
    psmouse_sliced_command(request, j, c); j++;
    request->commands[j].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[j].inOrOut = kDP_GetMouseInformation;
    j++;
    request->commands[j].command = kPS2C_ReadDataPort;
    request->commands[j].inOrOut = 0;
    j++;
    request->commands[j].command = kPS2C_ReadDataPort;
    request->commands[j].inOrOut = 0;
    j++;
    request->commands[j].command = kPS2C_ReadDataPort;
    request->commands[j].inOrOut = 0;
    j++;
    request->commandsCount = j;
    _device->submitRequestAndBlock(request);
    
    param[0] = request->commands[j-3].inOrOut;
    param[1] = request->commands[j-2].inOrOut;
    param[2] = request->commands[j-1].inOrOut;
    _device->freeRequest(request);

    DEBUG_LOG("synaptics_send_cmd result 0x%02x, 0x%02x, 0x%02x.",
              param[0], param[1], param[2]);
    
    return 0;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

int ApplePS2ElanTrackpad::elantech_send_cmd( IOService * provider, UInt8 c, UInt8 *param )
{
    _device = (ApplePS2MouseDevice *) provider;
    
    PS2Request * request = _device->allocateRequest();
    
    if ( !request ) return -1;
    DEBUG_LOG("elantech_send_cmd: cmd = 0x%02x", c);
    
    int j = 0;
    request->commands[j].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[j].inOrOut = kDP_SetAllMakeRelease;
    j++;
    request->commands[j].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[j].inOrOut = c;
    j++;
    request->commands[j].command = kPS2C_SendMouseCommandAndCompareAck;
    request->commands[j].inOrOut = kDP_GetMouseInformation;
    j++;
    request->commands[j].command = kPS2C_ReadDataPort;
    request->commands[j].inOrOut = 0;
    j++;
    request->commands[j].command = kPS2C_ReadDataPort;
    request->commands[j].inOrOut = 0;
    j++;
    request->commands[j].command = kPS2C_ReadDataPort;
    request->commands[j].inOrOut = 0;
    j++;
    request->commandsCount = j;
    _device->submitRequestAndBlock(request);
    
    param[0] = request->commands[j-3].inOrOut;
    param[1] = request->commands[j-2].inOrOut;
    param[2] = request->commands[j-1].inOrOut;
    _device->freeRequest(request);

    DEBUG_LOG("elantech_send_cmd result 0x%02x, 0x%02x, 0x%02x.",
              param[0], param[1], param[2]);

    return 0;
}
