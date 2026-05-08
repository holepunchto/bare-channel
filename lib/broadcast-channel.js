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

    this._flush = null

    this._handle = binding.broadcastPortInit(channel.handle)
  }

  async read() {
    while (this._flush !== null) await this._flush.promise

    this._flush = Promise.withResolvers()

    while (true) {
      if (this._queue.length > 0) {
        this._onflush()

        return this._queue.shift()
      }

      while (this._queue.length < this._queue.capacity) {
        const data = binding.broadcastPortRead(this._handle, this._channel.handle)

        if (data === null) break

        this._queue.push(decode(this._channel, data))
      }

      await new Promise((resolve) => setTimeout(resolve))
    }
  }

  write(value, opts = {}) {
    const data = encode(this._channel, value, opts)

    return binding.broadcastPortWrite(this._handle, this._channel.handle, data)
  }

  _onflush() {
    if (this._flush === null) return

    const flushing = this._flush
    this._flush = null
    flushing.resolve()
  }
}
