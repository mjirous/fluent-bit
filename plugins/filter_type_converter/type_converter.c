/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*  Fluent Bit
 *  ==========
 *  Copyright (C) 2019-2021 The Fluent Bit Authors
 *  Copyright (C) 2015-2018 Treasure Data Inc.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include <fluent-bit/flb_filter_plugin.h>
#include <fluent-bit/flb_kv.h>
#include <fluent-bit/flb_time.h>
#include <fluent-bit/flb_mem.h>
#include <fluent-bit/flb_utils.h>
#include <fluent-bit/flb_mp.h>
#include <msgpack.h>
#include "type_converter.h"

static int delete_conv_entry(struct conv_entry *conv)
{
    if (conv == NULL) {
        return 0;
    }

    if (conv->from_key != NULL) {
        flb_sds_destroy(conv->from_key);
        conv->from_key = NULL;
    }
    if (conv->to_key != NULL) {
        flb_sds_destroy(conv->to_key);
        conv->to_key = NULL;
    }
    if (conv->rule != NULL) {
        flb_typecast_rule_destroy(conv->rule);
    }
    if (conv->from_ra != NULL) {
        flb_ra_destroy(conv->from_ra);
    }
    mk_list_del(&conv->_head);
    flb_free(conv);
    return 0;
}

static int configure(struct type_converter_ctx *ctx,
                     struct flb_filter_instance *f_ins)
{
    struct flb_kv          *kv = NULL;
    struct mk_list         *head = NULL;
    struct conv_entry      *entry = NULL;
    struct mk_list         *split = NULL;
    struct flb_split_entry *sentry = NULL;

    /* Iterate all filter properties */
    mk_list_foreach(head, &f_ins->properties) {
        kv = mk_list_entry(head, struct flb_kv, _head);
        if ((!strcasecmp(kv->key, "int_key")) || (!strcasecmp(kv->key, "str_key")) || 
            !strcasecmp(kv->key, "uint_key") ||(!strcasecmp(kv->key, "float_key"))) {
            entry = flb_malloc(sizeof(struct conv_entry));
            if (entry == NULL) {
                flb_errno();
                continue;
            }
            entry->rule = NULL;

            split = flb_utils_split(kv->val, ' ', 3);
            if (mk_list_size(split) != 3) {
                flb_plg_error(ctx->ins, "invalid record parameters, "
                              "expects 'from_key to_key type' %d", mk_list_size(split));
                flb_free(entry);
                flb_utils_split_free(split);
                continue;
            }
            /* from_key name */
            sentry          = mk_list_entry_first(split, struct flb_split_entry, _head);
            entry->from_key = flb_sds_create_len(sentry->value, sentry->len);

            /* to_key name */
            sentry = mk_list_entry_next(&sentry->_head, struct flb_split_entry,
                                        _head, split);
            entry->to_key   = flb_sds_create_len(sentry->value, sentry->len);

            sentry = mk_list_entry_last(split, struct flb_split_entry, _head);
            if (!strcasecmp(kv->key, "int_key")) {
                entry->rule = flb_typecast_rule_create("int", 3,
                                                       sentry->value,
                                                       sentry->len);
            }
            else if (!strcasecmp(kv->key, "uint_key")) {
                entry->rule = flb_typecast_rule_create("uint", 4,
                                                       sentry->value,
                                                       sentry->len);
            }
            else if(!strcasecmp(kv->key, "float_key")) {
                entry->rule = flb_typecast_rule_create("float", 5,
                                                       sentry->value,
                                                       sentry->len);
            }
            else if(!strcasecmp(kv->key, "str_key")) {
                entry->rule = flb_typecast_rule_create("string", 6,
                                                       sentry->value,
                                                       sentry->len);
            }
            entry->from_ra = flb_ra_create(entry->from_key, FLB_FALSE);
            if (entry->rule == NULL || entry->from_ra == NULL) {
                flb_plg_error(ctx->ins,
                              "configuration error. ignore the key=%s",
                              entry->from_key);
                flb_utils_split_free(split);
                delete_conv_entry(entry);
                continue;
            }
            flb_utils_split_free(split);
            mk_list_add(&entry->_head, &ctx->conv_entries);
        }
    }
    if (mk_list_size(&ctx->conv_entries) == 0) {
        flb_plg_error(ctx->ins, "no rules");
        return -1;
    }

    return 0;
}

static int delete_list(struct type_converter_ctx *ctx)
{
    struct mk_list *tmp;
    struct mk_list *head;
    struct conv_entry *conv;

    mk_list_foreach_safe(head, tmp, &ctx->conv_entries) {
        conv = mk_list_entry(head, struct conv_entry,  _head);
        delete_conv_entry(conv);
    }
    return 0;
}

