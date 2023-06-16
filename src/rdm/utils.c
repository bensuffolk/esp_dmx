#include "utils.h"

#include <ctype.h>
#include <string.h>

#include "dmx/driver.h"
#include "dmx/hal.h"
#include "endian.h"
#include "esp_dmx.h"
#include "esp_log.h"
#include "rdm/types.h"

static const char *TAG = "rdm_utils";

extern dmx_driver_t *dmx_driver[DMX_NUM_MAX];
extern spinlock_t dmx_spinlock[DMX_NUM_MAX];

void *uidcpy(void *restrict destination, const void *restrict source) {
  *(uint16_t *)destination = bswap16(*(uint16_t *)source);
  *(uint32_t *)(destination + 2) = bswap32(*(uint32_t *)(source + 2));
  return destination;
}

void *uidmove(void *destination, const void *source) {
  const rdm_uid_t temp = {.man_id = ((rdm_uid_t *)source)->man_id,
                          .dev_id = ((rdm_uid_t *)source)->dev_id};
  return uidcpy(destination, &temp);
}

inline bool uid_is_eq(const rdm_uid_t *a, const rdm_uid_t *b) {
  return a->man_id == b->man_id && a->dev_id == b->dev_id;
}

inline bool uid_is_lt(const rdm_uid_t *a, const rdm_uid_t *b) {
  return a->man_id < b->man_id ||
         (a->man_id == b->man_id && a->dev_id < b->dev_id);
}

inline bool uid_is_gt(const rdm_uid_t *a, const rdm_uid_t *b) {
  return a->man_id > b->man_id ||
         (a->man_id == b->man_id && a->dev_id > b->dev_id);
}

inline bool uid_is_le(const rdm_uid_t *a, const rdm_uid_t *b) {
  return !uid_is_gt(a, b);
}

inline bool uid_is_ge(const rdm_uid_t *a, const rdm_uid_t *b) {
  return !uid_is_lt(a, b);
}

inline bool uid_is_broadcast(const rdm_uid_t *uid) {
  return uid->dev_id == 0xffffffff;
}

inline bool uid_is_null(const rdm_uid_t *uid) {
  return uid->man_id == 0 && uid->dev_id == 0;
}

inline bool uid_is_target(const rdm_uid_t *uid, const rdm_uid_t *alias) {
  return ((alias->man_id == 0xffff || alias->man_id == uid->man_id) &&
          alias->dev_id == 0xffffffff) ||
         uid_is_eq(uid, alias);
}

static size_t rdm_param_parse(const char *format, bool *is_singleton) {
  *is_singleton = (*format == '\0');
  int param_size = 0;
  for (const char *f = format; *f != '\0'; ++f) {
    size_t field_size = 0;
    if (*f == 'b' || *f == 'B') {
      field_size = sizeof(uint8_t);  // Handle 8-bit byte
    } else if (*f == 'w' || *f == 'W') {
      field_size = sizeof(uint16_t);  // Handle 16-bit word
    } else if (*f == 'd' || *f == 'D') {
      field_size = sizeof(uint32_t);  // Handle 32-bit dword
    } else if (*f == 'u' || *f == 'U') {
      field_size = sizeof(rdm_uid_t);  // Handle 48-bit UID
    } else if (*f == 'v' || *f == 'V') {
      if (f[1] != '\0' && f[1] != '$') {
        ESP_LOGE(TAG, "Optional UID not at end of parameter.");
        return 0;
      }
      *is_singleton = true;  // Can't declare parameter array with optional UID
      field_size = sizeof(rdm_uid_t);
    } else if (*f == 'a' || *f == 'A') {
      // Handle ASCII string
      char *end_ptr;
      const bool str_has_fixed_len = isdigit((int)f[1]);
      field_size = str_has_fixed_len ? (size_t)strtol(&f[1], &end_ptr, 10) : 32;
      if (!str_has_fixed_len && (f[1] != '\0' && f[1] != '$')) {
        ESP_LOGE(TAG, "Variable-length string not at end of parameter.");
        return -1;
      } else if (str_has_fixed_len) {
        if (field_size == 0) {
          ESP_LOGE(TAG, "Fixed-length string has no size.");
          return 0;
        } else if (field_size > (231 - param_size)) {
          ESP_LOGE(TAG, "Fixed-length string is too big.");
          return 0;
        }
      }
      if (str_has_fixed_len) {
        f = end_ptr;
      } else {
        *is_singleton = true;
      }
    } else if (*f == '#') {
      // Handle integer literal
      ++f;  // Ignore '#' character
      int num_chars = 0;
      for (; num_chars <= 16; ++num_chars) {
        if (!isxdigit((int)f[num_chars])) break;
      }
      if (num_chars > 16) {
        ESP_LOGE(TAG, "Integer literal is too big");
        return 0;
      }
      field_size = (num_chars / 2) + (num_chars % 2);
      f += num_chars;
      if (*f != 'h' && *f != 'H') {
        ESP_LOGE(TAG, "Improperly terminated integer literal.");
        return 0;
      }
    } else if (*f == '$') {
      if (f[1] != '\0') {
        ESP_LOGE(TAG, "Improperly placed end-of-parameter anchor.");
        return 0;
      }
      *is_singleton = true;
    } else {
      ESP_LOGE(TAG, "Unknown symbol '%c' found at index %i.", *f, f - format);
      return 0;
    }

    // Ensure format size doesn't exceed MDB size.
    if (param_size + field_size > 231) {
      ESP_LOGE(TAG, "Parameter is too big.");
      return 0;
    }
    param_size += field_size;
  }
  return param_size;
}

