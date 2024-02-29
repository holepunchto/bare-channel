#include <assert.h>
#include <bare.h>
#include <js.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#define BARE_CHANNEL_PORT_CAPACITY 1024

typedef struct bare_channel_s bare_channel_t;
typedef struct bare_channel_port_s bare_channel_port_t;
typedef struct bare_channel_message_s bare_channel_message_t;

enum {
  bare_channel_port_state_inited = 0x1,
  bare_channel_port_state_waiting = 0x2,
  bare_channel_port_state_ended = 0x4,
  bare_channel_port_state_destroying = 0x8,
  bare_channel_port_state_destroyed = 0x16
} bare_channel_port_state_t;

struct bare_channel_message_s {
  enum {
    bare_channel_message_end,
    bare_channel_message_buffer,
    bare_channel_message_arraybuffer,
    bare_channel_message_sharedarraybuffer,
    bare_channel_message_external,
  } type;

  union {
    uv_buf_t buffer;
    js_arraybuffer_backing_store_t *backing_store;
    void *external;
  };
};

struct bare_channel_port_s {
  uint8_t id;

  bare_channel_t *channel;

  uv_sem_t wait;

  atomic_int state;

  bare_channel_message_t messages[BARE_CHANNEL_PORT_CAPACITY];

  struct {
    atomic_int read;
    atomic_int write;
  } cursors;

  struct {
    uv_async_t drain;
    uv_async_t flush;
  } signals;

  int closing;

  js_env_t *env;
  js_ref_t *ctx;
  js_ref_t *on_drain;
  js_ref_t *on_flush;
  js_ref_t *on_end;
  js_ref_t *on_destroy;
};

struct bare_channel_s {
  atomic_int next_port;

  bare_channel_port_t ports[2];
};

static void
on_drain (uv_async_t *handle) {
  int err;

  bare_channel_port_t *port = handle->data;

  js_env_t *env = port->env;

  js_handle_scope_t *scope;
  err = js_open_handle_scope(env, &scope);
  assert(err == 0);

  js_value_t *ctx;
  err = js_get_reference_value(env, port->ctx, &ctx);
  assert(err == 0);

  js_value_t *on_read;
  err = js_get_reference_value(env, port->on_drain, &on_read);
  assert(err == 0);

  js_call_function(env, ctx, on_read, 0, NULL, NULL);

  err = js_close_handle_scope(env, scope);
  assert(err == 0);
}

static void
on_flush (uv_async_t *handle) {
  int err;

  bare_channel_port_t *port = handle->data;

  js_env_t *env = port->env;

  js_handle_scope_t *scope;
  err = js_open_handle_scope(env, &scope);
  assert(err == 0);

  js_value_t *ctx;
  err = js_get_reference_value(env, port->ctx, &ctx);
  assert(err == 0);

  js_value_t *on_write;
  err = js_get_reference_value(env, port->on_flush, &on_write);
  assert(err == 0);

  js_call_function(env, ctx, on_write, 0, NULL, NULL);

  err = js_close_handle_scope(env, scope);
  assert(err == 0);
}

static void
on_close (uv_handle_t *handle) {
  int err;

  bare_channel_port_t *port = handle->data;

  if (--port->closing != 0) return;

  js_env_t *env = port->env;

  js_handle_scope_t *scope;
  err = js_open_handle_scope(env, &scope);
  assert(err == 0);

  js_value_t *ctx;
  err = js_get_reference_value(env, port->ctx, &ctx);
  assert(err == 0);

  js_value_t *on_destroy;
  err = js_get_reference_value(env, port->on_destroy, &on_destroy);
  assert(err == 0);

  err = js_delete_reference(env, port->on_drain);
  assert(err == 0);

  err = js_delete_reference(env, port->on_flush);
  assert(err == 0);

  err = js_delete_reference(env, port->on_end);
  assert(err == 0);

  err = js_delete_reference(env, port->on_destroy);
  assert(err == 0);

  err = js_delete_reference(env, port->ctx);
  assert(err == 0);

  uv_sem_destroy(&port->wait);
  port->state = bare_channel_port_state_destroyed;

  js_call_function(env, ctx, on_destroy, 0, NULL, NULL);

  err = js_close_handle_scope(env, scope);
  assert(err == 0);
}

static js_value_t *
bare_channel_init (js_env_t *env, js_callback_info_t *info) {
  int err;

  bare_channel_t *channel = malloc(sizeof(bare_channel_t));

  channel->next_port = 0;

  for (uint8_t i = 0; i < 2; i++) {
    bare_channel_port_t *port = &channel->ports[i];

    port->id = i;
    port->channel = channel;
    port->state = 0;
    port->cursors.read = 0;
    port->cursors.write = 0;
  }

  js_value_t *result;
  err = js_create_external(env, (void *) channel, NULL, NULL, &result);
  assert(err == 0);

  return result;
}

