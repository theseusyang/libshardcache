#ifndef __MESSAGING_H__
#define __MESSAGING_H__

#include <sys/types.h>
#include <iomux.h>
#include <fbuf.h>
#include <rbuf.h>
#include "shardcache.h"

/* For the protocol specification check the 'docs/protocol.txt' file
 * in the libshardcache source distribution
 */

// in bytes
#define SHARDCACHE_MSG_SIG_LEN 8
#define SHARDCACHE_MSG_MAX_RECORD_LEN (1<<28) // 256MB

// last byte holds the protocol version
#define SHC_PROTOCOL_VERSION 1
#define SHC_MAGIC 0x73686301

typedef enum {
    // data commands
    SHC_HDR_GET              = 0x01,
    SHC_HDR_SET              = 0x02,
    SHC_HDR_DELETE           = 0x03,
    SHC_HDR_EVICT            = 0x04,
    SHC_HDR_GET_ASYNC        = 0x05,
    SHC_HDR_GET_OFFSET       = 0x06,
    SHC_HDR_ADD              = 0x07,
    SHC_HDR_EXISTS           = 0x08,
    SHC_HDR_TOUCH            = 0x09,

    // migration commands
    SHC_HDR_MIGRATION_ABORT  = 0x21,
    SHC_HDR_MIGRATION_BEGIN  = 0x22,
    SHC_HDR_MIGRATION_END    = 0x23,

    // administrative commands
    SHC_HDR_CHECK            = 0x31,
    SHC_HDR_STATS            = 0x32,

    // index-related commands
    SHC_HDR_GET_INDEX        = 0x41,
    SHC_HDR_INDEX_RESPONSE   = 0x42,

    // no-op (for ping/health-check)
    SHC_HDR_NOOP             = 0x90,

    // generic response header
    SHC_HDR_RESPONSE         = 0x99,

    SHC_HDR_REPLICA_COMMAND  = 0xA0,
    SHC_HDR_REPLICA_RESPONSE = 0xA1,
    SHC_HDR_REPLICA_PING     = 0xA2,
    SHC_HDR_REPLICA_ACK      = 0xA3,

    // signature headers
    SHC_HDR_SIGNATURE_SIP    = 0xF0,
    SHC_HDR_CSIGNATURE_SIP   = 0xF1

} shardcache_hdr_t;

typedef enum {
    SHC_RES_OK     = 0x00,
    SHC_RES_YES    = 0x01,
    SHC_RES_EXISTS = 0x02,
    SHC_RES_NO     = 0xFE,
    SHC_RES_ERR    = 0xFF
} shardcache_res_t;

typedef struct __shardcache_record_s {
    void *v;
    size_t l;
} shardcache_record_t;

#define SHARDCACHE_RSEP 0x80

// TODO - Document all exposed functions

int global_tcp_timeout(int tcp_timeout);

// synchronously read a message (blocking)
int read_message(int fd,
                 char *auth,
                 fbuf_t **out,
                 int expected_records,
                 shardcache_hdr_t *hdr,
                 int ignore_timeout);

// synchronously write a message (blocking)
int write_message(int fd,
                  char *auth,
                  unsigned char sig_hdr,
                  unsigned char hdr,
                  shardcache_record_t *records,
                  int num_records);

// build a valid shardcache message containing the provided records
int build_message(char *auth,
                  unsigned char sig_hdr,
                  unsigned char hdr,
                  shardcache_record_t *records,
                  int num_records,
                  fbuf_t *out);

// delete a key from a peer
int delete_from_peer(char *peer,
                     char *auth,
                     unsigned char sig_hdr,
                     void *key,
                     size_t klen,
                     int fd,
                     int expect_response);

// evict a key from a peer
int
evict_from_peer(char *peer,
                char *auth,
                unsigned char sig,
                void *key,
                size_t klen,
                int fd,
                int expect_response);


// send a new value for a given key to a peer
int send_to_peer(char *peer,
                 char *auth,
                 unsigned char sig_hdr,
                 void *key,
                 size_t klen,
                 void *value,
                 size_t vlen,
                 uint32_t expire,
                 int fd,
                 int expect_response);


// add a new value (set if not exists) for a given key to a peer 
int
add_to_peer(char *peer,
            char *auth,
            unsigned char sig,
            void *key,
            size_t klen,
            void *value,
            size_t vlen,
            uint32_t expire,
            int fd,
            int expect_response);

// fetch the value for a given key from a peer
int fetch_from_peer(char *peer,
                    char *auth,
                    unsigned char sig_hdr,
                    void *key,
                    size_t len,
                    fbuf_t *out,
                    int fd);

