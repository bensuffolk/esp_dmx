#include "controller.h"

#include "dmx/bus_ctl.h"
#include "dmx/driver.h"
#include "dmx/struct.h"
#include "endian.h"
#include "rdm/utils/bus_ctl.h"
#include "rdm/utils/uid.h"

bool rdm_send_get_dmx_start_address(dmx_port_t dmx_num, rdm_header_t *header,
                                    uint16_t *dmx_start_address,
                                    rdm_ack_t *ack) {
  DMX_CHECK(dmx_num < DMX_NUM_MAX, 0, "dmx_num error");
  DMX_CHECK(header != NULL, 0, "header is null");
  DMX_CHECK(dmx_start_address != NULL, 0, "dmx_start_address is null");
  DMX_CHECK(dmx_driver_is_installed(dmx_num), 0, "driver is not installed");

  rdm_uid_get(dmx_num, &header->src_uid);
  header->port_id = dmx_num + 1;
  header->cc = RDM_CC_GET_COMMAND;
  header->pid = RDM_PID_DMX_START_ADDRESS;
  header->pdl = 0;

  // uint16_t pd;
  // size_t pdl = sizeof(pd);
  bool ret = false; //rdm_send_request(dmx_num, header, NULL, &pd, &pdl, ack);
  if (ret) {
    // rdm_pd_deserialize(dmx_start_address, sizeof(*dmx_start_address), "w$",&pd);
  }

  return ret;
}

bool rdm_send_set_dmx_start_address(dmx_port_t dmx_num, rdm_header_t *header,
                                    const uint16_t dmx_start_address,
                                    rdm_ack_t *ack) {
  DMX_CHECK(dmx_num < DMX_NUM_MAX, 0, "dmx_num error");
  DMX_CHECK(header != NULL, 0, "header is null");
  DMX_CHECK(dmx_start_address < 513, 0, "dmx_start_address is invalid");
  DMX_CHECK(dmx_driver_is_installed(dmx_num), 0, "driver is not installed");

  rdm_uid_get(dmx_num, &header->src_uid);
  header->port_id = dmx_num + 1;
  header->cc = RDM_CC_SET_COMMAND;
  header->pid = RDM_PID_DMX_START_ADDRESS;
  header->pdl = sizeof(dmx_start_address);

  // uint16_t pd;
  // size_t pdl = sizeof(pd);
  // rdm_pd_serialize(&pd, sizeof(pd), "w$", &dmx_start_address);
  return false; //rdm_send_request(dmx_num, header, &pd, NULL, &pdl, ack);
}