static js_value_t *
bare_channel_destroy (js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 1;
  js_value_t *argv[1];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  assert(argc == 1);

  bare_channel_t *channel;
  err = js_get_value_external(env, argv[0], (void **) &channel);
  assert(err == 0);

  free(channel);

  return NULL;
}

static js_value_t *
bare_channel_port_init (js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 6;
  js_value_t *argv[6];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  assert(argc == 6);

  bare_channel_t *channel;
  err = js_get_value_external(env, argv[0], (void **) &channel);
  assert(err == 0);

  if (channel->next_port >= 2) {
    js_throw_error(env, NULL, "channel already fully connected");
    return NULL;
  }

  uv_loop_t *loop;
  js_get_env_loop(env, &loop);

  bare_channel_port_t *port = &channel->ports[channel->next_port++];

  uv_sem_init(&port->wait, 1);

  port->env = env;

  err = js_create_reference(env, argv[1], 1, &port->ctx);
  assert(err == 0);

  err = js_create_reference(env, argv[2], 1, &port->on_drain);
  assert(err == 0);

  err = js_create_reference(env, argv[3], 1, &port->on_flush);
  assert(err == 0);

  err = js_create_reference(env, argv[4], 1, &port->on_end);
  assert(err == 0);

  err = js_create_reference(env, argv[5], 1, &port->on_destroy);
  assert(err == 0);

  err = uv_async_init(loop, &port->signals.drain, on_drain);
  assert(err == 0);

  port->signals.drain.data = (void *) port;

  err = uv_async_init(loop, &port->signals.flush, on_flush);
  assert(err == 0);

  port->signals.flush.data = (void *) port;

  port->state |= bare_channel_port_state_inited;

  uv_async_send(&port->signals.flush);

  js_value_t *result;
  err = js_create_external(env, (void *) port, NULL, NULL, &result);
  assert(err == 0);

  return result;

  return NULL;
}

static js_value_t *
bare_channel_port_destroy (js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 1;
  js_value_t *argv[1];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  assert(argc == 1);

  bare_channel_port_t *port;
  err = js_get_value_external(env, argv[0], (void **) &port);
  assert(err == 0);

  port->state |= bare_channel_port_state_destroying;

  port->closing = 2;

  uv_close((uv_handle_t *) &port->signals.drain, on_close);

  uv_close((uv_handle_t *) &port->signals.flush, on_close);

  return NULL;
}

static js_value_t *
bare_channel_port_wait (js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 1;
  js_value_t *argv[1];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  assert(argc == 1);

  bare_channel_port_t *port;
  err = js_get_value_external(env, argv[0], (void **) &port);
  assert(err == 0);

  while (port->cursors.read == port->cursors.write) {
    port->state |= bare_channel_port_state_waiting;
    uv_sem_wait(&port->wait);
    port->state &= ~bare_channel_port_state_waiting;
  }

  return NULL;
}

static js_value_t *
bare_channel_port_read (js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 1;
  js_value_t *argv[1];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  assert(argc == 1);

  bare_channel_port_t *port;
  err = js_get_value_external(env, argv[0], (void **) &port);
  assert(err == 0);

  bare_channel_port_t *sender = &port->channel->ports[(port->id + 1) & 1];

  js_value_t *result;

  if (port->cursors.read == port->cursors.write) {
    err = js_get_null(env, &result);
    assert(err == 0);
  } else {
    bare_channel_message_t *message = &port->messages[port->cursors.read];

    switch (message->type) {
    case bare_channel_message_end: {
      js_value_t *ctx;
      err = js_get_reference_value(env, port->ctx, &ctx);
      assert(err == 0);

      js_value_t *on_end;
      err = js_get_reference_value(env, port->on_end, &on_end);
      assert(err == 0);

      js_call_function(env, ctx, on_end, 0, NULL, NULL);

      err = js_get_null(env, &result);
      assert(err == 0);
      break;
    }

    case bare_channel_message_buffer:
    default: {
      js_value_t *arraybuffer;

      void *data;
      err = js_create_arraybuffer(env, message->buffer.len, &data, &arraybuffer);
      assert(err == 0);

      memcpy(data, message->buffer.base, message->buffer.len);

      free(message->buffer.base);

      err = js_create_typedarray(env, js_uint8_array, message->buffer.len, arraybuffer, 0, &result);
      assert(err == 0);
      break;
    }

    case bare_channel_message_arraybuffer: {
      void *data;
      err = js_create_arraybuffer(env, message->buffer.len, &data, &result);
      assert(err == 0);

      memcpy(data, message->buffer.base, message->buffer.len);

      free(message->buffer.base);
      break;
    }

    case bare_channel_message_sharedarraybuffer: {
      err = js_create_sharedarraybuffer_with_backing_store(env, message->backing_store, NULL, NULL, &result);
      assert(err == 0);
      break;
    }

    case bare_channel_message_external: {
      err = js_create_external(env, message->external, NULL, NULL, &result);
      assert(err == 0);
      break;
    }
    }

    port->cursors.read = (port->cursors.read + 1) % BARE_CHANNEL_PORT_CAPACITY;

    if (sender->state & bare_channel_port_state_inited) {
      uv_async_send(&sender->signals.drain);
    }
  }

  return result;
}

