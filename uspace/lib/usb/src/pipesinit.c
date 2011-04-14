/*
 * Copyright (c) 2011 Vojtech Horky
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * - The name of the author may not be used to endorse or promote products
 *   derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/** @addtogroup libusb
 * @{
 */
/** @file
 * Initialization of endpoint pipes.
 *
 */
#include <usb/usb.h>
#include <usb/pipes.h>
#include <usb/dp.h>
#include <usb/request.h>
#include <usbhc_iface.h>
#include <errno.h>
#include <assert.h>

#define CTRL_PIPE_MIN_PACKET_SIZE 8
#define DEV_DESCR_MAX_PACKET_SIZE_OFFSET 7


#define NESTING(parentname, childname) \
	{ \
		.child = USB_DESCTYPE_##childname, \
		.parent = USB_DESCTYPE_##parentname, \
	}
#define LAST_NESTING { -1, -1 }

/** Nesting pairs of standard descriptors. */
static usb_dp_descriptor_nesting_t descriptor_nesting[] = {
	NESTING(CONFIGURATION, INTERFACE),
	NESTING(INTERFACE, ENDPOINT),
	NESTING(INTERFACE, HUB),
	NESTING(INTERFACE, HID),
	NESTING(HID, HID_REPORT),
	LAST_NESTING
};

/** Tells whether given descriptor is of endpoint type.
 *
 * @param descriptor Descriptor in question.
 * @return Whether the given descriptor is endpoint descriptor.
 */
static inline bool is_endpoint_descriptor(uint8_t *descriptor)
{
	return descriptor[1] == USB_DESCTYPE_ENDPOINT;
}

/** Tells whether found endpoint corresponds to endpoint described by user.
 *
 * @param wanted Endpoint description as entered by driver author.
 * @param found Endpoint description obtained from endpoint descriptor.
 * @return Whether the @p found descriptor fits the @p wanted descriptor.
 */
static bool endpoint_fits_description(const usb_endpoint_description_t *wanted,
    usb_endpoint_description_t *found)
{
#define _SAME(fieldname) ((wanted->fieldname) == (found->fieldname))

	if (!_SAME(direction)) {
		return false;
	}

	if (!_SAME(transfer_type)) {
		return false;
	}

	if ((wanted->interface_class >= 0) && !_SAME(interface_class)) {
		return false;
	}

	if ((wanted->interface_subclass >= 0) && !_SAME(interface_subclass)) {
		return false;
	}

	if ((wanted->interface_protocol >= 0) && !_SAME(interface_protocol)) {
		return false;
	}

#undef _SAME

	return true;
}

/** Find endpoint mapping for a found endpoint.
 *
 * @param mapping Endpoint mapping list.
 * @param mapping_count Number of endpoint mappings in @p mapping.
 * @param found_endpoint Description of found endpoint.
 * @param interface_number Number of currently processed interface.
 * @return Endpoint mapping corresponding to @p found_endpoint.
 * @retval NULL No corresponding endpoint found.
 */
static usb_endpoint_mapping_t *find_endpoint_mapping(
    usb_endpoint_mapping_t *mapping, size_t mapping_count,
    usb_endpoint_description_t *found_endpoint,
    int interface_number, int interface_setting)
{
	while (mapping_count > 0) {
		bool interface_number_fits = (mapping->interface_no < 0)
		    || (mapping->interface_no == interface_number);

		bool interface_setting_fits = (mapping->interface_setting < 0)
		    || (mapping->interface_setting == interface_setting);

		bool endpoint_descriptions_fits = endpoint_fits_description(
		    mapping->description, found_endpoint);

		if (interface_number_fits
		    && interface_setting_fits
		    && endpoint_descriptions_fits) {
			return mapping;
		}

		mapping++;
		mapping_count--;
	}
	return NULL;
}

/** Process endpoint descriptor.
 *
 * @param mapping Endpoint mapping list.
 * @param mapping_count Number of endpoint mappings in @p mapping.
 * @param interface Interface descriptor under which belongs the @p endpoint.
 * @param endpoint Endpoint descriptor.
 * @param wire Connection backing the endpoint pipes.
 * @return Error code.
 */
