/*******************************************************************************
 * This file is part of the Incubed project.
 * Sources: https://github.com/slockit/in3-c
 * 
 * Copyright (C) 2018-2019 slock.it GmbH, Blockchains LLC
 * 
 * 
 * COMMERCIAL LICENSE USAGE
 * 
 * Licensees holding a valid commercial license may use this file in accordance 
 * with the commercial license agreement provided with the Software or, alternatively, 
 * in accordance with the terms contained in a written agreement between you and 
 * slock.it GmbH/Blockchains LLC. For licensing terms and conditions or further 
 * information please contact slock.it at in3@slock.it.
 * 	
 * Alternatively, this file may be used under the AGPL license as follows:
 *    
 * AGPL LICENSE USAGE
 * 
 * This program is free software: you can redistribute it and/or modify it under the
 * terms of the GNU Affero General Public License as published by the Free Software 
 * Foundation, either version 3 of the License, or (at your option) any later version.
 *  
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY 
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A 
 * PARTICULAR PURPOSE. See the GNU Affero General Public License for more details.
 * [Permissions of this strong copyleft license are conditioned on making available 
 * complete source code of licensed works and modifications, which include larger 
 * works using a licensed work, under the same license. Copyright and license notices 
 * must be preserved. Contributors provide an express grant of patent rights.]
 * You should have received a copy of the GNU Affero General Public License along 
 * with this program. If not, see <https://www.gnu.org/licenses/>.
 *******************************************************************************/

#include "nodelist.h"
#include "../util/data.h"
#include "../util/log.h"
#include "../util/mem.h"
#include "../util/utils.h"
#include "cache.h"
#include "context.h"
#include "keys.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define DAY 24 * 2600

static void free_nodeList(in3_node_t* nodelist, int count) {
  // clean chain..
  for (int i = 0; i < count; i++) {
    if (nodelist[i].url) _free(nodelist[i].url);
    if (nodelist[i].address) b_free(nodelist[i].address);
  }
  _free(nodelist);
}

static in3_ret_t fill_chain(in3_chain_t* chain, in3_ctx_t* ctx, d_token_t* result) {
  in3_ret_t      res  = IN3_OK;
  _time_t        _now = _time(); // TODO here we might get a -1 or a unsuable number if the device does not know the current timestamp.
  const uint64_t now  = (uint64_t) max(0, _now);

  // read the nodes
  d_token_t *nodes = d_get(result, K_NODES), *t;
  int        len   = d_len(nodes);

  if (!nodes || d_type(nodes) != T_ARRAY)
    return ctx_set_error(ctx, "No Nodes in the result", IN3_EINVALDT);

  if (!(t = d_get(result, K_LAST_BLOCK_NUMBER)))
    return ctx_set_error(ctx, "LastBlockNumer is missing", IN3_EINVALDT);

  // update last blockNumber
  const uint64_t last_block = d_long(t);
  if (last_block > chain->last_block)
    chain->last_block = last_block;
  else
    return IN3_OK; // if the last block is older than the current one we don't update, but simply ignore it.

  // new nodelist
  in3_node_t*        newList = _calloc(len, sizeof(in3_node_t));
  in3_node_weight_t* weights = _calloc(len, sizeof(in3_node_weight_t));
  d_token_t*         node    = NULL;

  // set new values
  for (int i = 0; i < len; i++) {
    in3_node_t* n = newList + i;
    node          = node ? d_next(node) : d_get_at(nodes, i);
    if (!node) {
      res = ctx_set_error(ctx, "node missing", IN3_EINVALDT);
      break;
    }

    int old_index                = i;
    weights[i].weight            = 1;
    n->capacity                  = d_get_intkd(node, K_CAPACITY, 1);
    n->index                     = d_get_intkd(node, K_INDEX, i);
    n->deposit                   = d_get_longk(node, K_DEPOSIT);
    n->props                     = d_get_longkd(node, K_PROPS, 65535);
    n->url                       = d_get_stringk(node, K_URL);
    n->address                   = d_get_byteskl(node, K_ADDRESS, 20);
    const uint64_t register_time = d_get_longk(node, K_REGISTER_TIME);

    if (n->address)
      n->address = b_dup(n->address); // create a copy since the src will be freed.
    else {
      res = ctx_set_error(ctx, "missing address in nodelist", IN3_EINVALDT);
      break;
    }

    // restore the nodeweights if the address was known in the old nodeList
    if (chain->nodelist_length <= i || !b_cmp(chain->nodelist[i].address, n->address)) {
      old_index = -1;
      for (int j = 0; j < chain->nodelist_length; j++) {
        if (b_cmp(chain->nodelist[j].address, n->address)) {
          old_index = j;
          break;
        }
      }
    }
    if (old_index >= 0) memcpy(weights + i, chain->weights + old_index, sizeof(in3_node_weight_t));

    // if this is a newly registered node, we wait 24h before we use it, since this is the time where mallicous nodes may be unregistered.
    if (now && register_time + DAY > now && now > register_time)
      weights[i].blacklisted_until = register_time + DAY;

    // clone the url since the src will be freed
    if (n->url)
      n->url = _strdupn(n->url, -1);
    else {
      res = ctx_set_error(ctx, "missing url in nodelist", IN3_EINVALDT);
      break;
    }
  }

  if (res == IN3_OK) {
    // successfull, so we can update the chain.
    free_nodeList(chain->nodelist, chain->nodelist_length);
    _free(chain->weights);
    chain->nodelist        = newList;
    chain->nodelist_length = len;
    chain->weights         = weights;
  } else {
    free_nodeList(newList, len);
    _free(weights);
  }

  return res;
}

