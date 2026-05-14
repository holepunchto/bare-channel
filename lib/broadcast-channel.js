const binding = require('../binding')
const Queue = require('./queue')
const { encode, decode } = require('./encode-decode')

module.exports = class BroadcastChannel {
  constructor(opts = {}) {
    const { handle = binding.channelBroadcastInit(), interfaces = [] } = opts

    this.handle = handle
    this.interfaces = interfaces
  }

  connect() {
    return new BroadcastPort(this)
  }

  static from(handle, opts = {}) {
    return new BroadcastChannel({ ...opts, handle })
  }
}

class BroadcastPort {
  constructor(channel) {
    this._channel = channel
    this._queue = new Queue()

    this._drain = null
    this._flush = null
    this._close = null

    this._handle = binding.broadcastPortInit(this._channel.handle)
  }

  async read() {
    while (this._flush !== null) await this._flush.promise

    this._flush = Promise.withResolvers()

    while (true) {
      if (this._queue.length > 0) {
        this._onflush()

        return this._queue.shift()
      }

      await new Promise((resolve) => setTimeout(resolve))

      while (this._queue.length < this._queue.capacity) {
        const data = binding.broadcastPortRead(this._handle)

        if (data === null) break

        this._queue.push(decode(this._channel, data))
      }
    }
  }

  async write(value, opts = {}) {
    if (value === null) return false

    while (this._drain !== null) await this._drain.promise

    this._drain = Promise.withResolvers()

    const data = encode(this._channel, value, opts)

    while (true) {
      const flushed = binding.broadcastPortWrite(this._handle, data)

      if (flushed) {
        this._ondrain()

        return true
      }

      await new Promise((resolve) => setTimeout(resolve))
    }
  }

  async close() {
    while (this._drain !== null) await this._drain.promise

    if (this._close !== null) return this._close.promise

    this._close = Promise.withResolvers()

    binding.broadcastPortClose(this._handle)

    this._close.resolve()
  }

  _ondrain() {
    if (this._drain === null) return

    const draining = this._drain
    this._drain = null
    draining.resolve()
  }

  _onflush() {
    if (this._flush === null) return

    const flushing = this._flush
    this._flush = null
    flushing.resolve()
  }
}
