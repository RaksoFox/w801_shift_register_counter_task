/******************************************************************************
 *
 *  Copyright (C) 2003-2012 Broadcom Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

/******************************************************************************
 *
 *  This file contains the GATT client discovery procedures and cache
 *  related functions.
 *
 ******************************************************************************/

#define LOG_TAG "bt_bta_gattc"

#include "bt_target.h"
#if defined(BTA_GATT_INCLUDED) && (BTA_GATT_INCLUDED == TRUE)

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "bta_gattc_int.h"
#include "bta_sys.h"
#include "btm_api.h"
#include "btm_ble_api.h"
#include "btm_int.h"
#include "bt_common.h"
#include "gki.h"
#include "osi/include/log.h"
#include "sdp_api.h"
#include "sdpdefs.h"
#include "utl.h"

static void bta_gattc_cache_write(BD_ADDR server_bda, uint16_t num_attr, tBTA_GATTC_NV_ATTR *attr);
static void bta_gattc_char_dscpt_disc_cmpl(uint16_t conn_id, tBTA_GATTC_SERV *p_srvc_cb);
static tBTA_GATT_STATUS bta_gattc_sdp_service_disc(uint16_t conn_id, tBTA_GATTC_SERV *p_server_cb);
extern void bta_to_btif_uuid(tls_bt_uuid_t *p_dest, tBT_UUID *p_src);
tBTA_GATTC_SERVICE  *bta_gattc_find_matching_service(const list_t *services, uint16_t handle);
tBTA_GATTC_DESCRIPTOR  *bta_gattc_get_descriptor_srcb(tBTA_GATTC_SERV *p_srcb, uint16_t handle);
tBTA_GATTC_CHARACTERISTIC  *bta_gattc_get_characteristic_srcb(tBTA_GATTC_SERV *p_srcb,
        uint16_t handle);

#define BTA_GATT_SDP_DB_SIZE 4096

#define GATT_CACHE_PREFIX "/data/misc/bluetooth/gatt_cache_"
#define GATT_CACHE_VERSION 2