static js_value_t *
bare_channel_port_write (js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 2;
  js_value_t *argv[2];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  assert(argc == 2);

  bare_channel_port_t *port;
  err = js_get_value_external(env, argv[0], (void **) &port);
  assert(err == 0);

  bare_channel_port_t *receiver = &port->channel->ports[(port->id + 1) & 1];

  bool success = true;

  int next = (receiver->cursors.write + 1) % BARE_CHANNEL_PORT_CAPACITY;

  if (next == receiver->cursors.read) success = false;
  else {
    bare_channel_message_t *message = &receiver->messages[receiver->cursors.write];

    bool is_type;

    err = js_is_typedarray(env, argv[1], &is_type);
    assert(err == 0);

    if (is_type) {
      message->type = bare_channel_message_buffer;
    } else {
      err = js_is_arraybuffer(env, argv[1], &is_type);
      assert(err == 0);

      if (is_type) {
        message->type = bare_channel_message_arraybuffer;
      } else {
        err = js_is_sharedarraybuffer(env, argv[1], &is_type);
        assert(err == 0);

        if (is_type) {
          message->type = bare_channel_message_sharedarraybuffer;
        } else {
          err = js_is_external(env, argv[1], &is_type);
          assert(err == 0);

          if (is_type) {
            message->type = bare_channel_message_external;
          } else {
            js_throw_error(env, NULL, "supported value");
            return NULL;
          }
        }
      }
    }

    switch (message->type) {
    case bare_channel_message_buffer:
    default: {
      void *data;
      err = js_get_typedarray_info(env, argv[1], NULL, &data, &message->buffer.len, NULL, NULL);
      assert(err == 0);

      message->buffer.base = malloc(message->buffer.len);

      memcpy(message->buffer.base, data, message->buffer.len);
      break;
    }

    case bare_channel_message_arraybuffer: {

      void *data;
      err = js_get_arraybuffer_info(env, argv[1], &data, &message->buffer.len);
      assert(err == 0);

      message->buffer.base = malloc(message->buffer.len);

      memcpy(message->buffer.base, data, message->buffer.len);
      break;
    }

    case bare_channel_message_sharedarraybuffer: {
      err = js_get_sharedarraybuffer_backing_store(env, argv[1], &message->backing_store);
      assert(err == 0);
      break;
    }

    case bare_channel_message_external: {
      err = js_get_value_external(env, argv[1], &message->external);
      assert(err == 0);
      break;
    }
    }

    receiver->cursors.write = next;

    if (receiver->state & bare_channel_port_state_inited) {
      if (receiver->state & bare_channel_port_state_waiting) {
        uv_sem_post(&receiver->wait);
      } else {
        uv_async_send(&receiver->signals.flush);
      }
    }
  }

  js_value_t *result;
  err = js_get_boolean(env, success, &result);
  assert(err == 0);

  return result;
}

static js_value_t *
bare_channel_port_end (js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 1;
  js_value_t *argv[1];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  assert(argc == 1);

  bare_channel_port_t *port;
  err = js_get_value_external(env, argv[0], (void **) &port);
  assert(err == 0);

  bare_channel_port_t *receiver = &port->channel->ports[(port->id + 1) & 1];

  bool success = true;

  int next = (receiver->cursors.write + 1) % BARE_CHANNEL_PORT_CAPACITY;

  if (next == receiver->cursors.read) success = false;
  else {
    bare_channel_message_t *message = &receiver->messages[receiver->cursors.write];

    message->type = bare_channel_message_end;

    receiver->cursors.write = next;

    if (receiver->state & bare_channel_port_state_inited) {
      uv_async_send(&receiver->signals.flush);
    }
  }

  js_value_t *result;
  err = js_get_boolean(env, success, &result);
  assert(err == 0);

  return result;
}

static js_value_t *
init (js_env_t *env, js_value_t *exports) {
#define V(name, fn) \
  { \
    js_value_t *val; \
    js_create_function(env, name, -1, fn, NULL, &val); \
    js_set_named_property(env, exports, name, val); \
  }
  V("channelInit", bare_channel_init)
  V("channelDestroy", bare_channel_destroy)

  V("portInit", bare_channel_port_init)
  V("portDestroy", bare_channel_port_destroy)
  V("portWait", bare_channel_port_wait)
  V("portRead", bare_channel_port_read)
  V("portWrite", bare_channel_port_write)
  V("portEnd", bare_channel_port_end)
#undef V

  return exports;
}

BARE_MODULE(bare_channel, init)
