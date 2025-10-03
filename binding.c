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

  struct {
    atomic_bool active;
    bool ended;
    bool remote_ended;
    uint8_t closing;
    bool exiting;
  } state;

  bare_channel_message_t messages[BARE_CHANNEL_PORT_CAPACITY];

  struct {
    atomic_int read;
    atomic_int write;
  } cursors;

  struct {
    uv_mutex_t drain;
    uv_mutex_t flush;
  } locks;

  struct {
    uv_cond_t drain;
    uv_cond_t flush;
  } conditions;

  struct {
    uv_async_t drain;
    uv_async_t flush;
    uv_async_t end;
  } signals;

  js_env_t *env;
  js_ref_t *ctx;
  js_ref_t *on_drain;
  js_ref_t *on_flush;
  js_ref_t *on_end;
  js_ref_t *on_remoteend;
  js_ref_t *on_close;

  js_deferred_teardown_t *teardown;
};

struct bare_channel_s {
  atomic_int next;

  bare_channel_port_t ports[2];
};

static inline bare_channel_message_t *
bare_channel__peek_read(bare_channel_port_t *port) {
  int read = atomic_load_explicit(&port->cursors.read, memory_order_relaxed);

  int write = atomic_load_explicit(&port->cursors.write, memory_order_acquire);

  if (read == write) return NULL;

  return &port->messages[read];
}

static inline void
bare_channel__push_read(bare_channel_port_t *port) {
  int err;

  int read = atomic_load_explicit(&port->cursors.read, memory_order_relaxed);

  int next = (read + 1) & (BARE_CHANNEL_PORT_CAPACITY - 1);

  atomic_store_explicit(&port->cursors.read, next, memory_order_release);

  bare_channel_port_t *sender = &port->channel->ports[(port->id + 1) & 1];

  uv_mutex_lock(&sender->locks.drain);

  uv_cond_signal(&sender->conditions.drain);

  uv_mutex_unlock(&sender->locks.drain);

  err = uv_async_send(&sender->signals.drain);
  assert(err == 0);
}

static inline bare_channel_message_t *
bare_channel__peek_write(bare_channel_port_t *port) {
  int write = atomic_load_explicit(&port->cursors.write, memory_order_relaxed);

  int read = atomic_load_explicit(&port->cursors.read, memory_order_acquire);

  int next = (write + 1) & (BARE_CHANNEL_PORT_CAPACITY - 1);

  if (next == read) return NULL;

  return &port->messages[write];
}

static inline void
bare_channel__push_write(bare_channel_port_t *port) {
  int err;

  int write = atomic_load_explicit(&port->cursors.write, memory_order_relaxed);

  int next = (write + 1) & (BARE_CHANNEL_PORT_CAPACITY - 1);

  atomic_store_explicit(&port->cursors.write, next, memory_order_release);

  if (atomic_load_explicit(&port->state.active, memory_order_acquire)) {
    uv_mutex_lock(&port->locks.flush);

    uv_cond_signal(&port->conditions.flush);

    uv_mutex_unlock(&port->locks.flush);

    err = uv_async_send(&port->signals.flush);
    assert(err == 0);
  }
}

static inline void
bare_channel__port_close(bare_channel_port_t *port);