size_t uid_encode(void *destination, const rdm_uid_t *uid,
                  size_t preamble_len) {
  // Encode the preamble and delimiter
  if (preamble_len > 7) {
    preamble_len = 7;
  }
  for (int i = 0; i < preamble_len; ++i) {
    *((uint8_t *)(destination + i)) = RDM_PREAMBLE;
  }
  *((uint8_t *)(destination + preamble_len)) = RDM_DELIMITER;

  // Encode the EUID  // FIXME: loop?
  uint8_t *d = destination + preamble_len + 1;
  d[0] = ((uint8_t *)&(uid->man_id))[1] | 0xaa;
  d[1] = ((uint8_t *)&(uid->man_id))[1] | 0x55;
  d[2] = ((uint8_t *)&(uid->man_id))[0] | 0xaa;
  d[3] = ((uint8_t *)&(uid->man_id))[0] | 0x55;
  d[4] = ((uint8_t *)&(uid->dev_id))[3] | 0xaa;
  d[5] = ((uint8_t *)&(uid->dev_id))[3] | 0x55;
  d[6] = ((uint8_t *)&(uid->dev_id))[2] | 0xaa;
  d[7] = ((uint8_t *)&(uid->dev_id))[2] | 0x55;
  d[8] = ((uint8_t *)&(uid->dev_id))[1] | 0xaa;
  d[9] = ((uint8_t *)&(uid->dev_id))[1] | 0x55;
  d[10] = ((uint8_t *)&(uid->dev_id))[0] | 0xaa;
  d[11] = ((uint8_t *)&(uid->dev_id))[0] | 0x55;

  // Calculate and encode the checksum
  uint16_t checksum = 0;
  for (int i = 0; i < 12; ++i) {
    checksum += d[i];
  }
  d[12] = (checksum >> 8) | 0xaa;
  d[13] = (checksum >> 8) | 0x55;
  d[14] = (checksum & 0xff) | 0xaa;
  d[15] = (checksum & 0xff) | 0x55;

  return preamble_len + 1 + 16;
}

size_t uid_decode(rdm_uid_t *uid, const void *source, size_t size) {
  // Ensure the source buffer is big enough
  if (size < 17) {
    return 0;  // Source buffer must be at least 17 bytes
  }

  // Get the preamble length
  const size_t preamble_len = get_preamble_len(source);
  if (preamble_len > 7 || size < preamble_len + 17) {
    return 0;  // Preamble is too long or size too small
  }

  // Decode the EUID
  uint8_t buf[6];
  const uint8_t *d = source + preamble_len + 1;
  for (int i = 0, j = 0; i < 6; ++i, j += 2) {
    buf[i] = d[j] & d[j + 1];
  }
  uidcpy(uid, buf);

  return preamble_len + 1 + 16;
}

