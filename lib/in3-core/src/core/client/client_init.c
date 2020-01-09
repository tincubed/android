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

#include "../util/data.h"
#include "../util/log.h"
#include "../util/mem.h"
#include "cache.h"
#include "client.h"
#include "nodelist.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// set the defaults
static in3_transport_send     default_transport = NULL;
static in3_storage_handler_t* default_storage   = NULL;
static in3_signer_t*          default_signer    = NULL;

/**
 * defines a default transport which is used when creating a new client.
 */
void in3_set_default_transport(in3_transport_send transport) {
  default_transport = transport;
}

/**
 * defines a default storage handler which is used when creating a new client.
 */
void in3_set_default_storage(in3_storage_handler_t* cacheStorage) {
  default_storage = cacheStorage;
}
/**
 * defines a default signer which is used when creating a new client.
 */
void in3_set_default_signer(in3_signer_t* signer) {
  default_signer = signer;
}

static void whitelist_free(in3_whitelist_t* wl) {
  if (!wl) return;
  if (wl->addresses.data) _free(wl->addresses.data);
  _free(wl);
}

void initChain(in3_chain_t* chain, chain_id_t chain_id, char* contract, char* registry_id, uint8_t version, int boot_node_count, in3_chain_type_t type, char* wl_contract) {
  chain->chain_id        = chain_id;
  chain->init_addresses  = NULL;
  chain->last_block      = 0;
  chain->contract        = hex_to_new_bytes(contract, 40);
  chain->needs_update    = chain_id == ETH_CHAIN_ID_LOCAL ? 0 : 1;
  chain->nodelist        = _malloc(sizeof(in3_node_t) * boot_node_count);
  chain->nodelist_length = boot_node_count;
  chain->weights         = _malloc(sizeof(in3_node_weight_t) * boot_node_count);
  chain->type            = type;
  chain->version         = version;
  chain->whitelist       = NULL;
  if (wl_contract) {
    chain->whitelist                 = _malloc(sizeof(in3_whitelist_t));
    chain->whitelist->addresses.data = NULL;
    chain->whitelist->addresses.len  = 0;
    chain->whitelist->needs_update   = true;
    chain->whitelist->last_block     = 0;
    hex_to_bytes(wl_contract, -1, chain->whitelist->contract, 20);
  }
  memset(chain->registry_id, 0, 32);
  if (version > 1) {
    int l = hex_to_bytes(registry_id, -1, chain->registry_id, 32);
    if (l < 32) {
      memmove(chain->registry_id + 32 - l, chain->registry_id, l);
      memset(chain->registry_id, 0, 32 - l);
    }
  }
}

static void initNode(in3_chain_t* chain, int node_index, char* address, char* url) {
  in3_node_t* node = chain->nodelist + node_index;
  node->address    = hex_to_new_bytes(address, 40);
  node->index      = node_index;
  node->capacity   = 1;
  node->deposit    = 0;
  node->props      = chain->chain_id == ETH_CHAIN_ID_LOCAL ? 0x0 : 0xFF;
  node->url        = _malloc(strlen(url) + 1);
  memcpy(node->url, url, strlen(url) + 1);
  node->whitelisted = false;

  in3_node_weight_t* weight   = chain->weights + node_index;
  weight->blacklisted_until   = 0;
  weight->response_count      = 0;
  weight->total_response_time = 0;
  weight->weight              = 1;
}

static void init_ipfs(in3_chain_t* chain) {
  // ipfs
  initChain(chain, 0x7d0, "f0fb87f4757c77ea3416afe87f36acaa0496c7e9", NULL, 1, 2, CHAIN_IPFS, NULL);
  initNode(chain, 0, "784bfa9eb182c3a02dbeb5285e3dba92d717e07a", "https://in3.slock.it/ipfs/nd-1");
  initNode(chain, 1, "243D5BB48A47bEd0F6A89B61E4660540E856A33D", "https://in3.slock.it/ipfs/nd-5");
}

static void init_mainnet(in3_chain_t* chain) {
  initChain(chain, 0x01, "ac1b824795e1eb1f6e609fe0da9b9af8beaab60f", "23d5345c5c13180a8080bd5ddbe7cde64683755dcce6e734d95b7b573845facb", 2, 2, CHAIN_ETH, NULL);
  initNode(chain, 0, "45d45e6ff99e6c34a235d263965910298985fcfe", "https://in3-v2.slock.it/mainnet/nd-1");
  initNode(chain, 1, "1fe2e9bf29aa1938859af64c413361227d04059a", "https://in3-v2.slock.it/mainnet/nd-2");
}