void in3_client_run_chain_whitelisting(in3_chain_t* chain) {
  if (!chain->whitelist)
    return;

  for (int j = 0; j < chain->nodelist_length; ++j)
    chain->nodelist[j].whitelisted = false;

  for (size_t i = 0; i < chain->whitelist->addresses.len / 20; i += 20) {
    for (int j = 0; j < chain->nodelist_length; ++j)
      if (!memcmp(chain->whitelist->addresses.data + i, chain->nodelist[j].address, 20))
        chain->nodelist[j].whitelisted = true;
  }
}

static in3_ret_t in3_client_fill_chain_whitelist(in3_chain_t* chain, in3_ctx_t* ctx, d_token_t* result) {
  in3_whitelist_t* wl    = chain->whitelist;
  int              i     = 0;
  d_token_t *      nodes = d_get(result, K_NODES), *t = NULL;

  if (!wl) return ctx_set_error(ctx, "No whitelist set", IN3_EINVALDT);
  if (!nodes || d_type(nodes) != T_ARRAY) return ctx_set_error(ctx, "No Nodes in the result", IN3_EINVALDT);

  const int len = d_len(nodes);
  if (!(t = d_get(result, K_LAST_BLOCK_NUMBER)))
    return ctx_set_error(ctx, "LastBlockNumer is missing", IN3_EINVALDT);

  // update last blockNumber
  const uint64_t last_block = d_long(t);
  if (last_block > wl->last_block)
    wl->last_block = last_block;
  else
    return IN3_OK; // if the last block is older than the current one we don't update, but simply ignore it.

  // now update the addresses
  if (wl->addresses.data) _free(wl->addresses.data);
  wl->addresses = bytes(_malloc(len * 20), len * 20);
  if (!wl->addresses.data) return IN3_ENOMEM;

  for (d_iterator_t iter = d_iter(nodes); iter.left; d_iter_next(&iter), i += 20)
    d_bytes_to(iter.token, wl->addresses.data + i, 20);

  in3_client_run_chain_whitelisting(chain);
  return IN3_OK;
}