static int process_endpoint(
    usb_endpoint_mapping_t *mapping, size_t mapping_count,
    usb_standard_interface_descriptor_t *interface,
    usb_standard_endpoint_descriptor_t *endpoint,
    usb_device_connection_t *wire)
{
	usb_endpoint_description_t description;

	/*
	 * Get endpoint characteristics.
	 */

	/* Actual endpoint number is in bits 0..3 */
	usb_endpoint_t ep_no = endpoint->endpoint_address & 0x0F;

	/* Endpoint direction is set by bit 7 */
	description.direction = (endpoint->endpoint_address & 128)
	    ? USB_DIRECTION_IN : USB_DIRECTION_OUT;
	/* Transfer type is in bits 0..2 and the enum values corresponds 1:1 */
	description.transfer_type = endpoint->attributes & 3;

	/*
	 * Get interface characteristics.
	 */
	description.interface_class = interface->interface_class;
	description.interface_subclass = interface->interface_subclass;
	description.interface_protocol = interface->interface_protocol;

	/*
	 * Find the most fitting mapping and initialize the pipe.
	 */
	usb_endpoint_mapping_t *ep_mapping = find_endpoint_mapping(mapping,
	    mapping_count, &description,
	    interface->interface_number, interface->alternate_setting);
	if (ep_mapping == NULL) {
		return ENOENT;
	}

	if (ep_mapping->pipe == NULL) {
		return EBADMEM;
	}
	if (ep_mapping->present) {
		return EEXISTS;
	}

	int rc = usb_pipe_initialize(ep_mapping->pipe, wire,
	    ep_no, description.transfer_type, endpoint->max_packet_size,
	    description.direction);
	if (rc != EOK) {
		return rc;
	}

	ep_mapping->present = true;
	ep_mapping->descriptor = endpoint;
	ep_mapping->interface = interface;

	return EOK;
}

/** Process whole USB interface.
 *
 * @param mapping Endpoint mapping list.
 * @param mapping_count Number of endpoint mappings in @p mapping.
 * @param parser Descriptor parser.
 * @param parser_data Descriptor parser data.
 * @param interface_descriptor Interface descriptor.
 * @return Error code.
 */
static int process_interface(
    usb_endpoint_mapping_t *mapping, size_t mapping_count,
    usb_dp_parser_t *parser, usb_dp_parser_data_t *parser_data,
    uint8_t *interface_descriptor)
{
	uint8_t *descriptor = usb_dp_get_nested_descriptor(parser,
	    parser_data, interface_descriptor);

	if (descriptor == NULL) {
		return ENOENT;
	}

	do {
		if (is_endpoint_descriptor(descriptor)) {
			(void) process_endpoint(mapping, mapping_count,
			    (usb_standard_interface_descriptor_t *)
			        interface_descriptor,
			    (usb_standard_endpoint_descriptor_t *)
			        descriptor,
			    (usb_device_connection_t *) parser_data->arg);
		}

		descriptor = usb_dp_get_sibling_descriptor(parser, parser_data,
		    interface_descriptor, descriptor);
	} while (descriptor != NULL);

	return EOK;
}

/** Initialize endpoint pipes from configuration descriptor.
 *
 * The mapping array is expected to conform to following rules:
 * - @c pipe must point to already allocated structure with uninitialized pipe
 * - @c description must point to prepared endpoint description
 * - @c descriptor does not need to be initialized (will be overwritten)
 * - @c interface does not need to be initialized (will be overwritten)
 * - @c present does not need to be initialized (will be overwritten)
 *
 * After processing the configuration descriptor, the mapping is updated
 * in the following fashion:
 * - @c present will be set to @c true when the endpoint was found in the
 *   configuration
 * - @c descriptor will point inside the configuration descriptor to endpoint
 *   corresponding to given description (or NULL for not found descriptor)
 * - @c interface will point inside the configuration descriptor to interface
 *   descriptor the endpoint @c descriptor belongs to (or NULL for not found
 *   descriptor)
 * - @c pipe will be initialized when found, otherwise left untouched
 * - @c description will be untouched under all circumstances
 *
 * @param mapping Endpoint mapping list.
 * @param mapping_count Number of endpoint mappings in @p mapping.
 * @param configuration_descriptor Full configuration descriptor (is expected
 *	to be in USB endianness: i.e. as-is after being retrieved from
 *	the device).
 * @param configuration_descriptor_size Size of @p configuration_descriptor
 *	in bytes.
 * @param connection Connection backing the endpoint pipes.
 * @return Error code.
 */