static void init_kovan(in3_chain_t* chain) {
#ifdef IN3_STAGING
  // kovan
  initChain(chain, 0x2a, "0604014f2a5fdfafce3f2ec10c77c31d8e15ce6f", "d440f01322c8529892c204d3705ae871c514bafbb2f35907832a07322e0dc868", 2, 2, CHAIN_ETH, NULL);
  initNode(chain, 0, "784bfa9eb182c3a02dbeb5285e3dba92d717e07a", "https://in3.stage.slock.it/kovan/nd-1");
  initNode(chain, 1, "17cdf9ec6dcae05c5686265638647e54b14b41a2", "https://in3.stage.slock.it/kovan/nd-2");
#else
  // kovan
  initChain(chain, 0x2a, "4c396dcf50ac396e5fdea18163251699b5fcca25", "92eb6ad5ed9068a24c1c85276cd7eb11eda1e8c50b17fbaffaf3e8396df4becf", 2, 2, CHAIN_ETH, NULL);
  initNode(chain, 0, "45d45e6ff99e6c34a235d263965910298985fcfe", "https://in3-v2.slock.it/kovan/nd-1");
  initNode(chain, 1, "1fe2e9bf29aa1938859af64c413361227d04059a", "https://in3-v2.slock.it/kovan/nd-2");
#endif
}

static void init_goerli(in3_chain_t* chain) {

#ifdef IN3_STAGING
  // goerli
  initChain(chain, 0x05, "814fb2203f9848192307092337340dcf791a3fed", "0f687341e0823fa5288dc9edd8a00950b35cc7e481ad7eaccaf61e4e04a61e08", 2, 2, CHAIN_ETH, NULL);
  initNode(chain, 0, "45d45e6ff99e6c34a235d263965910298985fcfe", "https://in3.stage.slock.it/goerli/nd-1");
  initNode(chain, 1, "1fe2e9bf29aa1938859af64c413361227d04059a", "https://in3.stage.slock.it/goerli/nd-2");
#else
  // goerli
  initChain(chain, 0x05, "5f51e413581dd76759e9eed51e63d14c8d1379c8", "67c02e5e272f9d6b4a33716614061dd298283f86351079ef903bf0d4410a44ea", 2, 2, CHAIN_ETH, NULL);
  initNode(chain, 0, "45d45e6ff99e6c34a235d263965910298985fcfe", "https://in3-v2.slock.it/goerli/nd-1");
  initNode(chain, 1, "1fe2e9bf29aa1938859af64c413361227d04059a", "https://in3-v2.slock.it/goerli/nd-2");
#endif
}

static in3_ret_t in3_client_init(in3_t* c, chain_id_t chain_id) {
  c->auto_update_list     = 1;
  c->cache                = NULL;
  c->signer               = NULL;
  c->cache_timeout        = 0;
  c->use_binary           = 0;
  c->use_http             = 0;
  c->include_code         = 0;
  c->chain_id             = chain_id ? chain_id : ETH_CHAIN_ID_MAINNET; // mainnet
  c->key                  = NULL;
  c->finality             = 0;
  c->max_attempts         = 3;
  c->max_block_cache      = 0;
  c->max_code_cache       = 0;
  c->min_deposit          = 0;
  c->node_limit           = 0;
  c->proof                = PROOF_STANDARD;
  c->replace_latest_block = 0;
  c->request_count        = 1;
  c->chains_length        = chain_id ? 1 : 5;
  c->chains               = _malloc(sizeof(in3_chain_t) * c->chains_length);
  c->filters              = NULL;

  //TODO check for failed malloc!

  in3_chain_t* chain = c->chains;

  if (!chain_id || chain_id == ETH_CHAIN_ID_MAINNET)
    init_mainnet(chain++);

  if (!chain_id || chain_id == ETH_CHAIN_ID_KOVAN)
    init_kovan(chain++);

  if (!chain_id || chain_id == ETH_CHAIN_ID_GOERLI)
    init_goerli(chain++);

  if (!chain_id || chain_id == ETH_CHAIN_ID_IPFS)
    init_ipfs(chain++);

  if (!chain_id || chain_id == ETH_CHAIN_ID_LOCAL) {
    initChain(chain, 0xFFFF, "f0fb87f4757c77ea3416afe87f36acaa0496c7e9", NULL, 1, 1, CHAIN_ETH, NULL);
    initNode(chain++, 0, "784bfa9eb182c3a02dbeb5285e3dba92d717e07a", "http://localhost:8545");
  }
  if (chain_id && chain == c->chains) {
    c->chains_length = 0;
    return IN3_ECONFIG;
  }
  return IN3_OK;
}

