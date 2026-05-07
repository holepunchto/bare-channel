const binding = require('../binding')
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

    this._handle = binding.broadcastPortInit(channel.handle)
  }

  read() {
    const data = binding.broadcastPortRead(this._handle, this._channel.handle)

    if (data == null) return null

    return decode(this._channel, data)
  }

  write(value, opts = {}) {
    const data = encode(this._channel, value, opts)

    return binding.broadcastPortWrite(this._handle, this._channel.handle, data)
  }
}