size_t pd_emplace(void *destination, size_t dest_size, const char *format,
                  const void *source, size_t src_size,
                  const bool encode_nulls) {
  // Clamp the size to the maximum MDB length
  if (src_size > 231) {
    src_size = 231;
  }

  // Ensure that the format string syntax is correct
  bool param_is_singleton;
  const int param_size = rdm_param_parse(format, &param_is_singleton);
  if (param_size < 1) {
    return 0;
  }

  // Get the number of parameters that can be encoded
  const size_t size = dest_size < src_size ? dest_size : src_size;
  const int num_params_to_copy = param_is_singleton ? 1 : size / param_size;

  // Encode the fields into the destination
  size_t n = 0;
  for (int i = 0; i < num_params_to_copy; ++i) {
    for (const char *f = format; *f != '\0'; ++f) {
      if (*f == 'b' || *f == 'B') {
        *(uint8_t *)(destination + n) = *(uint8_t *)(source + n);
        n += sizeof(uint8_t);
      } else if (*f == 'w' || *f == 'W') {
        *(uint16_t *)(destination + n) = bswap16(*(uint16_t *)(source + n));
        n += sizeof(uint16_t);
      } else if (*f == 'd' || *f == 'D') {
        *(uint32_t *)(destination + n) = bswap32(*(uint32_t *)(source + n));
        n += sizeof(uint32_t);
      } else if (*f == 'u' || *f == 'U' || *f == 'v' || *f == 'V') {
        if ((*f == 'v' || *f == 'V') && !encode_nulls &&
            uid_is_null(source + n)) {
          break;  // Optional UIDs will be at end of parameter string
        }
        uidmove(destination + n, source + n);
        n += sizeof(rdm_uid_t);
      } else if (*f == 'a' || *f == 'A') {
        size_t len = atoi(f + 1);
        if (len == 0) {
          // Field is a variable-length string
          const size_t str_size = size - (encode_nulls ? 1 : 0);
          const size_t max_len = (str_size - n) < 32 ? (str_size - n) : 32;
          len = strnlen(source + n, max_len);
        }
        memmove(destination + n, source + n, len);
        if (encode_nulls) {
          *((uint8_t *)destination + len) = '\0';
          ++n;  // Null terminator was encoded
        }
        n += len;
      } else if (*f == '#') {
        ++f;  // Skip '#' character
        char *end_ptr;
        const uint64_t literal = strtol(f, &end_ptr, 16);
        const int literal_len = ((end_ptr - f) / 2) + ((end_ptr - f) % 2);
        for (int j = 0, k = literal_len - 1; j < literal_len; ++j, --k) {
          ((uint8_t *)destination + n)[j] = ((uint8_t *)&literal)[k];
        }
        f = end_ptr;
        n += literal_len;
      }
    }
  }
  return n;
}

size_t get_preamble_len(const void *data) {
  size_t preamble_len = 0;
  for (const uint8_t *d = data; preamble_len <= 7; ++preamble_len) {
    if (d[preamble_len] == RDM_DELIMITER) break;
  }
  return preamble_len;
}

size_t rdm_read(dmx_port_t dmx_num, rdm_header_t *header, uint8_t *pdl,
                void *pd) {
  DMX_CHECK(dmx_num < DMX_NUM_MAX, 0, "dmx_num error");
  DMX_CHECK(dmx_driver_is_installed(dmx_num), 0, "driver is not installed");

  size_t read = 0;

  spinlock_t *const restrict spinlock = &dmx_spinlock[dmx_num];
  dmx_driver_t *const driver = dmx_driver[dmx_num];

  // Get pointers to driver data buffer locations and declare checksum
  uint16_t checksum = 0;
  uint8_t *header_ptr = driver->data.buffer;
  uint8_t *message_len_ptr = header_ptr + 2;
  uint8_t *pdl_ptr = header_ptr + 24;
  void *pd_ptr = header_ptr + 25;

  taskENTER_CRITICAL(spinlock);

  // Verify start code and sub-start code are correct
  if (*(uint16_t *)header_ptr != (RDM_SC | (RDM_SUB_SC << 8))) {
    taskEXIT_CRITICAL(spinlock);
    return read;
  }

  // Verify checksum is correct
  for (int i = 0; i < *message_len_ptr; ++i) {
    checksum += header_ptr[i];
  }
  if (checksum != bswap16(*(uint16_t *)(header_ptr + *message_len_ptr))) {
    taskEXIT_CRITICAL(spinlock);
    return read;
  }

  // Copy the header and pd from the driver
  if (header != NULL) {
    pd_emplace(header, sizeof(*header), "#cc01#18huubbbwbw", header_ptr, 513,
              true);
  }
  const size_t cpy_size = pdl == NULL || *pdl > *pdl_ptr ? *pdl_ptr : *pdl;
  if (pd != NULL) {
    memcpy(pd, pd_ptr, cpy_size);
  }

  // Update the PDL and the read size
  if (pdl != NULL) {
    *pdl = cpy_size;
  }
  read = *message_len_ptr + 2;

  taskEXIT_CRITICAL(spinlock);

  return read;
}