static void bta_gattc_generate_cache_file_name(char *buffer, BD_ADDR bda)
{
    sprintf(buffer, "%s%02x%02x%02x%02x%02x%02x", GATT_CACHE_PREFIX,
            bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
}

/*****************************************************************************
**  Constants and data types
*****************************************************************************/

typedef struct {
    tSDP_DISCOVERY_DB   *p_sdp_db;
    uint16_t              sdp_conn_id;
} tBTA_GATTC_CB_DATA;

#if (defined BTA_GATT_DEBUG && BTA_GATT_DEBUG == TRUE)
static char *bta_gattc_attr_type[] = {
    "I", /* Included Service */
    "C", /* Characteristic */
    "D" /* Characteristic Descriptor */
};
/* utility functions */

uint8_t display_cache_attribute(void *data, void *context)
{
    //tBTA_GATTC_CACHE_ATTR *p_attr = data;
    //APPL_TRACE_ERROR("\t Attr handle[%d] uuid[0x%04x] type[%s] prop[0x%1x]",
    //                 p_attr->handle, p_attr->uuid.uu.uuid16,
    //                 bta_gattc_attr_type[p_attr->attr_type], p_attr->property);
    return true;
}

uint8_t display_cache_service(void *data, void *context)
{
    tBTA_GATTC_SERVICE    *p_cur_srvc = data;
    APPL_TRACE_ERROR("Service: handle[%d ~ %d] %s[0x%04x] inst[%d]",
                     p_cur_srvc->s_handle, p_cur_srvc->e_handle,
                     ((p_cur_srvc->uuid.len == 2) ? "uuid16" : "uuid128"),
                     p_cur_srvc->uuid.uu.uuid16,
                     p_cur_srvc->handle);

    if(p_cur_srvc->characteristics != NULL) {
        list_foreach(p_cur_srvc->characteristics, display_cache_attribute, NULL);
    }

    return true;
}

/*******************************************************************************
**
** Function         bta_gattc_display_cache_server
**
** Description      debug function to display the server cache.
**
** Returns          none.
**
*******************************************************************************/
static void bta_gattc_display_cache_server(list_t *p_cache)
{
    APPL_TRACE_ERROR("<================Start Server Cache =============>");
    list_foreach(p_cache, display_cache_service, NULL);
    APPL_TRACE_ERROR("<================End Server Cache =============>");
    APPL_TRACE_ERROR(" ");
}

/*******************************************************************************
**
** Function         bta_gattc_display_explore_record
**
** Description      debug function to display the exploration list
**
** Returns          none.
**
*******************************************************************************/
static void bta_gattc_display_explore_record(tBTA_GATTC_ATTR_REC *p_rec, uint8_t num_rec)
{
    uint8_t i;
    tBTA_GATTC_ATTR_REC *pp = p_rec;
    APPL_TRACE_ERROR("<================Start Explore Queue =============>");

    for(i = 0; i < num_rec; i ++, pp ++) {
        APPL_TRACE_ERROR("\t rec[%d] uuid[0x%04x] s_handle[%d] e_handle[%d] is_primary[%d]",
                         i + 1, pp->uuid.uu.uuid16, pp->s_handle, pp->e_handle, pp->is_primary);
    }

    APPL_TRACE_ERROR("<================ End Explore Queue =============>");
    APPL_TRACE_ERROR(" ");
}
#endif  /* BTA_GATT_DEBUG == TRUE */

/*******************************************************************************
**
** Function         bta_gattc_init_cache
**
** Description      Initialize the database cache and discovery related resources.
**
** Returns          status
**
*******************************************************************************/
tBTA_GATT_STATUS bta_gattc_init_cache(tBTA_GATTC_SERV *p_srvc_cb)
{
    if(p_srvc_cb->p_srvc_cache != NULL) {
        list_free(p_srvc_cb->p_srvc_cache);
        p_srvc_cb->p_srvc_cache = NULL;
    }

    GKI_freebuf(p_srvc_cb->p_srvc_list);
    p_srvc_cb->p_srvc_list =
                    (tBTA_GATTC_ATTR_REC *)GKI_getbuf(BTA_GATTC_ATTR_LIST_SIZE);
    p_srvc_cb->total_srvc = 0;
    p_srvc_cb->cur_srvc_idx = 0;
    p_srvc_cb->cur_char_idx = 0;
    p_srvc_cb->next_avail_idx = 0;
    return BTA_GATT_OK;
}

static void characteristic_free(void *ptr)
{
    tBTA_GATTC_CHARACTERISTIC *p_char = ptr;
    list_free(p_char->descriptors);
    GKI_freebuf(p_char);
}

static void service_free(void *ptr)
{
    tBTA_GATTC_SERVICE *srvc = ptr;
    list_free(srvc->characteristics);
    list_free(srvc->included_svc);
    GKI_freebuf(srvc);
}

/*******************************************************************************
**
** Function         bta_gattc_add_srvc_to_cache
**
** Description      Add a service into database cache.
**
** Returns          status
**
*******************************************************************************/
static tBTA_GATT_STATUS bta_gattc_add_srvc_to_cache(tBTA_GATTC_SERV *p_srvc_cb,
        uint16_t s_handle, uint16_t e_handle,
        tBT_UUID *p_uuid,
        uint8_t is_primary)
{
#if (defined BTA_GATT_DEBUG && BTA_GATT_DEBUG == TRUE)
    APPL_TRACE_DEBUG("Add a service into Service");
#endif
    tBTA_GATTC_SERVICE *p_new_srvc = GKI_getbuf(sizeof(tBTA_GATTC_SERVICE));
    /* update service information */
    p_new_srvc->s_handle = s_handle;
    p_new_srvc->e_handle = e_handle;
    p_new_srvc->is_primary = is_primary;
    wm_memcpy(&p_new_srvc->uuid, p_uuid, sizeof(tBT_UUID));
    p_new_srvc->handle = s_handle;
    p_new_srvc->characteristics = list_new(characteristic_free);
    p_new_srvc->included_svc = list_new(GKI_freebuf);

    if(p_srvc_cb->p_srvc_cache == NULL) {
        p_srvc_cb->p_srvc_cache = list_new(service_free);
    }

    list_append(p_srvc_cb->p_srvc_cache, p_new_srvc);
    return BTA_GATT_OK;
}

static tBTA_GATT_STATUS bta_gattc_add_char_to_cache(tBTA_GATTC_SERV *p_srvc_cb,
        uint16_t attr_handle,
        uint16_t value_handle,
        tBT_UUID *p_uuid,
        uint8_t property)
{
#if (defined BTA_GATT_DEBUG && BTA_GATT_DEBUG == TRUE)
    APPL_TRACE_DEBUG("%s: Add a characteristic into Service", __func__);
    APPL_TRACE_DEBUG("handle=%d uuid16=0x%x property=0x%x",
                     value_handle, p_uuid->uu.uuid16, property);
#endif
    tBTA_GATTC_SERVICE *service = bta_gattc_find_matching_service(p_srvc_cb->p_srvc_cache, attr_handle);

    if(!service) {
        APPL_TRACE_ERROR("Illegal action to add char/descr/incl srvc for non-existing service!");
        return GATT_WRONG_STATE;
    }

    /* TODO(jpawlowski): We should use attribute handle, not value handle to refer to characteristic.
       This is just a temporary workaround.
    */
    if(service->e_handle < value_handle) {
        service->e_handle = value_handle;
    }

    tBTA_GATTC_CHARACTERISTIC *characteristic = GKI_getbuf(sizeof(tBTA_GATTC_CHARACTERISTIC));
    characteristic->handle = value_handle;
    characteristic->properties = property;
    characteristic->descriptors = list_new(GKI_freebuf);
    wm_memcpy(&characteristic->uuid, p_uuid, sizeof(tBT_UUID));
    characteristic->service = service;
    list_append(service->characteristics, characteristic);
    return BTA_GATT_OK;
}

/*******************************************************************************
**
** Function         bta_gattc_add_attr_to_cache
**
** Description      Add an attribute into database cache buffer.
**
** Returns          status
**
*******************************************************************************/
static tBTA_GATT_STATUS bta_gattc_add_attr_to_cache(tBTA_GATTC_SERV *p_srvc_cb,
        uint16_t handle,
        tBT_UUID *p_uuid,
        uint8_t property,
        uint16_t incl_srvc_s_handle,
        tBTA_GATTC_ATTR_TYPE type)
{
#if (defined BTA_GATT_DEBUG && BTA_GATT_DEBUG == TRUE)
    APPL_TRACE_DEBUG("%s: Add a [%s] into Service", __func__, bta_gattc_attr_type[type]);
    APPL_TRACE_DEBUG("handle=%d uuid16=0x%x property=0x%x type=%d",
                     handle, p_uuid->uu.uuid16, property, type);
#endif
    tBTA_GATTC_SERVICE *service = bta_gattc_find_matching_service(p_srvc_cb->p_srvc_cache, handle);

    if(!service) {
        APPL_TRACE_ERROR("Illegal action to add char/descr/incl srvc for non-existing service!");
        return GATT_WRONG_STATE;
    }

    if(type == BTA_GATTC_ATTR_TYPE_INCL_SRVC) {
        tBTA_GATTC_INCLUDED_SVC *isvc =
                        GKI_getbuf(sizeof(tBTA_GATTC_INCLUDED_SVC));
        isvc->handle = handle;
        wm_memcpy(&isvc->uuid, p_uuid, sizeof(tBT_UUID));
        isvc->owning_service = service;
        isvc->included_service = bta_gattc_find_matching_service(
                p_srvc_cb->p_srvc_cache, incl_srvc_s_handle);

        if(!isvc->included_service) {
            APPL_TRACE_ERROR("%s: Illegal action to add non-existing included service!", __func__);
            GKI_freebuf(isvc);
            return GATT_WRONG_STATE;
        }

        list_append(service->included_svc, isvc);
    } else if(type == BTA_GATTC_ATTR_TYPE_CHAR_DESCR) {
        tBTA_GATTC_DESCRIPTOR *descriptor =
                        GKI_getbuf(sizeof(tBTA_GATTC_DESCRIPTOR));
        descriptor->handle = handle;
        wm_memcpy(&descriptor->uuid, p_uuid, sizeof(tBT_UUID));

        if(service->characteristics == NULL) {
            APPL_TRACE_ERROR("%s: Illegal action to add descriptor before adding a characteristic!",
                             __func__);
            GKI_freebuf(descriptor);
            return GATT_WRONG_STATE;
        }

        tBTA_GATTC_CHARACTERISTIC *char_node = list_back(service->characteristics);
        descriptor->characteristic = char_node;
        list_append(char_node->descriptors, descriptor);
    }

    return BTA_GATT_OK;
}

/*******************************************************************************
**
** Function         bta_gattc_get_disc_range
**
** Description      get discovery stating and ending handle range.
**
** Returns          None.
**
*******************************************************************************/
void bta_gattc_get_disc_range(tBTA_GATTC_SERV *p_srvc_cb, uint16_t *p_s_hdl, uint16_t *p_e_hdl,
                              uint8_t is_srvc)
{
    tBTA_GATTC_ATTR_REC *p_rec = NULL;

    if(is_srvc) {
        p_rec = p_srvc_cb->p_srvc_list + p_srvc_cb->cur_srvc_idx;
        *p_s_hdl = p_rec->s_handle;
    } else {
        p_rec = p_srvc_cb->p_srvc_list + p_srvc_cb->cur_char_idx;
        *p_s_hdl = p_rec->s_handle + 1;
    }

    *p_e_hdl = p_rec->e_handle;
#if (defined BTA_GATT_DEBUG && BTA_GATT_DEBUG == TRUE)
    APPL_TRACE_DEBUG("discover range [%d ~ %d]", p_rec->s_handle, p_rec->e_handle);
#endif
    return;
}
/*******************************************************************************
**
** Function         bta_gattc_discover_pri_service
**
** Description      Start primary service discovery
**
** Returns          status of the operation.
**
*******************************************************************************/
tBTA_GATT_STATUS bta_gattc_discover_pri_service(uint16_t conn_id, tBTA_GATTC_SERV *p_server_cb,
        uint8_t disc_type)
{
    tBTA_GATTC_CLCB     *p_clcb = bta_gattc_find_clcb_by_conn_id(conn_id);
    tBTA_GATT_STATUS    status =  BTA_GATT_ERROR;

    if(p_clcb) {
        if(p_clcb->transport == BTA_TRANSPORT_LE) {
            status = bta_gattc_discover_procedure(conn_id, p_server_cb, disc_type);
        } else {
            status = bta_gattc_sdp_service_disc(conn_id, p_server_cb);
        }
    }

    return status;
}
/*******************************************************************************
**
** Function         bta_gattc_discover_procedure
**
** Description      Start a particular type of discovery procedure on server.
**
** Returns          status of the operation.
**
*******************************************************************************/
tBTA_GATT_STATUS bta_gattc_discover_procedure(uint16_t conn_id, tBTA_GATTC_SERV *p_server_cb,
        uint8_t disc_type)
{
    tGATT_DISC_PARAM param;
    uint8_t is_service = TRUE;
    wm_memset(&param, 0, sizeof(tGATT_DISC_PARAM));

    if(disc_type == GATT_DISC_SRVC_ALL || disc_type == GATT_DISC_SRVC_BY_UUID) {
        param.s_handle = 1;
        param.e_handle = 0xFFFF;
    } else {
        if(disc_type == GATT_DISC_CHAR_DSCPT) {
            is_service = FALSE;
        }

        bta_gattc_get_disc_range(p_server_cb, &param.s_handle, &param.e_handle, is_service);

        if(param.s_handle > param.e_handle) {
            return GATT_ERROR;
        }
    }

    return GATTC_Discover(conn_id, disc_type, &param);
}
/*******************************************************************************
**
** Function         bta_gattc_start_disc_include_srvc
**
** Description      Start discovery for included service
**
** Returns          status of the operation.
**
*******************************************************************************/
tBTA_GATT_STATUS bta_gattc_start_disc_include_srvc(uint16_t conn_id, tBTA_GATTC_SERV *p_srvc_cb)
{
    return bta_gattc_discover_procedure(conn_id, p_srvc_cb, GATT_DISC_INC_SRVC);
}
/*******************************************************************************
**
** Function         bta_gattc_start_disc_char
**
** Description      Start discovery for characteristic
**
** Returns          status of the operation.
**
*******************************************************************************/
tBTA_GATT_STATUS bta_gattc_start_disc_char(uint16_t conn_id, tBTA_GATTC_SERV *p_srvc_cb)
{
    p_srvc_cb->total_char = 0;
    return bta_gattc_discover_procedure(conn_id, p_srvc_cb, GATT_DISC_CHAR);
}
/*******************************************************************************
**
** Function         bta_gattc_start_disc_char_dscp
**
** Description      Start discovery for characteristic descriptor
**
** Returns          none.
**
*******************************************************************************/
void bta_gattc_start_disc_char_dscp(uint16_t conn_id, tBTA_GATTC_SERV *p_srvc_cb)
{
    APPL_TRACE_DEBUG("starting discover characteristics descriptor");

    if(bta_gattc_discover_procedure(conn_id, p_srvc_cb, GATT_DISC_CHAR_DSCPT) != 0) {
        bta_gattc_char_dscpt_disc_cmpl(conn_id, p_srvc_cb);
    }
}
/*******************************************************************************
**
** Function         bta_gattc_explore_srvc
**
** Description      process the service discovery complete event
**
** Returns          status
**
*******************************************************************************/
static void bta_gattc_explore_srvc(uint16_t conn_id, tBTA_GATTC_SERV *p_srvc_cb)
{
    tBTA_GATTC_ATTR_REC *p_rec = p_srvc_cb->p_srvc_list + p_srvc_cb->cur_srvc_idx;
    tBTA_GATTC_CLCB *p_clcb = bta_gattc_find_clcb_by_conn_id(conn_id);
    APPL_TRACE_DEBUG("Start service discovery: srvc_idx = %d", p_srvc_cb->cur_srvc_idx);
    p_srvc_cb->cur_char_idx = p_srvc_cb->next_avail_idx = p_srvc_cb->total_srvc;

    if(p_clcb == NULL) {
        APPL_TRACE_ERROR("unknown connection ID");
        return;
    }

    /* start expore a service if there is service not been explored */
    if(p_srvc_cb->cur_srvc_idx < p_srvc_cb->total_srvc) {
        /* add the first service into cache */
        if(bta_gattc_add_srvc_to_cache(p_srvc_cb,
                                       p_rec->s_handle,
                                       p_rec->e_handle,
                                       &p_rec->uuid,
                                       p_rec->is_primary) == 0) {
            /* start discovering included services */
            bta_gattc_start_disc_include_srvc(conn_id, p_srvc_cb);
            return;
        }
    }

    /* no service found at all, the end of server discovery*/
    LOG_WARN(LOG_TAG, "%s no more services found", __func__);
#if (defined BTA_GATT_DEBUG && BTA_GATT_DEBUG == TRUE)
    bta_gattc_display_cache_server(p_srvc_cb->p_srvc_cache);
#endif
    /* save cache to NV */
    p_clcb->p_srcb->state = BTA_GATTC_SERV_SAVE;

    if(btm_sec_is_a_bonded_dev(p_srvc_cb->server_bda)) {
        bta_gattc_cache_save(p_clcb->p_srcb, p_clcb->bta_conn_id);
    }

    bta_gattc_reset_discover_st(p_clcb->p_srcb, BTA_GATT_OK);
}
/*******************************************************************************
**
** Function         bta_gattc_incl_srvc_disc_cmpl
**
** Description      process the relationship discovery complete event
**
** Returns          status
**
*******************************************************************************/
static void bta_gattc_incl_srvc_disc_cmpl(uint16_t conn_id, tBTA_GATTC_SERV *p_srvc_cb)
{
    p_srvc_cb->cur_char_idx = p_srvc_cb->total_srvc;
    /* start discoverying characteristic */
    bta_gattc_start_disc_char(conn_id, p_srvc_cb);
}
/*******************************************************************************
**
** Function         bta_gattc_char_disc_cmpl
**
** Description      process the characteristic discovery complete event
**
** Returns          status
**
*******************************************************************************/
static void bta_gattc_char_disc_cmpl(uint16_t conn_id, tBTA_GATTC_SERV *p_srvc_cb)
{
    tBTA_GATTC_ATTR_REC *p_rec = p_srvc_cb->p_srvc_list + p_srvc_cb->cur_char_idx;

    /* if there are characteristic needs to be explored */
    if(p_srvc_cb->total_char > 0) {
        /* add the first characteristic into cache */
        bta_gattc_add_char_to_cache(p_srvc_cb,
                                    p_rec->char_decl_handle,
                                    p_rec->s_handle,
                                    &p_rec->uuid,
                                    p_rec->property);
        /* start discoverying characteristic descriptor , if failed, disc for next char*/
        bta_gattc_start_disc_char_dscp(conn_id, p_srvc_cb);
    } else { /* otherwise start with next service */
        p_srvc_cb->cur_srvc_idx ++;
        bta_gattc_explore_srvc(conn_id, p_srvc_cb);
    }
}
/*******************************************************************************
**
** Function         bta_gattc_char_dscpt_disc_cmpl
**
** Description      process the char descriptor discovery complete event
**
** Returns          status
**
*******************************************************************************/
static void bta_gattc_char_dscpt_disc_cmpl(uint16_t conn_id, tBTA_GATTC_SERV *p_srvc_cb)
{
    tBTA_GATTC_ATTR_REC *p_rec = NULL;

    if(-- p_srvc_cb->total_char > 0) {
        p_rec = p_srvc_cb->p_srvc_list + (++ p_srvc_cb->cur_char_idx);
        /* add the next characteristic into cache */
        bta_gattc_add_char_to_cache(p_srvc_cb,
                                    p_rec->char_decl_handle,
                                    p_rec->s_handle,
                                    &p_rec->uuid,
                                    p_rec->property);
        /* start discoverying next characteristic for char descriptor */
        bta_gattc_start_disc_char_dscp(conn_id, p_srvc_cb);
    } else
        /* all characteristic has been explored, start with next service if any */
    {
#if (defined BTA_GATT_DEBUG && BTA_GATT_DEBUG == TRUE)
        APPL_TRACE_ERROR("all char has been explored");
#endif
        p_srvc_cb->cur_srvc_idx ++;
        bta_gattc_explore_srvc(conn_id, p_srvc_cb);
    }
}
static uint8_t bta_gattc_srvc_in_list(tBTA_GATTC_SERV *p_srvc_cb, uint16_t s_handle,
                                      uint16_t e_handle, tBT_UUID uuid)
{
    tBTA_GATTC_ATTR_REC *p_rec = NULL;
    uint8_t   i;
    uint8_t exist_srvc = FALSE;
    UNUSED(uuid);

    if(!GATT_HANDLE_IS_VALID(s_handle) || !GATT_HANDLE_IS_VALID(e_handle)) {
        APPL_TRACE_ERROR("invalid included service handle: [0x%04x ~ 0x%04x]", s_handle, e_handle);
        exist_srvc = TRUE;
    } else {
        for(i = 0; i < p_srvc_cb->next_avail_idx; i ++) {
            p_rec = p_srvc_cb->p_srvc_list + i;

            /* a new service should not have any overlap with other service handle range */
            if(p_rec->s_handle == s_handle || p_rec->e_handle == e_handle) {
                exist_srvc = TRUE;
                break;
            }
        }
    }

    return exist_srvc;
}
/*******************************************************************************
**
** Function         bta_gattc_add_srvc_to_list
**
** Description      Add a service into explore pending list
**
** Returns          status
**
*******************************************************************************/
static tBTA_GATT_STATUS bta_gattc_add_srvc_to_list(tBTA_GATTC_SERV *p_srvc_cb,
        uint16_t s_handle, uint16_t e_handle,
        tBT_UUID uuid, uint8_t is_primary)
{
    tBTA_GATTC_ATTR_REC *p_rec = NULL;
    tBTA_GATT_STATUS    status = BTA_GATT_OK;

    if(p_srvc_cb->p_srvc_list && p_srvc_cb->next_avail_idx < BTA_GATTC_MAX_CACHE_CHAR) {
        p_rec = p_srvc_cb->p_srvc_list + p_srvc_cb->next_avail_idx;
        APPL_TRACE_DEBUG("%s handle=%d, service type=0x%04x",
                         __func__, s_handle, uuid.uu.uuid16);
        p_rec->s_handle     = s_handle;
        p_rec->e_handle     = e_handle;
        p_rec->is_primary   = is_primary;
        wm_memcpy(&p_rec->uuid, &uuid, sizeof(tBT_UUID));
        p_srvc_cb->total_srvc ++;
        p_srvc_cb->next_avail_idx ++;
    } else {
        /* allocate bigger buffer ?? */
        status = GATT_DB_FULL;
        APPL_TRACE_ERROR("service not added, no resources or wrong state");
    }

    return status;
}
/*******************************************************************************
**
** Function         bta_gattc_add_char_to_list
**
** Description      Add a characteristic into explore pending list
**
** Returns          status
**
*******************************************************************************/
static tBTA_GATT_STATUS bta_gattc_add_char_to_list(tBTA_GATTC_SERV *p_srvc_cb,
        uint16_t decl_handle, uint16_t value_handle,
        tBT_UUID uuid, uint8_t property)
{
    tBTA_GATTC_ATTR_REC *p_rec = NULL;
    tBTA_GATT_STATUS    status = BTA_GATT_OK;

    if(p_srvc_cb->p_srvc_list == NULL) {
        APPL_TRACE_ERROR("No service available, unexpected char discovery result");
        status = BTA_GATT_INTERNAL_ERROR;
    } else if(p_srvc_cb->next_avail_idx < BTA_GATTC_MAX_CACHE_CHAR) {
        p_rec = p_srvc_cb->p_srvc_list + p_srvc_cb->next_avail_idx;
        p_srvc_cb->total_char ++;
        p_rec->s_handle = value_handle;
        p_rec->char_decl_handle = decl_handle;
        p_rec->property = property;
        p_rec->e_handle = (p_srvc_cb->p_srvc_list + p_srvc_cb->cur_srvc_idx)->e_handle;
        wm_memcpy(&p_rec->uuid, &uuid, sizeof(tBT_UUID));

        /* update the endind handle of pervious characteristic if available */
        if(p_srvc_cb->total_char > 1) {
            p_rec -= 1;
            p_rec->e_handle = decl_handle - 1;
        }

        p_srvc_cb->next_avail_idx ++;
    } else {
        APPL_TRACE_ERROR("char not added, no resources");
        /* allocate bigger buffer ?? */
        status = BTA_GATT_DB_FULL;
    }

    return status;
}

/*******************************************************************************
**
** Function         bta_gattc_sdp_callback
**
** Description      Process the discovery result from sdp
**
** Returns          void
**
*******************************************************************************/
void bta_gattc_sdp_callback(uint16_t sdp_status, void *user_data)
{
    tSDP_DISC_REC       *p_sdp_rec = NULL;
    tBT_UUID            service_uuid;
    tSDP_PROTOCOL_ELEM  pe;
    uint16_t              start_handle = 0, end_handle = 0;
    tBTA_GATTC_CB_DATA  *cb_data = user_data;
    tBTA_GATTC_SERV     *p_srvc_cb = bta_gattc_find_scb_by_cid(cb_data->sdp_conn_id);

    if(((sdp_status == SDP_SUCCESS) || (sdp_status == SDP_DB_FULL)) && p_srvc_cb != NULL) {
        do {
            /* find a service record, report it */
            p_sdp_rec = SDP_FindServiceInDb(cb_data->p_sdp_db, 0, p_sdp_rec);

            if(p_sdp_rec) {
                if(SDP_FindServiceUUIDInRec(p_sdp_rec, &service_uuid)) {
                    if(SDP_FindProtocolListElemInRec(p_sdp_rec, UUID_PROTOCOL_ATT, &pe)) {
                        start_handle    = (uint16_t) pe.params[0];
                        end_handle      = (uint16_t) pe.params[1];
#if (defined BTA_GATT_DEBUG && BTA_GATT_DEBUG == TRUE)
                        APPL_TRACE_EVENT("Found ATT service [0x%04x] handle[0x%04x ~ 0x%04x]",
                                         service_uuid.uu.uuid16, start_handle, end_handle);
#endif

                        if(GATT_HANDLE_IS_VALID(start_handle) && GATT_HANDLE_IS_VALID(end_handle) &&
                                p_srvc_cb != NULL) {
                            /* discover services result, add services into a service list */
                            bta_gattc_add_srvc_to_list(p_srvc_cb,
                                                       start_handle,
                                                       end_handle,
                                                       service_uuid,
                                                       TRUE);
                        } else {
                            APPL_TRACE_ERROR("invalid start_handle = %d end_handle = %d",
                                             start_handle, end_handle);
                        }
                    }
                }
            }
        } while(p_sdp_rec);
    }

    if(p_srvc_cb != NULL) {
        /* start discover primary service */
        bta_gattc_explore_srvc(cb_data->sdp_conn_id, p_srvc_cb);
    } else {
        APPL_TRACE_ERROR("GATT service discovery is done on unknown connection");
    }

    /* both were allocated in bta_gattc_sdp_service_disc */
    GKI_freebuf(cb_data->p_sdp_db);
    GKI_freebuf(cb_data);
}
/*******************************************************************************
**
** Function         bta_gattc_sdp_service_disc
**
** Description      Start DSP Service Discovert
**
** Returns          void
**
*******************************************************************************/
static tBTA_GATT_STATUS bta_gattc_sdp_service_disc(uint16_t conn_id, tBTA_GATTC_SERV *p_server_cb)
{
    tSDP_UUID       uuid;
    uint16_t          num_attrs = 2;
    uint16_t          attr_list[2];
    wm_memset(&uuid, 0, sizeof(tSDP_UUID));
    uuid.len = LEN_UUID_16;
    uuid.uu.uuid16 = UUID_PROTOCOL_ATT;
    /*
     * On success, cb_data will be freed inside bta_gattc_sdp_callback,
     * otherwise it will be freed within this function.
     */
    tBTA_GATTC_CB_DATA *cb_data =
                    (tBTA_GATTC_CB_DATA *)GKI_getbuf(sizeof(tBTA_GATTC_CB_DATA));
    cb_data->p_sdp_db = (tSDP_DISCOVERY_DB *)GKI_getbuf(BTA_GATT_SDP_DB_SIZE);
    attr_list[0] = ATTR_ID_SERVICE_CLASS_ID_LIST;
    attr_list[1] = ATTR_ID_PROTOCOL_DESC_LIST;
    SDP_InitDiscoveryDb(cb_data->p_sdp_db, BTA_GATT_SDP_DB_SIZE, 1,
                        &uuid, num_attrs, attr_list);

    if(!SDP_ServiceSearchAttributeRequest2(p_server_cb->server_bda,
                                           cb_data->p_sdp_db, &bta_gattc_sdp_callback, cb_data)) {
        GKI_freebuf(cb_data->p_sdp_db);
        GKI_freebuf(cb_data);
        return BTA_GATT_ERROR;
    }

    cb_data->sdp_conn_id = conn_id;
    return BTA_GATT_OK;
}
/*******************************************************************************
**
** Function         bta_gattc_disc_res_cback
**                  bta_gattc_disc_cmpl_cback
**
** Description      callback functions to GATT client stack.
**
** Returns          void
**
*******************************************************************************/
void bta_gattc_disc_res_cback(uint16_t conn_id, tGATT_DISC_TYPE disc_type, tGATT_DISC_RES *p_data)
{
    tBTA_GATTC_SERV *p_srvc_cb = NULL;
    uint8_t          pri_srvc;
    tBTA_GATTC_CLCB *p_clcb = bta_gattc_find_clcb_by_conn_id(conn_id);
    p_srvc_cb = bta_gattc_find_scb_by_cid(conn_id);

    if(p_srvc_cb != NULL && p_clcb != NULL && p_clcb->state == BTA_GATTC_DISCOVER_ST) {
        switch(disc_type) {
            case GATT_DISC_SRVC_ALL:
                /* discover services result, add services into a service list */
                bta_gattc_add_srvc_to_list(p_srvc_cb,
                                           p_data->handle,
                                           p_data->value.group_value.e_handle,
                                           p_data->value.group_value.service_type,
                                           TRUE);
                break;

            case GATT_DISC_SRVC_BY_UUID:
                bta_gattc_add_srvc_to_list(p_srvc_cb,
                                           p_data->handle,
                                           p_data->value.group_value.e_handle,
                                           p_data->value.group_value.service_type,
                                           TRUE);
                break;

            case GATT_DISC_INC_SRVC:
                /* add included service into service list if it's secondary or it never showed up
                   in the primary service search */
                pri_srvc = bta_gattc_srvc_in_list(p_srvc_cb,
                                                  p_data->value.incl_service.s_handle,
                                                  p_data->value.incl_service.e_handle,
                                                  p_data->value.incl_service.service_type);

                if(!pri_srvc)
                    bta_gattc_add_srvc_to_list(p_srvc_cb,
                                               p_data->value.incl_service.s_handle,
                                               p_data->value.incl_service.e_handle,
                                               p_data->value.incl_service.service_type,
                                               FALSE);

                /* add into database */
                bta_gattc_add_attr_to_cache(p_srvc_cb,
                                            p_data->handle,
                                            &p_data->value.incl_service.service_type,
                                            pri_srvc,
                                            p_data->value.incl_service.s_handle,
                                            BTA_GATTC_ATTR_TYPE_INCL_SRVC);
                break;

            case GATT_DISC_CHAR:
                /* add char value into database */
                bta_gattc_add_char_to_list(p_srvc_cb,
                                           p_data->handle,
                                           p_data->value.dclr_value.val_handle,
                                           p_data->value.dclr_value.char_uuid,
                                           p_data->value.dclr_value.char_prop);
                break;

            case GATT_DISC_CHAR_DSCPT:
                bta_gattc_add_attr_to_cache(p_srvc_cb, p_data->handle, &p_data->type, 0,
                                            0 /* incl_srvc_handle */,
                                            BTA_GATTC_ATTR_TYPE_CHAR_DESCR);
                break;
        }
    }
}
void bta_gattc_disc_cmpl_cback(uint16_t conn_id, tGATT_DISC_TYPE disc_type, tGATT_STATUS status)
{
    tBTA_GATTC_SERV *p_srvc_cb;
    tBTA_GATTC_CLCB *p_clcb = bta_gattc_find_clcb_by_conn_id(conn_id);

    if(p_clcb && (status != GATT_SUCCESS || p_clcb->status != GATT_SUCCESS)) {
        if(status == GATT_SUCCESS) {
            p_clcb->status = status;
        }

        bta_gattc_sm_execute(p_clcb, BTA_GATTC_DISCOVER_CMPL_EVT, NULL);
        return;
    }

    p_srvc_cb = bta_gattc_find_scb_by_cid(conn_id);

    if(p_srvc_cb != NULL) {
        switch(disc_type) {
            case GATT_DISC_SRVC_ALL:
            case GATT_DISC_SRVC_BY_UUID:
#if (defined BTA_GATT_DEBUG && BTA_GATT_DEBUG == TRUE)
                bta_gattc_display_explore_record(p_srvc_cb->p_srvc_list, p_srvc_cb->next_avail_idx);
#endif
                bta_gattc_explore_srvc(conn_id, p_srvc_cb);
                break;

            case GATT_DISC_INC_SRVC:
                bta_gattc_incl_srvc_disc_cmpl(conn_id, p_srvc_cb);
                break;

            case GATT_DISC_CHAR:
#if (defined BTA_GATT_DEBUG && BTA_GATT_DEBUG == TRUE)
                bta_gattc_display_explore_record(p_srvc_cb->p_srvc_list, p_srvc_cb->next_avail_idx);
#endif
                bta_gattc_char_disc_cmpl(conn_id, p_srvc_cb);
                break;

            case GATT_DISC_CHAR_DSCPT:
                bta_gattc_char_dscpt_disc_cmpl(conn_id, p_srvc_cb);
                break;
        }
    }
}

/*******************************************************************************
**
** Function         bta_gattc_search_service
**
** Description      search local cache for matching service record.
**
** Returns          FALSE if map can not be found.
**
*******************************************************************************/
void bta_gattc_search_service(tBTA_GATTC_CLCB *p_clcb, tBT_UUID *p_uuid)
{
    tBTA_GATTC          cb_data;

    if(!p_clcb->p_srcb->p_srvc_cache || list_is_empty(p_clcb->p_srcb->p_srvc_cache)) {
        return;
    }

    for(list_node_t *sn = list_begin(p_clcb->p_srcb->p_srvc_cache);
            sn != list_end(p_clcb->p_srcb->p_srvc_cache); sn = list_next(sn)) {
        tBTA_GATTC_SERVICE *p_cache = list_node(sn);

        if(!bta_gattc_uuid_compare(p_uuid, &p_cache->uuid, FALSE)) {
            continue;
        }

#if (defined BTA_GATT_DEBUG && BTA_GATT_DEBUG == TRUE)
        APPL_TRACE_DEBUG("found service [0x%04x], inst[%d] handle [%d]",
                         p_cache->uuid.uu.uuid16,
                         p_cache->handle,
                         p_cache->s_handle);
#endif

        if(!p_clcb->p_rcb->p_cback) {
            continue;
        }

        wm_memset(&cb_data, 0, sizeof(tBTA_GATTC));
        cb_data.srvc_res.conn_id = p_clcb->bta_conn_id;
        cb_data.srvc_res.service_uuid.inst_id = p_cache->handle;
        wm_memcpy(&cb_data.srvc_res.service_uuid.uuid, &p_cache->uuid, sizeof(tBTA_GATT_ID));
        (* p_clcb->p_rcb->p_cback)(BTA_GATTC_SEARCH_RES_EVT, &cb_data);
    }
}

list_t *bta_gattc_get_services_srcb(tBTA_GATTC_SERV *p_srcb)
{
    if(!p_srcb || !p_srcb->p_srvc_cache || list_is_empty(p_srcb->p_srvc_cache)) {
        return NULL;
    }

    return p_srcb->p_srvc_cache;
}

const list_t *bta_gattc_get_services(uint16_t conn_id)
{
    tBTA_GATTC_CLCB *p_clcb = bta_gattc_find_clcb_by_conn_id(conn_id);

    if(p_clcb == NULL) {
        return NULL;
    }

    tBTA_GATTC_SERV *p_srcb = p_clcb->p_srcb;
    return bta_gattc_get_services_srcb(p_srcb);
}

tBTA_GATTC_SERVICE  *bta_gattc_find_matching_service(const list_t *services, uint16_t handle)
{
    if(!services || list_is_empty(services)) {
        return NULL;
    }

    for(list_node_t *sn = list_begin(services);
            sn != list_end(services); sn = list_next(sn)) {
        tBTA_GATTC_SERVICE *service = list_node(sn);

        if(handle >= service->s_handle && handle <= service->e_handle) {
            return service;
        }
    }

    return NULL;
}

const tBTA_GATTC_SERVICE  *bta_gattc_get_service_for_handle_srcb(tBTA_GATTC_SERV *p_srcb,
        uint16_t handle)
{
    const list_t *services = bta_gattc_get_services_srcb(p_srcb);
    return bta_gattc_find_matching_service(services, handle);
}

const tBTA_GATTC_SERVICE  *bta_gattc_get_service_for_handle(uint16_t conn_id, uint16_t handle)
{
    const list_t *services = bta_gattc_get_services(conn_id);
    return bta_gattc_find_matching_service(services, handle);
}

tBTA_GATTC_CHARACTERISTIC  *bta_gattc_get_characteristic_srcb(tBTA_GATTC_SERV *p_srcb,
        uint16_t handle)
{
    const tBTA_GATTC_SERVICE *service = bta_gattc_get_service_for_handle_srcb(p_srcb, handle);

    if(!service) {
        return NULL;
    }

    for(list_node_t *cn = list_begin(service->characteristics);
            cn != list_end(service->characteristics); cn = list_next(cn)) {
        tBTA_GATTC_CHARACTERISTIC *p_char = list_node(cn);

        if(handle == p_char->handle) {
            return p_char;
        }
    }

    return NULL;
}

tBTA_GATTC_CHARACTERISTIC  *bta_gattc_get_characteristic(uint16_t conn_id, uint16_t handle)
{
    tBTA_GATTC_CLCB *p_clcb = bta_gattc_find_clcb_by_conn_id(conn_id);

    if(p_clcb == NULL) {
        return NULL;
    }

    tBTA_GATTC_SERV *p_srcb = p_clcb->p_srcb;
    return bta_gattc_get_characteristic_srcb(p_srcb, handle);
}

tBTA_GATTC_DESCRIPTOR  *bta_gattc_get_descriptor_srcb(tBTA_GATTC_SERV *p_srcb, uint16_t handle)
{
    const tBTA_GATTC_SERVICE *service = bta_gattc_get_service_for_handle_srcb(p_srcb, handle);

    if(!service) {
        return NULL;
    }

    for(list_node_t *cn = list_begin(service->characteristics);
            cn != list_end(service->characteristics); cn = list_next(cn)) {
        tBTA_GATTC_CHARACTERISTIC *p_char = list_node(cn);

        for(list_node_t *dn = list_begin(p_char->descriptors);
                dn != list_end(p_char->descriptors); dn = list_next(dn)) {
            tBTA_GATTC_DESCRIPTOR *p_desc = list_node(dn);

            if(handle == p_desc->handle) {
                return p_desc;
            }
        }
    }

    return NULL;
}

tBTA_GATTC_DESCRIPTOR  *bta_gattc_get_descriptor(uint16_t conn_id, uint16_t handle)
{
    tBTA_GATTC_CLCB *p_clcb = bta_gattc_find_clcb_by_conn_id(conn_id);

    if(p_clcb == NULL) {
        return NULL;
    }

    tBTA_GATTC_SERV *p_srcb = p_clcb->p_srcb;
    return bta_gattc_get_descriptor_srcb(p_srcb, handle);
}

/*******************************************************************************
**
** Function         bta_gattc_fill_gatt_db_el
**
** Description      fill a btgatt_db_element_t value
**
** Returns          None.
**
*******************************************************************************/
void bta_gattc_fill_gatt_db_el(tls_btgatt_db_element_t *p_attr,
                               tls_bt_gatt_db_attribute_type_t type,
                               uint16_t att_handle,
                               uint16_t s_handle, uint16_t e_handle,
                               uint16_t id, tBT_UUID uuid, uint8_t prop)
{
    p_attr->type             = type;
    p_attr->attribute_handle = att_handle;
    p_attr->start_handle     = s_handle;
    p_attr->end_handle       = e_handle;
    p_attr->id               = id;
    p_attr->properties       = prop;
    bta_to_btif_uuid(&p_attr->uuid, &uuid);
}

/*******************************************************************************
** Returns          number of elements inside db from start_handle to end_handle
*******************************************************************************/
static size_t bta_gattc_get_db_size(list_t *services,
                                    uint16_t start_handle, uint16_t end_handle)
{
    if(!services || list_is_empty(services)) {
        return 0;
    }

    size_t db_size = 0;

    for(list_node_t *sn = list_begin(services);
            sn != list_end(services); sn = list_next(sn)) {
        tBTA_GATTC_SERVICE *p_cur_srvc = list_node(sn);

        if(p_cur_srvc->s_handle < start_handle) {
            continue;
        }

        if(p_cur_srvc->e_handle > end_handle) {
            break;
        }

        db_size++;

        if(!p_cur_srvc->characteristics || list_is_empty(p_cur_srvc->characteristics)) {
            continue;
        }

        for(list_node_t *cn = list_begin(p_cur_srvc->characteristics);
                cn != list_end(p_cur_srvc->characteristics); cn = list_next(cn)) {
            tBTA_GATTC_CHARACTERISTIC *p_char = list_node(cn);
            db_size++;

            if(p_char->descriptors) {
                db_size += list_length(p_char->descriptors);
            }
        }

        if(p_cur_srvc->included_svc) {
            db_size += list_length(p_cur_srvc->included_svc);
        }
    }

    return db_size;
}

/*******************************************************************************
**
** Function         bta_gattc_get_gatt_db_impl
**
** Description      copy the server GATT database into db parameter.
**
** Parameters       p_srvc_cb: server.
**                  db: output parameter which will contain GATT database copy.
**                      Caller is responsible for freeing it.
**                  count: output parameter which will contain number of
**                  elements in database.
**
** Returns          None.
**
*******************************************************************************/
static void bta_gattc_get_gatt_db_impl(tBTA_GATTC_SERV *p_srvc_cb,
                                       uint16_t start_handle, uint16_t end_handle,
                                       tls_btgatt_db_element_t **db,
                                       int *count)
{
    APPL_TRACE_DEBUG("%s: start_handle 0x%04x, end_handle 0x%04x",
                     __func__, start_handle, end_handle);

    if(!p_srvc_cb->p_srvc_cache || list_is_empty(p_srvc_cb->p_srvc_cache)) {
        *count = 0;
        *db = NULL;
        return;
    }

    size_t db_size = bta_gattc_get_db_size(p_srvc_cb->p_srvc_cache, start_handle, end_handle);
    void *buffer = GKI_getbuf(db_size * sizeof(tls_btgatt_db_element_t));
    tls_btgatt_db_element_t *curr_db_attr = buffer;

    for(list_node_t *sn = list_begin(p_srvc_cb->p_srvc_cache);
            sn != list_end(p_srvc_cb->p_srvc_cache); sn = list_next(sn)) {
        tBTA_GATTC_SERVICE *p_cur_srvc = list_node(sn);

        if(p_cur_srvc->s_handle < start_handle) {
            continue;
        }

        if(p_cur_srvc->e_handle > end_handle) {
            break;
        }

        bta_gattc_fill_gatt_db_el(curr_db_attr,
                                  p_cur_srvc->is_primary ?
                                  WM_BTGATT_DB_PRIMARY_SERVICE :
                                  WM_BTGATT_DB_SECONDARY_SERVICE,
                                  0 /* att_handle */,
                                  p_cur_srvc->s_handle,
                                  p_cur_srvc->e_handle,
                                  p_cur_srvc->s_handle,
                                  p_cur_srvc->uuid,
                                  0 /* prop */);
        curr_db_attr++;

        if(!p_cur_srvc->characteristics || list_is_empty(p_cur_srvc->characteristics)) {
            continue;
        }

        for(list_node_t *cn = list_begin(p_cur_srvc->characteristics);
                cn != list_end(p_cur_srvc->characteristics); cn = list_next(cn)) {
            tBTA_GATTC_CHARACTERISTIC *p_char = list_node(cn);
            bta_gattc_fill_gatt_db_el(curr_db_attr,
                                      WM_BTGATT_DB_CHARACTERISTIC,
                                      p_char->handle,
                                      0 /* s_handle */,
                                      0 /* e_handle */,
                                      p_char->handle,
                                      p_char->uuid,
                                      p_char->properties);
            curr_db_attr++;

            if(!p_char->descriptors || list_is_empty(p_char->descriptors)) {
                continue;
            }

            for(list_node_t *dn = list_begin(p_char->descriptors);
                    dn != list_end(p_char->descriptors); dn = list_next(dn)) {
                tBTA_GATTC_DESCRIPTOR *p_desc = list_node(dn);
                bta_gattc_fill_gatt_db_el(curr_db_attr,
                                          WM_BTGATT_DB_DESCRIPTOR,
                                          p_desc->handle,
                                          0 /* s_handle */,
                                          0 /* e_handle */,
                                          p_desc->handle,
                                          p_desc->uuid,
                                          0 /* property */);
                curr_db_attr++;
            }
        }

        if(!p_cur_srvc->included_svc || list_is_empty(p_cur_srvc->included_svc)) {
            continue;
        }

        for(list_node_t *isn = list_begin(p_cur_srvc->included_svc);
                isn != list_end(p_cur_srvc->included_svc); isn = list_next(isn)) {
            tBTA_GATTC_INCLUDED_SVC *p_isvc = list_node(isn);
            bta_gattc_fill_gatt_db_el(curr_db_attr,
                                      WM_BTGATT_DB_INCLUDED_SERVICE,
                                      p_isvc->handle,
                                      0 /* s_handle */,
                                      0 /* e_handle */,
                                      p_isvc->handle,
                                      p_isvc->uuid,
                                      0 /* property */);
            curr_db_attr++;
        }
    }

    *db = buffer;
    *count = db_size;
}

/*******************************************************************************
**
** Function         bta_gattc_get_gatt_db
**
** Description      copy the server GATT database into db parameter.
**
** Parameters       conn_id: connection ID which identify the server.
**                  db: output parameter which will contain GATT database copy.
**                      Caller is responsible for freeing it.
**                  count: number of elements in database.
**
** Returns          None.
**
*******************************************************************************/
void bta_gattc_get_gatt_db(uint16_t conn_id, uint16_t start_handle, uint16_t end_handle,
                           tls_btgatt_db_element_t **db, int *count)
{
    tBTA_GATTC_CLCB *p_clcb = bta_gattc_find_clcb_by_conn_id(conn_id);
    LOG_DEBUG(LOG_TAG, "%s", __func__);

    if(p_clcb == NULL) {
        APPL_TRACE_ERROR("Unknown conn ID: %d", conn_id);
        *db = NULL;
        *count = -1;
        return;
    }

    if(p_clcb->state != BTA_GATTC_CONN_ST) {
        APPL_TRACE_ERROR("server cache not available, CLCB state = %d",
                         p_clcb->state);
        *db = NULL;
        *count = -1;
        return;
    }

    if(!p_clcb->p_srcb || p_clcb->p_srcb->p_srvc_list ||  /* no active discovery */
            !p_clcb->p_srcb->p_srvc_cache) {
        APPL_TRACE_ERROR("No server cache available");
    }

    bta_gattc_get_gatt_db_impl(p_clcb->p_srcb, start_handle, end_handle, db, count);
}

/*******************************************************************************
**
** Function         bta_gattc_rebuild_cache
**
** Description      rebuild server cache from NV cache.
**
** Parameters
**
** Returns          None.
**
*******************************************************************************/
void bta_gattc_rebuild_cache(tBTA_GATTC_SERV *p_srvc_cb, uint16_t num_attr,
                             tBTA_GATTC_NV_ATTR *p_attr)
{
    /* first attribute loading, initialize buffer */
    APPL_TRACE_ERROR("%s: bta_gattc_rebuild_cache", __func__);
    list_free(p_srvc_cb->p_srvc_cache);
    p_srvc_cb->p_srvc_cache = NULL;

    while(num_attr > 0 && p_attr != NULL) {
        switch(p_attr->attr_type) {
            case BTA_GATTC_ATTR_TYPE_SRVC:
                bta_gattc_add_srvc_to_cache(p_srvc_cb,
                                            p_attr->s_handle,
                                            p_attr->e_handle,
                                            &p_attr->uuid,
                                            p_attr->is_primary);
                break;

            case BTA_GATTC_ATTR_TYPE_CHAR:
                //TODO(jpawlowski): store decl_handle properly.
                bta_gattc_add_char_to_cache(p_srvc_cb,
                                            p_attr->s_handle,
                                            p_attr->s_handle,
                                            &p_attr->uuid,
                                            p_attr->prop);
                break;

            case BTA_GATTC_ATTR_TYPE_CHAR_DESCR:
            case BTA_GATTC_ATTR_TYPE_INCL_SRVC:
                bta_gattc_add_attr_to_cache(p_srvc_cb,
                                            p_attr->s_handle,
                                            &p_attr->uuid,
                                            p_attr->prop,
                                            p_attr->incl_srvc_handle,
                                            p_attr->attr_type);
                break;
        }

        p_attr ++;
        num_attr --;
    }
}

/*******************************************************************************
**
** Function         bta_gattc_fill_nv_attr
**
** Description      fill a NV attribute entry value
**
** Returns          None.
**
*******************************************************************************/
void bta_gattc_fill_nv_attr(tBTA_GATTC_NV_ATTR *p_attr, uint8_t type, uint16_t s_handle,
                            uint16_t e_handle, tBT_UUID uuid, uint8_t prop, uint16_t incl_srvc_handle,
                            uint8_t is_primary)
{
    p_attr->s_handle    = s_handle;
    p_attr->e_handle    = e_handle;
    p_attr->attr_type   = type;
    p_attr->is_primary  = is_primary;
    p_attr->id          = 0;
    p_attr->prop        = prop;
    p_attr->incl_srvc_handle = incl_srvc_handle;
    wm_memcpy(&p_attr->uuid, &uuid, sizeof(tBT_UUID));
}

/*******************************************************************************
**
** Function         bta_gattc_cache_save
**
** Description      save the server cache into NV
**
** Returns          None.
**
*******************************************************************************/
void bta_gattc_cache_save(tBTA_GATTC_SERV *p_srvc_cb, uint16_t conn_id)
{
    if(!p_srvc_cb->p_srvc_cache || list_is_empty(p_srvc_cb->p_srvc_cache)) {
        return;
    }

    int i = 0;
    size_t db_size = bta_gattc_get_db_size(p_srvc_cb->p_srvc_cache, 0x0000, 0xFFFF);
    tBTA_GATTC_NV_ATTR *nv_attr = GKI_getbuf(db_size * sizeof(tBTA_GATTC_NV_ATTR));

    for(list_node_t *sn = list_begin(p_srvc_cb->p_srvc_cache);
            sn != list_end(p_srvc_cb->p_srvc_cache); sn = list_next(sn)) {
        tBTA_GATTC_SERVICE *p_cur_srvc = list_node(sn);
        bta_gattc_fill_nv_attr(&nv_attr[i++],
                               BTA_GATTC_ATTR_TYPE_SRVC,
                               p_cur_srvc->s_handle,
                               p_cur_srvc->e_handle,
                               p_cur_srvc->uuid,
                               0 /* properties */,
                               0 /* incl_srvc_handle */,
                               p_cur_srvc->is_primary);
    }

    for(list_node_t *sn = list_begin(p_srvc_cb->p_srvc_cache);
            sn != list_end(p_srvc_cb->p_srvc_cache); sn = list_next(sn)) {
        tBTA_GATTC_SERVICE *p_cur_srvc = list_node(sn);

        if(!p_cur_srvc->characteristics || list_is_empty(p_cur_srvc->characteristics)) {
            continue;
        }

        for(list_node_t *cn = list_begin(p_cur_srvc->characteristics);
                cn != list_end(p_cur_srvc->characteristics); cn = list_next(cn)) {
            tBTA_GATTC_CHARACTERISTIC *p_char = list_node(cn);
            bta_gattc_fill_nv_attr(&nv_attr[i++],
                                   BTA_GATTC_ATTR_TYPE_CHAR,
                                   p_char->handle,
                                   0,
                                   p_char->uuid,
                                   p_char->properties,
                                   0 /* incl_srvc_handle */,
                                   FALSE);

            if(!p_char->descriptors || list_is_empty(p_char->descriptors)) {
                continue;
            }

            for(list_node_t *dn = list_begin(p_char->descriptors);
                    dn != list_end(p_char->descriptors); dn = list_next(dn)) {
                tBTA_GATTC_DESCRIPTOR *p_desc = list_node(dn);
                bta_gattc_fill_nv_attr(&nv_attr[i++],
                                       BTA_GATTC_ATTR_TYPE_CHAR_DESCR,
                                       p_desc->handle,
                                       0,
                                       p_desc->uuid,
                                       0 /* properties */,
                                       0 /* incl_srvc_handle */,
                                       FALSE);
            }
        }

        if(!p_cur_srvc->included_svc || list_is_empty(p_cur_srvc->included_svc)) {
            continue;
        }

        for(list_node_t *an = list_begin(p_cur_srvc->included_svc);
                an != list_end(p_cur_srvc->included_svc); an = list_next(an)) {
            tBTA_GATTC_INCLUDED_SVC *p_isvc = list_node(an);
            bta_gattc_fill_nv_attr(&nv_attr[i++],
                                   BTA_GATTC_ATTR_TYPE_INCL_SRVC,
                                   p_isvc->handle,
                                   0,
                                   p_isvc->uuid,
                                   0 /* properties */,
                                   p_isvc->included_service->s_handle,
                                   FALSE);
        }
    }

    bta_gattc_cache_write(p_srvc_cb->server_bda, db_size, nv_attr);
    GKI_freebuf(nv_attr);
}

/*******************************************************************************
**
** Function         bta_gattc_cache_load
**
** Description      Load GATT cache from storage for server.
**
** Parameter        p_clcb: pointer to server clcb, that will
**                          be filled from storage
** Returns          true on success, false otherwise
**
*******************************************************************************/
uint8_t bta_gattc_cache_load(tBTA_GATTC_CLCB *p_clcb)
{
    char fname[255] = {0};
    bta_gattc_generate_cache_file_name(fname, p_clcb->p_srcb->server_bda);
    FILE *fd = fopen(fname, "rb");

    if(!fd) {
        APPL_TRACE_ERROR("%s: can't open GATT cache file %s for reading, error: %s",
                         __func__, fname, strerror(errno));
        return false;
    }

    uint16_t cache_ver = 0;
    tBTA_GATTC_NV_ATTR  *attr = NULL;
    uint8_t success = false;

    if(fread(&cache_ver, sizeof(uint16_t), 1, fd) != 1) {
        APPL_TRACE_ERROR("%s: can't read GATT cache version from: %s", __func__, fname);
        goto done;
    }

    if(cache_ver != GATT_CACHE_VERSION) {
        APPL_TRACE_ERROR("%s: wrong GATT cache version: %s", __func__, fname);
        goto done;
    }

    uint16_t num_attr = 0;

    if(fread(&num_attr, sizeof(uint16_t), 1, fd) != 1) {
        APPL_TRACE_ERROR("%s: can't read number of GATT attributes: %s", __func__, fname);
        goto done;
    }

    attr = GKI_getbuf(sizeof(tBTA_GATTC_NV_ATTR) * num_attr);

    if(fread(attr, sizeof(tBTA_GATTC_NV_ATTR), 0xFF, fd) != num_attr) {
        APPL_TRACE_ERROR("%s: can't read GATT attributes: %s", __func__, fname);
        goto done;
    }

    bta_gattc_rebuild_cache(p_clcb->p_srcb, num_attr, attr);
    success = true;
done:
    GKI_freebuf(attr);
    fclose(fd);
    return success;
}

/*******************************************************************************
**
** Function         bta_gattc_cache_write
**
** Description      This callout function is executed by GATT when a server cache
**                  is available to save.
**
** Parameter        server_bda: server bd address of this cache belongs to
**                  num_attr: number of attribute to be save.
**                  attr: pointer to the list of attributes to save.
** Returns
**
*******************************************************************************/
static void bta_gattc_cache_write(BD_ADDR server_bda, uint16_t num_attr,
                                  tBTA_GATTC_NV_ATTR *attr)
{
    char fname[255] = {0};
    bta_gattc_generate_cache_file_name(fname, server_bda);
    FILE *fd = fopen(fname, "wb");

    if(!fd) {
        APPL_TRACE_ERROR("%s: can't open GATT cache file for writing: %s", __func__, fname);
        return;
    }

    uint16_t cache_ver = GATT_CACHE_VERSION;

    if(fwrite(&cache_ver, sizeof(uint16_t), 1, fd) != 1) {
        APPL_TRACE_ERROR("%s: can't write GATT cache version: %s", __func__, fname);
        fclose(fd);
        return;
    }

    if(fwrite(&num_attr, sizeof(uint16_t), 1, fd) != 1) {
        APPL_TRACE_ERROR("%s: can't write GATT cache attribute count: %s", __func__, fname);
        fclose(fd);
        return;
    }

    if(fwrite(attr, sizeof(tBTA_GATTC_NV_ATTR), num_attr, fd) != num_attr) {
        APPL_TRACE_ERROR("%s: can't write GATT cache attributes: %s", __func__, fname);
        fclose(fd);
        return;
    }

    fclose(fd);
}

/*******************************************************************************
**
** Function         bta_gattc_cache_reset
**
** Description      This callout function is executed by GATTC to reset cache in
**                  application
**
** Parameter        server_bda: server bd address of this cache belongs to
**
** Returns          void.
**
*******************************************************************************/
void bta_gattc_cache_reset(BD_ADDR server_bda)
{
    BTIF_TRACE_DEBUG("%s", __func__);
    char fname[255] = {0};
    bta_gattc_generate_cache_file_name(fname, server_bda);
#if 0
    unlink(fname);
#endif
}
#endif /* BTA_GATT_INCLUDED */