int usb_pipe_initialize_from_configuration(
    usb_endpoint_mapping_t *mapping, size_t mapping_count,
    uint8_t *configuration_descriptor, size_t configuration_descriptor_size,
    usb_device_connection_t *connection)
{
	assert(connection);

	if (configuration_descriptor == NULL) {
		return EBADMEM;
	}
	if (configuration_descriptor_size
	    < sizeof(usb_standard_configuration_descriptor_t)) {
		return ERANGE;
	}

	/*
	 * Go through the mapping and set all endpoints to not present.
	 */
	size_t i;
	for (i = 0; i < mapping_count; i++) {
		mapping[i].present = false;
		mapping[i].descriptor = NULL;
		mapping[i].interface = NULL;
	}

	/*
	 * Prepare the descriptor parser.
	 */
	usb_dp_parser_t dp_parser = {
		.nesting = descriptor_nesting
	};
	usb_dp_parser_data_t dp_data = {
		.data = configuration_descriptor,
		.size = configuration_descriptor_size,
		.arg = connection
	};

	/*
	 * Iterate through all interfaces.
	 */
	uint8_t *interface = usb_dp_get_nested_descriptor(&dp_parser,
	    &dp_data, configuration_descriptor);
	if (interface == NULL) {
		return ENOENT;
	}
	do {
		(void) process_interface(mapping, mapping_count,
		    &dp_parser, &dp_data,
		    interface);
		interface = usb_dp_get_sibling_descriptor(&dp_parser, &dp_data,
		    configuration_descriptor, interface);
	} while (interface != NULL);

	return EOK;
}

/** Initialize USB endpoint pipe.
 *
 * @param pipe Endpoint pipe to be initialized.
 * @param connection Connection to the USB device backing this pipe (the wire).
 * @param endpoint_no Endpoint number (in USB 1.1 in range 0 to 15).
 * @param transfer_type Transfer type (e.g. interrupt or bulk).
 * @param max_packet_size Maximum packet size in bytes.
 * @param direction Endpoint direction (in/out).
 * @return Error code.
 */
int usb_pipe_initialize(usb_pipe_t *pipe,
    usb_device_connection_t *connection, usb_endpoint_t endpoint_no,
    usb_transfer_type_t transfer_type, size_t max_packet_size,
    usb_direction_t direction)
{
	assert(pipe);
	assert(connection);

	fibril_mutex_initialize(&pipe->guard);
	pipe->wire = connection;
	pipe->hc_phone = -1;
	fibril_mutex_initialize(&pipe->hc_phone_mutex);
	pipe->endpoint_no = endpoint_no;
	pipe->transfer_type = transfer_type;
	pipe->max_packet_size = max_packet_size;
	pipe->direction = direction;
	pipe->refcount = 0;
	pipe->auto_reset_halt = false;

	return EOK;
}


/** Initialize USB endpoint pipe as the default zero control pipe.
 *
 * @param pipe Endpoint pipe to be initialized.
 * @param connection Connection to the USB device backing this pipe (the wire).
 * @return Error code.
 */
int usb_pipe_initialize_default_control(usb_pipe_t *pipe,
    usb_device_connection_t *connection)
{
	assert(pipe);
	assert(connection);

	int rc = usb_pipe_initialize(pipe, connection,
	    0, USB_TRANSFER_CONTROL, CTRL_PIPE_MIN_PACKET_SIZE,
	    USB_DIRECTION_BOTH);

	pipe->auto_reset_halt = true;

	return rc;
}

/** Probe default control pipe for max packet size.
 *
 * The function tries to get the correct value of max packet size several
 * time before giving up.
 *
 * The session on the pipe shall not be started.
 *
 * @param pipe Default control pipe.
 * @return Error code.
 */