size_t rdm_write(dmx_port_t dmx_num, rdm_header_t *header, uint8_t pdl,
                 const void *pd) {
  DMX_CHECK(dmx_num < DMX_NUM_MAX, 0, "dmx_num error");
  DMX_CHECK(pdl <= 231 && !(pd == NULL && pdl > 0), 0, "pdl is invalid");
  DMX_CHECK((header != NULL) || (pd != NULL && pdl > 0), 0,
            "header and pd are null");
  DMX_CHECK(dmx_driver_is_installed(dmx_num), 0, "driver is not installed");

  size_t written = 0;

  spinlock_t *const restrict spinlock = &dmx_spinlock[dmx_num];
  dmx_driver_t *const driver = dmx_driver[dmx_num];

  // Get pointers to driver data buffer locations and declare checksum
  uint16_t checksum = RDM_SC + RDM_SUB_SC;
  uint8_t *header_ptr = driver->data.buffer;
  uint8_t *message_len_ptr = header_ptr + 2;
  uint8_t *pdl_ptr = header_ptr + 24;
  void *pd_ptr = header_ptr + 25;

  taskENTER_CRITICAL(spinlock);

  // RDM writes must be synchronous to prevent data corruption
  if (driver->is_sending) {
    taskEXIT_CRITICAL(spinlock);
    return written;
  } else if (dmx_uart_get_rts(driver->uart) == 1) {
    dmx_uart_set_rts(driver->uart, 0);  // Stops writes from being overwritten
  }

  // Copy the header, pd, message_len, and pdl into the driver
  pd_emplace(header_ptr, 513, "#cc01#18huubbbwbw", header, sizeof(*header),
             false);
  memcpy(pd_ptr, pd, pdl);
  *message_len_ptr += pdl;
  *pdl_ptr = pdl;

  // Calculate and copy the checksum
  for (int i = 2; i < *message_len_ptr; ++i) {
    checksum += header_ptr[i];
  }
  *(uint16_t *)(header_ptr + *message_len_ptr) = bswap16(checksum);

  // Update written size and driver transmit size
  written = *message_len_ptr + 2;
  driver->data.tx_size = written;

  taskEXIT_CRITICAL(spinlock);

  return written;
}