in3_chain_t* in3_find_chain(in3_t* c, chain_id_t chain_id) {
  // shortcut for single chain
  if (c->chains_length == 1)
    return c->chains->chain_id == chain_id ? c->chains : NULL;

  // search for multi chain
  for (int i = 0; i < c->chains_length; i++) {
    if (c->chains[i].chain_id == chain_id) return &c->chains[i];
  }
  return NULL;
}

in3_ret_t in3_client_register_chain(in3_t* c, chain_id_t chain_id, in3_chain_type_t type, address_t contract, bytes32_t registry_id, uint8_t version, address_t wl_contract) {
  in3_chain_t* chain = in3_find_chain(c, chain_id);
  if (!chain) {
    c->chains = _realloc(c->chains, sizeof(in3_chain_t) * (c->chains_length + 1), sizeof(in3_chain_t) * c->chains_length);
    if (c->chains == NULL) return IN3_ENOMEM;
    chain                  = c->chains + c->chains_length;
    chain->nodelist        = NULL;
    chain->nodelist_length = 0;
    chain->weights         = NULL;
    chain->init_addresses  = NULL;
    chain->whitelist       = NULL;
    chain->last_block      = 0;
    c->chains_length++;

  } else {
    if (chain->contract)
      b_free(chain->contract);
    if (chain->whitelist)
      whitelist_free(chain->whitelist);
  }

  chain->chain_id     = chain_id;
  chain->contract     = b_new((char*) contract, 20);
  chain->needs_update = false;
  chain->type         = type;
  chain->version      = version;
  chain->whitelist    = NULL;
  memcpy(chain->registry_id, registry_id, 32);
  if (wl_contract) {
    chain->whitelist                 = _malloc(sizeof(in3_whitelist_t));
    chain->whitelist->addresses.data = NULL;
    chain->whitelist->addresses.len  = 0;
    chain->whitelist->needs_update   = true;
    chain->whitelist->last_block     = 0;
    memcpy(chain->whitelist->contract, wl_contract, 20);
  }

  return chain->contract ? IN3_OK : IN3_ENOMEM;
}

in3_ret_t in3_client_add_node(in3_t* c, chain_id_t chain_id, char* url, in3_node_props_t props, address_t address) {
  in3_chain_t* chain = in3_find_chain(c, chain_id);
  if (!chain) return IN3_EFIND;
  in3_node_t* node       = NULL;
  int         node_index = chain->nodelist_length;
  for (int i = 0; i < chain->nodelist_length; i++) {
    if (memcmp(chain->nodelist[i].address->data, address, 20) == 0) {
      node       = chain->nodelist + i;
      node_index = i;
      break;
    }
  }
  if (!node) {
    chain->nodelist = chain->nodelist
                          ? _realloc(chain->nodelist, sizeof(in3_node_t) * (chain->nodelist_length + 1), sizeof(in3_node_t) * chain->nodelist_length)
                          : _calloc(chain->nodelist_length + 1, sizeof(in3_node_t));
    chain->weights = chain->weights
                         ? _realloc(chain->weights, sizeof(in3_node_weight_t) * (chain->nodelist_length + 1), sizeof(in3_node_weight_t) * chain->nodelist_length)
                         : _calloc(chain->nodelist_length + 1, sizeof(in3_node_weight_t));
    if (!chain->nodelist || !chain->weights) return IN3_ENOMEM;
    node           = chain->nodelist + chain->nodelist_length;
    node->address  = b_new((char*) address, 20);
    node->index    = chain->nodelist_length;
    node->capacity = 1;
    node->deposit  = 0;
    chain->nodelist_length++;
    node->whitelisted = false;
  } else
    _free(node->url);

  node->props = props;
  node->url   = _malloc(strlen(url) + 1);
  memcpy(node->url, url, strlen(url) + 1);

  in3_node_weight_t* weight   = chain->weights + node_index;
  weight->blacklisted_until   = 0;
  weight->response_count      = 0;
  weight->total_response_time = 0;
  weight->weight              = 1;
  return IN3_OK;
}
in3_ret_t in3_client_remove_node(in3_t* c, chain_id_t chain_id, address_t address) {
  in3_chain_t* chain = in3_find_chain(c, chain_id);
  if (!chain) return IN3_EFIND;
  int node_index = -1;
  for (int i = 0; i < chain->nodelist_length; i++) {
    if (memcmp(chain->nodelist[i].address->data, address, 20) == 0) {
      node_index = i;
      break;
    }
  }
  if (node_index == -1) return IN3_EFIND;
  if (chain->nodelist[node_index].url)
    _free(chain->nodelist[node_index].url);
  if (chain->nodelist[node_index].address)
    b_free(chain->nodelist[node_index].address);

  if (node_index < chain->nodelist_length - 1) {
    memmove(chain->nodelist + node_index, chain->nodelist + node_index + 1, sizeof(in3_node_t) * (chain->nodelist_length - 1 - node_index));
    memmove(chain->weights + node_index, chain->weights + node_index + 1, sizeof(in3_node_weight_t) * (chain->nodelist_length - 1 - node_index));
  }
  chain->nodelist_length--;
  if (!chain->nodelist_length) {
    _free(chain->nodelist);
    _free(chain->weights);
    chain->nodelist = NULL;
    chain->weights  = NULL;
  }
  return IN3_OK;
}
in3_ret_t in3_client_clear_nodes(in3_t* c, chain_id_t chain_id) {
  in3_chain_t* chain = in3_find_chain(c, chain_id);
  if (!chain) return IN3_EFIND;
  in3_nodelist_clear(chain);
  chain->nodelist        = NULL;
  chain->weights         = NULL;
  chain->nodelist_length = 0;
  return IN3_OK;
}