int usb_pipe_probe_default_control(usb_pipe_t *pipe)
{
	assert(pipe);
	assert(DEV_DESCR_MAX_PACKET_SIZE_OFFSET < CTRL_PIPE_MIN_PACKET_SIZE);

	if ((pipe->direction != USB_DIRECTION_BOTH) ||
	    (pipe->transfer_type != USB_TRANSFER_CONTROL) ||
	    (pipe->endpoint_no != 0)) {
		return EINVAL;
	}

#define TRY_LOOP(attempt_var) \
	for (attempt_var = 0; attempt_var < 3; attempt_var++)

	size_t failed_attempts;
	int rc;

	rc = usb_pipe_start_long_transfer(pipe);
	if (rc != EOK) {
		return rc;
	}


	uint8_t dev_descr_start[CTRL_PIPE_MIN_PACKET_SIZE];
	size_t transferred_size;
	TRY_LOOP(failed_attempts) {
		rc = usb_request_get_descriptor(pipe, USB_REQUEST_TYPE_STANDARD,
		    USB_REQUEST_RECIPIENT_DEVICE, USB_DESCTYPE_DEVICE,
		    0, 0, dev_descr_start, CTRL_PIPE_MIN_PACKET_SIZE,
		    &transferred_size);
		if (rc == EOK) {
			if (transferred_size != CTRL_PIPE_MIN_PACKET_SIZE) {
				rc = ELIMIT;
				continue;
			}
			break;
		}
	}
	usb_pipe_end_long_transfer(pipe);
	if (rc != EOK) {
		return rc;
	}

	pipe->max_packet_size
	    = dev_descr_start[DEV_DESCR_MAX_PACKET_SIZE_OFFSET];

	return EOK;
}

/** Register endpoint with the host controller.
 *
 * @param pipe Pipe to be registered.
 * @param interval Polling interval.
 * @param hc_connection Connection to the host controller (must be opened).
 * @return Error code.
 */
int usb_pipe_register(usb_pipe_t *pipe,
    unsigned int interval,
    usb_hc_connection_t *hc_connection)
{
	return usb_pipe_register_with_speed(pipe, USB_SPEED_MAX + 1,
	    interval, hc_connection);
}

/** Register endpoint with a speed at the host controller.
 *
 * You will rarely need to use this function because it is needed only
 * if the registered endpoint is of address 0 and there is no other way
 * to tell speed of the device at address 0.
 *
 * @param pipe Pipe to be registered.
 * @param speed Speed of the device
 *	(invalid speed means use previously specified one).
 * @param interval Polling interval.
 * @param hc_connection Connection to the host controller (must be opened).
 * @return Error code.
 */
int usb_pipe_register_with_speed(usb_pipe_t *pipe, usb_speed_t speed,
    unsigned int interval,
    usb_hc_connection_t *hc_connection)
{
	assert(pipe);
	assert(hc_connection);

	if (!usb_hc_connection_is_opened(hc_connection)) {
		return EBADF;
	}

#define _PACK2(high, low) (((high) << 16) + (low))
#define _PACK3(high, middle, low) (((((high) << 8) + (middle)) << 8) + (low))

	return async_req_4_0(hc_connection->hc_phone,
	    DEV_IFACE_ID(USBHC_DEV_IFACE), IPC_M_USBHC_REGISTER_ENDPOINT,
	    _PACK2(pipe->wire->address, pipe->endpoint_no),
	    _PACK3(speed, pipe->transfer_type, pipe->direction),
	    _PACK2(pipe->max_packet_size, interval));

#undef _PACK2
#undef _PACK3
}

/** Revert endpoint registration with the host controller.
 *
 * @param pipe Pipe to be unregistered.
 * @param hc_connection Connection to the host controller (must be opened).
 * @return Error code.
 */
int usb_pipe_unregister(usb_pipe_t *pipe,
    usb_hc_connection_t *hc_connection)
{
	assert(pipe);
	assert(hc_connection);

	if (!usb_hc_connection_is_opened(hc_connection)) {
		return EBADF;
	}

	return async_req_4_0(hc_connection->hc_phone,
	    DEV_IFACE_ID(USBHC_DEV_IFACE), IPC_M_USBHC_UNREGISTER_ENDPOINT,
	    pipe->wire->address, pipe->endpoint_no, pipe->direction);
}

/**
 * @}
 */