// fetch part of the value for a given key from a peer
int offset_from_peer(char *peer,
                     char *auth,
                     unsigned char sig_hdr,
                     void *key,
                     size_t len,
                     uint32_t offset,
                     uint32_t dlen,
                     fbuf_t *out,
                     int fd);

// check if a key exists on a peer
int exists_on_peer(char *peer,
                   char *auth,
                   unsigned char sig_hdr,
                   void *key,
                   size_t len,
                   int fd,
                   int expect_response);

// touch a key on a peer (loads into cache if responsible and timestamp is updated)
int
touch_on_peer(char *peer,
              char *auth,
              unsigned char sig_hdr,
              void *key,
              size_t klen,
              int fd);

// retrieve all the stats counters from a peer
int stats_from_peer(char *peer,
                    char *auth,
                    unsigned char sig_hdr,
                    char **out,
                    size_t *len,
                    int fd);

// check if a peer is alive (using the CHK command)
int check_peer(char *peer,
               char *auth,
               unsigned char sig_hdr,
               int fd);

// start migration
int migrate_peer(char *peer,
                 char *auth,
                 unsigned char sig_hdr,
                 void *msgdata,
                 size_t len,
                 int fd);

// abort migration
int abort_migrate_peer(char *peer, char *auth, unsigned char sig_hdr, int fd);


// connect to a given peer and return the opened filedescriptor
int connect_to_peer(char *address_string, unsigned int timeout);

// retrieve the index of keys stored in a given peer
// NOTE: caller must use shardcache_free_index() to release memory used
//       by the returned shardcache_storage_index_t pointer
shardcache_storage_index_t *index_from_peer(char *peer,
                                            char *auth,
                                            unsigned char sig_hdr,
                                            int fd);

// idx = -1 , data == NULL, len = 0 when finished
// idx = -2 , data == NULL, len = 0 if an error occurred
typedef int (*async_read_callback_t)(void *data,
                                     size_t len,
                                     int  idx,
                                     void *priv);


typedef struct __async_read_ctx_s async_read_ctx_t;

async_read_ctx_t *async_read_context_create(char *auth,
                                            async_read_callback_t cb,
                                            void *priv);
void async_read_context_destroy(async_read_ctx_t *ctx);

typedef enum {
    SHC_STATE_READING_NONE    = 0x00,
    SHC_STATE_READING_MAGIC   = 0x01,
    SHC_STATE_READING_SIG_HDR = 0x02,
    SHC_STATE_READING_HDR     = 0x03,
    SHC_STATE_READING_RECORD  = 0x04,
    SHC_STATE_READING_RSEP    = 0x05,
    SHC_STATE_READING_AUTH    = 0x06,
    SHC_STATE_READING_DONE    = 0x07,
    SHC_STATE_READING_ERR     = 0x08,
    SHC_STATE_AUTH_ERR        = 0x09
} async_read_context_state_t;

int async_read_context_state(async_read_ctx_t *ctx);
shardcache_hdr_t async_read_context_hdr(async_read_ctx_t *ctx);
shardcache_hdr_t async_read_context_sig_hdr(async_read_ctx_t *ctx);

async_read_context_state_t async_read_context_consume_data(async_read_ctx_t *ctx, rbuf_t *input);
async_read_context_state_t async_read_context_input_data(async_read_ctx_t *ctx, void *data, int len, int *processed);
async_read_context_state_t async_read_context_update(async_read_ctx_t *ctx);

typedef int (*fetch_from_peer_async_cb)(char *peer,
                                        void *key,
                                        size_t klen,
                                        void *data,
                                        size_t len,
                                        int status,
                                        void *priv);

// NOTE: when the async_read_wrk_t param is provided to the following
//       functions accepting it, they will return immediately and 
//       initialize the given pointer so that the caller can
//       use the callbacks and the context to create a new iomux
//       connection and retreive the data asynchronously.
//       If the async_read_wrk_t param is not provided the functions
//       will block until the response has been fully retrieved
#pragma pack(push, 1)
typedef struct {
    async_read_ctx_t *ctx;
    iomux_callbacks_t cbs;
    int fd;
} async_read_wrk_t;
#pragma pack(pop)

int fetch_from_peer_async(char *peer,
                          char *auth,
                          unsigned char sig_hdr,
                          void *key,
                          size_t klen,
                          size_t offset,
                          size_t len,
                          fetch_from_peer_async_cb cb,
                          void *priv,
                          int fd,
                          async_read_wrk_t **async_read_wrk_t);


int read_message_async(int fd,
                   char *auth,
                   async_read_callback_t cb,
                   void *priv,
                   async_read_wrk_t **worker);

#endif

// vim: tabstop=4 shiftwidth=4 expandtab:
/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