/* frees the data */
void in3_free(in3_t* a) {
  int i;
  for (i = 0; i < a->chains_length; i++) {
    in3_nodelist_clear(a->chains + i);
    b_free(a->chains[i].contract);
    whitelist_free(a->chains[i].whitelist);
  }
  if (a->signer) _free(a->signer);
  _free(a->chains);

  if (a->filters != NULL) {
    in3_filter_t* f = NULL;
    for (size_t j = 0; j < a->filters->count; j++) {
      f = a->filters->array[j];
      if (f) f->release(f);
    }
    _free(a->filters->array);
    _free(a->filters);
  }
  _free(a);
}

in3_t* in3_for_chain(chain_id_t chain_id) {

  // initialize random with the timestamp as seed
  _srand(_time());

  // create new client
  in3_t* c = _calloc(1, sizeof(in3_t));
  if (in3_client_init(c, chain_id) != IN3_OK) {
    in3_free(c);
    return NULL;
  }

  if (default_transport) c->transport = default_transport;
  if (default_storage) c->cache = default_storage;
  if (default_signer) c->signer = default_signer;

#ifndef TEST
  in3_log_set_quiet(1);
#endif
  return c;
}

in3_t* in3_new() {
  return in3_for_chain(0);
}

static chain_id_t chain_id(d_token_t* t) {
  if (d_type(t) == T_STRING) {
    char* c = d_string(t);
    if (!strcmp(c, "mainnet")) return 1;
    if (!strcmp(c, "kovan")) return 0x2a;
    if (!strcmp(c, "goerli")) return 0x5;
    return 1;
  }
  return d_long(t);
}