static in3_ret_t update_nodelist(in3_t* c, in3_chain_t* chain, in3_ctx_t* parent_ctx) {
  // is there a useable required ctx?
  in3_ctx_t* ctx = ctx_find_required(parent_ctx, "in3_nodeList");

  if (ctx)
    switch (in3_ctx_state(ctx)) {
      case CTX_ERROR:
        return ctx_set_error(parent_ctx, "Error updating node_list", ctx_set_error(parent_ctx, ctx->error, IN3_ERPC));
      case CTX_WAITING_FOR_REQUIRED_CTX:
      case CTX_WAITING_FOR_RESPONSE:
        return IN3_WAITING;
      case CTX_SUCCESS: {

        d_token_t* r = d_get(ctx->responses[0], K_RESULT);
        if (r) {
          // we have a result....
          const in3_ret_t res = fill_chain(chain, ctx, r);
          if (res < 0)
            return ctx_set_error(parent_ctx, "Error updating node_list", ctx_set_error(parent_ctx, ctx->error, res));
          else if (c->cache)
            in3_cache_store_nodelist(ctx, chain);
          ctx_remove_required(parent_ctx, ctx);
          in3_client_run_chain_whitelisting(chain);
          return IN3_OK;
        } else
          return ctx_set_error(parent_ctx, "Error updating node_list", ctx_check_response_error(ctx, 0));
      }
    }

  in3_log_debug("update the nodelist...\n");

  // create random seed
  char seed[67];
  sprintf(seed, "0x%08x%08x%08x%08x%08x%08x%08x%08x", _rand(), _rand(), _rand(), _rand(), _rand(), _rand(), _rand(), _rand());

  // create request
  char* req = _malloc(300);
  sprintf(req, "{\"method\":\"in3_nodeList\",\"jsonrpc\":\"2.0\",\"id\":1,\"params\":[%i,\"%s\",[]]}", c->node_limit, seed);

  // new client
  return ctx_add_required(parent_ctx, ctx_new(c, req));
}

static in3_ret_t update_whitelist(in3_t* c, in3_chain_t* chain, in3_ctx_t* parent_ctx) {
  // is there a useable required ctx?
  in3_ctx_t* ctx = ctx_find_required(parent_ctx, "in3_whiteList");

  if (ctx)
    switch (in3_ctx_state(ctx)) {
      case CTX_ERROR:
        return ctx_set_error(parent_ctx, "Error updating white_list", ctx_set_error(parent_ctx, ctx->error, IN3_ERPC));
      case CTX_WAITING_FOR_REQUIRED_CTX:
      case CTX_WAITING_FOR_RESPONSE:
        return IN3_WAITING;
      case CTX_SUCCESS: {

        d_token_t* result = d_get(ctx->responses[0], K_RESULT);
        if (result) {
          // we have a result....
          const in3_ret_t res = in3_client_fill_chain_whitelist(chain, ctx, result);
          if (res < 0)
            return ctx_set_error(parent_ctx, "Error updating white_list", ctx_set_error(parent_ctx, ctx->error, res));
          else if (c->cache)
            in3_cache_store_whitelist(ctx, chain);
          in3_client_run_chain_whitelisting(chain);
          ctx_remove_required(parent_ctx, ctx);
          return IN3_OK;
        } else
          return ctx_set_error(parent_ctx, "Error updating white_list", ctx_check_response_error(ctx, 0));
      }
    }

  in3_log_debug("update the whitelist...\n");

  // create request
  char* req     = _malloc(300);
  char  tmp[41] = {0};
  bytes_to_hex(chain->whitelist->contract, 20, tmp);
  sprintf(req, "{\"method\":\"in3_whiteList\",\"jsonrpc\":\"2.0\",\"id\":1,\"params\":[\"0x%s\"]}", tmp);

  // new client
  return ctx_add_required(parent_ctx, ctx_new(c, req));
}

void in3_ctx_free_nodes(node_weight_t* node) {
  node_weight_t* last_node = NULL;
  while (node) {
    last_node = node;
    node      = node->next;
    _free(last_node);
  }
}

in3_ret_t update_nodes(in3_t* c, in3_chain_t* chain) {
  in3_ctx_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  chain->needs_update = false;

  in3_ret_t ret = update_nodelist(c, chain, &ctx);
  if (ret == IN3_WAITING && ctx.required) {
    ret = in3_send_ctx(ctx.required);
    if (ret) return ret;
    return update_nodelist(c, chain, &ctx);
  }
  return ret;
}