size_t rdm_request(dmx_port_t dmx_num, rdm_header_t *header,
                   const uint8_t pdl_in, const void *pd_in, uint8_t *pdl_out,
                   void *pd_out, rdm_ack_t *ack) {
  DMX_CHECK(dmx_num < DMX_NUM_MAX, 0, "dmx_num error");
  DMX_CHECK(header != NULL, 0, "header is null");
  DMX_CHECK(pd_in != NULL || pdl_in == 0, 0, "pdl_in is invalid");
  DMX_CHECK(pd_out != NULL || (pdl_out == NULL || *pdl_out == 0), 0,
            "pdl_out is invalid");
  DMX_CHECK(!uid_is_null(&header->dest_uid), 0, "dest_uid is invalid");
  DMX_CHECK(!uid_is_broadcast(&header->src_uid), 0, "src_uid is invalid");
  DMX_CHECK(header->cc == RDM_CC_DISC_COMMAND ||
                header->cc == RDM_CC_GET_COMMAND ||
                header->cc == RDM_CC_SET_COMMAND,
            0, "cc is invalid");
  DMX_CHECK(
      header->sub_device < 513 || (header->sub_device == RDM_SUB_DEVICE_ALL &&
                                   header->cc != RDM_CC_GET_COMMAND),
      0, "sub_device is invalid");
  DMX_CHECK(dmx_driver_is_installed(dmx_num), 0, "driver is not installed");

  spinlock_t *const restrict spinlock = &dmx_spinlock[dmx_num];
  dmx_driver_t *const driver = dmx_driver[dmx_num];

  // Update the optional components of the header to allowed values
  if (header->port_id == 0) {
    header->port_id = dmx_num + 1;
  }
  if (uid_is_null(&header->src_uid)) {
    rdm_driver_get_uid(dmx_num, &header->src_uid);
  }

  // Set header values that the user cannot set themselves
  taskENTER_CRITICAL(spinlock);
  header->tn = driver->rdm.tn;
  taskEXIT_CRITICAL(spinlock);
  header->message_count = 0;

  // Write and sdn the response and determind if a response is expected
  size_t size = rdm_write(dmx_num, header, pdl_in, pd_in);
  const bool response_expected = !uid_is_broadcast(&header->dest_uid) ||
                                 (header->pid == RDM_PID_DISC_UNIQUE_BRANCH &&
                                  header->cc == RDM_CC_DISC_COMMAND);
  dmx_send(dmx_num, size);

  // Return early if a packet error occurred or if no response was expected
  if (response_expected) {
    dmx_packet_t packet;
    size = dmx_receive(dmx_num, &packet, 2);
    if (ack != NULL) {
      ack->err = packet.err;
    }
    if (packet.err) {
      if (ack != NULL) {
        ack->type = RDM_RESPONSE_TYPE_INVALID;
        ack->num = 0;
      }
      return size;
    }
  } else {
    if (ack != NULL) {
      ack->type = RDM_RESPONSE_TYPE_NONE;
      ack->num = 0;
    }
    dmx_wait_sent(dmx_num, 2);
    return size;
  }

  // Handle the RDM response packet
  if (header->pid != RDM_PID_DISC_UNIQUE_BRANCH) {
    const rdm_header_t req = *header;
    rdm_response_type_t response_type;
    if (!rdm_read(dmx_num, header, pdl_out, pd_out)) {
      response_type = RDM_RESPONSE_TYPE_INVALID;  // Data or checksum error
    } else if (header->response_type != RDM_RESPONSE_TYPE_ACK &&
               header->response_type != RDM_RESPONSE_TYPE_ACK_TIMER &&
               header->response_type != RDM_RESPONSE_TYPE_NACK_REASON &&
               header->response_type != RDM_RESPONSE_TYPE_ACK_OVERFLOW) {
      response_type = RDM_RESPONSE_TYPE_INVALID;  // Invalid response_type
    } else if (req.cc != (header->cc - 1) || req.pid != header->pid ||
               req.tn != header->tn ||
               !uid_is_target(&header->src_uid, &req.dest_uid) ||
               !uid_is_eq(&header->dest_uid, &req.src_uid)) {
      response_type = RDM_RESPONSE_TYPE_INVALID;  // Invalid response format
    } else {
      response_type = header->response_type;  // Response is ok
    }

    int decoded;
    // Handle the response based on the response type
    if (response_type == RDM_RESPONSE_TYPE_ACK) {
      // Get the size of the packet
      decoded = size;
    } else if (response_type == RDM_RESPONSE_TYPE_ACK_TIMER) {
      // Get and convert the estimated response time to FreeRTOS ticks
      decoded = pdMS_TO_TICKS(bswap16(*(uint16_t *)pd_out) * 10);
    } else if (response_type == RDM_RESPONSE_TYPE_NACK_REASON) {
      // Get and report the received NACK reason
      decoded = bswap16(*(uint16_t *)pd_out);
    } else if (response_type == RDM_RESPONSE_TYPE_ACK_OVERFLOW) {
      ESP_LOGW(TAG, "RDM_RESPONSE_TYPE_ACK_OVERFLOW is not yet supported.");
      decoded = 0;
    } else {
      decoded = 0;  // This code should never run
    }

    // Report the results back to the caller
    if (ack != NULL) {
      ack->type = response_type;
      ack->num = decoded;
    }
  } else {
    // Clamp the size argument
    if (size > 24) {
      size = 24;
    }

    // Decode the EUID from the discovery response packet
    rdm_uid_t uid;
    uint8_t euid[24];
    dmx_read(dmx_num, euid, size);
    size_t decode_size = uid_decode(&uid, euid, size);
    if (decode_size == 0) {
      if (ack != NULL) {
        ack->type = RDM_RESPONSE_TYPE_INVALID;
        ack->num = 0;
      }
      return size;
    }
    size = decode_size;

    // Copy fake data to the header to generalize function return states
    header->src_uid = uid;
    header->dest_uid = RDM_UID_NULL;
    header->tn = 0;
    header->response_type = RDM_RESPONSE_TYPE_ACK;
    header->message_count = 0;
    header->sub_device = RDM_SUB_DEVICE_ROOT;
    header->cc = RDM_CC_DISC_COMMAND_RESPONSE;
    header->pid = RDM_PID_DISC_UNIQUE_BRANCH;

    // Report ack back to the caller
    if (ack != NULL) {
      ack->type = RDM_RESPONSE_TYPE_ACK;
      ack->num = 0;
    }
  }

  return size;
}