static void
bare_channel__on_drain(uv_async_t *handle) {
  int err;

  bare_channel_port_t *port = handle->data;

  if (port->state.ended) return;

  if (port->state.exiting) {
    bare_channel_port_t *receiver = &port->channel->ports[(port->id + 1) & 1];

    bare_channel_message_t *message = bare_channel__peek_write(receiver);

    if (message) {
      message->type = bare_channel_message_end;

      bare_channel__push_write(receiver);

      port->state.ended = true;
    }
  } else {
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
}

static void
bare_channel__on_flush(uv_async_t *handle) {
  int err;

  bare_channel_port_t *port = handle->data;

  if (port->state.remote_ended) return;

  if (port->state.exiting) {
    while (true) {
      bare_channel_message_t *message = bare_channel__peek_read(port);

      if (message == NULL) break;

      if (message->type == bare_channel_message_end) {
        bare_channel__push_read(port);

        port->state.remote_ended = true;

        bare_channel_port_t *sender = &port->channel->ports[(port->id + 1) & 1];

        err = uv_async_send(&sender->signals.end);
        assert(err == 0);

        if (port->state.ended) bare_channel__port_close(port);
      } else {
        bare_channel__push_read(port);
      }
    }
  } else {
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
}

static void
bare_channel__on_end(uv_async_t *handle) {
  int err;

  bare_channel_port_t *port = handle->data;

  if (port->state.exiting) {
    if (port->state.remote_ended) bare_channel__port_close(port);
  } else {
    js_env_t *env = port->env;

    js_handle_scope_t *scope;
    err = js_open_handle_scope(env, &scope);
    assert(err == 0);

    js_value_t *ctx;
    err = js_get_reference_value(env, port->ctx, &ctx);
    assert(err == 0);

    js_value_t *on_end;
    err = js_get_reference_value(env, port->on_end, &on_end);
    assert(err == 0);

    js_call_function(env, ctx, on_end, 0, NULL, NULL);

    err = js_close_handle_scope(env, scope);
    assert(err == 0);
  }
}

static void
bare_channel__on_close(uv_handle_t *handle) {
  int err;

  bare_channel_port_t *port = handle->data;

  if (--port->state.closing != 0) return;

  js_deferred_teardown_t *teardown = port->teardown;

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

  err = js_delete_reference(env, port->on_remoteend);
  assert(err == 0);

  err = js_delete_reference(env, port->on_close);
  assert(err == 0);

  err = js_delete_reference(env, port->ctx);
  assert(err == 0);

#define V(lock) \
  uv_mutex_destroy(&port->locks.lock);
  V(drain)
  V(flush)
#undef V

#define V(condition) \
  uv_cond_destroy(&port->conditions.condition);
  V(drain)
  V(flush)
#undef V

  if (!port->state.exiting) js_call_function(env, ctx, on_destroy, 0, NULL, NULL);

  err = js_close_handle_scope(env, scope);
  assert(err == 0);

  err = js_finish_deferred_teardown_callback(teardown);
  assert(err == 0);
}

static void
bare_channel__on_teardown(js_deferred_teardown_t *handle, void *data) {
  int err;

  bare_channel_port_t *port = data;

  port->state.exiting = true;

  if (port->state.closing) return;

  if (!port->state.ended) {
    err = uv_async_send(&port->signals.drain);
    assert(err == 0);
  }

  if (!port->state.remote_ended) {
    err = uv_async_send(&port->signals.flush);
    assert(err == 0);
  }
}

static js_value_t *
bare_channel_init(js_env_t *env, js_callback_info_t *info) {
  int err;

  js_value_t *handle;

  bare_channel_t *channel;
  err = js_create_sharedarraybuffer(env, sizeof(bare_channel_t), (void **) &channel, &handle);
  assert(err == 0);

  atomic_init(&channel->next, 0);

  for (uint8_t id = 0; id < 2; id++) {
    bare_channel_port_t *port = &channel->ports[id];

    port->id = id;
    port->channel = channel;

    memset(&port->state, 0, sizeof(port->state));

    atomic_init(&port->state.active, false);

    atomic_init(&port->cursors.read, 0);
    atomic_init(&port->cursors.write, 0);
  }

  return handle;
}

static js_value_t *
bare_channel_port_init(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 7;
  js_value_t *argv[7];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  assert(argc == 7);

  bare_channel_t *channel;
  err = js_get_sharedarraybuffer_info(env, argv[0], (void **) &channel, NULL);
  assert(err == 0);

  int id = atomic_fetch_add_explicit(&channel->next, 1, memory_order_acq_rel);

  if (id >= 2) {
    err = js_throw_error(env, NULL, "Channel already fully connected");
    assert(err == 0);

    return NULL;
  }

  uv_loop_t *loop;
  err = js_get_env_loop(env, &loop);
  assert(err == 0);

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

  err = js_create_reference(env, argv[5], 1, &port->on_remoteend);
  assert(err == 0);

  err = js_create_reference(env, argv[6], 1, &port->on_close);
  assert(err == 0);

  err = js_add_deferred_teardown_callback(env, bare_channel__on_teardown, (void *) port, &port->teardown);
  assert(err == 0);

#define V(lock) \
  err = uv_mutex_init(&port->locks.lock); \
  assert(err == 0);
  V(drain)
  V(flush)
#undef V

#define V(condition) \
  err = uv_cond_init(&port->conditions.condition); \
  assert(err == 0);
  V(drain)
  V(flush)
#undef V

#define V(signal) \
  err = uv_async_init(loop, &port->signals.signal, bare_channel__on_##signal); \
  assert(err == 0); \
  port->signals.signal.data = (void *) port;
  V(drain)
  V(flush)
  V(end)
#undef V

  err = uv_async_send(&port->signals.flush);
  assert(err == 0);

  atomic_store_explicit(&port->state.active, true, memory_order_release);

  js_value_t *result;
  err = js_create_int32(env, id, &result);
  assert(err == 0);

  return result;
}

static js_value_t *
bare_channel_port_wait_drain(js_env_t *env, js_callback_info_t *info) {
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

  uv_mutex_lock(&port->locks.drain);

  while (bare_channel__peek_write(port) == NULL) {
    uv_cond_wait(&port->conditions.drain, &port->locks.drain);
  }

  uv_mutex_unlock(&port->locks.drain);

  return NULL;
}

static js_value_t *
bare_channel_port_wait_flush(js_env_t *env, js_callback_info_t *info) {
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

  uv_mutex_lock(&port->locks.flush);

  while (bare_channel__peek_read(port) == NULL) {
    uv_cond_wait(&port->conditions.flush, &port->locks.flush);
  }

  uv_mutex_unlock(&port->locks.flush);

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

  bare_channel_message_t *message = bare_channel__peek_read(port);

  js_value_t *result;

  if (message) {
    switch (message->type) {
    case bare_channel_message_end: {
      bare_channel__push_read(port);

      port->state.remote_ended = true;

      bare_channel_port_t *sender = &port->channel->ports[(port->id + 1) & 1];

      err = uv_async_send(&sender->signals.end);
      assert(err == 0);

      js_value_t *ctx;
      err = js_get_reference_value(env, port->ctx, &ctx);
      assert(err == 0);

      js_value_t *on_remoteend;
      err = js_get_reference_value(env, port->on_remoteend, &on_remoteend);
      assert(err == 0);

      js_call_function(env, ctx, on_remoteend, 0, NULL, NULL);

      err = js_get_null(env, &result);
      assert(err == 0);
      break;
    }

    case bare_channel_message_data:
    default:
      err = js_create_arraybuffer_with_backing_store(env, message->backing_store, NULL, NULL, &result);
      assert(err == 0);

      err = js_release_arraybuffer_backing_store(env, message->backing_store);
      assert(err == 0);

      bare_channel__push_read(port);
    }
  } else {
    err = js_get_null(env, &result);
    assert(err == 0);
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

  bare_channel_port_t *receiver = &channel->ports[(id + 1) & 1];

  bare_channel_message_t *message = bare_channel__peek_write(receiver);

  if (message) {
    message->type = bare_channel_message_data;

    err = js_get_arraybuffer_backing_store(env, argv[2], &message->backing_store);
    assert(err == 0);

    err = js_detach_arraybuffer(env, argv[2]);
    assert(err == 0);

    bare_channel__push_write(receiver);
  }

  js_value_t *result;
  err = js_get_boolean(env, message != NULL, &result);
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

  bare_channel_port_t *receiver = &channel->ports[(id + 1) & 1];

  bare_channel_message_t *message = bare_channel__peek_write(receiver);

  if (message) {
    message->type = bare_channel_message_end;

    bare_channel__push_write(receiver);

    bare_channel_port_t *port = &channel->ports[(id + 1) & 1];

    port->state.ended = true;
  }

  js_value_t *result;
  err = js_get_boolean(env, message != NULL, &result);
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
  V(end)
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
  V(end)
#undef V

  return NULL;
}

static inline void
bare_channel__port_close(bare_channel_port_t *port) {
  port->state.closing = 3;

#define V(signal) \
  uv_close((uv_handle_t *) &port->signals.signal, bare_channel__on_close);
  V(drain)
  V(flush)
  V(end)
#undef V
}

static js_value_t *
bare_channel_port_close(js_env_t *env, js_callback_info_t *info) {
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

  bare_channel__port_close(port);

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
  V("portWaitDrain", bare_channel_port_wait_drain)
  V("portWaitFlush", bare_channel_port_wait_flush)
  V("portRead", bare_channel_port_read)
  V("portWrite", bare_channel_port_write)
  V("portEnd", bare_channel_port_end)
  V("portRef", bare_channel_port_ref)
  V("portUnref", bare_channel_port_unref)
  V("portClose", bare_channel_port_close)
#undef V

  return exports;
}

BARE_MODULE(bare_channel, bare_channel_exports)