#if defined(TEST) || defined(FILTER_NODES)

IN3_EXPORT_TEST bool
in3_node_props_match(const in3_node_props_t np_config, const in3_node_props_t np) {
  if (((np_config & np) & 0xFFFFFFFF) != (np_config & 0XFFFFFFFF)) return false;
  uint32_t min_blk_ht_conf = in3_node_props_get(np_config, NODE_PROP_MIN_BLOCK_HEIGHT);
  uint32_t min_blk_ht      = in3_node_props_get(np, NODE_PROP_MIN_BLOCK_HEIGHT);
  return (min_blk_ht >= min_blk_ht_conf);
}

#endif

node_weight_t* in3_node_list_fill_weight(in3_t* c, chain_id_t chain_id, in3_node_t* all_nodes, in3_node_weight_t* weights,
                                         int len, _time_t now, float* total_weight, int* total_found,
                                         in3_node_props_t props) {

  int                found      = 0;
  float              weight_sum = 0;
  in3_node_t*        nodeDef    = NULL;
  in3_node_weight_t* weightDef  = NULL;
  node_weight_t*     prev       = NULL;
  node_weight_t*     current    = NULL;
  node_weight_t*     first      = NULL;
  *total_found                  = 0;
  const in3_chain_t* chain      = in3_find_chain(c, chain_id);
  if (!chain) return NULL;

  for (int i = 0; i < len; i++) {
    nodeDef = all_nodes + i;
    if (chain->whitelist && !nodeDef->whitelisted) continue;
    if (nodeDef->deposit < c->min_deposit) continue;

#ifdef FILTER_NODES
    // fixme: this compile time check will be redundant once the registry contract is deployed with correct node prop values
    if (!in3_node_props_match(props, nodeDef->props)) continue;
#else
    UNUSED_VAR(props);
#endif

    weightDef = weights + i;
    if (weightDef->blacklisted_until > (uint64_t) now) continue;
    current = _malloc(sizeof(node_weight_t));
    if (!current) {
      // TODO clean up memory
      return NULL;
    }
    if (!first) first = current;
    current->node   = nodeDef;
    current->weight = weightDef;
    current->next   = NULL;
    current->s      = weight_sum;
    current->w      = weightDef->weight * nodeDef->capacity * (500 / (weightDef->response_count ? (weightDef->total_response_time / weightDef->response_count) : 500));
    weight_sum += current->w;
    found++;
    if (prev) prev->next = current;
    prev = current;
  }
  *total_weight = weight_sum;
  *total_found  = found;
  return first;
}

in3_ret_t in3_node_list_get(in3_ctx_t* ctx, chain_id_t chain_id, bool update, in3_node_t** nodelist, int* nodelist_length, in3_node_weight_t** weights) {
  in3_ret_t    res   = IN3_EFIND;
  in3_chain_t* chain = in3_find_chain(ctx->client, chain_id);

  if (!chain) {
    ctx_set_error(ctx, "invalid chain_id", IN3_EFIND);
    return IN3_EFIND;
  }

  // do we need to update the nodelist?
  if (chain->needs_update || update || ctx_find_required(ctx, "in3_nodeList")) {
    chain->needs_update = false;
    // now update the nodeList
    res = update_nodelist(ctx->client, chain, ctx);
    if (res < 0) return res;
  }

  // do we need to update the whiitelist?
  if (chain->whitelist                                                                         // only if we have a whitelist
      && (chain->whitelist->needs_update || update || ctx_find_required(ctx, "in3_whiteList")) // which has the needs_update-flag (or forced) or we have already sent the request and are now picking up the result
      && !memiszero(chain->whitelist->contract, 20)) {                                         // and we need to have a contract set, zero-contract = manual whitelist, which will not be updated.
    chain->whitelist->needs_update = false;
    // now update the whiteList
    res = update_whitelist(ctx->client, chain, ctx);
    if (res < 0) return res;
  }

  // now update the results
  *nodelist_length = chain->nodelist_length;
  *nodelist        = chain->nodelist;
  *weights         = chain->weights;
  return IN3_OK;
}