in3_ret_t in3_configure(in3_t* c, char* config) {
  d_track_keynames(1);
  d_clear_keynames();
  json_ctx_t* cnf = parse_json(config);
  d_track_keynames(0);
  in3_ret_t res = IN3_OK;

  if (!cnf || !cnf->result) return IN3_EINVAL;
  for (d_iterator_t iter = d_iter(cnf->result); iter.left; d_iter_next(&iter)) {
    if (iter.token->key == key("autoUpdateList"))
      c->auto_update_list = d_int(iter.token) ? true : false;
    else if (iter.token->key == key("chainId"))
      c->chain_id = chain_id(iter.token);
    else if (iter.token->key == key("signatureCount"))
      c->signature_count = (uint8_t) d_int(iter.token);
    else if (iter.token->key == key("finality"))
      c->finality = (uint_fast16_t) d_int(iter.token);
    else if (iter.token->key == key("includeCode"))
      c->include_code = d_int(iter.token) ? true : false;
    else if (iter.token->key == key("maxAttempts"))
      c->max_attempts = d_int(iter.token);
    else if (iter.token->key == key("keepIn3"))
      c->keep_in3 = d_int(iter.token);
    else if (iter.token->key == key("maxBlockCache"))
      c->max_block_cache = d_int(iter.token);
    else if (iter.token->key == key("maxCodeCache"))
      c->max_code_cache = d_int(iter.token);
    else if (iter.token->key == key("minDeposit"))
      c->min_deposit = d_long(iter.token);
    else if (iter.token->key == key("nodeLimit"))
      c->node_limit = (uint16_t) d_int(iter.token);
    else if (iter.token->key == key("proof"))
      c->proof = strcmp(d_string(iter.token), "full") == 0
                     ? PROOF_FULL
                     : (strcmp(d_string(iter.token), "standard") == 0 ? PROOF_STANDARD : PROOF_NONE);
    else if (iter.token->key == key("replaceLatestBlock"))
      c->replace_latest_block = (uint16_t) d_int(iter.token);
    else if (iter.token->key == key("requestCount"))
      c->request_count = (uint8_t) d_int(iter.token);
    else if (iter.token->key == key("rpc")) {
      c->proof         = PROOF_NONE;
      c->chain_id      = ETH_CHAIN_ID_LOCAL;
      c->request_count = 1;
      in3_node_t* n    = &in3_find_chain(c, c->chain_id)->nodelist[0];
      if (n->url) _free(n->url);
      n->url = malloc(d_len(iter.token) + 1);
      if (!n->url) {
        res = IN3_ENOMEM;
        goto cleanup;
      }
      strcpy(n->url, d_string(iter.token));
    } else if (iter.token->key == key("servers") || iter.token->key == key("nodes"))
      for (d_iterator_t ct = d_iter(iter.token); ct.left; d_iter_next(&ct)) {
        // register chain
        chain_id_t   chain_id = char_to_long(d_get_keystr(ct.token->key), -1);
        in3_chain_t* chain    = in3_find_chain(c, chain_id);
        if (!chain) {
          bytes_t* contract    = d_get_byteskl(ct.token, key("contract"), 20);
          bytes_t* registry_id = d_get_byteskl(ct.token, key("registryId"), 32);
          bytes_t* wl_contract = d_get_byteskl(ct.token, key("whiteListContract"), 20);
          if (!contract || !registry_id) {
            res = IN3_EINVAL;
            goto cleanup;
          }
          if ((res = in3_client_register_chain(c, chain_id, CHAIN_ETH, contract->data, registry_id->data, 2, wl_contract ? wl_contract->data : NULL)) != IN3_OK) goto cleanup;
          chain = in3_find_chain(c, chain_id);
          assert(chain != NULL);
        }

        // chain_props
        for (d_iterator_t cp = d_iter(ct.token); cp.left; d_iter_next(&cp)) {
          if (cp.token->key == key("contract"))
            memcpy(chain->contract->data, cp.token->data, cp.token->len);
          else if (cp.token->key == key("whiteListContract")) {
            if (d_type(cp.token) != T_BYTES || d_len(cp.token) != 20) {
              res = IN3_EINVAL;
              goto cleanup;
            }

            if (!chain->whitelist) {
              chain->whitelist               = _calloc(1, sizeof(in3_whitelist_t));
              chain->whitelist->needs_update = true;
              memcpy(chain->whitelist->contract, cp.token->data, 20);
            } else if (memcmp(chain->whitelist->contract, cp.token->data, 20)) {
              memcpy(chain->whitelist->contract, cp.token->data, 20);
              chain->whitelist->needs_update = true;
            }
          } else if (cp.token->key == key("whiteList")) {
            if (d_type(cp.token) != T_ARRAY) {
              res = IN3_EINVAL;
              goto cleanup;
            }
            int len = d_len(cp.token), i = 0;
            whitelist_free(chain->whitelist);
            chain->whitelist            = _calloc(1, sizeof(in3_whitelist_t));
            chain->whitelist->addresses = bytes(_malloc(len * 20), len * 20);
            for (d_iterator_t n = d_iter(cp.token); n.left; d_iter_next(&n), i += 20)
              d_bytes_to(n.token, chain->whitelist->addresses.data + i, 20);
          } else if (cp.token->key == key("registryId")) {
            bytes_t data = d_to_bytes(cp.token);
            if (data.len != 32 || !data.data) {
              res = IN3_EINVAL;
              goto cleanup;
            } else
              memcpy(chain->registry_id, data.data, 32);
          } else if (cp.token->key == key("needsUpdate"))
            chain->needs_update = d_int(cp.token) ? true : false;
          else if (cp.token->key == key("nodeList")) {
            if (in3_client_clear_nodes(c, chain_id) < 0) goto cleanup;
            for (d_iterator_t n = d_iter(cp.token); n.left; d_iter_next(&n)) {
              if ((res = in3_client_add_node(c, chain_id, d_get_string(n.token, "url"),
                                             d_get_longkd(n.token, key("props"), 65535),
                                             d_get_byteskl(n.token, key("address"), 20)->data)) != IN3_OK) goto cleanup;
            }
          }
        }
        in3_client_run_chain_whitelisting(chain);
      }
  }

cleanup:
  json_free(cnf);
  return res;
}
