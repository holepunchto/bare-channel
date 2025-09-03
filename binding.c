#include <assert.h>
#include <bare.h>
#include <js.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
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
  bare_channel_port_state_destroyed = 0x10,
  bare_channel_port_state_exiting = 0x20
} bare_channel_port_state_t;

struct bare_channel_message_s {
  enum {
    bare_channel_message_end,
    bare_channel_message_data,
  } type;

  union {
    js_arraybuffer_backing_store_t *backing_store;
  };
};

struct bare_channel_port_s {
  bare_channel_t *channel;

  uint8_t id;

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
  js_ref_t *on_close;

  js_deferred_teardown_t *teardown;
};

struct bare_channel_s {
  atomic_int next_port;

  bare_channel_port_t ports[2];
};

static void
bare_channel__on_drain(uv_async_t *handle) {
  int err;

  bare_channel_port_t *port = handle->data;

  if (port->state & bare_channel_port_state_exiting) return;

  js_env_t *env = port->env;

  js_handle_scope_t *scope;
  err = js_open_handle_scope(env, &scope);
  assert(err == 0);

  js_value_t *ctx;
  err = js_get_reference_value(env, port->ctx, &ctx);
  assert(err == 0);

  js_value_t *on_drain;
  err = js_get_reference_value(env, port->on_drain, &on_drain);
  assert(err == 0);

  js_call_function(env, ctx, on_drain, 0, NULL, NULL);

  err = js_close_handle_scope(env, scope);
  assert(err == 0);
}

static void
bare_channel__on_flush(uv_async_t *handle) {
  int err;

  bare_channel_port_t *port = handle->data;

  if (port->state & bare_channel_port_state_exiting) return;

  js_env_t *env = port->env;

  js_handle_scope_t *scope;
  err = js_open_handle_scope(env, &scope);
  assert(err == 0);

  js_value_t *ctx;
  err = js_get_reference_value(env, port->ctx, &ctx);
  assert(err == 0);

  js_value_t *on_flush;
  err = js_get_reference_value(env, port->on_flush, &on_flush);
  assert(err == 0);

  js_call_function(env, ctx, on_flush, 0, NULL, NULL);

  err = js_close_handle_scope(env, scope);
  assert(err == 0);
}

static void
bare_channel__on_close(uv_handle_t *handle) {
  int err;

  bare_channel_port_t *port = handle->data;

  if (--port->closing != 0) return;

  js_deferred_teardown_t *teardown = port->teardown;

  port->state |= bare_channel_port_state_destroyed;

  js_env_t *env = port->env;

  js_handle_scope_t *scope;
  err = js_open_handle_scope(env, &scope);
  assert(err == 0);

  js_value_t *ctx;
  err = js_get_reference_value(env, port->ctx, &ctx);
  assert(err == 0);

  js_value_t *on_destroy;
  err = js_get_reference_value(env, port->on_close, &on_destroy);
  assert(err == 0);

  err = js_delete_reference(env, port->on_drain);
  assert(err == 0);

  err = js_delete_reference(env, port->on_flush);
  assert(err == 0);

  err = js_delete_reference(env, port->on_end);
  assert(err == 0);

  err = js_delete_reference(env, port->on_close);
  assert(err == 0);

  err = js_delete_reference(env, port->ctx);
  assert(err == 0);

  uv_sem_destroy(&port->wait);

  if ((port->state & bare_channel_port_state_exiting) == 0) {
    js_call_function(env, ctx, on_destroy, 0, NULL, NULL);
  }

  err = js_close_handle_scope(env, scope);
  assert(err == 0);

  err = js_finish_deferred_teardown_callback(teardown);
  assert(err == 0);
}

static void
bare_channel__on_teardown(js_deferred_teardown_t *handle, void *data) {
  bare_channel_port_t *port = data;

  port->state |= bare_channel_port_state_exiting;

  if (port->state & bare_channel_port_state_destroying) return;

  port->state |= bare_channel_port_state_destroying;
  port->closing = 2;

  bare_channel_port_t *receiver = &port->channel->ports[(port->id + 1) & 1];

  if (receiver->state & bare_channel_port_state_waiting) {
    uv_sem_post(&receiver->wait);
  }

#define V(signal) \
  uv_close((uv_handle_t *) &port->signals.signal, bare_channel__on_close);
  V(drain)
  V(flush)
#undef V
}

static js_value_t *
bare_channel_init(js_env_t *env, js_callback_info_t *info) {
  int err;

  js_value_t *handle;

  bare_channel_t *channel;
  err = js_create_sharedarraybuffer(env, sizeof(bare_channel_t), (void **) &channel, &handle);
  assert(err == 0);

  channel->next_port = 0;

  for (uint8_t id = 0; id < 2; id++) {
    bare_channel_port_t *port = &channel->ports[id];

    port->id = id;
    port->channel = channel;
    port->state = 0;
    port->cursors.read = 0;
    port->cursors.write = 0;
  }

  return handle;
}