in3_ret_t in3_node_list_pick_nodes(in3_ctx_t* ctx, node_weight_t** nodes, int request_count, in3_node_props_t props) {

  // get all nodes from the nodelist
  _time_t            now       = _time();
  in3_node_t*        all_nodes = NULL;
  in3_node_weight_t* weights   = NULL;
  float              total_weight;
  int                all_nodes_len, total_found;

  in3_ret_t res = in3_node_list_get(ctx, ctx->client->chain_id, false, &all_nodes, &all_nodes_len, &weights);
  if (res < 0)
    return ctx_set_error(ctx, "could not find the chain", res);

  // filter out nodes
  node_weight_t* found = in3_node_list_fill_weight(ctx->client, ctx->client->chain_id, all_nodes, weights, all_nodes_len, now, &total_weight, &total_found, props);

  if (total_found == 0) {
    // no node available, so we should check if we can retry some blacklisted
    int blacklisted = 0;
    for (int i = 0; i < all_nodes_len; i++) {
      if (weights[i].blacklisted_until > (uint64_t) now) blacklisted++;
    }

    // if morethan 50% of the nodes are blacklisted, we remove the mark and try again
    if (blacklisted > all_nodes_len / 2) {
      for (int i = 0; i < all_nodes_len; i++)
        weights[i].blacklisted_until = 0;
      found = in3_node_list_fill_weight(ctx->client, ctx->client->chain_id, all_nodes, weights, all_nodes_len, now, &total_weight, &total_found, props);
    }

    if (total_found == 0)
      return ctx_set_error(ctx, "No nodes found that match the criteria", IN3_EFIND);
  }

  int filled_len = total_found < request_count ? total_found : request_count;
  if (total_found == filled_len) {
    *nodes = found;
    return IN3_OK;
  }

  float          r;
  int            added   = 0;
  node_weight_t* last    = NULL;
  node_weight_t* first   = NULL;
  node_weight_t* next    = NULL;
  node_weight_t* current = NULL;

  // we want ot make sure this loop is run only max 10xthe number of requested nodes
  for (int i = 0; added < filled_len && i < filled_len * 10; i++) {
    // pick a random number
    r = total_weight * ((float) (_rand() % 10000)) / 10000.0;

    // find the first node matching it.
    current = found;
    while (current) {
      if (current->s <= r && current->s + current->w >= r) break;
      current = current->next;
    }

    if (current) {
      // check if we already added it,
      next = first;
      while (next) {
        if (next->node == current->node) break;
        next = next->next;
      }

      if (!next) {
        added++;
        next         = _calloc(1, sizeof(node_weight_t));
        next->s      = current->s;
        next->w      = current->w;
        next->weight = current->weight;
        next->node   = current->node;

        if (last) last->next = next;
        if (!first) first = next;
      }
    }
  }

  *nodes = first;
  in3_ctx_free_nodes(found);

  // select them based on random
  return res;
}

/** removes all nodes and their weights from the nodelist */
void in3_nodelist_clear(in3_chain_t* chain) {
  for (int i = 0; i < chain->nodelist_length; i++) {
    if (chain->nodelist[i].url) _free(chain->nodelist[i].url);
    if (chain->nodelist[i].address) b_free(chain->nodelist[i].address);
  }
  _free(chain->nodelist);
  _free(chain->weights);
}

void in3_node_props_set(in3_node_props_t* node_props, in3_node_props_type_t type, uint8_t value) {
  if (type == NODE_PROP_MIN_BLOCK_HEIGHT) {
    const uint64_t dp_ = value & 0xFFU;
    *node_props        = (*node_props & 0xFFFFFFFF) | (dp_ << 32U);
  } else {
    (value != 0) ? ((*node_props) |= type) : ((*node_props) &= ~type);
  }
}