static int cb_type_converter_init(struct flb_filter_instance *ins,
                                  struct flb_config *config,
                                  void *data)
{
    struct type_converter_ctx *ctx = NULL;
    int ret = 0;

    ctx = flb_calloc(1, sizeof(struct type_converter_ctx));
    if (!ctx) {
        flb_errno();
        return -1;
    }
    ctx->ins = ins;
    mk_list_init(&ctx->conv_entries);

    ret = configure(ctx, ins);
    if (ret < 0) {
        flb_plg_error(ins, "configuration error");
        flb_free(ctx);
        return -1;
    }
    /* set context */
    flb_filter_set_context(ins, ctx);

    return 0;
}

static int cb_type_converter_filter(const void *data, size_t bytes,
                                    const char *tag, int tag_len,
                                    void **out_buf, size_t *out_bytes,
                                    struct flb_filter_instance *ins,
                                    void *filter_context,
                                    struct flb_config *config)
{
    struct type_converter_ctx *ctx = filter_context;
    struct flb_time tm;
    size_t off = 0;
    int i;
    int map_num;
    int is_record_modified = FLB_FALSE;
    int ret;
    msgpack_sbuffer tmp_sbuf;
    msgpack_packer  tmp_pck;
    msgpack_unpacked result;
    msgpack_object  *obj;
    struct flb_mp_map_header mh;
    struct conv_entry *entry;
    struct mk_list *tmp;
    struct mk_list *head;

    msgpack_object *start_key;
    msgpack_object *out_key;
    msgpack_object *out_val;

    /* Create temporary msgpack buffer */
    msgpack_sbuffer_init(&tmp_sbuf);
    msgpack_packer_init(&tmp_pck, &tmp_sbuf, msgpack_sbuffer_write);

    /* Iterate each item to know map number */
    msgpack_unpacked_init(&result);
    while (msgpack_unpack_next(&result, data, bytes, &off) == MSGPACK_UNPACK_SUCCESS) {
        if (result.data.type != MSGPACK_OBJECT_ARRAY) {
            continue;
        }

        flb_time_pop_from_msgpack(&tm, &result, &obj);
        if (obj->type != MSGPACK_OBJECT_MAP) {
            continue;
        }
        map_num = obj->via.map.size;

        msgpack_pack_array(&tmp_pck, 2);
        flb_time_append_to_msgpack(&tm, &tmp_pck, 0);

        flb_mp_map_header_init(&mh, &tmp_pck);
        /* write original k/v */
        for (i=0; i<map_num; i++) {
            flb_mp_map_header_append(&mh);
            msgpack_pack_object(&tmp_pck, obj->via.map.ptr[i].key);
            msgpack_pack_object(&tmp_pck, obj->via.map.ptr[i].val);
        }
        mk_list_foreach_safe(head, tmp, &ctx->conv_entries) {
            start_key = NULL;
            out_key   = NULL;
            out_val   = NULL;
            entry = mk_list_entry(head, struct conv_entry, _head);
            ret = flb_ra_get_kv_pair(entry->from_ra, *obj, &start_key, &out_key, &out_val);
            if (start_key == NULL || out_key == NULL || out_val == NULL) {
                continue;
            }
            /* key is found. try to convert. */
            flb_mp_map_header_append(&mh);
            msgpack_pack_str(&tmp_pck, flb_sds_len(entry->to_key));
            msgpack_pack_str_body(&tmp_pck, entry->to_key, flb_sds_len(entry->to_key));
            ret = flb_typecast_pack(*out_val, entry->rule, &tmp_pck);
            if (ret < 0) {
                /* failed. try to write original val... */
                flb_plg_error(ctx->ins, "failed to convert. key=%s", entry->from_key);
                msgpack_pack_object(&tmp_pck, *out_val);
                continue;
            }
            is_record_modified = FLB_TRUE;
        }

        flb_mp_map_header_end(&mh);
    }
    msgpack_unpacked_destroy(&result);

    if (is_record_modified != FLB_TRUE) {
        /* Destroy the buffer to avoid more overhead */
        flb_plg_trace(ctx->ins, "no touch");
        msgpack_sbuffer_destroy(&tmp_sbuf);
        return FLB_FILTER_NOTOUCH;
    }
    /* link new buffers */
    *out_buf   = tmp_sbuf.data;
    *out_bytes = tmp_sbuf.size;
    return FLB_FILTER_MODIFIED;
}
static int cb_type_converter_exit(void *data, struct flb_config *config) {
    struct type_converter_ctx *ctx = data;

    if (ctx == NULL) {
        return 0;
    }
    delete_list(ctx);
    flb_free(ctx);
    return 0;
}

  

struct flb_filter_plugin filter_type_converter_plugin = {
    .name        = "type_converter",
    .description = "Data type converter",
    .cb_init     = cb_type_converter_init,
    .cb_filter   = cb_type_converter_filter,
    .cb_exit     = cb_type_converter_exit,
    .flags       = 0,
};