static js_value_t *
bare_channel_port_init(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 6;
  js_value_t *argv[6];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  assert(argc == 6);

  bare_channel_t *channel;
  err = js_get_sharedarraybuffer_info(env, argv[0], (void **) &channel, NULL);
  assert(err == 0);

  if (channel->next_port >= 2) {
    err = js_throw_error(env, NULL, "channel already fully connected");
    assert(err == 0);

    return NULL;
  }

  uv_loop_t *loop;
  err = js_get_env_loop(env, &loop);
  assert(err == 0);

  int id = channel->next_port++;

  bare_channel_port_t *port = &channel->ports[id];

  port->env = env;

  err = js_create_reference(env, argv[1], 1, &port->ctx);
  assert(err == 0);

  err = js_create_reference(env, argv[2], 1, &port->on_drain);
  assert(err == 0);

  err = js_create_reference(env, argv[3], 1, &port->on_flush);
  assert(err == 0);

  err = js_create_reference(env, argv[4], 1, &port->on_end);
  assert(err == 0);

  err = js_create_reference(env, argv[5], 1, &port->on_close);
  assert(err == 0);

  err = js_add_deferred_teardown_callback(env, bare_channel__on_teardown, (void *) port, &port->teardown);
  assert(err == 0);

  err = uv_sem_init(&port->wait, 1);
  assert(err == 0);

#define V(signal) \
  err = uv_async_init(loop, &port->signals.signal, bare_channel__on_##signal); \
  assert(err == 0); \
  port->signals.signal.data = (void *) port;
  V(drain)
  V(flush)
#undef V

  uv_async_send(&port->signals.flush);

  port->state |= bare_channel_port_state_inited;

  js_value_t *result;
  err = js_create_int32(env, id, &result);
  assert(err == 0);

  return result;
}

static js_value_t *
bare_channel_port_destroy(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 2;
  js_value_t *argv[2];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  assert(argc == 2);

  bare_channel_t *channel;
  err = js_get_sharedarraybuffer_info(env, argv[0], (void **) &channel, NULL);
  assert(err == 0);

  int id;
  err = js_get_value_int32(env, argv[1], &id);
  assert(err == 0);

  bare_channel_port_t *port = &channel->ports[id];

  port->state |= bare_channel_port_state_destroying;
  port->closing = 2;

#define V(signal) \
  uv_close((uv_handle_t *) &port->signals.signal, bare_channel__on_close);
  V(drain)
  V(flush)
#undef V

  return NULL;
}

static js_value_t *
bare_channel_port_wait(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 2;
  js_value_t *argv[2];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  assert(argc == 2);

  bare_channel_t *channel;
  err = js_get_sharedarraybuffer_info(env, argv[0], (void **) &channel, NULL);
  assert(err == 0);

  int id;
  err = js_get_value_int32(env, argv[1], &id);
  assert(err == 0);

  bare_channel_port_t *port = &channel->ports[id];

  while (port->cursors.read == port->cursors.write) {
    port->state |= bare_channel_port_state_waiting;

    uv_sem_wait(&port->wait);

    port->state &= ~bare_channel_port_state_waiting;
  }

  return NULL;
}

static js_value_t *
bare_channel_port_read(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 2;
  js_value_t *argv[2];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  assert(argc == 2);

  bare_channel_t *channel;
  err = js_get_sharedarraybuffer_info(env, argv[0], (void **) &channel, NULL);
  assert(err == 0);

  int id;
  err = js_get_value_int32(env, argv[1], &id);
  assert(err == 0);

  bare_channel_port_t *port = &channel->ports[id];
  bare_channel_port_t *sender = &channel->ports[(id + 1) & 1];

  js_value_t *result;

  if (sender->state & bare_channel_port_state_exiting) {
    js_value_t *ctx;
    err = js_get_reference_value(env, port->ctx, &ctx);
    assert(err == 0);

    js_value_t *on_end;
    err = js_get_reference_value(env, port->on_end, &on_end);
    assert(err == 0);

    js_call_function(env, ctx, on_end, 0, NULL, NULL);

    err = js_get_null(env, &result);
    assert(err == 0);

    return result;
  }

  if (port->cursors.read == port->cursors.write) {
    err = js_get_null(env, &result);
    assert(err == 0);

    return result;
  }

  bare_channel_message_t *message = &port->messages[port->cursors.read];

  switch (message->type) {
  case bare_channel_message_end: {
    port->state |= bare_channel_port_state_ended;

    js_value_t *ctx;
    err = js_get_reference_value(env, port->ctx, &ctx);
    assert(err == 0);

    js_value_t *on_end;
    err = js_get_reference_value(env, port->on_end, &on_end);
    assert(err == 0);

    js_call_function(env, ctx, on_end, 0, NULL, NULL);

    err = js_get_null(env, &result);
    assert(err == 0);

    return result;
  }

  case bare_channel_message_data:
  default:
    err = js_create_arraybuffer_with_backing_store(env, message->backing_store, NULL, NULL, &result);
    assert(err == 0);

    err = js_release_arraybuffer_backing_store(env, message->backing_store);
    assert(err == 0);
    break;
  }

  port->cursors.read = (port->cursors.read + 1) & (BARE_CHANNEL_PORT_CAPACITY - 1);

  if (sender->state & bare_channel_port_state_inited) {
    uv_async_send(&sender->signals.drain);
  }

  return result;
}

static js_value_t *
bare_channel_port_write(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 3;
  js_value_t *argv[3];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  assert(argc == 3);

  bare_channel_t *channel;
  err = js_get_sharedarraybuffer_info(env, argv[0], (void **) &channel, NULL);
  assert(err == 0);

  int id;
  err = js_get_value_int32(env, argv[1], &id);
  assert(err == 0);

  bare_channel_port_t *port = &channel->ports[id];
  bare_channel_port_t *receiver = &channel->ports[(id + 1) & 1];

  bool success = true;

  int next = (receiver->cursors.write + 1) & (BARE_CHANNEL_PORT_CAPACITY - 1);

  if (next == receiver->cursors.read) success = false;
  else {
    bare_channel_message_t *message = &receiver->messages[receiver->cursors.write];

    message->type = bare_channel_message_data;

    err = js_get_arraybuffer_backing_store(env, argv[2], &message->backing_store);
    assert(err == 0);

    err = js_detach_arraybuffer(env, argv[2]);
    assert(err == 0);

    receiver->cursors.write = (receiver->cursors.write + 1) & (BARE_CHANNEL_PORT_CAPACITY - 1);

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
bare_channel_port_end(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 2;
  js_value_t *argv[2];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  assert(argc == 2);

  bare_channel_t *channel;
  err = js_get_sharedarraybuffer_info(env, argv[0], (void **) &channel, NULL);
  assert(err == 0);

  int id;
  err = js_get_value_int32(env, argv[1], &id);
  assert(err == 0);

  bare_channel_port_t *port = &channel->ports[id];
  bare_channel_port_t *receiver = &channel->ports[(id + 1) & 1];

  bool success = true;

  int next = (receiver->cursors.write + 1) & (BARE_CHANNEL_PORT_CAPACITY - 1);

  if (next == receiver->cursors.read) success = false;
  else {
    bare_channel_message_t *message = &receiver->messages[receiver->cursors.write];

    message->type = bare_channel_message_end;

    receiver->cursors.write = (receiver->cursors.write + 1) & (BARE_CHANNEL_PORT_CAPACITY - 1);

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
bare_channel_port_ref(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 2;
  js_value_t *argv[2];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  assert(argc == 2);

  bare_channel_t *channel;
  err = js_get_sharedarraybuffer_info(env, argv[0], (void **) &channel, NULL);
  assert(err == 0);

  int id;
  err = js_get_value_int32(env, argv[1], &id);
  assert(err == 0);

  bare_channel_port_t *port = &channel->ports[id];

#define V(signal) \
  uv_ref((uv_handle_t *) &port->signals.signal);
  V(drain)
  V(flush)
#undef V

  return NULL;
}

static js_value_t *
bare_channel_port_unref(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 2;
  js_value_t *argv[2];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  assert(argc == 2);

  bare_channel_t *channel;
  err = js_get_sharedarraybuffer_info(env, argv[0], (void **) &channel, NULL);
  assert(err == 0);

  int id;
  err = js_get_value_int32(env, argv[1], &id);
  assert(err == 0);

  bare_channel_port_t *port = &channel->ports[id];

#define V(signal) \
  uv_unref((uv_handle_t *) &port->signals.signal);
  V(drain)
  V(flush)
#undef V

  return NULL;
}

static js_value_t *
bare_channel_exports(js_env_t *env, js_value_t *exports) {
  int err;

#define V(name, fn) \
  { \
    js_value_t *val; \
    err = js_create_function(env, name, -1, fn, NULL, &val); \
    assert(err == 0); \
    err = js_set_named_property(env, exports, name, val); \
    assert(err == 0); \
  }

  V("channelInit", bare_channel_init)

  V("portInit", bare_channel_port_init)
  V("portDestroy", bare_channel_port_destroy)
  V("portWait", bare_channel_port_wait)
  V("portRead", bare_channel_port_read)
  V("portWrite", bare_channel_port_write)
  V("portEnd", bare_channel_port_end)
  V("portRef", bare_channel_port_ref)
  V("portUnref", bare_channel_port_unref)
#undef V

  return exports;
}

BARE_MODULE(bare_channel, bare_channel_exports)
